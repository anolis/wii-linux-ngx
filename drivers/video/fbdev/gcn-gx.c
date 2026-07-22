/*
 * drivers/video/fbdev/gcn-gx.c
 *
 * Nintendo GameCube/Wii GX GPU minimal driver
 *
 * Provides hardware EFB->XFB copy to replace the per-vsync software
 * RGB->YUV conversion done in gcnfb.c. The GX has a fixed-function
 * EFB->XFB blit unit that performs this conversion in hardware, freeing
 * the 729MHz Broadway CPU from doing it every frame.
 *
 * Pipeline (called once per vsync from vi_irq_handler):
 *   1. Tile the linear virtual framebuffer into GX texture format
 *   2. Point texture map 0 at the tiled buffer
 *   3. Draw a fullscreen textured quad to the EFB
 *   4. Trigger EFB->XFB copy (hardware RGB->YUV)
 *
 * The GX command processor (CP) is fed via the Write-Gather Pipe (wgPipe),
 * a 32-byte MMIO window at 0xCC008000 that batches writes into a FIFO.
 * Commands:
 *   0x08 + regidx(u8) + val(u32)             → CP register write
 *   0x10 + ((n-1)<<16|addr)(u32) + n×u32     → XF register write
 *   0x61 + val(u32)                           → BP register write
 *   0x80|vtxfmt + count(u16) + vertices       → draw primitive
 *
 * Register reference derived from libogc (devkitPro/libogc, MIT licence)
 * and YAGCD (Yet Another GameCube Documentation).
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/irqdomain.h>
#include <linux/string.h>
#include <linux/workqueue.h>
#include <asm/cacheflush.h>
#include <asm/div64.h>
#include <asm/page.h>

#include "gcn-gx.h"

static void __iomem *hw_base;		/* 0x0C000000, size GX_HW_MAP_SIZE */
static u16 __iomem *cp_regs;		/* hw_base + 0x0000 */
static u16 __iomem *pe_regs;		/* hw_base + 0x1000 */
static u32 __iomem *pi_regs;		/* hw_base + 0x3000 (PI, 32-bit) */

/* Byte offset of the next GX command byte within gx_fifo_buf.
 * GX commands are written here directly; gx_submit_cmds() advances
 * PI_FIFO_WPTR so the CP picks them up — bypasses the wgPipe entirely.
 */
static u32 fifo_pos;

/* FIFO buffer — must be in memory the GPU can DMA, 32-byte aligned */
#define GX_FIFO_MEM1_PHYS	0x01684000
static void *gx_fifo_buf_raw;
static void *gx_fifo_buf;

/* Texture tile buffer: virtual FB converted to GX 4×4 tiled format */
static void *gx_tex_raw;
static void *gx_tex_buf;
static bool gx_log_next_submit;
static u32 gx_current_frame;
static u16 gx_expected_token;
static unsigned int gx_pe_finish_irq;
static u32 gx_pe_finish_count;

#define GX_PE_FINISH_HWIRQ	10

enum gx_finish_diag_phase {
	GX_DIAG_SEED,
	GX_DIAG_WAIT_SEED,
	GX_DIAG_WAIT_GREEN,
	GX_DIAG_WAIT_DRAW,
	GX_DIAG_DONE,
};

static enum gx_finish_diag_phase gx_diag_phase;
static u32 gx_diag_finish_baseline;

struct gx_rgb565_work {
	struct work_struct work;
	const void *vfb;
	u32 xfb_phys;
	u16 width;
	u16 height;
};

static struct gx_rgb565_work gx_rgb565_work;
static DEFINE_SPINLOCK(gx_rgb565_work_lock);
static u32 gx_rgb565_work_runs;
static u32 gx_rgb565_ready_xfb;
static u32 gx_rgb565_present_count;
static bool gx_rgb565_work_busy;
static bool gx_rgb565_boot_deferred;

static inline u16 pe_read(int reg)
{
	return in_be16(pe_regs + reg);
}

static inline void pe_write(int reg, u16 val)
{
	out_be16(pe_regs + reg, val);
}

static irqreturn_t gx_pe_finish_handler(int irq, void *data)
{
	static unsigned int log_count;
	u16 status = pe_read(PE_REG_INTR_STATUS);
	u32 count;

	/* PE status bits are write-one-to-clear; preserve both enable bits. */
	pe_write(PE_REG_INTR_STATUS, (status & 0x0003) | PE_FINISH_BIT);
	count = ACCESS_ONCE(gx_pe_finish_count) + 1;
	ACCESS_ONCE(gx_pe_finish_count) = count;
	if (log_count++ < 4)
		pr_info("gcn-gx: PE finish IRQ count=%u status=%04x\n",
			count, status);

	return IRQ_HANDLED;
}

/* Set to true after successful gcn_gx_init(); guards vsync path */
bool gx_accel_ready;
EXPORT_SYMBOL_GPL(gx_accel_ready);

/* ------------------------------------------------------------------ */
/* Low-level CP / PI / wgPipe helpers                                  */
/* ------------------------------------------------------------------ */

static inline void cp_write(int reg, u16 val)
{
	out_be16(cp_regs + reg, val);
}

static inline u16 cp_read(int reg)
{
	return in_be16(cp_regs + reg);
}

/*
 * PI FIFO registers (u32, big-endian) at 0x0C003000 + offset:
 *   index 3 (0x0C): FIFO_BASE  — physical start of FIFO buffer
 *   index 4 (0x10): FIFO_END   — physical end of FIFO buffer
 *   index 5 (0x14): FIFO_WPTR  — write pointer; wgPipe DMA bursts here
 * All read back as 0x00000000 after mini, meaning wgPipe WPTR = 0
 * (physical address 0x00000000 = kernel exception vectors = crash on first burst).
 */
#define PI_REG_FIFO_BASE	3
#define PI_REG_FIFO_END		4
#define PI_REG_FIFO_WPTR	5

static inline void pi_write(int reg, u32 val)
{
	iowrite32be(val, pi_regs + reg);
}

static inline u32 pi_read(int reg)
{
	return ioread32be(pi_regs + reg);
}

/*
 * GX command byte writers — append to gx_fifo_buf at fifo_pos.
 * gx_submit_cmds() later flushes dcache and advances PI_FIFO_WPTR
 * so the CP picks up the commands, bypassing the wgPipe entirely.
 * (Write-through PTEs for the wgPipe address cause the CPU to attempt
 * a cache-line-fill READ from the write-only wgPipe hardware, hanging
 * the bus; this approach avoids the problem.)
 */
static inline void gx_wr8(u8 val)
{
	((u8 *)gx_fifo_buf)[fifo_pos++] = val;
}

static inline void gx_wr16be(u16 val)
{
	gx_wr8(val >> 8);
	gx_wr8(val & 0xff);
}

static inline void gx_wr32be(u32 val)
{
	gx_wr8(val >> 24);
	gx_wr8((val >> 16) & 0xff);
	gx_wr8((val >> 8) & 0xff);
	gx_wr8(val & 0xff);
}

static inline void gx_load_bp_reg(u32 val)
{
	gx_wr8(GX_CMD_LOAD_BP_REG);
	gx_wr32be(val);
}

static inline void gx_load_cp_reg(u8 reg, u32 val)
{
	gx_wr8(0x08);
	gx_wr8(reg);
	gx_wr32be(val);
}

static inline void gx_load_xf_reg(u32 addr, u32 val)
{
	gx_wr8(0x10);
	gx_wr32be(addr & 0xffff);
	gx_wr32be(val);
}

static inline void gx_load_xf_regs_n(u32 addr, u32 count)
{
	gx_wr8(0x10);
	gx_wr32be(((count - 1) << 16) | (addr & 0xffff));
}

static inline void wg_f32_bits(u32 bits)
{
	gx_wr32be(bits);
}

/* IEEE 754 constants */
#define F32_ZERO	0x00000000U
#define F32_ONE		0x3F800000U
#define F32_NEG_ONE	0xBF800000U
#define F32_16M		0x4B7FFFFFU	/* 16777215.0 */
#define F32_NEG(b)	((b) ^ 0x80000000U)

/* f32_from_u16 - encode a u16 integer as IEEE 754 single-precision bits */
static u32 f32_from_u16(u16 n)
{
	u32 msb;

	if (!n)
		return F32_ZERO;
	msb = 31 - __builtin_clz((u32)n);
	return ((127 + msb) << 23) | (((u32)n << (23 - msb)) & 0x7FFFFF);
}

/*
 * f32_div_u16 - compute num/den as IEEE 754 bits using 64-bit fixed-point.
 * Precision: ~40 significant bits; error < 2^-17 relative (adequate for
 * GPU viewport and projection math).
 */
static u32 f32_div_u16(u16 num, u16 den)
{
	u64 q;
	int msb, exp;
	u32 mant;

	if (!num)
		return F32_ZERO;
	q = (u64)num << 40;
	do_div(q, (u32)den);	/* avoids __udivdi3 on 32-bit PowerPC */
	if (!q)
		return F32_ZERO;
	msb = 63 - __builtin_clzll(q);
	exp = 127 + msb - 40;
	if (exp <= 0 || exp >= 255)
		return F32_ZERO;
	if (msb >= 23)
		mant = (u32)((q >> (msb - 23)) & 0x7FFFFF);
	else
		mant = (u32)((q << (23 - msb)) & 0x7FFFFF);
	return ((u32)exp << 23) | mant;
}

static void gx_load_identity_pos_mtx0(void)
{
	/* GX_LoadPosMtxImm(identity, GX_PNMTX0): XF 0x0000–0x000B */
	gx_load_xf_regs_n(0x0000, 12);
	wg_f32_bits(F32_ONE);  wg_f32_bits(F32_ZERO);
	wg_f32_bits(F32_ZERO); wg_f32_bits(F32_ZERO);
	wg_f32_bits(F32_ZERO); wg_f32_bits(F32_ONE);
	wg_f32_bits(F32_ZERO); wg_f32_bits(F32_ZERO);
	wg_f32_bits(F32_ZERO); wg_f32_bits(F32_ZERO);
	wg_f32_bits(F32_ONE);  wg_f32_bits(F32_ZERO);

	/* GX_SetCurrentMtx(GX_PNMTX0): libogc writes CP 0x30 and XF 0x1018. */
	gx_load_cp_reg(0x30, 0);
	gx_load_xf_reg(0x1018, 0);

	/*
	 * GX_LoadTexMtxImm(identity, GX_TEXMTX0, GX_MTX2x4): XF 0x0078–0x007F
	 *
	 * TEXMTX0 starts immediately after PNMTX9 (10 matrices × 12 regs =
	 * 0x78 regs).  Hardware reset value is undefined; garbage here causes
	 * the XF to produce a q ≈ 0 (or NaN) homogeneous texcoord when using
	 * GX_TG_MTX3x4, which permanently stalls the rasterizer perspective-
	 * divide unit when actual vertex TEX0 data drives the transform.
	 *
	 * Identity 2×4 maps (S, T, 1, 0) → (S, T) unchanged.
	 */
	gx_load_xf_regs_n(0x0078, 8);
	wg_f32_bits(F32_ONE);  wg_f32_bits(F32_ZERO);
	wg_f32_bits(F32_ZERO); wg_f32_bits(F32_ZERO);
	wg_f32_bits(F32_ZERO); wg_f32_bits(F32_ONE);
	wg_f32_bits(F32_ZERO); wg_f32_bits(F32_ZERO);
}

static void gx_load_pos_to_tex_mtx0(u16 width, u16 height)
{
	/*
	 * TEXMTX0 for GX_TG_POS: map object-space quad positions
	 * (x=0..width, y=0..height) to normalized texture coordinates
	 * (s=0..1, t=0..1).  This avoids the direct TEX0 vertex attribute path,
	 * which hardware testing shows wedges the downstream pipeline when
	 * texgen output is enabled.
	 */
	gx_load_xf_regs_n(0x0078, 8);
	wg_f32_bits(f32_div_u16(1, width)); wg_f32_bits(F32_ZERO);
	wg_f32_bits(F32_ZERO);              wg_f32_bits(F32_ZERO);
	wg_f32_bits(F32_ZERO);              wg_f32_bits(f32_div_u16(1, height));
	wg_f32_bits(F32_ZERO);              wg_f32_bits(F32_ZERO);

	/*
	 * GX_SetTexCoordGen(..., GX_TEXMTX0) records GX_TEXMTX0 (30) in the
	 * texcoord0 matrix-index field.  libogc writes this to both CP reg 0x30
	 * and XF 0x1018.  Without this, texcoord0 uses matrix index 0 and ignores
	 * TEXMTX0 entirely.
	 */
	gx_load_cp_reg(0x30, 30 << 6);
	gx_load_xf_reg(0x1018, 30 << 6);
}

static void gx_load_identity_post_mtx(void)
{
	/* GX_LoadTexMtxImm(identity, GX_DTTIDENTITY, GX_MTX3x4). */
	gx_load_xf_regs_n(0x05f4, 12);
	wg_f32_bits(F32_ONE);  wg_f32_bits(F32_ZERO);
	wg_f32_bits(F32_ZERO); wg_f32_bits(F32_ZERO);
	wg_f32_bits(F32_ZERO); wg_f32_bits(F32_ONE);
	wg_f32_bits(F32_ZERO); wg_f32_bits(F32_ZERO);
	wg_f32_bits(F32_ZERO); wg_f32_bits(F32_ZERO);
	wg_f32_bits(F32_ONE);  wg_f32_bits(F32_ZERO);
}

/* gx_wait_idle - wait for the GP to finish processing the FIFO */
static void gx_wait_idle(void)
{
	int timeout = 1000;

	/*
	 * With endian-correct CP access, SR bit 3 is observed after CP_CTRL=0
	 * and SR bit 2 is observed after the GP has consumed the submitted
	 * FIFO.  Either state is quiescent enough to reprogram the FIFO.
	 */
	while (timeout--) {
		u16 sr = cp_read(CP_REG_STATUS);
		u32 rd, wt;

		if (sr & 0x000c)
			return;
		rd = ((u32)cp_read(CP_REG_RD_HI) << 16) |
			cp_read(CP_REG_RD_LO);
		wt = ((u32)cp_read(CP_REG_WT_HI) << 16) |
			cp_read(CP_REG_WT_LO);
		if (rd == wt)
			return;
		udelay(10);
	}
	pr_warn_once("gcn-gx: timed out waiting for GP idle (SR=0x%04x)\n",
		     cp_read(CP_REG_STATUS));
}

static void gx_wait_fifo_empty(void)
{
	int timeout = 1000;

	while (timeout--) {
		u32 rd = ((u32)cp_read(CP_REG_RD_HI) << 16) |
			cp_read(CP_REG_RD_LO);
		u32 wt = ((u32)cp_read(CP_REG_WT_HI) << 16) |
			cp_read(CP_REG_WT_LO);

		if (rd == wt)
			return;
		udelay(10);
	}
	pr_warn_once("gcn-gx: timed out waiting for FIFO empty (SR=0x%04x)\n",
		     cp_read(CP_REG_STATUS));
}

/* ------------------------------------------------------------------ */
/* CP / FIFO initialisation                                             */
/* ------------------------------------------------------------------ */

static int gx_fifo_init(void)
{
	u32 phys_start = (u32)virt_to_phys(gx_fifo_buf);

	pr_info("gcn-gx: fifo_init: phys=0x%08x\n", phys_start);

	/*
	 * Disable CP reads.  Do not write CP BASE/END/WT/RD — writing those
	 * registers causes deferred bus errors or immediate GP faults on this
	 * hardware (bisected over many boots).
	 *
	 * Do not enable GPRESET here either: the first test showed that with
	 * GPRESET active and an empty (zeroed) FIFO, a VI retrace wgPipe burst
	 * feeds zero-bytes to the GP as invalid GX opcodes → crash.  GPRESET
	 * is enabled by gx_submit_cmds() only after valid commands are queued.
	 */
	cp_write(CP_REG_CTRL, 0);
	pr_info("gcn-gx: fifo_init: CR=0 SR=0x%04x\n", cp_read(CP_REG_STATUS));

	/*
	 * All remaining setup (PI BASE/END/WPTR, LINKEN, GPRESET) is
	 * deferred to gx_submit_cmds().  LINKEN causes a deferred CP error
	 * when set here because CP_BASE/END still hold mini's invalid values;
	 * in interrupt context at submit time that error can't propagate.
	 */

	pr_info("gcn-gx: fifo_init: done\n");
	return 0;
}

/* ------------------------------------------------------------------ */
/* Texture tiling                                                       */
/* ------------------------------------------------------------------ */

/*
 * gx_tile_rgb565 - convert linear RGB565 to GX 4×4 tiled format.
 *
 * GX stores GX_TF_RGB565 textures as 4×4 pixel blocks (32 bytes each),
 * tiled left-to-right then top-to-bottom. Within a block, pixels are
 * row-major (4 pixels × 2 bytes = 8 bytes/row, 4 rows per block).
 */
static void gx_tile_rgb565(const u16 *src, u16 *dst, u32 width, u32 height)
{
	u32 bw = width >> 2;	/* blocks wide */
	u32 bh = height >> 2;	/* blocks tall */
	u32 tx, ty, row;

	for (ty = 0; ty < bh; ty++) {
		for (tx = 0; tx < bw; tx++) {
			u16 *tile = dst + (ty * bw + tx) * 16;

			for (row = 0; row < 4; row++) {
				const u16 *sl = src + (ty * 4 + row) * width + tx * 4;

				tile[row * 4 + 0] = sl[0];
				tile[row * 4 + 1] = sl[1];
				tile[row * 4 + 2] = sl[2];
				tile[row * 4 + 3] = sl[3];
			}
		}
	}
}

static u16 gx_reference_rgb565_pixel(u32 x, u32 y, u32 width, u32 height)
{
	u16 color;

	if (x < width / 2 && y < height / 2)
		color = 0xf800;
	else if (x >= width / 2 && y < height / 2)
		color = 0x07e0;
	else if (x < width / 2)
		color = 0x001f;
	else
		color = 0xffff;

	if ((x % 32) == 0 || (y % 32) == 0)
		color = 0x0000;
	if (x == y || x + y == width - 1)
		color = 0xffe0;

	return color;
}

static void gx_fill_reference_rgb565(u16 *dst, u32 width, u32 height)
{
	u32 bw = width >> 2;
	u32 bh = height >> 2;
	u32 tx, ty, x, y;

	for (ty = 0; ty < bh; ty++) {
		for (tx = 0; tx < bw; tx++) {
			u16 *tile = dst + (ty * bw + tx) * 16;

			for (y = 0; y < 4; y++) {
				for (x = 0; x < 4; x++) {
					u32 px = tx * 4 + x;
					u32 py = ty * 4 + y;

					tile[y * 4 + x] =
						gx_reference_rgb565_pixel(px, py,
									  width, height);
				}
			}
		}
	}
}

/*
 * gx_tile_rgb888 - convert linear RGB888 (packed u32) to GX RGB565 tiles.
 *
 * Converts to RGB565 during tiling to avoid the complex GX_TF_RGBA8
 * interleaved block layout. Minor quality loss (5-6-5 truncation).
 */
static void gx_tile_rgb888(const u32 *src, u16 *dst, u32 width, u32 height)
{
	u32 bw = width >> 2;
	u32 bh = height >> 2;
	u32 tx, ty, row;

	for (ty = 0; ty < bh; ty++) {
		for (tx = 0; tx < bw; tx++) {
			u16 *tile = dst + (ty * bw + tx) * 16;

			for (row = 0; row < 4; row++) {
				const u32 *sl = src + (ty * 4 + row) * width + tx * 4;
				int col;

				for (col = 0; col < 4; col++) {
					u32 p = sl[col];

					tile[row * 4 + col] =
						((p >> 8) & 0xf800) |
						((p >> 5) & 0x07e0) |
						((p >> 3) & 0x001f);
				}
			}
		}
	}
}

/* ------------------------------------------------------------------ */
/* GX 2-D rendering state                                              */
/* ------------------------------------------------------------------ */

/*
 * gx_setup_2d_state - configure GX for a fullscreen textured 2-D blit.
 *
 * Programs BP, XF, and CP registers for single-TEV-stage passthrough
 * render from texture map 0 to the full EFB viewport.  Values derived
 * from libogc gx.c and YAGCD.
 */
static void gx_setup_2d_state(u16 width, u16 height)
{
	u32 xo, yo;

	/* ---- BP 0x40: zMode — disable Z compare and Z write for 2D ----
	 * bit 0 = enable, bit 7 = update enable; both 0 = fully disabled.
	 * If mini left Z-compare enabled (e.g. GX_LEQUAL), all pixels rendered
	 * to a fresh EFB would fail the test and nothing would reach the EFB.
	 */
	gx_load_bp_reg(0x40000000);

	/* ---- BP 0x41: blendMode ----
	 * Hardware reset value is 0x00: colorupdate=0, alphaupdate=0 — the PE
	 * silently drops every rasterized pixel without writing the EFB.
	 * bit[3] = colorupdate = 1, bit[4] = alphaupdate = 1, blend disabled.
	 */
	gx_load_bp_reg(0x41000018);

	/*
	 * BP 0x43: PE control.  Match libogc's GX_SetZCompLoc(GX_TRUE) +
	 * GX_SetPixelFmt(GX_PF_RGB8_Z24, GX_ZC_LINEAR): RGB8/Z24 EFB, linear Z,
	 * and Z compare before texture.  If mini left a different EFB pixel
	 * format behind, rasterized pixels can be stored/copied incorrectly.
	 */
	gx_load_bp_reg(0x43000040);

	/* ---- BP 0xF3: alphaCompare ----
	 * Hardware reset value is 0x00: comp0 = NEVER (0), comp1 = NEVER (0),
	 * logic = AND (0).  NEVER AND NEVER = ALWAYS_FAIL — every rasterized
	 * fragment is discarded by the alpha test before it can reach the PE,
	 * making colorupdate meaningless.
	 *
	 * Bit layout (Dolphin BPMemory.h AlphaTest):
	 *   [7:0]   ref0   = 0
	 *   [15:8]  ref1   = 0
	 *   [18:16] comp0  = 7 (GX_ALWAYS)
	 *   [21:19] comp1  = 7 (GX_ALWAYS)
	 *   [23:22] logic  = 0 (GX_AOP_AND)
	 * 0xF33F0000 → ref0=0, ref1=0, comp0=ALWAYS, comp1=ALWAYS, logic=AND
	 * Result: every fragment passes, no pixels are discarded.
	 */
	gx_load_bp_reg(0xF33F0000);

	/* ---- BP 0x00: genMode ----
	 * [2:0]   numtexgens = 1
	 * [6:4]   numcolchans = 0
	 * [13:10] numtevstages - 1 = 0
	 */
	gx_load_bp_reg(0x00000001);

	/* ---- BP 0x20/0x21: scissor ----
	 * GX adds 0x156 (342) to all coordinates internally.
	 */
	xo = 0x156;
	yo = 0x156;
	gx_load_bp_reg(0x20000000 | ((xo & 0x7ff) << 12) | (yo & 0x7ff));
	gx_load_bp_reg(0x21000000 |
		       (((xo + width  - 1) & 0x7ff) << 12) |
		       ((yo + height - 1) & 0xfff));

	/* ---- BP 0xC0: TEV stage 0 colour input ----
	 * formula: result = (a*(1-c) + b*c + d) * scale
	 * a=[15:12], b=[11:8], c=[7:4], d=[3:0]  (CC_TEXC=8, CC_ZERO=15)
	 * a=b=c=ZERO(15), d=TEXC(8) → output = texture colour
	 * clamp=[19]=1, all other op bits = 0
	 */
	gx_load_bp_reg(0xC008FFF8);

	/* ---- BP 0xC1: TEV stage 0 alpha input ----
	 * a=[15:13], b=[12:10], c=[9:7], d=[6:4]  (CA_TEXA=4, CA_ZERO=7)
	 * a=b=c=ZERO(7), d=TEXA(4) → output = texture alpha
	 * clamp=[19]=1
	 */
	gx_load_bp_reg(0xC108FFC0);

	/* ---- BP 0x25: TEV order stages 0/1 (tevRasOrder[0]) ----
	 * [2:0] texmap=0, [5:3] texcoord=0, [6] texenable=1,
	 * [9:7] rascolor=GX_ALPHA_BUMP=7
	 */
	gx_load_bp_reg(0x250003C0);

	/* ---- XF 0x103f: numtexcoord generators = 1 ---- */
	gx_load_xf_reg(0x103f, 1);

	/* ---- XF 0x1040: texCoordGen[0] ----
	 * Hardware TexMtxInfo bit layout (Dolphin XFMemory.h):
	 *   bit[0]    = projection (0=ST output/no divide, 1=STQ output/perspective divide)
	 *   bits[3:1] = inputform+texgentype (0 = regular matrix multiply)
	 *   bits[11:7] = sourcerow (4 = GX_TG_TEX0)
	 *
	 * CRITICAL: bit[0]=1 (projection=1) tells XF to produce 3 components (STQ)
	 * for perspective divide.  With only a 2×4 matrix at TEXMTX0 (2 rows, no
	 * Q row), the hardware reads a garbage/zero Q from the uninitialized 3rd
	 * row and the rasterizer hangs on ST/Q divide-by-zero from frame 2 onward
	 * (SR=0x0004, CmdIdle stall).
	 *
	 * 0x200 = (srcrow=4 << 7) | 0 → projection=0 (ST), type=regular, src=TEX0.
	 * Matches the 2×4 identity matrix at XF 0x0078 (2-row, no Q component).
	 */
	gx_load_xf_reg(0x1040, 0x200);

	/* ---- XF 0x1050: texCoordGen2[0] ----
	 * normalize=0, postmtx=GX_DTTIDENTITY=63
	 */
	gx_load_xf_reg(0x1050, 0x3F);

	gx_load_identity_pos_mtx0();

	/* ---- XF 0x101a-0x101f: viewport ----
	 * GX_SetViewport(0, 0, w, h, 0, 1):
	 *   x0=w/2, y0=-h/2, z=16777215, x1=w/2+342, y1=h/2+342, f=16777215
	 */
	gx_load_xf_regs_n(0x101a, 6);
	wg_f32_bits(f32_from_u16(width >> 1));
	wg_f32_bits(F32_NEG(f32_from_u16(height >> 1)));
	wg_f32_bits(F32_16M);
	wg_f32_bits(f32_from_u16((width >> 1) + 342));
	wg_f32_bits(f32_from_u16((height >> 1) + 342));
	wg_f32_bits(F32_16M);

	/* ---- XF 0x1020-0x1026: orthographic projection ----
	 * Maps pixel coords [0,w]×[0,h] to NDC [-1,1]×[-1,1] (Y flipped):
	 *   mt[0][0]=2/w, mt[0][3]=-1, mt[1][1]=-2/h, mt[1][3]=1,
	 *   mt[2][2]=-1,  mt[2][3]=0,  type=GX_ORTHOGRAPHIC=1
	 */
	gx_load_xf_regs_n(0x1020, 7);
	wg_f32_bits(f32_div_u16(2, width));
	wg_f32_bits(F32_NEG_ONE);
	wg_f32_bits(F32_NEG(f32_div_u16(2, height)));
	wg_f32_bits(F32_ONE);
	wg_f32_bits(F32_NEG_ONE);
	wg_f32_bits(F32_ZERO);
	gx_wr32be(1);			/* GX_ORTHOGRAPHIC */

	/* ---- CP 0x50/0x60: vertex descriptor ----
	 * VCD_LO [10:9] = GX_VA_POS = GX_DIRECT(1) → 0x200
	 * VCD_HI [1:0]  = GX_VA_TEX0 = GX_DIRECT(1) → 0x001
	 */
	gx_load_cp_reg(0x50, 0x200);
	gx_load_cp_reg(0x60, 0x001);

	/* ---- CP 0x70-0x90: VTXFMT0 attribute format ----
	 * VAT0: pos cnt=[0]=GX_POS_XY(0), type=[3:1]=GX_F32(4) → 0x08
	 *       tex0 cnt=[21]=GX_TEX_ST(1), type=[24:22]=GX_F32(4) → 0x1200000
	 *       bit[30] = libogc validity marker = 0x40000000
	 * VAT1: 0x80000000 (libogc __GX_InitRevBits default)
	 * VAT2: 0x00000000
	 */
	gx_load_cp_reg(0x70, 0x41200008);
	gx_load_cp_reg(0x80, 0x80000000);
	gx_load_cp_reg(0x90, 0x00000000);
}

static void gx_setup_texcoord_parse_state(u16 width, u16 height)
{
	u32 xo, yo;

	gx_load_bp_reg(0x40000000);	/* Z disabled */
	gx_load_bp_reg(0x41000018);	/* colour/alpha update enabled */
	gx_load_bp_reg(0x43000040);	/* RGB8/Z24 EFB, linear Z, zcomp before tex */
	gx_load_bp_reg(0x44000003);	/* GX_SetFieldMask(GX_TRUE, GX_TRUE) */
	gx_load_bp_reg(0x68000000);	/* GX_SetFieldMode(GX_FALSE, GX_FALSE) */
	gx_load_bp_reg(0xF33F0000);	/* alpha test always passes */

	/* genMode: 1 texgen, 0 colour channels, 1 TEV stage */
	gx_load_bp_reg(0x00000001);

	xo = 0x156;
	yo = 0x156;
	gx_load_bp_reg(0x20000000 | ((xo & 0x7ff) << 12) | (yo & 0x7ff));
	gx_load_bp_reg(0x21000000 |
		       (((xo + width  - 1) & 0x7ff) << 12) |
		       ((yo + height - 1) & 0xfff));

	/* TEV stage 0: output texture colour/alpha.
	 * raschan=7 (GX_COLOR_NULL, bits[9:7]=0b111 → 0x380): with numcolchans=0
	 * there is no raster colour token in the pipeline; raschan=0 (GX_COLOR0A0)
	 * causes the TEV to wait forever for a colour that never arrives, stalling
	 * the entire backend from frame 2 onwards (SR stays 0x0004).
	 * texenable=1 (bit[6]=1): TMU fetch from texmap 0.
	 */
	gx_load_bp_reg(0xC008FFF8);
	gx_load_bp_reg(0xC108FFC0);
	gx_load_bp_reg(0x250003C0);

	/*
	 * DIAGNOSTIC: source texcoord 0 from position, not direct TEX0 payload.
	 * TEXMTX0 scales pixel XY into normalized ST for texture fetch.
	 */
	gx_load_xf_reg(0x103f, 1);
	gx_load_xf_reg(0x1040, 0x004);
	gx_load_xf_reg(0x1050, 0x3F);
	gx_load_identity_pos_mtx0();
	gx_load_pos_to_tex_mtx0(width, height);

	gx_load_xf_regs_n(0x101a, 6);
	wg_f32_bits(f32_from_u16(width >> 1));
	wg_f32_bits(F32_NEG(f32_from_u16(height >> 1)));
	wg_f32_bits(F32_16M);
	wg_f32_bits(f32_from_u16((width >> 1) + 342));
	wg_f32_bits(f32_from_u16((height >> 1) + 342));
	wg_f32_bits(F32_16M);

	gx_load_xf_regs_n(0x1020, 7);
	wg_f32_bits(F32_ONE);
	wg_f32_bits(F32_ZERO);
	wg_f32_bits(F32_ONE);
	wg_f32_bits(F32_ZERO);
	wg_f32_bits(F32_ONE);
	wg_f32_bits(F32_ZERO);
	gx_wr32be(1);

	/*
	 * BP 0x30/0x31: suSsize/suTsize for texcoord 0.
	 * The rasterizer uses these to compute per-pixel texcoord stepping and
	 * LOD derivatives even when TEV texture fetch is disabled (texenable=0).
	 * Without explicit values the rasterizer uses whatever mini left, which
	 * may not match our large unnormalized texcoords (0..width, 0..height),
	 * causing it to permanently stall on the first non-zero vertex (frame 2).
	 */
	gx_load_bp_reg(0x30000000 | (u32)(width  - 1));
	gx_load_bp_reg(0x31000000 | (u32)(height - 1));

	/* VCD/VAT: direct XY position only; no TEX0 attribute in the FIFO. */
	gx_load_cp_reg(0x50, 0x200);
	gx_load_cp_reg(0x60, 0x000);
	gx_load_cp_reg(0x70, 0x40000008);
	gx_load_cp_reg(0x80, 0x80000000);
	gx_load_cp_reg(0x90, 0x00000000);
}

static void gx_setup_constant_white_state(u16 width, u16 height)
{
	u32 xo, yo;

	/*
	 * DIAGNOSTIC: the actual devkitPro wii-examples GX triangle demo
	 * (graphics/gx/triangle/source/triangle.c, fetched as a known-working
	 * reference) enables Z-testing -- GX_SetZMode(GX_TRUE, GX_LEQUAL,
	 * GX_TRUE) -- with GX_POS_XYZ vertices, unlike our long-standing
	 * Z-disabled/XY-only setup.  Our copy-clear already writes BP 0x51 =
	 * 0x00ffffff (Z-clear to far/max), so a Z=0 (near) vertex should pass
	 * LEQUAL cleanly -- no obvious confound.  This exact combination (Z
	 * enabled + XYZ together) has not been tried; earlier XYZ-position
	 * tests in this project used Z disabled.  bit0=enable(1),
	 * bits[3:1]=func(GX_LEQUAL=3)<<1=0x6, bit4=update(1)<<4=0x10 -> 0x17.
	 */
	gx_load_bp_reg(0x40000017);	/* GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE) */
	gx_load_bp_reg(0x41000018);	/* colour/alpha update enabled */
	/*
	 * DIAGNOSTIC (PE control-block gap test): BP 0x42 (peCMode1 / dst-alpha)
	 * sits between 0x41 (BLENDMODE, a confirmed pixel-discard trap -- see
	 * Known pitfalls) and 0x43 (PE_CONTROL, which we do set), but this
	 * driver has never written it anywhere.  libogc's GX_Init() explicitly
	 * sets it via GX_SetDstAlpha(GX_DISABLE, 0).  Every raster-state
	 * permutation tried so far shares this same gap; this fills it with
	 * the libogc default (dst-alpha disabled, value 0) to test whether an
	 * uninitialised "mini" leftover here is gating pixel writes.
	 */
	gx_load_bp_reg(0x42000000);	/* GX_SetDstAlpha(GX_DISABLE, 0) */
	gx_load_bp_reg(0x43000040);	/* RGB8/Z24 EFB, linear Z, zcomp before tex */
	gx_load_bp_reg(0x44000003);	/* GX_SetFieldMask(GX_TRUE, GX_TRUE) */
	gx_load_bp_reg(0x68000000);	/* GX_SetFieldMode(GX_FALSE, GX_FALSE) */
	gx_load_bp_reg(0xF33F0000);	/* alpha test always passes */

	/* genMode: 0 texgens, 0 colour channels, 1 TEV stage */
	gx_load_bp_reg(0x00000000);

	/* Full-screen primitive coverage. */
	xo = 0x156;
	yo = 0x156;
	gx_load_bp_reg(0x20000000 | ((xo & 0x7ff) << 12) | (yo & 0x7ff));
	gx_load_bp_reg(0x21000000 |
		       (((xo + width  - 1) & 0x7ff) << 12) |
		       ((yo + height - 1) & 0xfff));
	gx_load_bp_reg(0x59000000);	/* GX_SetScissorBoxOffset(0, 0) */

	/*
	 * DIAGNOSTIC (control/sanity check): d=ONE gave near-black, d=HALF
	 * scale=1x gave mid-tone, d=HALF scale=2x gave complete invisibility
	 * (still-green, no write) -- three non-obviously-related outcomes.
	 * Before trusting any of them further, re-deploy the exact d=HALF
	 * scale=1x configuration unchanged to confirm the mid-tone result is
	 * still reproducible (rules out build/card-state drift as a
	 * confound). See docs/gx-accel-handoff-2026-07-02.md.
	 */
	gx_load_bp_reg(0xC008FFFD);	/* a=b=c=ZERO, d=GX_CC_HALF, scale=1x */
	gx_load_bp_reg(0xC108FFF0);	/* alpha = ZERO */
	gx_load_bp_reg(0x25000380);	/* raschan = GX_COLOR_NULL, tex disabled */

	/*
	 * XF: zero colour channels and zero texcoord generators.
	 */
	gx_load_xf_reg(0x1008, 0x00000000);
	gx_load_xf_reg(0x1009, 0x00000000);
	gx_load_xf_reg(0x100e, 0x00000401);
	gx_load_xf_reg(0x1010, 0x00000401);
	/*
	 * GX_CLIP_DISABLE was tested here (oversized clip-space geometry vs.
	 * the XF clipper) and ruled out: frame 2 still read back the known
	 * "still green" signature. Reverted to the normal GX_CLIP_ENABLE
	 * baseline; see docs/gx-accel-handoff-2026-07-02.md.
	 */
	gx_load_xf_reg(0x1005, 0);	/* GX_SetClipMode(GX_CLIP_ENABLE) */
	gx_load_xf_reg(0x103f, 0);

	gx_load_identity_pos_mtx0();

	gx_load_xf_regs_n(0x101a, 6);
	wg_f32_bits(f32_from_u16(width >> 1));
	wg_f32_bits(F32_NEG(f32_from_u16(height >> 1)));
	wg_f32_bits(F32_16M);
	wg_f32_bits(f32_from_u16((width >> 1) + 342));
	wg_f32_bits(f32_from_u16((height >> 1) + 342));
	wg_f32_bits(F32_16M);

	gx_load_xf_regs_n(0x1020, 7);
	wg_f32_bits(F32_ONE);
	wg_f32_bits(F32_ZERO);
	wg_f32_bits(F32_ONE);
	wg_f32_bits(F32_ZERO);
	wg_f32_bits(F32_ONE);
	wg_f32_bits(F32_ZERO);
	gx_wr32be(1);

	/*
	 * VCD/VAT: direct XYZ position (matching the Z-enable test above).
	 * VAT0 bit0=GX_POS_XYZ(1), bits[3:1]=GX_F32(4)<<1=0x08 -> 0x09.
	 */
	gx_load_cp_reg(0x50, 0x0200);
	gx_load_cp_reg(0x60, 0x0000);
	gx_load_cp_reg(0x70, 0x40000009);
	gx_load_cp_reg(0x80, 0x80000000);
	gx_load_cp_reg(0x90, 0x00000000);
}

static void gx_setup_vertex_color_state(u16 width, u16 height)
{
	u32 xo = 0x156;
	u32 yo = 0x156;

	/* Match the validated libogc capture's exact draw-time PE state. */
	gx_load_bp_reg(0x4000000E);
	gx_load_bp_reg(0x4100311C);
	gx_load_bp_reg(0x42000000);
	gx_load_bp_reg(0x43000040);
	gx_load_bp_reg(0x44000003);
	gx_load_bp_reg(0x68000000);
	gx_load_bp_reg(0xF33F0000);

	/* One colour channel, no texgens, one TEV stage, culling disabled. */
	gx_load_bp_reg(0x00000010);
	gx_load_bp_reg(0x20000000 | ((xo & 0x7ff) << 12) | (yo & 0x7ff));
	gx_load_bp_reg(0x21000000 |
		       (((xo + width - 1) & 0x7ff) << 12) |
		       ((yo + height - 1) & 0xfff));
	/* GX_SetScissorBoxOffset(0, 0): hardware stores (axis + 342) >> 1. */
	gx_load_bp_reg(0x5902ACAB);

	/* GX_SetCopyFilter(aa=false): center all twelve raster samples. */
	gx_load_bp_reg(0x01666666);
	gx_load_bp_reg(0x02666666);
	gx_load_bp_reg(0x03666666);
	gx_load_bp_reg(0x04666666);

	/* TEV stage 0 = rasterized vertex colour/alpha (GX_PASSCLR). */
	gx_load_bp_reg(0xC008FFFA);
	gx_load_bp_reg(0xC108FFD0);
	/* TEV swap table 0 = identity RGBA mapping. */
	gx_load_bp_reg(0xF6000004);
	gx_load_bp_reg(0xF700000E);
	gx_load_bp_reg(0x28000000);
	gx_load_bp_reg(0x30000000 | (u32)(width - 1));
	gx_load_bp_reg(0x31000000 | (u32)(height - 1));

	/* Match libogc's unconditional XF initialization before vertex state. */
	gx_load_xf_reg(0x1000, 0x0000003F);

	/* One direct colour channel, no texcoord generators. */
	gx_load_xf_reg(0x1008, 0x00000001);
	gx_load_xf_reg(0x1009, 0x00000001);
	gx_load_xf_reg(0x100e, 0x00000401);
	gx_load_xf_reg(0x1010, 0x00000401);
	gx_load_xf_reg(0x1005, 0);
	gx_load_xf_reg(0x103f, 0);
	gx_load_identity_pos_mtx0();

	/* Pixel-space viewport and orthographic projection. */
	gx_load_xf_regs_n(0x101a, 6);
	wg_f32_bits(f32_from_u16(width >> 1));
	wg_f32_bits(F32_NEG(f32_from_u16(height >> 1)));
	wg_f32_bits(F32_16M);
	wg_f32_bits(f32_from_u16((width >> 1) + 342));
	wg_f32_bits(f32_from_u16((height >> 1) + 342));
	wg_f32_bits(F32_16M);

	gx_load_xf_regs_n(0x1020, 7);
	wg_f32_bits(f32_div_u16(2, width));
	wg_f32_bits(F32_NEG_ONE);
	wg_f32_bits(F32_NEG(f32_div_u16(2, height)));
	wg_f32_bits(F32_ONE);
	wg_f32_bits(F32_NEG_ONE);
	wg_f32_bits(F32_NEG_ONE);
	gx_wr32be(1);

	/* VTXFMT0: direct XY/F32 position followed by direct RGBA8 colour. */
	gx_load_cp_reg(0x50, 0x00002200);
	gx_load_cp_reg(0x60, 0x00000000);
	gx_load_cp_reg(0x70, 0x40016008);
	gx_load_cp_reg(0x80, 0x80000000);
	gx_load_cp_reg(0x90, 0x00000000);
}

/* Add one position-derived texcoord and make TEV stage 0 sample texmap 0. */
static void gx_setup_rgb565_texture_state(u16 width, u16 height)
{
	gx_setup_vertex_color_state(width, height);

	/* GX_Init enables the post-transform selected by texCoordGen2. */
	gx_load_xf_reg(0x1012, 0x00000001);

	/* Keep the proven colour channel and add one texture-coordinate generator. */
	gx_load_bp_reg(0x00000011);
	gx_load_bp_reg(0xC008FFF8);
	gx_load_bp_reg(0xC108FFC0);
	gx_load_bp_reg(0x28000040);

	/* GX_TG_MTX2x4 from position through GX_TEXMTX0. */
	gx_load_xf_reg(0x103f, 0x00000001);
	gx_load_xf_reg(0x1040, 0x00000004);
	/* GX_DTTIDENTITY - GX_DTTMTX0 = 125 - 64 = 61 (0x3d). */
	gx_load_xf_reg(0x1050, 0x0000003D);
	gx_load_pos_to_tex_mtx0(width, height);
	gx_load_identity_post_mtx();
}

/*
 * gx_setup_texture_rgb565 - bind a tiled RGB565 buffer to texmap 0.
 *
 * Writes 8 BP registers. Values derived from libogc GX_InitTexObj /
 * GX_LoadTexObjPreloaded for a non-mipmapped RGB565 texture at mapid=0.
 */
static void gx_setup_texture_rgb565(void *tile_buf, u16 width, u16 height)
{
	u32 phys = virt_to_phys(tile_buf);
	u32 img0;

	/*
	 * BP 0x80 texMode0: match explicit libogc nearest/nearest sampling
	 * with CLAMP wrapping, no mipmaps, and edge LOD disabled.
	 */
	gx_load_bp_reg(0x80000100);

	/* BP 0x84 texMode1: LOD disabled */
	gx_load_bp_reg(0x84000000);

	/* BP 0x88 texImage0: [9:0]=width-1, [19:10]=height-1, [23:20]=GX_TF_RGB565=4 */
	img0 = ((u32)(width  - 1) & 0x3ff) |
	       (((u32)(height - 1) & 0x3ff) << 10) |
	       (4U << 20);
	gx_load_bp_reg(0x88000000 | img0);

	/*
	 * BP 0x8C/0x90 texImage1/2: RVL libogc texRegion[mapid+8], selected
	 * for RGB565. GX_InitTexCacheRegion(..., even=0x00000, odd=0x80000,
	 * size_even=size_odd=GX_TEXCACHE_32K) encodes both TMEM bases and
	 * cache-size fields.
	 */
	gx_load_bp_reg(0x8C0D8000);
	gx_load_bp_reg(0x900DC000);

	/* BP 0x94 texImage3: physical address >> 5 */
	gx_load_bp_reg(0x94000000 | ((phys >> 5) & 0x00ffffff));

	/* Exact libogc GX_InvalidateTexAll() sequence. */
	gx_load_bp_reg(0x0F000000);
	gx_load_bp_reg(0x66001000);
	gx_load_bp_reg(0x66001100);
	gx_load_bp_reg(0x0F000000);

	/* BP 0x30/0x31 suSsize/suTsize for texcoord 0:
	 * [15:0] = texture dimension - 1 (normalises vertex UV to [0,1])
	 * [16] = wrap = 0 (GX_CLAMP)
	 */
	gx_load_bp_reg(0x30000000 | (u32)(width  - 1));
	gx_load_bp_reg(0x31000000 | (u32)(height - 1));
}

/*
 * gx_draw_fullscreen_quad - render a textured quad covering the whole EFB.
 *
 * GX_QUADS=0x80 | VTXFMT0=0, 4 vertices, each with XY pos + ST texcoord.
 */
static void gx_draw_fullscreen_quad(u16 width, u16 height)
{
	u32 fw = f32_from_u16(width);
	u32 fh = f32_from_u16(height);

	gx_wr8(0x80);			/* GX_QUADS | vtxfmt 0 */
	gx_wr16be(4);

	/*
	 * Texcoords are normalized [0,1] — NOT pixel coords.
	 * GX rasterizer interpolation stalls permanently when texcoords exceed
	 * ~1.0 (e.g. raw pixel values 0..576); suSsize/suTsize scale [0,1] to
	 * texel addresses at TMU sample time.
	 */
	/*
	 * DIAGNOSTIC: all texcoords = (0,0). Zero gradient → LOD = -∞ (safe).
	 * Tests whether non-zero texcoord derivatives are what stalls the
	 * rasterizer's LOD unit at frame 2 with XF=1.
	 */
	/* top-left:     pos=(0, 0),   tex=(0, 0) */
	wg_f32_bits(F32_ZERO); wg_f32_bits(F32_ZERO);
	wg_f32_bits(F32_ZERO); wg_f32_bits(F32_ZERO);

	/* top-right:    pos=(w, 0),   tex=(0, 0) */
	wg_f32_bits(fw);       wg_f32_bits(F32_ZERO);
	wg_f32_bits(F32_ZERO); wg_f32_bits(F32_ZERO);

	/* bottom-right: pos=(w, h),   tex=(0, 0) */
	wg_f32_bits(fw);       wg_f32_bits(fh);
	wg_f32_bits(F32_ZERO); wg_f32_bits(F32_ZERO);

	/* bottom-left:  pos=(0, h),   tex=(0, 0) */
	wg_f32_bits(F32_ZERO); wg_f32_bits(fh);
	wg_f32_bits(F32_ZERO); wg_f32_bits(F32_ZERO);
}

static void gx_draw_pos_quad(u16 width, u16 height)
{
	/*
	 * DIAGNOSTIC: oversized clip-space triangles, both windings.  This avoids
	 * pixel-space projection/ortho uncertainty and stale cull winding state.
	 * Z=0 (near plane) per vertex, matching VAT0's GX_POS_XYZ format and
	 * the Z-enable test in gx_setup_constant_white_state -- see there for
	 * why Z=0 should cleanly pass GX_LEQUAL against the far/max Z-clear.
	 */
	gx_wr8(0x90);			/* GX_TRIANGLES | vtxfmt 0 */
	gx_wr16be(6);

	wg_f32_bits(0xC0800000); wg_f32_bits(0xC0800000); wg_f32_bits(F32_ZERO); /* (-4, -4, 0) */
	wg_f32_bits(0x40800000); wg_f32_bits(0xC0800000); wg_f32_bits(F32_ZERO); /* ( 4, -4, 0) */
	wg_f32_bits(F32_ZERO);   wg_f32_bits(0x40800000); wg_f32_bits(F32_ZERO); /* ( 0,  4, 0) */

	wg_f32_bits(0xC0800000); wg_f32_bits(0xC0800000); wg_f32_bits(F32_ZERO); /* (-4, -4, 0) */
	wg_f32_bits(F32_ZERO);   wg_f32_bits(0x40800000); wg_f32_bits(F32_ZERO); /* ( 0,  4, 0) */
	wg_f32_bits(0x40800000); wg_f32_bits(0xC0800000); wg_f32_bits(F32_ZERO); /* ( 4, -4, 0) */
}

static void gx_draw_color_quad(u16 width, u16 height, u8 r, u8 g, u8 b)
{
	u32 fw = f32_from_u16(width);
	u32 fh = f32_from_u16(height);

	gx_wr8(0x80);			/* GX_QUADS | vtxfmt 0 */
	gx_wr16be(4);

	wg_f32_bits(F32_ZERO); wg_f32_bits(F32_ZERO);
	gx_wr8(r); gx_wr8(g); gx_wr8(b); gx_wr8(0xff);

	wg_f32_bits(fw);       wg_f32_bits(F32_ZERO);
	gx_wr8(r); gx_wr8(g); gx_wr8(b); gx_wr8(0xff);

	wg_f32_bits(fw);       wg_f32_bits(fh);
	gx_wr8(r); gx_wr8(g); gx_wr8(b); gx_wr8(0xff);

	wg_f32_bits(F32_ZERO); wg_f32_bits(fh);
	gx_wr8(r); gx_wr8(g); gx_wr8(b); gx_wr8(0xff);
}

/* ------------------------------------------------------------------ */
/* EFB -> XFB display copy                                             */
/* ------------------------------------------------------------------ */

static void gx_set_copy_clear_rgb(u8 r, u8 g, u8 b)
{
	/* GX_SetCopyClear({r,g,b,255}, 0x00ffffff) */
	gx_load_bp_reg(0x4F000000 | (0xff << 8) | r);
	gx_load_bp_reg(0x50000000 | ((u32)g << 8) | b);
	gx_load_bp_reg(0x51000000 | 0x00ffffff);
}

static void gx_setup_display_copy_state(void)
{
	/* Remove display-copy state inherited from Mini before the first copy. */
	gx_load_bp_reg(0x42000000); /* destination alpha disabled */
	gx_load_bp_reg(0x43000040); /* RGB8/Z24 EFB, linear Z */
	gx_load_bp_reg(0x44000003); /* update both fields */
	gx_load_bp_reg(0x68000000); /* field mode disabled */

	/* GX_SetCopyFilter(aa=false), matching the proven libogc frame. */
	gx_load_bp_reg(0x01666666);
	gx_load_bp_reg(0x02666666);
	gx_load_bp_reg(0x03666666);
	gx_load_bp_reg(0x04666666);
	gx_load_bp_reg(0x5330A208);
	gx_load_bp_reg(0x5400820A);

	/* GX_SetDispCopyYScale(1.0). */
	gx_load_bp_reg(0x4E000100);
}

/*
 * gcn_gx_copy_efb_to_xfb - trigger hardware EFB->XFB blit.
 *
 * The GX fixed-function copy unit reads from the EFB, converts RGB to
 * YUYV, and writes into the XFB for the VI to scan out.  Replaces
 * vi_transcode_RGB565 / vi_transcode_RGB888 when gx_accel_ready is set.
 */
static void gx_copy_efb_to_xfb(u32 xfb_phys, u16 width, u16 height, bool clear)
{
	u32 ctrl;

	if (clear) {
		/*
		 * Match libogc GX_CopyDisp(clear=GX_TRUE): temporarily force
		 * Z/colour modes that allow the copy-clear operation, then set
		 * the clear bit in copy control.
		 */
		gx_load_bp_reg(0x4000000F);
		gx_load_bp_reg(0x41000018);
	}

	/* BP 0x49: copy source top-left = (0, 0) */
	gx_load_bp_reg((BP_DISP_COPY_TL << 24) | 0);

	/* BP 0x4a: source width-1, height-1 */
	gx_load_bp_reg((BP_DISP_COPY_WH << 24) |
		       (((u32)(height - 1) & 0x3ff) << 10) |
		       ((u32)(width  - 1) & 0x3ff));

	/* BP 0x4d: dest stride in units of 32 bytes (one cache line) */
	gx_load_bp_reg((BP_DISP_COPY_DST << 24) | ((width * 2) >> 5));

	/* BP 0x4b: dest physical address (right-shifted 5) */
	gx_load_bp_reg((BP_DISP_COPY_ADDR << 24) | ((xfb_phys >> 5) & 0xffffff));

	/* BP 0x52: copy control — gamma 1.0, optional clear, execute */
	ctrl = (BP_DISP_COPY_CTRL << 24) |
	       (GX_GM_1_0 << COPY_CTRL_GAMMA_SHIFT) |
	       (clear ? COPY_CTRL_CLEAR : 0) |
	       COPY_CTRL_EXECUTE;
	gx_load_bp_reg(ctrl);

	/*
	 * BP 0x45 = 2: PE draw-done trigger (libogc GX_DrawDone/GX_SetDrawDone).
	 * Queued behind the copy command. PE-finish polling has not yet passed a
	 * positive control, so this command must not currently be treated as a
	 * validated software fence.
	 */
	gx_load_bp_reg(0x45000002);
}

void gcn_gx_copy_efb_to_xfb(u32 xfb_phys, u16 width, u16 height)
{
	gx_copy_efb_to_xfb(xfb_phys, width, height, false);
}
EXPORT_SYMBOL_GPL(gcn_gx_copy_efb_to_xfb);

/* ------------------------------------------------------------------ */
/* Public blit API — called from vi_irq_handler in gcnfb.c            */
/* ------------------------------------------------------------------ */

/*
 * gx_submit_cmds - submit commands in gx_fifo_buf to the CP.
 *
 * Pads to 32-byte alignment, flushes dcache so GP DMA sees the writes,
 * then configures CP BASE/END/RD/WT and enables GP reads.
 *
 * Called from the RGB565 worker.  The VI DI1 hard IRQ only queues that work,
 * keeping the 640x480 tiling pass and hardware polling out of IRQ context.
 *
 * Critical: flush gx_fifo_buf BEFORE setting WT or enabling the GP.
 * The setup functions write commands into CPU cache; without the flush
 * the GP's DMA bus reads stale zeros from physical RAM.
 */
static void gx_submit_cmds(const char *phase)
{
	static int frame_log;	/* log frames 0-3 in detail */
	static bool logged_first_slow;
	static bool logged_first_stall;
	bool do_log = frame_log < 4 || gx_log_next_submit;
	bool token_seen;
	u32 log_frame = gx_log_next_submit ? gx_current_frame : frame_log;
	u32 phys_start = (u32)virt_to_phys(gx_fifo_buf);
	u32 phys_end   = phys_start + GX_FIFO_SIZE - 4;
	u32 phys_wt;
	u32 cp_rd, cp_wt;
	u16 pe_status, pe_token;
	int pe_timeout;
	int timeout;

	/* End every submission with a unique, directly readable PE marker. */
	gx_expected_token++;
	if (!gx_expected_token)
		gx_expected_token++;
	gx_load_bp_reg(0x48000000 | gx_expected_token);
	gx_load_bp_reg(0x47000000 | gx_expected_token);

	/* Pad to 32-byte boundary (GP DMA requires 32-byte alignment) */
	while (fifo_pos & 0x1f)
		gx_wr8(0);
	phys_wt = phys_start + fifo_pos;

	cp_write(CP_REG_CTRL, 0);

	flush_dcache_range((unsigned long)gx_fifo_buf,
			   (unsigned long)gx_fifo_buf + fifo_pos);

	/* Program CP FIFO extent and read/write pointers */
	cp_write(CP_REG_FIFO_BASE_HI, phys_start >> 16);
	cp_write(CP_REG_FIFO_BASE_LO, phys_start & 0xffff);
	cp_write(CP_REG_FIFO_END_HI,  phys_end   >> 16);
	cp_write(CP_REG_FIFO_END_LO,  phys_end   & 0xffff);
	cp_write(CP_REG_RD_HI, phys_start >> 16);
	cp_write(CP_REG_RD_LO, phys_start & 0xffff);
	cp_write(CP_REG_WT_HI, phys_wt >> 16);
	cp_write(CP_REG_WT_LO, phys_wt & 0xffff);

	pi_write(PI_REG_FIFO_BASE, phys_start & ~0x1fu);
	pi_write(PI_REG_FIFO_END,  phys_end   & ~0x1fu);
	pi_write(PI_REG_FIFO_WPTR, phys_wt);

	if (do_log)
		pr_info("gcn-gx: f%u %s pre: SR=%04x RD=%04x WT=%04x pos=%u\n",
			log_frame, phase, cp_read(CP_REG_STATUS), 0,
			phys_wt - phys_start, fifo_pos);

	/* Enable PE events and acknowledge stale token/finish status. */
	pe_write(PE_REG_INTR_STATUS,
		 PE_TOKEN_ENABLE | PE_FINISH_ENABLE |
		 PE_TOKEN_BIT | PE_FINISH_BIT);
	cp_write(CP_REG_CTRL, CP_CR_GPRESET | CP_CR_LINKEN);

	/*
	 * Positive control for PE event delivery. The command stream contains
	 * libogc's exact BP 0x48/BP 0x47 draw-sync sequence with a new token each
	 * frame. Poll both PE token status and the token-value register so either
	 * independently observable effect can validate downstream BP execution.
	 */
	pe_timeout = 2000;
	do {
		pe_status = pe_read(PE_REG_INTR_STATUS);
		pe_token = pe_read(PE_REG_TOKEN);
		if ((pe_status & PE_TOKEN_BIT) || pe_token == gx_expected_token)
			break;
		udelay(10);
	} while (--pe_timeout);
	token_seen = (pe_status & PE_TOKEN_BIT) || pe_token == gx_expected_token;

	if (pe_status & (PE_TOKEN_BIT | PE_FINISH_BIT)) {
		/* Preserve enable bits and acknowledge only asserted status. */
		pe_write(PE_REG_INTR_STATUS,
			 (pe_status & 0x0003) |
			 (pe_status & (PE_TOKEN_BIT | PE_FINISH_BIT)));
	}
	if (!token_seen) {
		pr_warn_once("gcn-gx: PE token positive control timed out (PE=%04x token=%04x expected=%04x)\n",
			     pe_status, pe_token, gx_expected_token);
	}
	if (do_log)
		pr_info("gcn-gx: f%u %s PE token_status=%u finish=%u token=%04x expected=%04x wait_us=%u status=%04x\n",
			log_frame, phase, !!(pe_status & PE_TOKEN_BIT),
			!!(pe_status & PE_FINISH_BIT), pe_token,
			gx_expected_token, (2000 - pe_timeout) * 10, pe_status);

	/* Read back RD after delay: confirms GP consumed commands */
	cp_rd = ((u32)cp_read(CP_REG_RD_HI) << 16) |
		cp_read(CP_REG_RD_LO);
	cp_wt = ((u32)cp_read(CP_REG_WT_HI) << 16) |
		cp_read(CP_REG_WT_LO);
	if (cp_rd != cp_wt) {
		if (!logged_first_slow) {
			pr_warn("gcn-gx: first_slow f%u SR=%04x RDoff=%04x WToff=%04x PIoff=%04x pos=%u\n",
				gx_current_frame, cp_read(CP_REG_STATUS),
				cp_rd - phys_start, cp_wt - phys_start,
				pi_read(PI_REG_FIFO_WPTR) - phys_start, fifo_pos);
			logged_first_slow = true;
		}

		/*
		 * Do not stop CP while it is mid-FIFO.  Frame 13 has been seen at
		 * RD=0x120/WT=0x180 after the fixed 2 ms delay; disabling CP there
		 * truncates the command stream and leaves later frames unrestartable.
		 */
		timeout = 800;
		while (timeout-- && cp_rd != cp_wt) {
			udelay(10);
			cp_rd = ((u32)cp_read(CP_REG_RD_HI) << 16) |
				cp_read(CP_REG_RD_LO);
			cp_wt = ((u32)cp_read(CP_REG_WT_HI) << 16) |
				cp_read(CP_REG_WT_LO);
		}
	}
	if (do_log) {
		pr_info("gcn-gx: f%u %s post: SR=%04x RDoff=%04x WToff=%04x\n",
			log_frame, phase, cp_read(CP_REG_STATUS),
			cp_rd - phys_start, cp_wt - phys_start);
		if (gx_log_next_submit)
			gx_log_next_submit = false;
		else
			frame_log++;
	}
	if (!logged_first_stall && cp_rd != cp_wt) {
		u32 off = cp_rd - phys_start;
		u8 *fifo = (u8 *)gx_fifo_buf;
		u32 dump = off >= 16 ? off - 16 : 0;

		pr_warn("gcn-gx: first_stall f%u SR=%04x RDoff=%04x WToff=%04x PIoff=%04x pos=%u\n",
			gx_current_frame, cp_read(CP_REG_STATUS),
			off, cp_wt - phys_start,
			pi_read(PI_REG_FIFO_WPTR) - phys_start, fifo_pos);
		pr_warn("gcn-gx: stall_bytes @%04x: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
			dump,
			fifo[dump + 0], fifo[dump + 1], fifo[dump + 2], fifo[dump + 3],
			fifo[dump + 4], fifo[dump + 5], fifo[dump + 6], fifo[dump + 7],
			fifo[dump + 8], fifo[dump + 9], fifo[dump + 10], fifo[dump + 11],
			fifo[dump + 12], fifo[dump + 13], fifo[dump + 14], fifo[dump + 15]);
		dump = off;
		pr_warn("gcn-gx: stall_bytes @%04x: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
			dump,
			fifo[dump + 0], fifo[dump + 1], fifo[dump + 2], fifo[dump + 3],
			fifo[dump + 4], fifo[dump + 5], fifo[dump + 6], fifo[dump + 7],
			fifo[dump + 8], fifo[dump + 9], fifo[dump + 10], fifo[dump + 11],
			fifo[dump + 12], fifo[dump + 13], fifo[dump + 14], fifo[dump + 15]);
		logged_first_stall = true;
	}

	cp_write(CP_REG_CTRL, 0);

	/*
	 * Poll for GP command-idle (SR bit 3 = 0x0008) before returning.
	 * The rasterizer/TEV/PE backend keeps running after the CP stops
	 * reading the FIFO.  Without this wait, reprogramming CP BASE/END/
	 * RD/WT for the next frame races the still-active downstream pipeline,
	 * leaving SR=0x0000 at the next pre-log and causing the CP to never
	 * start reading (RDoff=0x0000).  Observed after ~16 TEX0 frames.
	 */
	{
		int t = 2000;

		while (t-- && !(cp_read(CP_REG_STATUS) & 0x0008))
			udelay(10);
		if (!(cp_read(CP_REG_STATUS) & 0x0008))
			pr_warn_once("gcn-gx: pipeline did not go idle after submit (SR=0x%04x)\n",
				     cp_read(CP_REG_STATUS));
	}
}

/*
 * One complete red 640x480 frame captured from libogc and independently
 * replayed red in Dolphin FIFO Player. Only BP 0x4b's XFB address is patched
 * before submission. Capture SHA-256:
 * 8b42cc84e28b8ab09e53f981f4c1b197ec57fa0ee0029704f78d1a819f1aa302
 */
static const u8 gx_reference_red_frame[] = {
	0x61, 0x40, 0x00, 0x00, 0x0e, 0x61, 0x41, 0x00, 0x31, 0x1c, 0x61, 0x43,
	0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x61,
	0x4f, 0x00, 0xff, 0x00, 0x61, 0x50, 0x00, 0xff, 0x00, 0x61, 0x51, 0xff,
	0xff, 0xff, 0x10, 0x00, 0x05, 0x10, 0x1a, 0x43, 0xa0, 0x00, 0x00, 0xc3,
	0x70, 0x00, 0x00, 0x4b, 0x7f, 0xff, 0xff, 0x44, 0x25, 0x80, 0x00, 0x44,
	0x11, 0x80, 0x00, 0x4b, 0x7f, 0xff, 0xff, 0x61, 0x4e, 0x00, 0x01, 0x00,
	0x61, 0x20, 0x15, 0x61, 0x56, 0x61, 0x21, 0x3d, 0x53, 0x35, 0x61, 0x01,
	0x66, 0x66, 0x66, 0x61, 0x02, 0x66, 0x66, 0x66, 0x61, 0x03, 0x66, 0x66,
	0x66, 0x61, 0x04, 0x66, 0x66, 0x66, 0x61, 0x53, 0x30, 0xa2, 0x08, 0x61,
	0x54, 0x00, 0x82, 0x0a, 0x61, 0x22, 0x00, 0x06, 0x06, 0x61, 0x0f, 0x00,
	0x00, 0x00, 0x61, 0x68, 0x00, 0x00, 0x00, 0x61, 0x0f, 0x00, 0x00, 0x00,
	0x61, 0x28, 0x04, 0x90, 0x00, 0x61, 0xc0, 0x08, 0xff, 0xfa, 0x61, 0xc1,
	0x08, 0xff, 0xd0, 0x61, 0xc0, 0x08, 0xff, 0xfa, 0x61, 0xc1, 0x08, 0xff,
	0xd0, 0x61, 0xc1, 0x08, 0xff, 0xd0, 0x61, 0xf6, 0x01, 0x80, 0x64, 0x61,
	0xf7, 0x01, 0x80, 0x6e, 0x10, 0x00, 0x00, 0x10, 0x05, 0x00, 0x00, 0x00,
	0x00, 0x61, 0x40, 0x00, 0x00, 0x0e, 0x61, 0x41, 0x00, 0x31, 0x1c, 0x61,
	0x41, 0x00, 0x31, 0x1c, 0x61, 0x41, 0x00, 0x31, 0x1c, 0x61, 0x42, 0x00,
	0x00, 0x00, 0x61, 0xf3, 0x3f, 0x00, 0x00, 0x61, 0x43, 0x00, 0x00, 0x40,
	0x10, 0x00, 0x0b, 0x00, 0x00, 0x3f, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x3f, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3f, 0x80, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x06, 0x10, 0x20, 0x3b, 0x4c,
	0xcc, 0xcd, 0xbf, 0x80, 0x00, 0x00, 0xbb, 0x88, 0x88, 0x89, 0x3f, 0x80,
	0x00, 0x00, 0xbf, 0x80, 0x00, 0x00, 0xbf, 0x80, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x01, 0x61, 0x00, 0x00, 0x00, 0x10, 0x08, 0x50, 0x00, 0x00, 0x22,
	0x00, 0x08, 0x60, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x10, 0x08,
	0x00, 0x00, 0x00, 0x01, 0x08, 0x70, 0x40, 0x01, 0x60, 0x08, 0x08, 0x80,
	0x80, 0x00, 0x00, 0x00, 0x08, 0x90, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
	0x00, 0x10, 0x09, 0x00, 0x00, 0x00, 0x01, 0x10, 0x00, 0x00, 0x10, 0x0e,
	0x00, 0x00, 0x04, 0x01, 0x10, 0x00, 0x00, 0x10, 0x10, 0x00, 0x00, 0x04,
	0x01, 0x10, 0x00, 0x00, 0x10, 0x3f, 0x00, 0x00, 0x00, 0x00, 0x08, 0x30,
	0x3c, 0xf3, 0xcf, 0x00, 0x10, 0x00, 0x00, 0x10, 0x18, 0x3c, 0xf3, 0xcf,
	0x00, 0x08, 0x40, 0x00, 0xf3, 0xcf, 0x3c, 0x10, 0x00, 0x00, 0x10, 0x19,
	0x00, 0xf3, 0xcf, 0x3c, 0x80, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0xff, 0x00, 0x00, 0xff, 0x44, 0x20, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0xff, 0x00, 0x00, 0xff, 0x44, 0x20, 0x00, 0x00, 0x43,
	0xf0, 0x00, 0x00, 0xff, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00, 0x43,
	0xf0, 0x00, 0x00, 0xff, 0x00, 0x00, 0xff, 0x61, 0x45, 0x00, 0x00, 0x02,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x61, 0x40, 0x00, 0x00,
	0x0f, 0x61, 0x41, 0x00, 0x31, 0x1c, 0x61, 0x43, 0x00, 0x00, 0x00, 0x61,
	0x49, 0x00, 0x00, 0x00, 0x61, 0x4a, 0x07, 0x7e, 0x7f, 0x61, 0x4d, 0x00,
	0x00, 0x28, 0x61, 0x4b, 0x00, 0x31, 0xd4, 0x61, 0x52, 0x00, 0x48, 0x03,
};

#define GX_REFERENCE_FRAME_SIZE		564
#define GX_REFERENCE_XFB_ADDR_OFFSET	0x22c

static void gx_load_libogc_init_preamble(void)
{
	u8 i;

	/* Low-level command preamble from Wii libogc GX_Init(), in order. */
	gx_load_bp_reg(0x0F0000FF);
	gx_load_bp_reg(0x690004ED);
	gx_load_bp_reg(0x0F0000FF);
	gx_load_bp_reg(0x46000273);

	for (i = 0; i < 8; i++)
		gx_load_cp_reg(0x80 | i, 0x80000000);
	gx_load_xf_reg(0x1000, 0x0000003F);
	gx_load_xf_reg(0x1012, 0x00000001);
	gx_load_bp_reg(0x5800000F);

	gx_load_cp_reg(0x20, 0x00000000);
	gx_load_xf_reg(0x1006, 0x00000000);
	gx_load_bp_reg(0x23000000);
	gx_load_bp_reg(0x24000000);
	gx_load_bp_reg(0x67000000);
	gx_load_bp_reg(0x0F000000);

	/* Wii __GX_SetTmemConfig(2). */
	gx_load_bp_reg(0x8C0D8000);
	gx_load_bp_reg(0x900DC000);
	gx_load_bp_reg(0x8D0D8800);
	gx_load_bp_reg(0x910DC800);
	gx_load_bp_reg(0x8E0D9000);
	gx_load_bp_reg(0x920DD000);
	gx_load_bp_reg(0x8F0D9800);
	gx_load_bp_reg(0x930DD800);
	gx_load_bp_reg(0xAC0DA000);
	gx_load_bp_reg(0xB00DC400);
	gx_load_bp_reg(0xAD0DA800);
	gx_load_bp_reg(0xB10DCC00);
	gx_load_bp_reg(0xAE0DB000);
	gx_load_bp_reg(0xB20DD400);
	gx_load_bp_reg(0xAF0DB800);
	gx_load_bp_reg(0xB30DDC00);
}

static void gx_load_reference_red_frame(u32 xfb_phys, u16 width, u16 height)
{
	u32 copy_addr = (xfb_phys >> 5) & 0x00ffffff;
	u8 *fifo = gx_fifo_buf;
	u32 start = fifo_pos;

	if (WARN_ON_ONCE(width != 640 || height != 480))
		return;
	BUILD_BUG_ON(sizeof(gx_reference_red_frame) != GX_REFERENCE_FRAME_SIZE);

	memcpy(fifo + start, gx_reference_red_frame,
	       sizeof(gx_reference_red_frame));
	fifo[start + GX_REFERENCE_XFB_ADDR_OFFSET + 0] = copy_addr >> 16;
	fifo[start + GX_REFERENCE_XFB_ADDR_OFFSET + 1] = copy_addr >> 8;
	fifo[start + GX_REFERENCE_XFB_ADDR_OFFSET + 2] = copy_addr;
	fifo_pos += sizeof(gx_reference_red_frame);
}

static void gx_submit_live_rgb565(const void *vfb, u32 xfb_phys,
				  u16 width, u16 height, const char *phase)
{
	int i;

	(void)vfb;
	gx_fill_reference_rgb565((u16 *)gx_tex_buf, width, height);
	flush_dcache_range((unsigned long)gx_tex_buf,
			   (unsigned long)gx_tex_buf +
			   (unsigned long)width * height * 2);

	fifo_pos = 0;
	gx_setup_rgb565_texture_state(width, height);
	gx_setup_texture_rgb565(gx_tex_buf, width, height);
	gx_draw_color_quad(width, height, 0xff, 0x00, 0x00);
	gx_load_bp_reg(0x45000002);
	for (i = 0; i < 32; i++)
		gx_wr8(0);
	gx_set_copy_clear_rgb(0x00, 0xff, 0x00);
	gx_copy_efb_to_xfb(xfb_phys, width, height, true);
	gx_submit_cmds(phase);
}

/*
 * gx_process_rgb565 - asynchronous PE-finish primitive diagnostic.
 *
 * Validate the real PE-finish IRQ with known copies, then submit a red draw
 * and return to the worker. A later worker invocation copies EFB only after
 * the independent PE-finish handler has observed draw completion.
 */
static bool gx_process_rgb565(const void *vfb, u32 xfb_phys,
			      u16 width, u16 height)
{
	u32 finish_count;
	bool submitted = false;

	finish_count = ACCESS_ONCE(gx_pe_finish_count);

	switch (gx_diag_phase) {
	case GX_DIAG_SEED:
		gx_diag_finish_baseline = finish_count;
		fifo_pos = 0;
		gx_setup_display_copy_state();
		gx_set_copy_clear_rgb(0x00, 0xff, 0x00);
		gx_copy_efb_to_xfb(xfb_phys, width, height, true);
		gx_submit_cmds("seed");
		gx_diag_phase = GX_DIAG_WAIT_SEED;
		submitted = true;
		break;

	case GX_DIAG_WAIT_SEED:
		if (finish_count == gx_diag_finish_baseline)
			break;
		pr_info("gcn-gx: seed PE finish IRQ validated at count=%u\n",
			finish_count);
		gx_diag_finish_baseline = finish_count;
		fifo_pos = 0;
		gx_set_copy_clear_rgb(0x00, 0xff, 0x00);
		gx_copy_efb_to_xfb(xfb_phys, width, height, true);
		gx_submit_cmds("green");
		gx_diag_phase = GX_DIAG_WAIT_GREEN;
		submitted = true;
		break;

	case GX_DIAG_WAIT_GREEN:
		if (finish_count == gx_diag_finish_baseline)
			break;
		pr_info("gcn-gx: green seed complete; enabling live RGB565 texture\n");
		gx_diag_finish_baseline = finish_count;
		gx_submit_live_rgb565(vfb, xfb_phys, width, height, "live0");
		gx_diag_phase = GX_DIAG_WAIT_DRAW;
		submitted = true;
		break;

	case GX_DIAG_WAIT_DRAW:
		if (finish_count == gx_diag_finish_baseline)
			break;
		pr_info("gcn-gx: first live RGB565 frame PE finish observed\n");
		gx_diag_phase = GX_DIAG_DONE;
		break;

	case GX_DIAG_DONE:
		gx_submit_live_rgb565(vfb, xfb_phys, width, height, "live");
		submitted = true;
		break;
	}

	return submitted;
}

static void gx_rgb565_workfn(struct work_struct *work)
{
	const void *vfb;
	u32 xfb_phys;
	u16 width, height;
	unsigned long flags;
	u32 run;
	bool submitted;

	(void)work;
	spin_lock_irqsave(&gx_rgb565_work_lock, flags);
	vfb = gx_rgb565_work.vfb;
	xfb_phys = gx_rgb565_work.xfb_phys;
	width = gx_rgb565_work.width;
	height = gx_rgb565_work.height;
	spin_unlock_irqrestore(&gx_rgb565_work_lock, flags);

	if (!vfb || !width || !height || !gx_accel_ready) {
		spin_lock_irqsave(&gx_rgb565_work_lock, flags);
		gx_rgb565_work_busy = false;
		spin_unlock_irqrestore(&gx_rgb565_work_lock, flags);
		return;
	}

	run = ++gx_rgb565_work_runs;
	if (run == 1 || run == 60 || run == 300 || run == 450 ||
	    run == 600 || run == 750)
		pr_info("gcn-gx: RGB565 worker run=%u\n", run);
	submitted = gx_process_rgb565(vfb, xfb_phys, width, height);

	spin_lock_irqsave(&gx_rgb565_work_lock, flags);
	if (submitted)
		gx_rgb565_ready_xfb = xfb_phys;
	else
		gx_rgb565_work_busy = false;
	spin_unlock_irqrestore(&gx_rgb565_work_lock, flags);
}

bool gcn_gx_take_completed_rgb565(u32 *xfb_phys)
{
	unsigned long flags;
	bool ready = false;

	spin_lock_irqsave(&gx_rgb565_work_lock, flags);
	if (gx_rgb565_ready_xfb) {
		*xfb_phys = gx_rgb565_ready_xfb;
		gx_rgb565_ready_xfb = 0;
		gx_rgb565_work_busy = false;
		gx_rgb565_present_count++;
		ready = true;
	}
	spin_unlock_irqrestore(&gx_rgb565_work_lock, flags);

	if (ready && gx_rgb565_present_count <= 4)
		pr_info("gcn-gx: present %u xfb=0x%08x\n",
			gx_rgb565_present_count, *xfb_phys);
	return ready;
}
EXPORT_SYMBOL_GPL(gcn_gx_take_completed_rgb565);

void gcn_gx_blit_fb_rgb565(const void *vfb, u32 xfb_phys, u16 width, u16 height)
{
	unsigned long flags;

	/* Do not contend with built-in driver init on this single-core system. */
	if (system_state != SYSTEM_RUNNING) {
		if (!gx_rgb565_boot_deferred) {
			gx_rgb565_boot_deferred = true;
			pr_info("gcn-gx: deferring RGB565 worker until SYSTEM_RUNNING\n");
		}
		return;
	}
	if (gx_rgb565_boot_deferred) {
		gx_rgb565_boot_deferred = false;
		pr_info("gcn-gx: SYSTEM_RUNNING; enabling RGB565 worker\n");
	}

	spin_lock_irqsave(&gx_rgb565_work_lock, flags);
	if (gx_rgb565_work_busy) {
		spin_unlock_irqrestore(&gx_rgb565_work_lock, flags);
		return;
	}
	gx_rgb565_work.vfb = vfb;
	gx_rgb565_work.xfb_phys = xfb_phys;
	gx_rgb565_work.width = width;
	gx_rgb565_work.height = height;
	gx_rgb565_work_busy = true;
	spin_unlock_irqrestore(&gx_rgb565_work_lock, flags);

	if (!schedule_work(&gx_rgb565_work.work)) {
		spin_lock_irqsave(&gx_rgb565_work_lock, flags);
		gx_rgb565_work_busy = false;
		spin_unlock_irqrestore(&gx_rgb565_work_lock, flags);
		pr_warn_once("gcn-gx: failed to queue idle RGB565 work\n");
	}
}
EXPORT_SYMBOL_GPL(gcn_gx_blit_fb_rgb565);

/*
 * gcn_gx_blit_fb_rgb888 - blit a linear RGB888 (packed u32) FB to the XFB.
 * Converts to RGB565 during tiling to avoid GX_TF_RGBA8's complex layout.
 */
void gcn_gx_blit_fb_rgb888(const void *vfb, u32 xfb_phys, u16 width, u16 height)
{
	fifo_pos = 0;

	gx_tile_rgb888((const u32 *)vfb, (u16 *)gx_tex_buf, width, height);

	flush_dcache_range((unsigned long)gx_tex_buf,
			   (unsigned long)gx_tex_buf +
			   (unsigned long)width * height * 2);

	gx_setup_2d_state(width, height);
	gx_setup_texture_rgb565(gx_tex_buf, width, height);
	gx_draw_fullscreen_quad(width, height);
	gcn_gx_copy_efb_to_xfb(xfb_phys, width, height);
	gx_submit_cmds("rgb888");
}
EXPORT_SYMBOL_GPL(gcn_gx_blit_fb_rgb888);

/* ------------------------------------------------------------------ */
/* Module lifecycle                                                     */
/* ------------------------------------------------------------------ */

int gcn_gx_init(void)
{
	int ret;
	u32 fifo_phys;

	/*
	 * Mini leaves PI_FIFO_WPTR=0x00000000.  VI hardware generates wgPipe
	 * bursts during retrace; those bursts DMA to PI_FIFO_WPTR.  With
	 * WPTR=0 they overwrite the exception vectors at physical 0.
	 *
	 * Order: alloc → ioremap → set WPTR → THEN any printk.
	 * A single printk can trigger a VI retrace via console output.
	 */
	gx_fifo_buf_raw = NULL;
	gx_fifo_buf = (void *)__va(GX_FIFO_MEM1_PHYS);
	memset(gx_fifo_buf, 0, GX_FIFO_SIZE);
	fifo_phys = (u32)virt_to_phys(gx_fifo_buf);
	flush_dcache_range((unsigned long)gx_fifo_buf,
			   (unsigned long)gx_fifo_buf + GX_FIFO_SIZE);

	hw_base = ioremap(GX_HW_BASE, GX_HW_MAP_SIZE);
	if (!hw_base) {
		ret = -ENOMEM;
		goto err_fifo;
	}
	cp_regs = (u16 __iomem *)(hw_base + GX_CP_OFFSET);
	pe_regs = (u16 __iomem *)(hw_base + GX_PE_OFFSET);
	pi_regs = (u32 __iomem *)(hw_base + 0x3000);

	/* Clear stale PE events before the finish IRQ line is unmasked. */
	pe_write(PE_REG_INTR_STATUS, PE_TOKEN_BIT | PE_FINISH_BIT);

	/* Redirect wgPipe DMA bursts to our zeroed buffer (was addr 0 in mini) */
	iowrite32be(fifo_phys, pi_regs + PI_REG_FIFO_WPTR);

	/* Now safe to printk */
	pr_info("gcn-gx: init: A wptr=0x%08x hw_base=%p\n", fifo_phys, hw_base);

	pr_info("gcn-gx: init: D fifo_init\n");
	ret = gx_fifo_init();
	pr_info("gcn-gx: init: E fifo_init ret=%d\n", ret);
	if (ret)
		goto err_hw;

	gx_pe_finish_irq = irq_create_mapping(NULL, GX_PE_FINISH_HWIRQ);
	if (!gx_pe_finish_irq) {
		ret = -ENXIO;
		pr_err("gcn-gx: failed to map PE finish hwirq %u\n",
		       GX_PE_FINISH_HWIRQ);
		goto err_hw;
	}
	ret = request_irq(gx_pe_finish_irq, gx_pe_finish_handler, 0,
			  "gcn-gx-pe-finish", &gx_pe_finish_irq);
	if (ret) {
		pr_err("gcn-gx: failed to request PE finish IRQ %u: %d\n",
		       gx_pe_finish_irq, ret);
		goto err_irq_mapping;
	}
	pe_write(PE_REG_INTR_STATUS,
		 PE_TOKEN_ENABLE | PE_FINISH_ENABLE |
		 PE_TOKEN_BIT | PE_FINISH_BIT);
	pr_info("gcn-gx: PE finish hwirq %u mapped to IRQ %u\n",
		GX_PE_FINISH_HWIRQ, gx_pe_finish_irq);

	/*
	 * Texture tile buffer: must be in MEM1.  The GX texture unit is
	 * GameCube-era hardware; it cannot address MEM2 (0x10000000+).
	 * kmalloc returns MEM2 on Wii Linux because MEM1 and MEM2 are
	 * coalesced into one logical range.  Use the DTS-reserved region.
	 */
	gx_tex_raw = NULL;
	gx_tex_buf = (void *)__va(GX_TEX_BUF_MEM1_PHYS);
	memset(gx_tex_buf, 0, GX_TEX_BUF_SIZE);
	INIT_WORK(&gx_rgb565_work.work, gx_rgb565_workfn);
	gx_rgb565_work.vfb = NULL;
	gx_rgb565_work_runs = 0;
	gx_rgb565_ready_xfb = 0;
	gx_rgb565_present_count = 0;
	gx_rgb565_work_busy = false;
	gx_rgb565_boot_deferred = false;
	gx_diag_phase = GX_DIAG_SEED;
	gx_diag_finish_baseline = 0;
	pr_info("gcn-gx: init: tex_buf phys=0x%08x virt=%p\n",
		GX_TEX_BUF_MEM1_PHYS, gx_tex_buf);

	pr_info("gcn-gx: init: H done (accel ON)\n");
	gx_accel_ready = true;
	return 0;

err_irq_mapping:
	irq_dispose_mapping(gx_pe_finish_irq);
	gx_pe_finish_irq = 0;

err_fifo:
	gx_fifo_buf_raw = NULL;
	gx_fifo_buf = NULL;

err_hw:
	iounmap(hw_base);
	hw_base = NULL;
	return ret;
}
EXPORT_SYMBOL_GPL(gcn_gx_init);

void gcn_gx_exit(void)
{
	gx_accel_ready = false;
	cancel_work_sync(&gx_rgb565_work.work);
	gx_wait_idle();
	cp_write(CP_REG_CTRL, 0);
	if (gx_pe_finish_irq) {
		pe_write(PE_REG_INTR_STATUS, PE_TOKEN_BIT | PE_FINISH_BIT);
		free_irq(gx_pe_finish_irq, &gx_pe_finish_irq);
		irq_dispose_mapping(gx_pe_finish_irq);
		gx_pe_finish_irq = 0;
	}

	/* gx_tex_raw is NULL (tex_buf is a MEM1 reserve, not kmalloc'd) */
	kfree(gx_tex_raw);
	if (gx_fifo_buf_raw)
		kfree(gx_fifo_buf_raw);
	gx_fifo_buf_raw = NULL;
	gx_fifo_buf = NULL;
	if (hw_base) {
		iounmap(hw_base);
		hw_base = NULL;
	}
}
EXPORT_SYMBOL_GPL(gcn_gx_exit);
