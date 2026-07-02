# Wii GX acceleration handoff - 2026-07-02

This report summarizes the current state of the direct GX framebuffer acceleration work in `wii-linux-ngx`.

## Goal

Replace the per-vsync software RGB565 to VI/XFB conversion in `drivers/video/fbdev/gcnfb.c` with a GX hardware path:

1. Tile the linear virtual framebuffer into a GX texture buffer.
2. Bind it as a texture.
3. Draw a fullscreen quad into EFB.
4. Use GX EFB-to-XFB copy to write the physical XFB scanned by VI.

The long-term motivation is a usable lightweight Wii desktop environment, where the CPU cost of software framebuffer conversion would be a major bottleneck.

## Current HEAD

Branch: `feature/gcn-gx-accel`

Current source HEAD:

```text
a06c3538569e Stop GX CP after submitted FIFO drains
```

Current built image hash after that commit:

```text
71befcfb66d2440514da8b03a86240efd9abf3289154097727807aff6c70e203  arch/powerpc/boot/zImage
```

Important: this `a06c3538569e` build was compiled but not boot-tested or deployed before the handoff request. The previous deployed and tested commit was `86ca3d80c91a`.

Untracked generated files remain intentionally uncommitted:

```text
build.log
drivers/video/fbdev/Module.symvers
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

## Critical discoveries

### 1. The original "freeze after H" was usually a display freeze

When `gx_accel_ready=true`, the RGB565 path in `gcnfb.c` takes the GX blit path. If that blit does not update `fb_mem`/XFB, VI keeps scanning the last software-transcoded frame and the screen appears frozen.

The CPU can still be alive and boot into a shell. This was confirmed by forcing the software transcode path back on.

Current diagnostic behavior in `gcnfb.c` keeps the software transcode running unconditionally in the RGB565 path and then optionally runs GX:

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

`gx_submit_cmds()` must flush `gx_fifo_buf` before enabling GP reads. The CPU builds command bytes in cache, while the GP DMA fetches from physical memory.

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

The original CP accessors used generic `ioread16/iowrite16`. That produced misleading status and bad pointer reads.

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

This was the breakthrough that made the GP consume the submitted commands.

### 5. The command FIFO currently uses reserved MEM1

`GFP_DMA` did not move the small FIFO out of MEM2. On Wii this kernel coalesces MEM1 and MEM2 into one logical memory range, so plain page/slab allocation still returned `0x12800000`.

Current DTS reserve:

```dts
/memreserve/ 0x01684000 0x0010000; /* GX command FIFO */
```

Current driver uses:

```c
#define GX_FIFO_MEM1_PHYS 0x01684000
gx_fifo_buf = (void *)__va(GX_FIFO_MEM1_PHYS);
```

This made `fifo_init: phys=0x01684000`.

Note: after CP endian was fixed, the MEM1 requirement may not be strictly necessary. It remains in place because it is stable and removes one variable.

## Boot evidence

### Before CP endian fix

With FIFO at `0x12800000`, CP readback lost `0x00800000`:

```text
submit: base=0x12800000 wt=0x12800120 pos=288 SR=0x0800
submit+1ms: SR=0x0800 RD=0x12000000 WT=0x12000020 PI_WPTR=0x12800120
```

After moving FIFO to MEM1 but before CP endian fix:

```text
fifo_init: phys=0x01684000
preGP: RDoff=0xffa00000 WToff=0xffa00020 PIoff=0x0120 pos=288
postGP: SR=0x0800 RDoff=0xffa00000 WToff=0xffa00020 PIoff=0x0120
```

This proved MEM1 alone was not enough.

### After CP endian fix

This was the major success:

```text
gcn-gx: init: A wptr=0x01684000 hw_base=d42f0000
gcn-gx: fifo_init: phys=0x01684000
gcn-gx: fifo_init: CR=0 SR=0x0008
gcn-gx: submit: base=0x01684000 wt=0x01684120 pos=288 SR=0x0008
gcn-gx: preGP: RDoff=0x0000 WToff=0x0120 PIoff=0x0120 pos=288
gcn-gx: postGP: SR=0x000c RDoff=0x0120 WToff=0x0120 PIoff=0x0120
```

Interpretation:

- The command buffer is 288 bytes.
- CP RD starts at FIFO base.
- CP WT and PI WPTR point at `base + 0x120`.
- After enabling GP, CP RD advances to `base + 0x120`.
- Therefore the GP is executing/fetching the whole submitted GX command buffer.

This is the most important known-good result.

## Current unresolved issue

Even after the first command buffer is consumed, later logs showed:

```text
gcn-gx: timed out waiting for GP idle (SR=0x0004)
```

Then after accepting `0x0004`:

```text
gcn-gx: timed out waiting for GP idle (SR=0x0000)
```

This happened about 3.4 seconds after the first logged GX command, so likely on a later DI1 frame, not during the first submit.

Hypothesis: leaving `CP_CR_GPRESET | CP_CR_LINKEN` enabled between frames allows background VI/console/wgPipe/PI behavior to disturb CP/PI FIFO state before the next frame. The last untested commit (`a06c3538569e`) tries to handle this by making submit synchronous:

1. Enable GP.
2. Wait for `RD == WT` via `gx_wait_fifo_empty()`.
3. Clear `CP_CTRL` to disable GP/link until the next submit.

Relevant current code:

```c
cp_write(CP_REG_CTRL, CP_CR_GPRESET | CP_CR_LINKEN);
...
gx_wait_fifo_empty();
cp_write(CP_REG_CTRL, 0);
```

This was built but not boot-tested before handoff.

## Commit timeline

Key commits in this session:

```text
a06c3538569e Stop GX CP after submitted FIFO drains
86ca3d80c91a Wait for GX FIFO before stopping GP
2724ff009637 Treat empty GX FIFO as idle
f347369d42f6 Accept both GX idle status bits
82edafbc61a2 Fix endian-correct GX idle status check
809fabffd511 Use big-endian CP register accessors
9272e318dce9 Use reserved MEM1 GX command FIFO
f2a84a6476fe Allocate GX command FIFO from DMA memory
6166bc729b5c Log GX FIFO pointer offsets
b3dd856b357d Log GX FIFO state after enabling GP
9a93165c5b88 Treat Wii GP stopped status as idle
161c72f11ed5 Restore RGB565 GX blit pipeline
08500067ec63 Test enabling PI FIFO for GX submit
8ae6787d43e3 Checkpoint Wii GX graphics diagnostics
```

The decisive commit is:

```text
809fabffd511 Use big-endian CP register accessors
```

The known-good GP-command-consumption evidence happened after that commit and before later idle experiments.

## Current code behavior

### GX init

`gcn_gx_init()`:

- Uses fixed reserved MEM1 FIFO at `0x01684000`.
- Flushes that FIFO range.
- Maps Hollywood registers at `GX_HW_BASE`.
- Immediately writes PI FIFO WPTR to the FIFO address to avoid mini's stale `WPTR=0`.
- Leaves CP disabled until submit time.
- Allocates texture tile buffer with `kmalloc`.
- Sets `gx_accel_ready = true`.

### Submit path

`gx_submit_cmds()`:

- Pads command buffer to 32 bytes.
- Logs first submit.
- Waits for previous FIFO idle/drain.
- Disables CP.
- Flushes the command FIFO dcache.
- Programs CP FIFO BASE/END/RD/WT.
- Programs PI FIFO BASE/END/WPTR/CTRL.
- Logs pre-GP first-submit pointer offsets.
- Enables `CP_CR_GPRESET | CP_CR_LINKEN`.
- Logs post-GP first-submit pointer offsets after 1 ms.
- Current HEAD then waits for FIFO empty and clears `CP_CTRL=0`.

### Blit path

`gcn_gx_blit_fb_rgb565()` currently runs the full pipeline:

```c
gx_tile_rgb565((const u16 *)vfb, (u16 *)gx_tex_buf, width, height);
flush_dcache_range(...);
fifo_pos = 0;
gx_setup_2d_state(width, height);
gx_setup_texture_rgb565(gx_tex_buf, width, height);
gx_draw_fullscreen_quad(width, height);
gcn_gx_copy_efb_to_xfb(xfb_phys, width, height);
gx_submit_cmds();
```

## Things not yet proven

- Whether the full draw/texture/EFB-copy produces correct pixels in XFB.
- Whether `gx_tex_buf` being in MEM2 is acceptable for texture fetch. It is currently at a virtual pointer like `d2800000`; physical may be MEM2. GP command fetch works now, but texture DMA/fetch path may have separate constraints.
- Whether the EFB-to-XFB copy format/state is correct for VI's expected YUYV/XFB layout.
- Whether the software transcode safety net can be removed.

## Suggested next steps

1. Boot-test current HEAD `a06c3538569e`.

   Expected first-submit lines should remain:

   ```text
   preGP: RDoff=0x0000 WToff=0x0120 PIoff=0x0120 pos=288
   postGP: SR=0x000c RDoff=0x0120 WToff=0x0120 PIoff=0x0120
   ```

   Watch for either:

   ```text
   timed out waiting for GP idle
   timed out waiting for FIFO empty
   ```

2. If current HEAD removes the delayed timeout, add a second one-shot log after `gx_wait_fifo_empty()` and `CP_CTRL=0` to confirm final CP/PI state.

3. If current HEAD still times out, temporarily disable repeated GX submits after the first frame:

   - Let first frame submit and drain.
   - Set a static flag so later DI1 frames only run software transcode.
   - If the delayed timeout disappears, the issue is definitely multi-frame FIFO reprogramming / linked PI state.

4. Once submit is stable over many frames, test visible GX output:

   - Keep software transcode enabled.
   - Modify GX path to intentionally copy a solid/color-pattern EFB or render a simple colored quad.
   - Look for flicker/color overwrite or any visible artifact.

5. If texture rendering appears wrong, try moving `gx_tex_buf` to reserved MEM1 or a known GPU-visible physical range.

6. Only after GX output is visibly correct, remove the unconditional software transcode safety net.

## Known pitfalls

- Do not interpret a frozen display as a CPU crash unless the shell/keyboard also dies. Incomplete GX output leaves VI scanning the last transcoded frame.
- Do not remove `flush_dcache_range()` for the command FIFO.
- Do not revert CP accessors to generic `ioread16/iowrite16`; this breaks pointer programming.
- Do not trust CP status bits alone as an idle condition. Pointer equality (`RD == WT`) has been more meaningful.
- `GFP_DMA` did not force MEM1 on this platform.
- Leaving `LINKEN` enabled between frames is suspicious and may let PI/wgPipe activity disturb FIFO state.

