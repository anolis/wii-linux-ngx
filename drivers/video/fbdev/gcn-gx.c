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

#include "gcn-gx.h"

/* MMIO mapped pointers */
static u16 __iomem *cp_regs;		/* CP: 0xCC000000 */
static u16 __iomem *pe_regs;		/* PE: 0xCC001000 */
static u8  __iomem *wgpipe;		/* wgPipe: 0xCC008000 */

/* FIFO buffer — must be in memory the GPU can DMA, 32-byte aligned */
static void *gx_fifo_buf_raw;
static void *gx_fifo_buf;

/* Texture tile buffer: virtual FB converted to GX 4×4 tiled format */
static void *gx_tex_raw;
static void *gx_tex_buf;

/* Set to true after successful gcn_gx_init(); guards vsync path */
bool gx_accel_ready;
EXPORT_SYMBOL_GPL(gx_accel_ready);

/* ------------------------------------------------------------------ */
/* Low-level CP / wgPipe helpers                                        */
/* ------------------------------------------------------------------ */

static inline void cp_write(int reg, u16 val)
{
	iowrite16(val, cp_regs + reg);
}

static inline u16 cp_read(int reg)
{
	return ioread16(cp_regs + reg);
}

/*
 * gx_load_bp_reg - write one BP register through the wgPipe.
 * The full 32-bit @val has the BP address in bits [31:24] and data
 * in bits [23:0].  Opcode 0x61 precedes the value.
 */
static inline void gx_load_bp_reg(u32 val)
{
	iowrite8(GX_CMD_LOAD_BP_REG, wgpipe);
	iowrite32be(val, wgpipe);
}

/* gx_load_cp_reg - write one CP register (opcode 0x08) */
static inline void gx_load_cp_reg(u8 reg, u32 val)
{
	iowrite8(0x08, wgpipe);
	iowrite8(reg, wgpipe);
	iowrite32be(val, wgpipe);
}

/* gx_load_xf_reg - write one XF register (opcode 0x10, n=1) */
static inline void gx_load_xf_reg(u32 addr, u32 val)
{
	iowrite8(0x10, wgpipe);
	iowrite32be(addr & 0xffff, wgpipe);
	iowrite32be(val, wgpipe);
}

/*
 * gx_load_xf_regs_n - begin an XF block write.
 * Follow with @count u32 values written via iowrite32be(v, wgpipe).
 */
static inline void gx_load_xf_regs_n(u32 addr, u32 count)
{
	iowrite8(0x10, wgpipe);
	iowrite32be(((count - 1) << 16) | (addr & 0xffff), wgpipe);
}

/* wg_f32 - write a float32 to the wgPipe as raw IEEE 754 big-endian bits */
static inline void wg_f32(float v)
{
	u32 bits;

	memcpy(&bits, &v, sizeof(bits));
	iowrite32be(bits, wgpipe);
}

/* gx_wait_idle - wait for the GP to finish processing the FIFO */
static void gx_wait_idle(void)
{
	int timeout = 10000;

	/* CP status: bit 2 = read idle, bit 3 = command idle */
	while (timeout--) {
		u16 sr = cp_read(CP_REG_STATUS);

		if ((sr & 0x0c) == 0x0c)
			return;
		udelay(10);
	}
	pr_warn("gcn-gx: timed out waiting for GP idle\n");
}

/* ------------------------------------------------------------------ */
/* CP / FIFO initialisation                                             */
/* ------------------------------------------------------------------ */

static int gx_fifo_init(void)
{
	u32 phys_start, phys_end;

	gx_fifo_buf_raw = kmalloc(GX_FIFO_SIZE + 32, GFP_KERNEL);
	if (!gx_fifo_buf_raw)
		return -ENOMEM;

	gx_fifo_buf = PTR_ALIGN(gx_fifo_buf_raw, 32);
	phys_start = (u32)virt_to_phys(gx_fifo_buf);
	phys_end   = phys_start + GX_FIFO_SIZE - 4;

	cp_write(CP_REG_CTRL, 0);	/* stop CP before reconfiguring */

	cp_write(CP_REG_FIFO_BASE_LO, phys_start & 0xffff);
	cp_write(CP_REG_FIFO_BASE_HI, phys_start >> 16);
	cp_write(CP_REG_FIFO_END_LO, phys_end & 0xffff);
	cp_write(CP_REG_FIFO_END_HI, phys_end >> 16);

	cp_write(CP_REG_FIFO_HIWM_LO, (GX_FIFO_SIZE - GX_FIFO_HIWATERMARK) & 0xffff);
	cp_write(CP_REG_FIFO_HIWM_HI, (GX_FIFO_SIZE - GX_FIFO_HIWATERMARK) >> 16);
	cp_write(CP_REG_FIFO_LOWM_LO, (GX_FIFO_SIZE >> 1) & 0xffff);
	cp_write(CP_REG_FIFO_LOWM_HI, (GX_FIFO_SIZE >> 1) >> 16);

	/* read/write pointers start at base, distance = 0 */
	cp_write(CP_REG_RWDST_LO, 0);
	cp_write(CP_REG_RWDST_HI, 0);
	cp_write(CP_REG_WT_LO, phys_start & 0xffff);
	cp_write(CP_REG_WT_HI, phys_start >> 16);
	cp_write(CP_REG_RD_LO, phys_start & 0xffff);
	cp_write(CP_REG_RD_HI, phys_start >> 16);

	/* enable GP read + link CPU/GP FIFOs */
	cp_write(CP_REG_CTRL, CP_CR_GPRESET | CP_CR_LINKEN);

	/* signal PE done */
	iowrite16(0x0f, pe_regs + PE_REG_DONE);

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
	float fw = (float)width;
	float fh = (float)height;
	u32 xo, yo;

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
	 * a=[15:13], b=[12:10], c=[9:7], d=[6:4]  (CA_TEXA=5, CA_ZERO=7)
	 * a=b=c=ZERO(7), d=TEXA(5) → output = texture alpha
	 * clamp=[19]=1
	 */
	gx_load_bp_reg(0xC108FFD0);

	/* ---- BP 0x28: TEV order stage 0 (tevRasOrder[3]) ----
	 * [2:0] texmap=0, [5:3] texcoord=0, [6] texenable=1,
	 * [9:7] rascolor=GX_ALPHA_BUMP=7
	 */
	gx_load_bp_reg(0x280003C0);

	/* ---- XF 0x103f: numtexcoord generators = 1 ---- */
	gx_load_xf_reg(0x103f, 1);

	/* ---- XF 0x1040: texCoordGen[0] ----
	 * GX_TG_MTX2x4, src=GX_TG_TEX0 (vtxrow=5, stq=0)
	 * texcoords = vtxrow << 7 = 0x280
	 */
	gx_load_xf_reg(0x1040, 0x280);

	/* ---- XF 0x1050: texCoordGen2[0] ----
	 * normalize=0, postmtx=GX_DTTIDENTITY=63
	 */
	gx_load_xf_reg(0x1050, 0x3F);

	/* ---- XF 0x101a-0x101f: viewport ----
	 * GX_SetViewport(0, 0, w, h, 0, 1):
	 *   x0=w/2, y0=-h/2, z=16777215, x1=w/2+342, y1=h/2+342, f=16777215
	 */
	gx_load_xf_regs_n(0x101a, 6);
	wg_f32(fw * 0.5f);
	wg_f32(fh * -0.5f);
	wg_f32(16777215.0f);
	wg_f32(fw * 0.5f + 342.0f);
	wg_f32(fh * 0.5f + 342.0f);
	wg_f32(16777215.0f);

	/* ---- XF 0x1020-0x1026: orthographic projection ----
	 * Maps pixel coords [0,w]×[0,h] to NDC [-1,1]×[-1,1] (Y flipped):
	 *   mt[0][0]=2/w, mt[0][3]=-1, mt[1][1]=-2/h, mt[1][3]=1,
	 *   mt[2][2]=-1,  mt[2][3]=0,  type=GX_ORTHOGRAPHIC=1
	 */
	gx_load_xf_regs_n(0x1020, 7);
	wg_f32(2.0f / fw);
	wg_f32(-1.0f);
	wg_f32(-2.0f / fh);
	wg_f32(1.0f);
	wg_f32(-1.0f);
	wg_f32(0.0f);
	iowrite32be(1, wgpipe);		/* GX_ORTHOGRAPHIC */

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

	/* BP 0x80 texMode0: wrap=CLAMP(0), no-mipmap(0x80), min/mag=LINEAR(0x240)
	 * Data 0x250 derived from libogc defaults for non-mipmapped texture.
	 */
	gx_load_bp_reg(0x80000250);

	/* BP 0x84 texMode1: LOD disabled */
	gx_load_bp_reg(0x84000000);

	/* BP 0x88 texImage0: [9:0]=width-1, [19:10]=height-1, [23:20]=GX_TF_RGB565=4 */
	img0 = ((u32)(width  - 1) & 0x3ff) |
	       (((u32)(height - 1) & 0x3ff) << 10) |
	       (4U << 20);
	gx_load_bp_reg(0x88000000 | img0);

	/* BP 0x8C texImage1: TMEM even bank — default region at offset 0 */
	gx_load_bp_reg(0x8C000000);

	/* BP 0x90 texImage2: TMEM odd bank — default region at offset 0x80000
	 * data = 0x80000 >> 5 = 0x4000
	 */
	gx_load_bp_reg(0x90004000);

	/* BP 0x94 texImage3: physical address >> 5 */
	gx_load_bp_reg(0x94000000 | ((phys >> 5) & 0x00ffffff));

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
	float fw = (float)width;
	float fh = (float)height;

	iowrite8(0x80, wgpipe);		/* GX_QUADS | vtxfmt 0 */
	iowrite16be(4, wgpipe);

	/* top-left */
	wg_f32(0.0f); wg_f32(0.0f);
	wg_f32(0.0f); wg_f32(0.0f);

	/* top-right */
	wg_f32(fw);   wg_f32(0.0f);
	wg_f32(fw);   wg_f32(0.0f);

	/* bottom-right */
	wg_f32(fw);   wg_f32(fh);
	wg_f32(fw);   wg_f32(fh);

	/* bottom-left */
	wg_f32(0.0f); wg_f32(fh);
	wg_f32(0.0f); wg_f32(fh);
}

/* ------------------------------------------------------------------ */
/* EFB -> XFB display copy                                             */
/* ------------------------------------------------------------------ */

/*
 * gcn_gx_copy_efb_to_xfb - trigger hardware EFB->XFB blit.
 *
 * The GX fixed-function copy unit reads from the EFB, converts RGB to
 * YUYV, and writes into the XFB for the VI to scan out.  Replaces
 * vi_transcode_RGB565 / vi_transcode_RGB888 when gx_accel_ready is set.
 */
void gcn_gx_copy_efb_to_xfb(void *xfb, u16 width, u16 height)
{
	u32 phys = virt_to_phys(xfb);
	u32 ctrl;

	/* BP 0x49: copy source top-left = (0, 0) */
	gx_load_bp_reg((BP_DISP_COPY_TL << 24) | 0);

	/* BP 0x4a: source width-1, height-1 */
	gx_load_bp_reg((BP_DISP_COPY_WH << 24) |
		       (((u32)(height - 1) & 0x3ff) << 10) |
		       ((u32)(width  - 1) & 0x3ff));

	/* BP 0x4d: dest stride in units of 16 bytes (YUYV: 2 bytes/pixel) */
	gx_load_bp_reg((BP_DISP_COPY_DST << 24) | ((width * 2) >> 4));

	/* BP 0x4b: dest physical address (right-shifted 5) */
	gx_load_bp_reg((BP_DISP_COPY_ADDR << 24) | ((phys >> 5) & 0xffffff));

	/* BP 0x52: copy control — gamma 1.0, no clear, execute */
	ctrl = (BP_DISP_COPY_CTRL << 24) |
	       (GX_GM_1_0 << COPY_CTRL_GAMMA_SHIFT) |
	       COPY_CTRL_EXECUTE;
	gx_load_bp_reg(ctrl);

	/* Flush wgPipe writes */
	(void)cp_read(CP_REG_STATUS);
}
EXPORT_SYMBOL_GPL(gcn_gx_copy_efb_to_xfb);

/* ------------------------------------------------------------------ */
/* Public blit API — called from vi_irq_handler in gcnfb.c            */
/* ------------------------------------------------------------------ */

/*
 * gcn_gx_blit_fb_rgb565 - blit a linear RGB565 virtual FB to the XFB.
 * Full pipeline: tile → bind texture → draw to EFB → EFB-to-XFB copy.
 */
void gcn_gx_blit_fb_rgb565(const void *vfb, void *xfb, u16 width, u16 height)
{
	gx_tile_rgb565((const u16 *)vfb, (u16 *)gx_tex_buf, width, height);

	flush_dcache_range((unsigned long)gx_tex_buf,
			   (unsigned long)gx_tex_buf +
			   (unsigned long)width * height * 2);

	gx_setup_2d_state(width, height);
	gx_setup_texture_rgb565(gx_tex_buf, width, height);
	gx_draw_fullscreen_quad(width, height);
	gcn_gx_copy_efb_to_xfb(xfb, width, height);
}
EXPORT_SYMBOL_GPL(gcn_gx_blit_fb_rgb565);

/*
 * gcn_gx_blit_fb_rgb888 - blit a linear RGB888 (packed u32) FB to the XFB.
 * Converts to RGB565 during tiling to avoid GX_TF_RGBA8's complex layout.
 */
void gcn_gx_blit_fb_rgb888(const void *vfb, void *xfb, u16 width, u16 height)
{
	gx_tile_rgb888((const u32 *)vfb, (u16 *)gx_tex_buf, width, height);

	flush_dcache_range((unsigned long)gx_tex_buf,
			   (unsigned long)gx_tex_buf +
			   (unsigned long)width * height * 2);

	gx_setup_2d_state(width, height);
	gx_setup_texture_rgb565(gx_tex_buf, width, height);
	gx_draw_fullscreen_quad(width, height);
	gcn_gx_copy_efb_to_xfb(xfb, width, height);
}
EXPORT_SYMBOL_GPL(gcn_gx_blit_fb_rgb888);

/* ------------------------------------------------------------------ */
/* Module lifecycle                                                     */
/* ------------------------------------------------------------------ */

int gcn_gx_init(void)
{
	int ret;

	cp_regs = ioremap(GX_CP_BASE, 0x80);
	if (!cp_regs) {
		pr_err("gcn-gx: failed to map CP registers\n");
		return -ENOMEM;
	}

	pe_regs = ioremap(GX_PE_BASE, 0x10);
	if (!pe_regs) {
		pr_err("gcn-gx: failed to map PE registers\n");
		ret = -ENOMEM;
		goto err_pe;
	}

	wgpipe = ioremap(GX_WGPIPE_BASE, 0x20);
	if (!wgpipe) {
		pr_err("gcn-gx: failed to map wgPipe\n");
		ret = -ENOMEM;
		goto err_wg;
	}

	ret = gx_fifo_init();
	if (ret) {
		pr_err("gcn-gx: FIFO init failed\n");
		goto err_fifo;
	}

	/*
	 * Pre-allocate the texture tile buffer: worst-case 640×480 RGB565 = 600KB.
	 * Must be physically contiguous (GPU DMA) and 32-byte aligned.
	 */
	gx_tex_raw = kmalloc(GX_TEX_BUF_SIZE + 32, GFP_KERNEL);
	if (!gx_tex_raw) {
		ret = -ENOMEM;
		goto err_tex;
	}
	gx_tex_buf = PTR_ALIGN(gx_tex_raw, 32);

	gx_accel_ready = true;
	pr_info("gcn-gx: GX hardware FB blit ready\n");
	return 0;

err_tex:
	kfree(gx_fifo_buf_raw);
err_fifo:
	iounmap(wgpipe);
err_wg:
	iounmap(pe_regs);
err_pe:
	iounmap(cp_regs);
	return ret;
}
EXPORT_SYMBOL_GPL(gcn_gx_init);

void gcn_gx_exit(void)
{
	gx_accel_ready = false;
	gx_wait_idle();
	cp_write(CP_REG_CTRL, 0);

	kfree(gx_tex_raw);
	kfree(gx_fifo_buf_raw);
	iounmap(wgpipe);
	iounmap(pe_regs);
	iounmap(cp_regs);
}
EXPORT_SYMBOL_GPL(gcn_gx_exit);
