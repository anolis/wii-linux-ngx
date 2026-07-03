/*
 * drivers/video/fbdev/gcn-gx.h
 *
 * Nintendo GameCube/Wii GX GPU minimal driver
 * Provides hardware EFB->XFB copy to replace software RGB->YUV conversion
 * in gcnfb.c (vi_transcode_RGB*).
 *
 * Register reference derived from libogc (devkitPro/libogc).
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef _GCN_GX_H
#define _GCN_GX_H

#include <linux/types.h>

/* Hollywood hardware base (physical).  One ioremap covers all GX sub-units.
 * On Wii/GC under Linux the registers are at 0x0C000000, not the 0xCC000000
 * BAT virtual address used in homebrew environments. See wii.dts ranges.
 */
#define GX_HW_BASE		0x0C000000
#define GX_HW_MAP_SIZE		0x9000		/* covers CP/PE/PI; wgPipe page included but unused */
#define GX_CP_OFFSET		0x0000		/* Command Processor */
#define GX_PE_OFFSET		0x1000		/* Pixel Engine */
#define GX_WGPIPE_OFFSET	0x8000		/* Write-Gather Pipe */

/* CP register indices (16-bit, word-indexed) */
#define CP_REG_STATUS		0	/* SR: status */
#define CP_REG_CTRL		1	/* CR: control */
#define CP_REG_CLR		2	/* clear (byte 0x04) */
#define CP_REG_FIFO_BASE_LO	16
#define CP_REG_FIFO_BASE_HI	17
#define CP_REG_FIFO_END_LO	18
#define CP_REG_FIFO_END_HI	19
#define CP_REG_FIFO_HIWM_LO	20
#define CP_REG_FIFO_HIWM_HI	21
#define CP_REG_FIFO_LOWM_LO	22
#define CP_REG_FIFO_LOWM_HI	23
#define CP_REG_RWDST_LO		24
#define CP_REG_RWDST_HI		25
#define CP_REG_WT_LO		26
#define CP_REG_WT_HI		27
#define CP_REG_RD_LO		28
#define CP_REG_RD_HI		29

/* CP control register bits */
#define CP_CR_GPRESET		BIT(0)	/* GP FIFO read enable */
#define CP_CR_RDINT_EN		BIT(2)	/* FIFO underflow interrupt */
#define CP_CR_WRINT_EN		BIT(3)	/* FIFO overflow interrupt */
#define CP_CR_LINKEN		BIT(4)	/* link CPU/GP FIFOs */

/* PE register indices (16-bit, word-indexed from PE base 0x0C001000) */
#define PE_REG_DONE		5	/* byte offset 0x0A: bit 1 = FINISH */
#define PE_FINISH_BIT		0x0002	/* set when BP 0x45=2 draw-done token is retired */

/* BP command opcode — written to wgPipe before a 32-bit BP register value */
#define GX_CMD_LOAD_BP_REG	0x61

/* BP register addresses (upper byte of the 32-bit BP write value) */
#define BP_DISP_COPY_TL		0x49	/* EFB copy source top-left */
#define BP_DISP_COPY_WH		0x4a	/* EFB copy source width/height */
#define BP_DISP_COPY_DST	0x4d	/* EFB copy dest stride */
#define BP_DISP_COPY_ADDR	0x4b	/* EFB copy dest address (>>5) */
#define BP_DISP_COPY_CTRL	0x52	/* EFB copy control/execute */

/* dispCopyCntrl bits */
#define COPY_CTRL_CLAMP_TOP	BIT(0)
#define COPY_CTRL_CLAMP_BOT	BIT(1)
#define COPY_CTRL_GAMMA_SHIFT	7	/* 2 bits */
#define COPY_CTRL_YSCALE	BIT(10)
#define COPY_CTRL_CLEAR		BIT(11)
#define COPY_CTRL_FRAME2FIELD	BIT(12)
#define COPY_CTRL_EXECUTE	BIT(14)	/* triggers the copy */

/* GX_GM_1_0 gamma (no correction) */
#define GX_GM_1_0		0

/* FIFO: 64KB, must be 32-byte aligned */
#define GX_FIFO_SIZE		(64 * 1024)
#define GX_FIFO_HIWATERMARK	(16 * 1024)

/* Texture tile buffer: max FB is 640×576 (PAL) RGB565 = 737,280 bytes.
 * Must live in MEM1: the GX texture unit is GameCube-era hardware that
 * cannot address MEM2 (0x10000000+).  kmalloc returns MEM2 on Wii Linux
 * (MEM1 and MEM2 are coalesced).  Use the DTS-reserved region instead. */
#define GX_TEX_BUF_SIZE		(640 * 576 * 2)
#define GX_TEX_BUF_MEM1_PHYS	0x01200000	/* reserved in wii.dts */

/* gx_accel_ready is set to true by gcn_gx_init() on success */
extern bool gx_accel_ready;

int  gcn_gx_init(void);
void gcn_gx_exit(void);
void gcn_gx_copy_efb_to_xfb(u32 xfb_phys, u16 width, u16 height);
void gcn_gx_blit_fb_rgb565(const void *vfb, u32 xfb_phys, u16 width, u16 height);
void gcn_gx_blit_fb_rgb888(const void *vfb, u32 xfb_phys, u16 width, u16 height);

#endif /* _GCN_GX_H */
