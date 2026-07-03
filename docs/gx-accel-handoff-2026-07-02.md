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
37328d48c240 Move tex_buf to MEM1; restore full pipeline; diagnose COPY_CTRL_CLEAR
78d43025d4e0 Update GX handoff: vertical bars diagnosis, stride fix, next steps
2ed459a6c78f Diagnose vertical-bars output; fix stride, drain wait, diagnostic blit
69027724c2c6 Document Wii GX acceleration handoff
a06c3538569e Stop GX CP after submitted FIFO drains
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

### 9. COPY_CTRL_CLEAR is clear-AFTER-copy, not clear-before (★ session 2)

After the stride fix, the display showed **RGB pixel noise** (random coloured pixels, full screen) instead of solid red.

This confirmed that:
- The stride fix worked — GP is now writing all 480 lines of the XFB (full-screen noise vs. the earlier half-screen alternating bands).
- `COPY_CTRL_CLEAR` (BP 0x52 bit 11) does **NOT** fill the EFB with the clear colour and then copy it.  It copies the current EFB contents **first**, then fills the EFB with the clear colour for the next frame.

Mini leaves the EFB in an uninitialised state.  Each frame the GP copies that random content to the XFB (noise), then clears the EFB to our red colour — which is then copied as noise again next frame by the same mechanism.

`COPY_CTRL_CLEAR` is a "prepare EFB for next frame" operation, not a "fill XFB with colour" operation.  The only way to get controlled content into the XFB is to **draw into the EFB** via the GX rendering pipeline first, then do a plain copy without `CLEAR`.

### 10. gx_tex_buf must be in MEM1 (★ session 2)

The GX texture fetch unit is GameCube-era hardware.  GameCube has only MEM1 (physical 0x00000000–0x01800000).  The Wii adds MEM2 (physical 0x10000000–0x14000000), but the GX hardware predates it and **cannot generate MEM2 bus addresses** for texture fetches.

`kmalloc(GX_TEX_BUF_SIZE, GFP_KERNEL)` returned a MEM2 virtual address on Wii Linux (~`0xC2900000` → physical ~`0x12900000`) because MEM1 and MEM2 are coalesced into a single logical range.  `GFP_DMA` did not help.

When `gx_setup_texture_rgb565` programmed BP 0x94 with `(phys >> 5)` for a MEM2 address, the texture unit fetched from the wrong bus, producing garbage in the EFB.

Fix (now applied):

```dts
/memreserve/ 0x01200000 0x000C0000; /* GX texture tile buffer 768 KB */
```

```c
#define GX_TEX_BUF_MEM1_PHYS  0x01200000
gx_tex_buf = (void *)__va(GX_TEX_BUF_MEM1_PHYS);
```

Physical `0x01200000` is safely in MEM1.  `phys >> 5 = 0x90000` (no bit-23 issue).  The DTS reserve prevents the allocator from handing this range to other users.

## Current pipeline state

`gcn_gx_blit_fb_rgb565()` has the full texture pipeline restored:

```c
fifo_pos = 0;
gx_tile_rgb565((const u16 *)vfb, (u16 *)gx_tex_buf, width, height);
flush_dcache_range((unsigned long)gx_tex_buf,
                   (unsigned long)gx_tex_buf + width * height * 2);
gx_setup_2d_state(width, height);
gx_setup_texture_rgb565(gx_tex_buf, width, height);
gx_draw_fullscreen_quad(width, height);
gcn_gx_copy_efb_to_xfb(xfb_phys, width, height);   /* no CLEAR */
gx_submit_cmds();
```

`gcnfb.c` still runs `vi_transcode_RGB565` unconditionally as a safety net.  The GX blit runs after it each frame and overwrites the XFB if GX output is correct.

## Suggested next steps

1. **Boot-test the restored pipeline.**  Observe the display.
   - Expected: framebuffer content (text, console output) rendered by GX hardware, identical to the software-transcoded image but produced by the GP.
   - If correct content appears → GX acceleration is working; move to cleanup.
   - If garbage / wrong colours appear → tiling or texture/TEV setup is wrong; see below.
   - If display shows the SW transcode output unchanged (no GX visible) → GP is running but EFB copy goes somewhere wrong; re-check `gx_fb_start` address.

2. **If the image is wrong colour or distorted**, check the tiling with a single-frame dump:
   - Add a `pr_info` with the first few bytes of `gx_tex_buf` after `gx_tile_rgb565`.
   - Compare against the expected tiled layout for the known on-screen colours.

3. **Remove the SW transcode safety net** once GX output is visually correct.  Change `gcnfb.c` RGB565 path to GX-only.

4. **Clean up**: remove diagnostic `pr_info` calls in `gx_submit_cmds`, remove `panic=10` and `init=/bin/sh` from bootargs once stable.

## Known pitfalls

- Do not interpret a frozen display as a CPU crash unless the shell/keyboard also dies.
- Do not remove `flush_dcache_range()` for the command FIFO — GP DMA reads physical memory.
- Do not revert CP accessors to generic `ioread16/iowrite16`; this breaks pointer programming.
- Do not leave `CP_CR_LINKEN` enabled between frames.
- `GFP_DMA` does not force MEM1 on this platform (MEM1 and MEM2 are coalesced).
- BP `0x4D` dispCopyDst is in **32-byte** units — `(width * 2) >> 5`, not `>> 4`.
- CP FIFO address registers drop bit 23 of physical addresses — MEM2 addresses fail silently.
- Do not trust CP status bits alone as an idle condition; `RD == WT` is more reliable.
- `COPY_CTRL_CLEAR` (BP 0x52 bit 11) clears the EFB **after** the copy, not before.  It does not fill the XFB with the clear colour.  Use the rendering pipeline to populate the EFB.
- The GX texture fetch unit cannot access MEM2 (0x10000000+).  `gx_tex_buf` and any other GPU-visible buffer must be in MEM1 via `/memreserve/` + `__va(phys)`.
