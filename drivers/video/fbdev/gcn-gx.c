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
#include <linux/string.h>
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
 *   index 2 (0x08): FIFO_BASE  — physical start of FIFO buffer
 *   index 3 (0x0C): FIFO_END   — physical end of FIFO buffer
 *   index 4 (0x10): FIFO_WPTR  — write pointer; wgPipe DMA bursts here
 *   index 5 (0x14): FIFO_CTRL  — bit 0 = enable
 * All read back as 0x00000000 after mini, meaning wgPipe WPTR = 0
 * (physical address 0x00000000 = kernel exception vectors = crash on first burst).
 */
#define PI_REG_FIFO_BASE	2
#define PI_REG_FIFO_END		3
#define PI_REG_FIFO_WPTR	4
#define PI_REG_FIFO_CTRL	5
#define PI_FIFO_CTRL_EN		BIT(0)

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
	 * All remaining setup (PI BASE/END/CTRL_EN, LINKEN, GPRESET) is
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

static void gx_setup_vertex_color_state(u16 width, u16 height)
{
	u32 xo, yo;

	gx_load_bp_reg(0x40000000);	/* Z disabled */
	gx_load_bp_reg(0x41000018);	/* colour/alpha update enabled */
	gx_load_bp_reg(0x43000040);	/* RGB8/Z24 EFB, linear Z, zcomp before tex */
	gx_load_bp_reg(0x44000003);	/* GX_SetFieldMask(GX_TRUE, GX_TRUE) */
	gx_load_bp_reg(0x68000000);	/* GX_SetFieldMode(GX_FALSE, GX_FALSE) */
	gx_load_bp_reg(0xF33F0000);	/* alpha test always passes */

	/* genMode: 0 texgens, 0 colour channels, 1 TEV stage */
	gx_load_bp_reg(0x00000000);

	xo = 0x156;
	yo = 0x156;
	gx_load_bp_reg(0x20000000 | ((xo & 0x7ff) << 12) | (yo & 0x7ff));
	gx_load_bp_reg(0x21000000 |
		       (((xo + width  - 1) & 0x7ff) << 12) |
		       ((yo + height - 1) & 0xfff));
	gx_load_bp_reg(0x59000000);	/* GX_SetScissorBoxOffset(0, 0) */

	/*
	 * DIAGNOSTIC: no vertex colour dependency.  With copy-clear isolated in
	 * its own submit, render a constant-white TEV primitive to test primitive
	 * coverage/PE writes without channel state.
	 */
	gx_load_bp_reg(0xC008FFFC);	/* a=b=c=ZERO, d=ONE */
	gx_load_bp_reg(0xC108FFF0);	/* alpha = ZERO */
	gx_load_bp_reg(0x25000380);	/* raschan = GX_COLOR_NULL, tex disabled */

	/*
	 * XF: one colour channel, one direct colour, zero texcoords.
	 *
	 * XF 0x100e/0x1010 (chan0 colour/alpha control): value derived from
	 * libogc GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG,
	 * GX_SRC_VTX, GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE) — the exact call
	 * GX_Init() itself makes for color0 by default.  Encoding:
	 *   bit0     matsrc   = 1 (GX_SRC_VTX)
	 *   bit1     enable   = 0 (lighting disabled)
	 *   bit6     ambsrc   = 0 (GX_SRC_REG, unused since lighting is off)
	 *   bit9     "(GX_AF_NONE-attn_fn)>0" = 0
	 *   bit10    "attn_fn>0"              = 1
	 * → 0x401.  The previous value 0x201 has bit9=1/bit10=0, which
	 * encodes attn_fn=GX_AF_SPEC (specular) instead of GX_AF_NONE — an
	 * invalid attenuation mode for a plain vertex-colour passthrough.
	 */
	gx_load_xf_reg(0x1008, 0x00000000);
	gx_load_xf_reg(0x1009, 0x00000000);
	gx_load_xf_reg(0x100e, 0x00000401);
	gx_load_xf_reg(0x1010, 0x00000401);
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
	wg_f32_bits(f32_div_u16(2, width));
	wg_f32_bits(F32_NEG_ONE);
	wg_f32_bits(F32_NEG(f32_div_u16(2, height)));
	wg_f32_bits(F32_ONE);
	wg_f32_bits(F32_NEG_ONE);
	wg_f32_bits(F32_ZERO);
	gx_wr32be(1);

	/* VCD/VAT: direct XY position only. */
	gx_load_cp_reg(0x50, 0x0200);
	gx_load_cp_reg(0x60, 0x0000);
	gx_load_cp_reg(0x70, 0x40000008);
	gx_load_cp_reg(0x80, 0x80000000);
	gx_load_cp_reg(0x90, 0x00000000);
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
	 * BP 0x80 texMode0: libogc GX_InitTexObj non-mipmap default with
	 * wrap_s/wrap_t=CLAMP, mag filter bit set, and non-mipmap min filter.
	 */
	gx_load_bp_reg(0x80000090);

	/* BP 0x84 texMode1: LOD disabled */
	gx_load_bp_reg(0x84000000);

	/* BP 0x88 texImage0: [9:0]=width-1, [19:10]=height-1, [23:20]=GX_TF_RGB565=4 */
	img0 = ((u32)(width  - 1) & 0x3ff) |
	       (((u32)(height - 1) & 0x3ff) << 10) |
	       (4U << 20);
	gx_load_bp_reg(0x88000000 | img0);

	/*
	 * BP 0x8C/0x90 texImage1/2: RVL libogc default texRegion[0].
	 * GX_InitTexCacheRegion(..., even=0x00000, odd=0x08000,
	 * size_even=size_odd=GX_TEXCACHE_32K) encodes both the TMEM base and
	 * cache-size fields.  The odd bank is 0x08000 on Wii, not 0x80000.
	 */
	gx_load_bp_reg(0x8C0D8000);
	gx_load_bp_reg(0x900D8400);

	/* BP 0x94 texImage3: physical address >> 5 */
	gx_load_bp_reg(0x94000000 | ((phys >> 5) & 0x00ffffff));

	/*
	 * BP 0x66: texture cache invalidate (BPMEM_TX_INVALIDATE).
	 * The TMU caches texture data fetched from main memory.  Without this,
	 * the GP serves stale data on every frame after the first — the cache
	 * holds the initial texture and ignores subsequent writes to the same
	 * physical address even after a CPU dcache flush.  Sent twice as
	 * libogc does (GX_InvalidateTexAll writes it twice for reliability).
	 */
	gx_load_bp_reg(0x66000000);
	gx_load_bp_reg(0x66000000);

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
	 * DIAGNOSTIC: avoid the pixel-space projection path.  Draw oversized
	 * clip-space triangles against an identity orthographic projection, using
	 * both windings to rule out stale cull/front-face state.
	 */
	gx_wr8(0x90);			/* GX_TRIANGLES | vtxfmt 0 */
	gx_wr16be(6);

	wg_f32_bits(0xC0800000); wg_f32_bits(0xC0800000); /* (-4, -4) */
	wg_f32_bits(0x40800000); wg_f32_bits(0xC0800000); /* ( 4, -4) */
	wg_f32_bits(F32_ZERO);   wg_f32_bits(0x40800000); /* ( 0,  4) */

	wg_f32_bits(0xC0800000); wg_f32_bits(0xC0800000); /* (-4, -4) */
	wg_f32_bits(F32_ZERO);   wg_f32_bits(0x40800000); /* ( 0,  4) */
	wg_f32_bits(0x40800000); wg_f32_bits(0xC0800000); /* ( 4, -4) */
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
	 * BP 0x65 = 2: PE draw-done trigger (BPMEM_PE_DONE / libogc GXSetDrawDone).
	 * Queued behind the copy command; when the PE processes it the PE FINISH
	 * signal fires.  On this hardware PE FINISH is interrupt-driven — the
	 * status bit in PE_CTRL_STAT clears before software can poll it.
	 * gx_submit_cmds therefore uses a fixed 2ms udelay rather than polling.
	 */
	gx_load_bp_reg(0x65000002);
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
 * Called from IRQ context (VI DI1).  No sleeping.  udelay in gx_wait_idle
 * is safe in IRQ context on PPC32.
 *
 * Critical: flush gx_fifo_buf BEFORE setting WT or enabling the GP.
 * The setup functions write commands into CPU cache; without the flush
 * the GP's DMA bus reads stale zeros from physical RAM.
 */
static void gx_submit_cmds(void)
{
	static int frame_log;	/* log frames 0-3 in detail */
	static bool logged_first_slow;
	static bool logged_first_stall;
	bool do_log = frame_log < 4 || gx_log_next_submit;
	u32 log_frame = gx_log_next_submit ? gx_current_frame : frame_log;
	u32 phys_start = (u32)virt_to_phys(gx_fifo_buf);
	u32 phys_end   = phys_start + GX_FIFO_SIZE - 4;
	u32 phys_wt;
	u32 cp_rd, cp_wt;
	int timeout;

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
	pi_write(PI_REG_FIFO_CTRL, PI_FIFO_CTRL_EN);

	if (do_log)
		pr_info("gcn-gx: f%u pre: SR=%04x RD=%04x WT=%04x pos=%u\n",
			log_frame, cp_read(CP_REG_STATUS), 0,
			phys_wt - phys_start, fifo_pos);

	out_be16(pe_regs + PE_REG_CTRL_STAT, 0x0003);
	cp_write(CP_REG_CTRL, CP_CR_GPRESET | CP_CR_LINKEN);

	/*
	 * DIAGNOSTIC: give the raster/PE backend much longer to drain.  The
	 * first hard stall lands inside the vertex payload after ~14 frames,
	 * which points at backend back-pressure rather than a malformed FIFO.
	 */
	udelay(10000);

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
		pr_info("gcn-gx: f%u post: SR=%04x RDoff=%04x WToff=%04x\n",
			log_frame, cp_read(CP_REG_STATUS),
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
 * gcn_gx_blit_fb_rgb565 - blit a linear RGB565 virtual FB to the XFB.
 * Full pipeline: tile → bind texture → draw to EFB → EFB-to-XFB copy.
 *
 * Frames 0-359 (~6s): normal rendering from vfb_mem (terminal content).
 * Frames 360-539 (~3s): textured solid red — visual confirm texture refresh.
 * Frames 540-719 (~3s): textured solid green
 * Frames 720-899 (~3s): textured solid blue
 * Frames 900+: back to normal.
 */
void gcn_gx_blit_fb_rgb565(const void *vfb, u32 xfb_phys, u16 width, u16 height)
{
	static u32 frame_count;
	u32 phase = frame_count++;

	gx_current_frame = phase;

	/*
	 * DIAGNOSTIC: seed EFB/XFB with the proven green copy-clear only once.
	 * Re-clearing every frame can mask a primitive that reaches EFB late or
	 * out of order relative to the following copy.  After frame 0, the only
	 * commands submitted are the constant-white primitive and final copy.
	 */
	if (phase == 0) {
		fifo_pos = 0;
		gx_set_copy_clear_rgb(0x00, 0xff, 0x00);
		gx_copy_efb_to_xfb(xfb_phys, width, height, true);
		gx_submit_cmds();
	}

	fifo_pos = 0;
	gx_setup_vertex_color_state(width, height);
	gx_draw_pos_quad(width, height);
	gx_submit_cmds();

	fifo_pos = 0;
	if (phase == 360)
		gx_log_next_submit = true;
	gx_copy_efb_to_xfb(xfb_phys, width, height, false);
	gx_submit_cmds();

	/* Diagnostic: log tex and XFB content for frames 0-3 and at color start */
	if (phase < 4 || phase == 360) {
		const u32 *xv = (const u32 *)__va(xfb_phys);
		u32 center_off = (u32)(height / 2) * (width / 2) + (width / 4);

		pr_info("gcn-gx: f%u diag=white xfb0=%08x xfb1=%08x xfbc=%08x\n",
			phase, xv[0], xv[1], xv[center_off]);
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
	gx_submit_cmds();
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

	/* Redirect wgPipe DMA bursts to our zeroed buffer (was addr 0 in mini) */
	iowrite32be(fifo_phys, pi_regs + PI_REG_FIFO_WPTR);

	/* Now safe to printk */
	pr_info("gcn-gx: init: A wptr=0x%08x hw_base=%p\n", fifo_phys, hw_base);

	pr_info("gcn-gx: init: D fifo_init\n");
	ret = gx_fifo_init();
	pr_info("gcn-gx: init: E fifo_init ret=%d\n", ret);
	if (ret)
		goto err_hw;

	/*
	 * Texture tile buffer: must be in MEM1.  The GX texture unit is
	 * GameCube-era hardware; it cannot address MEM2 (0x10000000+).
	 * kmalloc returns MEM2 on Wii Linux because MEM1 and MEM2 are
	 * coalesced into one logical range.  Use the DTS-reserved region.
	 */
	gx_tex_raw = NULL;
	gx_tex_buf = (void *)__va(GX_TEX_BUF_MEM1_PHYS);
	memset(gx_tex_buf, 0, GX_TEX_BUF_SIZE);
	pr_info("gcn-gx: init: tex_buf phys=0x%08x virt=%p\n",
		GX_TEX_BUF_MEM1_PHYS, gx_tex_buf);

	pr_info("gcn-gx: init: H done (accel ON)\n");
	gx_accel_ready = true;
	return 0;

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
	gx_wait_idle();
	cp_write(CP_REG_CTRL, 0);

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
