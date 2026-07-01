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
 * The GX command processor (CP) is fed via the Write-Gather Pipe (wgPipe),
 * a 32-byte MMIO window at 0xCC008000 that batches writes and forwards
 * them to the GPU FIFO. BP (Blitting Processor) register loads are sent
 * as: [ 0x61 (1 byte) | register_value (4 bytes) ].
 *
 * Register reference derived from libogc (devkitPro/libogc, MIT licence).
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/dma-mapping.h>
#include <linux/delay.h>
#include <asm/cacheflush.h>

#include "gcn-gx.h"

/* MMIO mapped pointers */
static u16 __iomem *cp_regs;		/* CP: 0xCC000000 */
static u16 __iomem *pe_regs;		/* PE: 0xCC001000 */
static u8  __iomem *wgpipe;		/* wgPipe: 0xCC008000 */

/* FIFO buffer — must be in memory the GPU can DMA, 32-byte aligned */
static void *gx_fifo_buf;
static dma_addr_t gx_fifo_phys;

/* ------------------------------------------------------------------ */
/* Low-level helpers                                                    */
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
 * Write a BP register through the wgPipe.
 * Format: opcode (0x61) as u8, then 32-bit value.
 * Writes must be strictly ordered — use __raw_writel after the byte.
 */
static inline void gx_load_bp_reg(u32 val)
{
	iowrite8(GX_CMD_LOAD_BP_REG, wgpipe);
	iowrite32be(val, wgpipe + 1);
}

/* Wait for the GP to finish reading from the FIFO */
static void gx_wait_idle(void)
{
	int timeout = 10000;

	/* poll CP status: bit 2 = read idle, bit 3 = command idle */
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

	gx_fifo_buf = kmalloc(GX_FIFO_SIZE + 32, GFP_KERNEL);
	if (!gx_fifo_buf)
		return -ENOMEM;

	/* 32-byte align */
	gx_fifo_buf = PTR_ALIGN(gx_fifo_buf, 32);
	gx_fifo_phys = virt_to_phys(gx_fifo_buf);

	phys_start = (u32)gx_fifo_phys;
	phys_end   = phys_start + GX_FIFO_SIZE - 4;

	/* Stop the CP before reconfiguring */
	cp_write(CP_REG_CTRL, 0);

	/* FIFO base */
	cp_write(CP_REG_FIFO_BASE_LO, phys_start & 0xffff);
	cp_write(CP_REG_FIFO_BASE_HI, phys_start >> 16);

	/* FIFO end */
	cp_write(CP_REG_FIFO_END_LO, phys_end & 0xffff);
	cp_write(CP_REG_FIFO_END_HI, phys_end >> 16);

	/* Watermarks */
	cp_write(CP_REG_FIFO_HIWM_LO, (GX_FIFO_SIZE - GX_FIFO_HIWATERMARK) & 0xffff);
	cp_write(CP_REG_FIFO_HIWM_HI, (GX_FIFO_SIZE - GX_FIFO_HIWATERMARK) >> 16);
	cp_write(CP_REG_FIFO_LOWM_LO, (GX_FIFO_SIZE >> 1) & 0xffff);
	cp_write(CP_REG_FIFO_LOWM_HI, (GX_FIFO_SIZE >> 1) >> 16);

	/* read == write pointer, distance = 0 */
	cp_write(CP_REG_RWDST_LO, 0);
	cp_write(CP_REG_RWDST_HI, 0);
	cp_write(CP_REG_WT_LO, phys_start & 0xffff);
	cp_write(CP_REG_WT_HI, phys_start >> 16);
	cp_write(CP_REG_RD_LO, phys_start & 0xffff);
	cp_write(CP_REG_RD_HI, phys_start >> 16);

	/* Enable GP read + link CPU/GP FIFOs */
	cp_write(CP_REG_CTRL, CP_CR_GPRESET | CP_CR_LINKEN);

	/* Signal PE done */
	iowrite16(0x0f, pe_regs + PE_REG_DONE);

	return 0;
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

/**
 * gcn_gx_copy_efb_to_xfb - trigger hardware EFB->XFB blit
 * @xfb:    physical destination (the VI's XFB buffer)
 * @width:  framebuffer width in pixels
 * @height: framebuffer height in lines
 *
 * Replaces vi_transcode_RGB565 / vi_transcode_RGB888. The GX's
 * fixed-function copy unit performs the RGB->YUV conversion in hardware
 * and writes YUYV directly into the XFB for the VI to scan out.
 *
 * Called from the VI vsync interrupt handler in gcnfb.c.
 */
void gcn_gx_copy_efb_to_xfb(void *xfb, u16 width, u16 height)
{
	u32 phys = virt_to_phys(xfb);
	u32 ctrl;

	/* BP 0x49: source top-left = (0, 0) */
	gx_load_bp_reg((BP_DISP_COPY_TL << 24) | 0);

	/* BP 0x4a: source width/height (stored as w-1, h-1) */
	gx_load_bp_reg((BP_DISP_COPY_WH << 24) |
		       (((u32)(height - 1) & 0x3ff) << 10) |
		       ((u32)(width  - 1) & 0x3ff));

	/* BP 0x4d: dest stride in units of 16 bytes */
	gx_load_bp_reg((BP_DISP_COPY_DST << 24) | ((width * 2) >> 4));

	/* BP 0x4b: dest physical address (right-shifted 5) */
	gx_load_bp_reg((BP_DISP_COPY_ADDR << 24) | ((phys >> 5) & 0xffffff));

	/* BP 0x52: copy control — gamma 1.0, no clear, execute */
	ctrl = (BP_DISP_COPY_CTRL << 24) |
	       (GX_GM_1_0 << COPY_CTRL_GAMMA_SHIFT) |
	       COPY_CTRL_EXECUTE;
	gx_load_bp_reg(ctrl);

	/* Flush wgPipe writes before returning */
	(void)cp_read(CP_REG_STATUS);
}
EXPORT_SYMBOL_GPL(gcn_gx_copy_efb_to_xfb);

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

	/* wgPipe: map as write-combining/uncached so writes go straight through */
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

	pr_info("gcn-gx: GX hardware EFB->XFB copy initialised\n");
	return 0;

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
	gx_wait_idle();
	cp_write(CP_REG_CTRL, 0);

	kfree(gx_fifo_buf);
	iounmap(wgpipe);
	iounmap(pe_regs);
	iounmap(cp_regs);
}
EXPORT_SYMBOL_GPL(gcn_gx_exit);
