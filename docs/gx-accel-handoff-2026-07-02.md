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

Recent commits before the latest diagnostic patch (most recent first):

```text
17578e99c698 gcn-gx: load identity position matrix for diagnostics
4b4a2259a872 gcn-gx: add solid raster-color diagnostic
d39ddb35f1f7 gcn-gx: fix stage-0 TEV order binding
4ff55b37bc23 gcn-gx: add color-cycle diagnostic, onboard logging, update handoff
02b97bd20a53 gcn-gx: fix three PE pixel-discard traps and sync mechanism
...
```

### Session 2026-07-03 latest diagnostic

The previous deployed build reached userspace and wrote `/dmesg.txt`, proving the
system was not broadly wedged before the log script ran.  The critical failure was
specific to the frame-360 direct raster-colour diagnostic:

```text
gcn-gx: f360 pre:  SR=0000 RD=0000 WT=0140 pos=320
gcn-gx: f360 post: SR=0000 RDoff=0000 WToff=0140
gcn-gx: f360 tex0=f800f800 xfb0=00800080 xfb1=00800080
```

Frames 0-3 consumed their FIFO normally (`RDoff=0180`), but frame 360 did not
advance RD at all.  To isolate whether that was the direct-colour command stream
or a time/state issue, the latest patch removes the active direct-colour path and
changes the frame-360 colour test to reuse the same textured draw path as frames
0-3, with a solid RGB565-filled texture.

**Current deployed image hash:** `3138062b1fc69cfcc177464993e10993edbdb8b9c812a94c6abc9990b9f945b5`

Boot result for that image:

```text
gcn-gx: f0 post:   SR=000c RDoff=0180 WToff=0180
gcn-gx: f1 post:   SR=000c RDoff=0180 WToff=0180
gcn-gx: f2 post:   SR=0004 RDoff=0180 WToff=0180
gcn-gx: f3 post:   SR=0004 RDoff=0180 WToff=0180
gcn-gx: f360 pre:  SR=0000 RD=0000 WT=0180 pos=384
gcn-gx: f360 post: SR=0000 RDoff=0000 WToff=0180
gcn-gx: f360 tex0=f800f800 xfb0=00800080 xfb1=00800080
--- init-diag: after blink ---
28.69 24.23
--- init-diag: before shell ---
28.72 24.23
```

Interpretation: the Wii did not broadly crash or reboot before the diagnostic
completed.  Userspace survived long enough to log and blink.  The direct-colour
diagnostic was not the root cause: frame 360 still fails when using the textured
path and the same 384-byte command size as frames 0-3.  The next question is:
which frame first stops advancing `RD`?

First-stall test result:

```text
gcn-gx: first_stall f13 SR=0000 RDoff=0120 WToff=0180 PIoff=0180 pos=384
```

Interpretation: frame 13 did not fully drain within the fixed 2 ms delay, but it
did consume the first `0x120` bytes of the `0x180` byte FIFO.  The old submit
path then disabled CP immediately, truncating the remaining `0x60` bytes.  That
likely corrupts or wedges later GP state and explains why f360 later starts from
`RDoff=0000`.

Latest uncommitted test patch: `gx_submit_cmds()` now logs the first slow frame,
waits up to an extra 8 ms for RD to catch WT, and only reports `first_stall` if
RD still does not match WT after that grace period.  This avoids disabling CP
while the GP is mid-command stream.

Boot result for the extended-drain image:

```text
gcn-gx: first_slow f13 SR=0000 RDoff=0120 WToff=0180 PIoff=0180 pos=384
gcn-gx: first_stall f13 SR=0000 RDoff=0120 WToff=0180 PIoff=0180 pos=384
```

The extra wait did not help.  Offset `0x120` is at the start of the fullscreen
quad vertex payload: state setup and texture binding were consumed, then the GP
blocked while feeding geometry into the backend.  Since early frames drained and
XFB readback stayed black, the likely failure is that EFB copy/PE state is not
completing, the backend fills, and later primitives stall.

Latest active test patch: the blit now skips `gcn_gx_copy_efb_to_xfb()` entirely
and submits only setup + texture + quad.  The display is expected not to update
from GX in this image.  The result is in `/dmesg.txt`: if `first_stall` disappears
or moves far later, the copy/PE path is the culprit.

No-copy result:

```text
gcn-gx: first_slow f14 SR=0000 RDoff=0120 WToff=0160 PIoff=0160 pos=352
gcn-gx: first_stall f14 SR=0000 RDoff=0120 WToff=0160 PIoff=0160 pos=352
```

Copy/PE is therefore not the trigger.  The stall point is still the quad stream.
Latest active test patch adds `stall_bytes` dumps around `RDoff` on the first
hard stall so the exact GP parser position can be decoded from `/dmesg.txt`.

Stall-byte result:

```text
gcn-gx: stall_bytes @0110: 31 00 01 af 80 00 04 00 00 00 00 00 00 00 00 00
gcn-gx: stall_bytes @0120: 00 00 00 00 00 00 00 44 10 00 00 00 00 00 00 44
```

Decode:

- `0x0110`: BP `0x31` (`suTsize`) value `0x0001af`
- `0x0114`: primitive command `0x80` (`GX_QUADS`) count `0x0004`
- `0x0117`: first vertex starts
- `0x0120`: inside the first vertex's all-zero texture-coordinate payload

The command stream is syntactically sane.  The GP is stalling while feeding the
primitive/vertex stream to the raster/PE backend.  Latest active patch increases
the post-submit diagnostic delay from 2 ms to 10 ms before checking RD/WT and
disabling CP.  If this eliminates `first_stall`, the bug is missing/incorrect
PE/raster completion sync between frames.

10 ms result:

```text
gcn-gx: first_slow f14 SR=0000 RDoff=0120 WToff=0160 PIoff=0160 pos=352
gcn-gx: first_stall f14 SR=0000 RDoff=0120 WToff=0160 PIoff=0160 pos=352
```

The longer delay did not help.  The next patch removed texture setup and TEX0
vertex data entirely: `gcn_gx_blit_fb_rgb565()` submitted a position-only quad
with no EFB->XFB copy.

Position-only result:

```text
gcn-gx: f0 post: SR=000c RDoff=0100 WToff=0100
gcn-gx: f1 post: SR=000c RDoff=0100 WToff=0100
gcn-gx: f2 post: SR=000c RDoff=0100 WToff=0100
gcn-gx: f3 post: SR=000c RDoff=0100 WToff=0100
gcn-gx: f360 post: SR=000c RDoff=0100 WToff=0100
```

Image hash for that successful position-only build:

```text
12a2a11f1c673898b1d3841b422ec7a2c65396cd4db6a7bbd57e3d7a0923d19b
```

Interpretation: the GP can parse and drain the primitive stream when the vertex
format is direct XY position only.  The recurring `RDoff=0120` stall is therefore
not a general primitive/raster failure and not caused by EFB->XFB copy.  It is
isolated to TEX0 texture-coordinate state or the texture fetch path.

Latest active test image: direct TEX0 parsing without texture fetch.

```text
2e3d0c85513fe50a685d7cfb5f7917f5b108dca6b36709aabb1e7c1d7c16ebbe
```

This image submits a fullscreen quad with direct XY position plus direct TEX0
vertex data and XF texcoord generation enabled, but leaves TEV texture fetch
disabled (`TEV_ORDER` texture enable clear) and still skips EFB->XFB copy.  The
test separates CP/VAT/VCD parsing of TEX0 payload from TMU texture binding/fetch.

Expected interpretation for `/dmesg.txt` from this image:

- If it stalls again at or near `RDoff=0120`, the failure is in the TEX0 vertex
  format / VAT / VCD / texcoord-generation setup.
- If it drains like the position-only image, the TEX0 parser is fine and the
  next suspect is texture object binding or TMU fetch state.

## Onboard diagnostic logging

**The kernel is booting with `init=/init-diag.sh` in the bootargs.**  This script:

1. Mounts `/proc` and `/sys`.
2. Remounts rootfs read-write.
3. Writes `/dmesg.txt` start/uptime markers.
4. Sleeps 20 seconds (captures the frame-360 diagnostic window).
5. Appends `dmesg`, `/proc/bus/input/devices`, and `/dev/input/` output.
6. Writes uptime markers before and after the slot-LED blink, then before shell exec.
7. Blinks the Wii slot LED 10 times when logging is complete.
8. Calls `exec /bin/sh -l` on `/dev/console` (interactive shell available after ~25s).

Script location on the rootfs (WII-LINUX-NGX1 partition):

```text
/media/anolis/WII-LINUX-NGX1/init-diag.sh
```

Script contents:

```sh
# Current script also contains blink_done() for the Wii slot LED.
echo "--- init-diag: start ---" > /dmesg.txt
cat /proc/uptime >> /dmesg.txt 2>&1
sync
sleep 20
echo "--- init-diag: after sleep ---" >> /dmesg.txt
cat /proc/uptime >> /dmesg.txt 2>&1
dmesg >> /dmesg.txt
echo "--- /proc/bus/input/devices ---" >> /dmesg.txt
cat /proc/bus/input/devices >> /dmesg.txt 2>&1
echo "--- /dev/input ---" >> /dmesg.txt
ls -la /dev/input/ >> /dmesg.txt 2>&1
sync
echo "--- init-diag: before blink ---" >> /dmesg.txt
blink_done
echo "--- init-diag: after blink ---" >> /dmesg.txt
echo "--- init-diag: before shell ---" >> /dmesg.txt
exec /bin/sh -l </dev/console >/dev/console 2>/dev/console
```

To retrieve results: boot Wii, wait ~25 seconds for logging to complete, power off, move SD card to host, read `/media/anolis/WII-LINUX-NGX1/dmesg.txt`.

All `gcn-gx:` log lines are written to the kernel ring buffer and captured by this script.

## Build and deploy

Build command:

```sh
make ARCH=powerpc CROSS_COMPILE=powerpc-linux-gnu- -j$(nproc)
```

Output image: `arch/powerpc/boot/dtbImage.wii`

Deploy target:

```text
/media/anolis/BOOTWII/gumboot/zImage.ngx
```

Deploy command:

```sh
cp arch/powerpc/boot/dtbImage.wii /media/anolis/BOOTWII/gumboot/zImage.ngx
sync
```

The SD card is manually moved between host and Wii.  BOOTWII (FAT, ~40 MB) and WII-LINUX-NGX1 (ext2, ~338 MB) are separate partitions on the same card.

## Key files

Main GX driver:

```text
drivers/video/fbdev/gcn-gx.c   ← all GX register programming
drivers/video/fbdev/gcn-gx.h   ← register addresses and bit definitions
```

Framebuffer / VI integration:

```text
drivers/video/fbdev/gcnfb.c    ← vi_dispatch_vtrace calls gcn_gx_blit_fb_rgb565
```

Wii memory reservation (FIFO, tex buf, XFB all reserved here):

```text
arch/powerpc/boot/dts/wii.dts
```

Boot diagnostic script (currently active):

```text
/media/anolis/WII-LINUX-NGX1/init-diag.sh
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

Fix: replace the polling wait with a fixed `udelay(2000)`.  At ~300 bytes of commands the GP finishes in well under 1 ms; 2 ms is safe within the 16 ms vsync budget and eliminates the spurious timeout.

### 7. EFB→XFB copy confirmed reaching the display (vertical-bars breakthrough)

After all fixes above were combined into a diagnostic path that:

- Sets EFB clear colour to bright red
- Does an EFB→XFB copy with `COPY_CTRL_CLEAR | COPY_CTRL_EXECUTE`

the display showed **vertical bars going all across the screen** instead of solid red.

This confirmed that:
- The copy IS executing and writing to the physical XFB.
- The VI IS scanning that XFB and displaying it on the TV.
- Bars rather than solid red meant something was still wrong — diagnosed as the stride bug (see §8).

### 8. EFB→XFB copy stride: 32-byte cache line units, not 16-byte

BP register `0x4D` (`dispCopyDst`) stores the XFB stride in **32-byte units** (one GX cache line), not 16-byte units.

For `width = 640`:
- `(640 × 2) >> 4` = **80** → hardware interprets as 80 × 32 = **2560 bytes/line** ✗
- `(640 × 2) >> 5` = **40** → hardware interprets as 40 × 32 = **1280 bytes/line** ✓

Fix (applied in all copy paths):

```c
gx_load_bp_reg((BP_DISP_COPY_DST << 24) | ((width * 2) >> 5));
```

### 9. COPY_CTRL_CLEAR is clear-AFTER-copy, not clear-before

`COPY_CTRL_CLEAR` (BP 0x52 bit 11) does **NOT** fill the EFB with the clear colour and then copy it.  It copies the current EFB contents **first**, then fills the EFB with the clear colour for the next frame.

`COPY_CTRL_CLEAR` is a "prepare EFB for next frame" operation, not a "fill XFB with colour" operation.  The only way to get controlled content into the XFB is to **draw into the EFB** via the GX rendering pipeline first, then do a plain copy without `CLEAR`.

### 10. gx_tex_buf must be in MEM1

The GX texture fetch unit is GameCube-era hardware and cannot address MEM2 (0x10000000+).  `kmalloc` and `GFP_DMA` both return MEM2 addresses on Wii Linux (MEM1+MEM2 are coalesced).  Programming BP 0x94 with a MEM2 physical address silently fetches from the wrong bus.

Fix:

```dts
/memreserve/ 0x01200000 0x000C0000; /* GX texture tile buffer 768 KB */
```

```c
#define GX_TEX_BUF_MEM1_PHYS  0x01200000
gx_tex_buf = (void *)__va(GX_TEX_BUF_MEM1_PHYS);
```

### 11. BP 0x41 (BLENDMODE) not set → colorupdate=0 → PE silently drops all pixels

**Symptom** (builds `be9660ba` and `6176efff`):

```text
[   15.597119] gcn-gx: f360 tex0=f800f800 xfb0=00800080 xfb1=00800080
```

At frame 360: `gx_tex_buf` correctly filled with solid red (`tex0=0xf800f800`), but `xfb0` remains YUYV black (`0x00800080`).  GP consumed all commands every frame.  Rasterizer was running; nothing reached the EFB.

**Root cause**: BP 0x41 (`BPMEM_BLENDMODE`) hardware reset value = 0x00.  Bit 3 (`colorupdate`) = 0 means the Pixel Engine **silently discards every rasterized pixel** without writing to the EFB.

BPMEM_BLENDMODE bit layout:

```
[0]     blendenable
[1]     logicopenable
[2]     dither
[3]     colorupdate  ← must be 1 for draws to write EFB color
[4]     alphaupdate
[7:5]   dstfactor
[10:8]  srcfactor
[11]    subtract
[15:12] logicmode
```

**Fix** (added to `gx_setup_2d_state`):

```c
/* colorupdate=1 (bit 3), alphaupdate=1 (bit 4), no blending */
gx_load_bp_reg(0x41000018);
```

### 12. EHCI spinlock BUG and DI1 interrupt delivery

At `t ≈ 0.508s` a pre-existing EHCI USB 2.0 controller bug fires and holds a spinlock for ~2.5 seconds:

```text
[    0.508101] BUG: spinlock bad magic at call site ehci_halt+0x...
[    3.064244] BUG: spinlock lockup suspected...
```

The GX blit frame counter stops at frame 3 during this window.  However, frame 360 appears at `t = 15.6s`, confirming DI1 resumes after the lockup.  DI1 fires at ~23-24 Hz (every-other-field VI behaviour, not 60 Hz).

The EHCI BUG does NOT permanently kill DI1 and is NOT the root cause of display issues.

### 13. BP 0xF3 (ALPHA_COMPARE) not set → alpha test NEVER → all fragments discarded (★ most recent fix — not yet boot-tested)

**Symptom** (build `6176efff`, after adding BP 0x41 fix from §11):

```text
[   15.096648] gcn-gx: f360 tex0=f800f800 xfb0=00800080 xfb1=00800080
```

`xfb0` still `0x00800080` after the BLENDMODE fix.  `colorupdate=1` is now set, but pixels are still not reaching the EFB.

**Root cause**: BP 0xF3 (`BPMEM_ALPHA_COMPARE`) hardware reset value = 0x00.

BPMEM_ALPHA_COMPARE bit layout (from Dolphin BPMemory.h `AlphaTest`):

```
[7:0]   ref0   — reference value for comparison 0
[15:8]  ref1   — reference value for comparison 1
[18:16] comp0  — 0=NEVER, 1=LESS, 2=EQUAL, 3=LEQUAL, 4=GREATER, 5=NEQUAL, 6=GEQUAL, 7=ALWAYS
[21:19] comp1  — same encoding
[23:22] logic  — 0=AND, 1=OR, 2=XOR, 3=XNOR
```

Reset value: `comp0=NEVER (0)`, `comp1=NEVER (0)`, `logic=AND (0)`.  
Result: `NEVER AND NEVER = ALWAYS_FAIL`.  **Every rasterized fragment is thrown away by the alpha test before it can reach the Pixel Engine**, making `colorupdate=1` completely irrelevant.

The GX alpha test runs in the pixel pipeline between TEV output and EFB write.  On hardware (unlike Dolphin's emulation), there is no fast-path optimization — comp0=NEVER means exactly NEVER, discarding all fragments regardless of their actual alpha value.

**Fix** (added to `gx_setup_2d_state`, after BP 0x41):

```c
/* comp0=ALWAYS (7), ref0=0, comp1=ALWAYS (7), ref1=0, logic=AND (0)
 * 7<<16 | 7<<19 = 0x3F0000 → every fragment passes, none discarded */
gx_load_bp_reg(0xF33F0000);
```

**The pattern**: ZMODE (BP 0x40), BLENDMODE (BP 0x41), and ALPHA_COMPARE (BP 0xF3) all have hardware reset values that discard pixels.  Each was found and fixed separately.  Always set all three explicitly in any GX 2D init sequence.

### 14. BP 0x25, not 0x28, is TEV order for stage 0

Booting the `b1c85bf6` diagnostic image showed that the GP consumes the full
FIFO and the color-cycle texture buffer contains solid red, but the copied XFB
is still black:

```text
[   15.096539] gcn-gx: f360 tex0=f800f800 xfb0=00800080 xfb1=00800080
```

This means the texture buffer is correct and EFB->XFB copy still runs, but the
draw is not producing color in EFB.

Review against libogc found a concrete register mismatch: stage-0 TEV order is
stored in `tevRasOrder[0]`, whose BP register id is `0x25`.  The code was
writing `0x280003C0`, which targets a later TEV order register and leaves stage
0's texmap/texcoord binding at reset state.

Fix:

```c
gx_load_bp_reg(0x250003C0);  /* stage 0: texmap=0, texcoord=0, texenable=1 */
```

The same pass also fixed the TEV alpha input constant from `0xC108FFD0` to
`0xC108FFC0`: libogc defines `GX_CA_TEXA = 4`, not 5.

Boot result after this fix was still black at frame 360:

```text
gcn-gx: f360 tex0=f800f800 xfb0=00800080 xfb1=00800080
```

### 15. Current diagnostic: solid raster-colour quad, no texture

The next diagnostic build changes only the frame 360-899 colour-cycle window.
Instead of sampling `gx_tex_buf`, it now draws a fullscreen quad with direct
RGBA vertex colours and TEV `GX_PASSCLR`:

- `genMode`: 0 texgens, 1 colour channel, 1 TEV stage.
- TEV order BP `0x25`: texture disabled, colour channel `GX_COLOR0A0`.
- TEV colour/alpha: pass raster colour/alpha.
- VCD/VAT: direct XY position plus direct RGBA8 colour.

Expected interpretation:

- If frame 360 XFB becomes non-black, vertex/raster/PE/copy work and the
  remaining bug is texture or textured-TEV state.
- If frame 360 stays `0x00800080`, the failure is not texture fetch; continue
  looking at vertex/raster/PE state or EFB state.

Boot result: frame 360 still stayed black:

```text
gcn-gx: f360 tex0=f800f800 xfb0=00800080 xfb1=00800080
```

### 16. Current diagnostic: explicitly load PNMTX0 identity/current matrix

The next diagnostic keeps the solid raster-colour quad, but adds the missing
position matrix state that libogc initializes during `GX_Init()`:

- `GX_LoadPosMtxImm(identity, GX_PNMTX0)`: XF regs `0x0000..0x000b`.
- `GX_SetCurrentMtx(GX_PNMTX0)`: XF reg `0x1018 = 0`.

This is important because the projection matrix is not the whole transform.
Vertices are first multiplied by the current position matrix; if PNMTX0 contains
reset garbage, the quad can be clipped before rasterization.  Frame 360 now also
logs submit pre/post so we can verify this larger diagnostic FIFO is consumed.

**Expected result for the current diagnostic build**: At frame 360 (`t ≈ 15s`),
`xfb0` should become non-black if missing PNMTX0/current-matrix state was why
the primitive never reached raster/PE.

## CP status register (SR) field meanings

| SR value | Meaning |
|----------|---------|
| `0x0008` | GP stopped / idle (CP_CTRL = 0) |
| `0x000c` | GP enabled + FIFO empty (idle, all commands consumed) |
| `0x0004` | GP enabled + FIFO not empty (still processing) |
| `0x0000` | Transitional / uncertain |

## Current pipeline state

`gx_setup_2d_state()` now sets these registers explicitly (in order):

```c
gx_load_bp_reg(0x40000000);  /* ZMODE:          Z disabled */
gx_load_bp_reg(0x41000018);  /* BLENDMODE:      colorupdate=1, alphaupdate=1 */
gx_load_bp_reg(0xF33F0000);  /* ALPHA_COMPARE:  comp0=ALWAYS, comp1=ALWAYS  */
gx_load_bp_reg(0x00000001);  /* GENMODE:        1 texgen, 1 TEV stage        */
gx_load_bp_reg(0x20...);     /* SCISSOR TL                                    */
gx_load_bp_reg(0x21...);     /* SCISSOR BR                                    */
gx_load_bp_reg(0xC008FFF8);  /* TEV COLOR:      output = texture color        */
gx_load_bp_reg(0xC108FFC0);  /* TEV ALPHA:      output = texture alpha        */
gx_load_bp_reg(0x250003C0);  /* TEV ORDER:      texmap=0, texcoord=0, enable  */
```

`gcnfb.c` currently takes the GX blit path directly when `gx_accel_ready` is true.
The software RGB565 transcode path is not running as a safety net in this build.

`gcn_gx_blit_fb_rgb565()` behaviour in the current deployed image:

- Does not tile `vfb_mem`.
- Does not bind or fetch from `gx_tex_buf`.
- Does not issue EFB->XFB copy.
- Sets GX state for direct XY position plus direct TEX0 vertex payload and draws
  the normal fullscreen quad.
- Logs frames 0-3 plus frame 360, and logs first slow/stalled submit with bytes
  around the CP read pointer.

## Suggested next steps

1. **Boot the current TEX0-parse/no-fetch image**:
   - Image hash: `2e3d0c85513fe50a685d7cfb5f7917f5b108dca6b36709aabb1e7c1d7c16ebbe`.
   - Pull `/dmesg.txt` from the rootfs after the slot LED blink.
   - If it stalls around `RDoff=0120`, inspect VCD/VAT/texcoord-generation setup.
   - If it drains, re-enable texture object setup and TEV texture fetch next.

2. **If f360 is still the first hard stall**:
   - Keep the textured solid path, but test whether filling/flushing the entire
     `gx_tex_buf` inside the VI IRQ is racing or taking too long.
   - Try preparing the solid texture once at init or before frame 360 instead of
     doing a full 640x480/576 memset-style fill in the DI1 IRQ.

3. **If an earlier frame is the first stall**:
   - Investigate repeated per-vsync CP stop/start, FIFO pointer programming, and
     whether `CP_REG_CTRL=0` leaves the GP in a non-restartable state after some
     number of submits.

4. **If color cycling eventually confirms end-to-end pipeline works**:
   - Remove the color cycling test block in `gcn_gx_blit_fb_rgb565`.
   - Remove the 4-frame CP SR logging in `gx_submit_cmds`.
   - Remove the XFB readback `pr_info` calls.
   - Verify actual terminal content (fbcon text) renders correctly via GX.

5. **Remove the SW transcode safety net** once GX output is visually confirmed correct.
   In `gcnfb.c` RGB565 path, remove the `vi_transcode_RGB565` call.

6. **Register a PE FINISH ISR** (future improvement): replace `udelay(2000)` with an interrupt-driven wait on the PE FINISH signal, freeing ~2ms of CPU time per frame.

## Known pitfalls

- **Do not interpret a frozen display as a CPU crash** — the screen can be static while the CPU boots normally.  Check keyboard/shell response before assuming a hard hang.
- Do not remove `flush_dcache_range()` for the FIFO or texture buffer — GP DMA reads physical memory, not CPU cache.
- Do not revert CP accessors to `ioread16/iowrite16` — must be `in_be16`/`out_be16`.
- Do not leave `CP_CR_LINKEN` enabled between frames.
- `GFP_DMA` does not guarantee MEM1 on this platform (MEM1+MEM2 coalesced); use `/memreserve/` + `__va(phys)` for all GPU-visible buffers.
- BP `0x4D` (dispCopyDst) is in **32-byte** units — `(width * 2) >> 5`, not `>> 4`.
- PE_CTRL_STAT is at byte offset 0x00 from PE base (`pe_regs[0]`), not 0x02 (`pe_regs[1]`).
- PE FINISH (BP 0x65) does not set a polling-visible bit; it fires a CPU interrupt.  Use `udelay(2000)` as the sync fence.
- CP FIFO and texture buffer addresses: the hardware drops bit 23 of physical addresses — any buffer at a MEM2 address will silently read from the wrong location.
- `COPY_CTRL_CLEAR` (BP 0x52 bit 11) clears the EFB **after** the copy, not before.
- **Three BP registers must be set explicitly — all are pixel-discard traps at hardware reset:**
  - BP 0x40 (ZMODE) reset = 0 → Z-compare function defaults may reject all pixels.
  - BP 0x41 (BLENDMODE) reset = 0 → `colorupdate=0` → PE discards all pixels silently.
  - BP 0xF3 (ALPHA_COMPARE) reset = 0 → `comp0=NEVER` → alpha test rejects all pixels.
  Any one of these being wrong will produce a completely black EFB with no visible error.
- `pos=320` is not a reliable change indicator for small FIFO additions: due to 32-byte alignment padding, adding 5 or 10 bytes of BP commands can leave `pos` unchanged at 320.
