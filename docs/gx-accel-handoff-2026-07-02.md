# Wii GX acceleration handoff - 2026-07-02

This report summarises the current state of the direct GX framebuffer acceleration work in `wii-linux-ngx`.  It is updated in place each session.

## Goal

Replace the per-vsync software RGB565 to VI/XFB conversion in `drivers/video/fbdev/gcnfb.c` with a GX hardware path:

1. Tile the linear virtual framebuffer into a GX texture buffer.
2. Bind it as a texture.
3. Draw a fullscreen quad into EFB.
4. Use GX EFB-to-XFB copy to write the physical XFB scanned by VI.

The long-term motivation is a usable lightweight Wii desktop environment, where the CPU cost of software framebuffer conversion would be a major bottleneck.

## Current HEAD

Branch: `feature/gcn-gx-accel`

Latest commits (most recent first):

```text
(pending) Fix EFB→XFB copy stride: >> 5 not >> 4 (32-byte cache line units)
(pending) Replace blit with EFB clear diagnostic; fix submit drain timing
69027724c2c6 Document Wii GX acceleration handoff
a06c3538569e Stop GX CP after submitted FIFO drains
86ca3d80c91a Wait for GX FIFO before stopping GP
...
```

## Build and deploy

Build command:

```sh
make ARCH=powerpc CROSS_COMPILE=powerpc-linux-gnu- zImage -j$(nproc)
```

Deploy target:

```text
/media/anolis/BOOTWII/gumboot/zImage.ngx
```

Deploy command:

```sh
cp arch/powerpc/boot/zImage /media/anolis/BOOTWII/gumboot/zImage.ngx
sha256sum /media/anolis/BOOTWII/gumboot/zImage.ngx
sync /media/anolis/BOOTWII/gumboot/zImage.ngx
```

The SD card is manually moved between host and Wii, so `/media/anolis/BOOTWII` may disappear between iterations.

## Key files

Main GX driver:

```text
drivers/video/fbdev/gcn-gx.c
drivers/video/fbdev/gcn-gx.h
```

Framebuffer / VI integration:

```text
drivers/video/fbdev/gcnfb.c
```

Wii memory reservation:

```text
arch/powerpc/boot/dts/wii.dts
```

## Critical discoveries (chronological)

### 1. The original "freeze after H" was usually a display freeze

When `gx_accel_ready=true`, the RGB565 path in `gcnfb.c` takes the GX blit path.  If that blit does not update `fb_mem`/XFB, VI keeps scanning the last software-transcoded frame and the screen appears frozen.

The CPU can still be alive and boot into a shell.  This was confirmed by forcing the software transcode path back on.

Current diagnostic behaviour in `gcnfb.c` keeps the software transcode running unconditionally in the RGB565 path and then optionally runs GX:

```c
case V4L2_PIX_FMT_RGB565:
    vi_transcode_RGB565(ctl);
    if (gx_accel_ready)
        gcn_gx_blit_fb_rgb565(vfb_mem, (u32)gx_fb_start,
                              info->var.xres, info->var.yres);
    break;
```

This is intentional while testing, because the screen stays live even if GX output is wrong.

### 2. The GP was not seeing command bytes until cache flush was added

`gx_submit_cmds()` must flush `gx_fifo_buf` before enabling GP reads.  The CPU builds command bytes in cache; the GP DMA fetches from physical memory.

Current code flushes:

```c
flush_dcache_range((unsigned long)gx_fifo_buf,
                   (unsigned long)gx_fifo_buf + fifo_pos);
```

### 3. PI FIFO control is required

Adding:

```c
pi_write(PI_REG_FIFO_CTRL, PI_FIFO_CTRL_EN);
```

was the change that first allowed the system to boot to a shell with GX submit code active.

### 4. CP register access must be big-endian

The original CP accessors used generic `ioread16/iowrite16`.  That produced misleading status and bad pointer reads.

Changing them to match the VI 16-bit register style fixed the CP pointer programming:

```c
static inline void cp_write(int reg, u16 val)
{
    out_be16(cp_regs + reg, val);
}

static inline u16 cp_read(int reg)
{
    return in_be16(cp_regs + reg);
}
```

This was the decisive breakthrough that made the GP consume submitted commands.

Boot evidence from after this fix:

```text
gcn-gx: submit: base=0x01684000 wt=0x01684120 pos=288 SR=0x0008
gcn-gx: preGP: RDoff=0x0000 WToff=0x0120 PIoff=0x0120 pos=288
gcn-gx: postGP: SR=0x000c RDoff=0x0120 WToff=0x0120 PIoff=0x0120
```

CP RD started at FIFO base and advanced to `base + 0x120` (all 288 bytes consumed) after enabling GP.  This is the first confirmed GP execution.

### 5. The command FIFO must live in MEM1 (bit-23 hardware limitation)

`GFP_DMA` did not move the small FIFO out of MEM2.  On Wii this kernel coalesces MEM1 and MEM2 into one logical memory range, so plain page/slab allocation still returned `0x12800000`.

The CP FIFO address registers do not implement bit 23 (`0x00800000`) of the physical address.  Any address with bit 23 set is stored with that bit dropped, so the GP reads from wrong memory:

```text
# Before MEM1 fix — address mangled by hardware:
submit: base=0x12800000  →  CP stores it as 0x12000000
```

Fix: use a `/memreserve/` region in DTS at a MEM1 address where bit 23 = 0:

```dts
/memreserve/ 0x01684000 0x0010000; /* GX command FIFO */
```

```c
#define GX_FIFO_MEM1_PHYS 0x01684000
gx_fifo_buf = (void *)__va(GX_FIFO_MEM1_PHYS);
```

### 6. FIFO drain polling is unreliable with LINKEN + PI_FIFO_CTRL_EN

After enabling the GP with `CP_CR_LINKEN | CP_CR_GPRESET` and writing `PI_FIFO_CTRL_EN`, the PI can continue advancing `CP_WT` after the GP drains the user-submitted commands.  This makes `RD == WT` never become true in a polling loop.

Symptom: repeated `"timed out waiting for FIFO empty (SR=0x0000)"` even though the GP had finished.

Fix: replace the polling wait with a fixed `udelay(2000)`.  At 288 bytes of commands the GP finishes in well under 1 ms; 2 ms is safe within the 16 ms vsync budget and eliminates the spurious timeout.

### 7. EFB→XFB copy confirmed reaching the display (vertical-bars breakthrough)

After all fixes above were combined into a diagnostic path that:

- Sets EFB clear colour to bright red (BP 0xE0 = `0xE000FFFF`, BP 0xE1 = `0xE1000000`)
- Does an EFB→XFB copy with `COPY_CTRL_CLEAR | COPY_CTRL_EXECUTE`

the display showed **vertical bars going all across the screen** instead of solid red.

This is a confirmed breakthrough:

- The copy IS executing and writing to the physical XFB.
- The VI IS scanning that XFB and displaying it on the TV.
- Bars rather than solid red meant something was still wrong — diagnosed as the stride bug (see §8).

### 8. EFB→XFB copy stride: 32-byte cache line units, not 16-byte (★ latest fix)

BP register `0x4D` (`dispCopyDst`) stores the XFB stride in **32-byte units** (one GX cache line), not 16-byte units.

The original comment and code were wrong:

```c
/* WRONG — comment said "units of 16 bytes" */
gx_load_bp_reg((BP_DISP_COPY_DST << 24) | ((width * 2) >> 4));
```

For `width = 640`:
- `(640 × 2) >> 4` = **80** → hardware interprets as 80 × 32 = **2560 bytes/line** ✗
- `(640 × 2) >> 5` = **40** → hardware interprets as 40 × 32 = **1280 bytes/line** ✓  
  (640 pixels × 2 bytes YUYV = 1280 bytes per scanline — correct)

**Effect of the wrong stride on display output:**

The GP wrote 1280 bytes of red YUYV at every 2560-byte offset (lines 0, 2560, 5120 …).  The 1280-byte gaps between GX writes still contained software-transcoded text from `vi_transcode_RGB565`.  When VI scanned this XFB at the correct stride of 1280 bytes per scanline:

- Even scanlines: GX wrote here → appeared red (or the YUYV equivalent)
- Odd scanlines: GX did NOT write here → still showed SW text

Alternating bands of red and text = "vertical bars" on a TV display.

**Fix** (applied in both copy paths):

```c
/* CORRECT — units of 32 bytes (one cache line) */
gx_load_bp_reg((BP_DISP_COPY_DST << 24) | ((width * 2) >> 5));
```

**Expected boot result after this fix:** display shows solid red while the EFB-clear diagnostic path is active.  If confirmed, the EFB→XFB copy path is fully verified and the texture pipeline can be restored.

## CP status register (SR) field meanings

After the endian fix, observed values and their meanings:

| SR value | Meaning |
|----------|---------|
| `0x0008` | GP stopped / idle (CP_CTRL = 0 after reset) |
| `0x000c` | GP enabled + idle (FIFO empty) |
| `0x0004` | GP enabled + running (commands in flight) |
| `0x0000` | Transitional / uncertain |

## Current diagnostic path

`gcn_gx_blit_fb_rgb565()` is temporarily a minimal diagnostic:

```c
fifo_pos = 0;
gx_load_bp_reg(0xE000FFFF);   /* BP 0xE0: A=FF R=FF */
gx_load_bp_reg(0xE1000000);   /* BP 0xE1: G=00 B=00 */
gx_load_bp_reg((BP_DISP_COPY_TL  << 24) | 0);
gx_load_bp_reg((BP_DISP_COPY_WH  << 24) | ((height-1) << 10) | (width-1));
gx_load_bp_reg((BP_DISP_COPY_DST << 24) | ((width * 2) >> 5));   /* 32-byte units */
gx_load_bp_reg((BP_DISP_COPY_ADDR<< 24) | ((xfb_phys >> 5) & 0xffffff));
ctrl = (BP_DISP_COPY_CTRL << 24) | COPY_CTRL_CLEAR | COPY_CTRL_EXECUTE;
gx_load_bp_reg(ctrl);
gx_submit_cmds();
```

The full texture pipeline (`gx_tile_rgb565` → texture bind → draw quad → copy) is commented out pending confirmation of solid-red output.

## Pending known issue: gx_tex_buf in MEM2

`gx_tex_buf` is allocated via `kmalloc`, which returns a MEM2 address (~`0x12900000`).  The GX texture unit stores texture addresses as `phys >> 5`.  This value may have bit 23 set, which the hardware truncates — the same bug as the original FIFO MEM2 issue.

When the texture pipeline is restored this must be tested.  If texture fetch is broken, move `gx_tex_buf` to a reserved MEM1 region (similar to the FIFO).

## Suggested next steps

1. **Boot-test the stride fix.**  Insert SD card, boot, observe display.
   - Expected: solid red (or a strong solid colour) instead of vertical bars.
   - If solid red → EFB→XFB copy path is fully verified, move to step 2.
   - If still bars but different pattern → check BP 0xE0/0xE1 clear colour format.
   - If display unchanged → check gx_submit_cmds is actually running (SR log).

2. **Restore the full texture pipeline in `gcn_gx_blit_fb_rgb565`.**

   ```c
   fifo_pos = 0;
   gx_tile_rgb565((const u16 *)vfb, (u16 *)gx_tex_buf, width, height);
   flush_dcache_range((unsigned long)gx_tex_buf,
                      (unsigned long)gx_tex_buf + (unsigned long)width * height * 2);
   gx_setup_2d_state(width, height);
   gx_setup_texture_rgb565(gx_tex_buf, width, height);
   gx_draw_fullscreen_quad(width, height);
   gcn_gx_copy_efb_to_xfb(xfb_phys, width, height);
   gx_submit_cmds();
   ```

3. **Verify texture output.**  With the SW transcode still running as a safety net, the GX output overwrites it each frame.  Look for the framebuffer contents (text / console output) rendered by GX.  If the display shows correct content → success.

4. **If texture fetch fails**, allocate `gx_tex_buf` from a reserved MEM1 region:

   ```dts
   /memreserve/ 0x01694000 0x0020000; /* GX texture tile buffer ~128 KB */
   ```

   and in driver:

   ```c
   #define GX_TEX_BUF_MEM1_PHYS  0x01694000
   gx_tex_buf = (void *)__va(GX_TEX_BUF_MEM1_PHYS);
   ```

   (Adjust DTS reserve to not conflict with existing `0x01694000` save-area reserve — that is only 16 KB; a 128 KB tex buf would need `0x016B4000` or similar.)

5. **Remove the SW transcode safety net** once GX output is visually correct.

6. **Clean up**: remove diagnostic `pr_info` calls in `gx_submit_cmds`, remove `panic=10` and `init=/bin/sh` from bootargs, remove `COPY_CTRL_CLEAR` diagnostic and restore normal copy.

## Known pitfalls

- Do not interpret a frozen display as a CPU crash unless the shell/keyboard also dies.
- Do not remove `flush_dcache_range()` for the command FIFO — GP DMA reads physical memory.
- Do not revert CP accessors to generic `ioread16/iowrite16`; this breaks pointer programming.
- Do not leave `CP_CR_LINKEN` enabled between frames.
- `GFP_DMA` does not force MEM1 on this platform.
- BP `0x4D` dispCopyDst is in **32-byte** units — `(width * 2) >> 5`, not `>> 4`.
- CP FIFO address registers drop bit 23 of physical addresses — MEM2 addresses fail silently.
- Do not trust CP status bits alone as an idle condition; `RD == WT` is more reliable.
