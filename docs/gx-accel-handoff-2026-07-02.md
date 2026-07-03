# Wii GX acceleration handoff - 2026-07-03

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

```text
313e82b6aaea gcn-gx: re-enable EFB copy to test BP 0x65 draw-done as pipeline fence
36b9d314a04f gcn-gx: fix TEV order raschan=0 stall when numcolchans=0
06f493af377f gcn-gx: set suSsize/suTsize in texcoord-parse diagnostic path
13e21f287714 gcn-gx: fix XF 0x1040 texgen type/source and init TEXMTX0
afc3b0327ee2 gcn-gx: wait for GP command-idle after each submit
6703dabfde10 gcn-gx: isolate TEX0 parsing from texture fetch
```

**Current deployed image**: built from `313e82b6`, deployed to `/media/anolis/BOOTWII/gumboot/zImage.ngx`.

**SD card not yet booted with this build** — awaiting Wii boot.

## Current gcn_gx_blit_fb_rgb565 pipeline (diagnostic)

```c
gx_setup_texcoord_parse_state(width, height);   // state: 0 tex fetches, 1 texgen
gx_draw_fullscreen_quad(width, height);          // quad: pos + TEX0 direct coords
gcn_gx_copy_efb_to_xfb(xfb_phys, width, height); // adds BP 0x65 draw-done fence
gx_submit_cmds();                                 // flush dcache, enable GP, wait drain
```

No tiling (vfb → gx_tex_buf), no texture bind. EFB content is zero/black.
XFB will be overwritten with black YUYV. Screen goes black while this diagnostic runs.

`gcnfb.c` RGB565 path: **GX-exclusive** (SW transcode runs only when `gx_accel_ready=false`).

## Current working hypothesis (not yet boot-tested)

**Root cause of SR=0x0004 after-FIFO-drain stall:** without BP 0x65 queued after the draw,
the PE never fires PEFinish, and CmdIdle (SR bit 3) stays 0 indefinitely.  GX hardware
requires an explicit draw-done fence for the pipeline to signal completion.

`gcn_gx_copy_efb_to_xfb()` queues BP 0x65 at the end of each FIFO batch.  With it present:

- CP processes state + quad vertices
- Rasterizer renders 248832 pixels to EFB
- Copy commands execute, EFB→XFB copy runs
- BP 0x65 fires PEFinish → CmdIdle goes to 1 → SR=000c

**Expected dmesg after this boot:**

```text
gcn-gx: f0 post:  SR=000c RDoff=0xxx WToff=0xxx
gcn-gx: f1 post:  SR=000c ...
gcn-gx: f2 post:  SR=000c ...   ← was SR=0004, this is the key frame
gcn-gx: f3 post:  SR=000c ...
(no "pipeline did not go idle" warning)
(no "first_stall" warning)
```

If instead we still see SR=0x0004 at f2 with the copy present, the CmdIdle stall is
not caused by a missing draw-done fence and a deeper investigation of the EFB write
pipeline is needed (EFB format, PE register state inherited from mini, etc).

## Session 2026-07-03 diagnostic history

### What was deployed before this session

Build `36b9d314` (raschan fix). Its dmesg:

```text
gcn-gx: f0 post:  SR=000c RDoff=0160 WToff=0160
gcn-gx: f1 post:  SR=000c RDoff=0160 WToff=0160
gcn-gx: f2 post:  SR=0004 RDoff=0160 WToff=0160
gcn-gx: f3 post:  SR=0004 RDoff=0160 WToff=0160
gcn-gx: pipeline did not go idle after submit (SR=0x0004)
gcn-gx: first_slow f14 SR=0000 RDoff=0120 WToff=0160 PIoff=0160 pos=352
gcn-gx: stall_bytes @0110: 00 80 00 04 00 00 00 00 00 00 00 00 00 00 00 00
gcn-gx: stall_bytes @0120: 00 00 00 00 44 10 00 00 00 00 00 00 44 10 00 00
```

### Sequence of TEX0 diagnostic commits this session

Each fix was deployed and tested on hardware.  None resolved the frame-2 SR=0x0004 stall.

#### `afc3b0327ee2` — GP idle-wait at end of gx_submit_cmds

**Problem:** frame 16 showed `SR=0x0000` at pre-log and `RDoff=0x0000` (CP never started
reading).  The downstream pipeline was still active from the previous frame when the next
frame reprogrammed CP BASE/END/RD/WT, putting the CP into a confused state.

**Fix:** after `cp_write(CP_REG_CTRL, 0)` at end of `gx_submit_cmds`, poll SR bit 3
(CmdIdle) with 2000 × 10 µs = 20 ms timeout.

**Result:** `RDoff=0x0000` failure eliminated.  Stall moved to f14 at `RDoff=0x0120`.

#### `13e21f287714` — Fix XF 0x1040 texgen type/source and init TEXMTX0

**Problem 1:** XF register 0x1040 was `0x280 = (5<<7)|0`:
- bit 0 = type 0 = MTX3x4 (wrong, should be MTX2x4 = 1)
- bits 14:7 = srcrow 5 = GX_TG_TEX1 (wrong, should be TEX0 = srcrow 4)

MTX3x4 with an uninitialized TEXMTX0 causes a perspective divide by q≈0/NaN in the
rasterizer, producing a pipeline stall.

**Fix:** `gx_load_xf_reg(0x1040, 0x201)` where `0x201 = (4<<7)|1` = MTX2x4 + srcrow=TEX0.

**Problem 2:** TEXMTX0 (XF 0x0078–0x007F) has an undefined hardware reset value.  With
MTX3x4 mode active, any garbage in those registers produces a bad perspective divide.

**Fix:** load identity 2×4 matrix into XF 0x0078–0x007F after loading PNMTX0:

```c
gx_load_xf_regs_n(0x0078, 8);
wg_f32_bits(F32_ONE);  wg_f32_bits(F32_ZERO);
wg_f32_bits(F32_ZERO); wg_f32_bits(F32_ZERO);
wg_f32_bits(F32_ZERO); wg_f32_bits(F32_ONE);
wg_f32_bits(F32_ZERO); wg_f32_bits(F32_ZERO);
```

**Result:** `RDoff=0x0000` stall eliminated.  Stall shifted to f14 at `RDoff=0x0120`.
Frame-2 SR=0x0004 unchanged.

#### `06f493af377f` — Set suSsize/suTsize in texcoord-parse path

**Problem:** BP 0x30 (suSsize) and 0x31 (suTsize) were missing from
`gx_setup_texcoord_parse_state`.  The rasterizer uses these for per-pixel texcoord
stepping and LOD derivative computation.

**Fix:** added:

```c
gx_load_bp_reg(0x30000000 | (u32)(width  - 1));
gx_load_bp_reg(0x31000000 | (u32)(height - 1));
```

**Result:** NO CHANGE.  Frame-2 SR=0x0004 unchanged.

#### `36b9d314a04f` — Fix TEV order raschan from 0 to 7

**Problem:** BP 0x25 had `raschan=0` (GX_COLOR0A0).  But `genMode` numcolchans=0 means
colour channel 0 does not exist.  TEV waits for a colour token from a channel that never
sends one → permanent stall.

**Fix:** `gx_load_bp_reg(0x25000380)` where bits[9:7] = 7 = GX_COLOR_NULL (no colour channel).

**Result:** NO CHANGE.  Frame-2 SR=0x0004 unchanged.

### Analysis of the frame-2 stall pattern

The FIFO at `RDoff=0x0120` stall happens because:

1. Frame 2 FIFO drains (RDoff=WToff=0x0160).
2. Frame 2 rasterizer runs 248832 pixels but CmdIdle never asserts (SR=0x0004).
3. `gx_submit_cmds` times out (20ms), calls `cp_write(CP_REG_CTRL, 0)`, warns.
4. Frame 3 submit starts.  `cp_write(GPRESET|LINKEN)` fires while frame-2 pipeline is
   still running.  Frame-3 vertex data enters a rasterizer that is backed up.
5. By frame 14, 12 undrained frames have filled the rasterizer command queue.
6. Frame 14 CP stalls at `RDoff=0x0120` (GX_QUADS opcode + vertex 0 consumed, vertex 1
   cannot enter because the rasterizer backend is full).

The position-only build (no TEX0, no copy) drained all frames with SR=000c.  The TEX0
parse build (no copy) stalled at frame 2.  Minimum state difference: one texgen enabled.

**Stall bytes decode:**

```text
@0110: 00  80 00 04  00 00 00 00  00 00 00 00  00 00 00 00
         ↑ GX_QUADS ↑ count=4    ↑ vtx0 pos (XY zeros)

@0120: 00 00 00 00  44 10 00 00  00 00 00 00  44 10 00 00
        ↑ vtx0 texS  ↑ vtx1 posX  (576.0=0x44100000)
```

CP consumed vtx0 (all zeros) but stalled at vtx1 boundary.  The command stream is
syntactically correct.  The failure is rasterizer back-pressure, not a malformed FIFO.

## XF register encodings (key discoveries)

### XF 0x1040 — texCoordGen[0]

Encoding: `(srcrow << 7) | type`

| type | value | meaning |
|------|-------|---------|
| 0    | MTX3x4 | 3×4 matrix (requires W, causes perspective divide) |
| 1    | MTX2x4 | 2×4 matrix (no W, no perspective divide) |

| srcrow | value | meaning |
|--------|-------|---------|
| 4      | GX_TG_TEX0 | use input TEX0 UV |
| 5      | GX_TG_TEX1 | use input TEX1 UV |

Correct value for 2D blit: `0x201 = (4<<7)|1` (TEX0 source, MTX2x4 type).
**Never use 0x280** (was MTX3x4+TEX1).

### XF 0x0078–0x007F — TEXMTX0

Hardware reset value is **undefined**.  Must be initialized to identity before use:

```c
/* 2×4 identity: row0=(1,0,0,0), row1=(0,1,0,0) */
gx_load_xf_regs_n(0x0078, 8);
wg_f32_bits(F32_ONE);  wg_f32_bits(F32_ZERO); wg_f32_bits(F32_ZERO); wg_f32_bits(F32_ZERO);
wg_f32_bits(F32_ZERO); wg_f32_bits(F32_ONE);  wg_f32_bits(F32_ZERO); wg_f32_bits(F32_ZERO);
```

### XF 0x103F — numTexCoordGens

Set to 1 for a single texgen.  0 disables all texgen (position-only mode).

### XF 0x1050 — post-texcoord generation

Set to 0x3F (disable all post-transforms).

## BP register quick reference

| BP addr | Name | Value used | Notes |
|---------|------|------------|-------|
| 0x40 | ZMODE | 0x40000000 | Z disabled |
| 0x41 | BLENDMODE | 0x41000018 | colorupdate=1, alphaupdate=1, no blend |
| 0xF3 | ALPHA_COMPARE | 0xF33F0000 | comp0=ALWAYS, comp1=ALWAYS |
| 0x00 | GENMODE | 0x00000001 | 1 texgen, 1 TEV stage |
| 0x20 | SCISSOR_TL | computed | (342,342) in GX internal coords |
| 0x21 | SCISSOR_BR | computed | (342+width, 342+height) |
| 0x30 | suSsize | `0x30\|width-1` | rasterizer texcoord step width |
| 0x31 | suTsize | `0x31\|height-1` | rasterizer texcoord step height |
| 0x25 | TEV_ORDER stage0 | 0x25000380 | texenable=0, raschan=7=COLOR_NULL |
| 0xC0 | TEV COLOR stage0 | 0xC008FFFF | all CC_ZERO = output 0 |
| 0xC1 | TEV ALPHA stage0 | 0xC108FFC0 | all CA_ZERO = output 0 |
| 0x49 | DISP_COPY_TL | 0x49000000 | copy source top-left (0,0) |
| 0x4a | DISP_COPY_WH | computed | copy source width/height |
| 0x4d | DISP_COPY_DST | `(w*2)>>5` | **32-byte** units, not 16 |
| 0x4b | DISP_COPY_ADDR | `phys>>5` | XFB physical addr right-shifted 5 |
| 0x52 | DISP_COPY_CTRL | execute | no CLEAR flag |
| 0x65 | PE_DONE | 0x65000002 | draw-done fence; must follow every draw |

## CP status register (SR) field meanings

| Bit | Name | Meaning |
|-----|------|---------|
| 2   | ReadIdle | 1 = CP FIFO read engine is idle (FIFO drained) |
| 3   | CmdIdle  | 1 = downstream pipeline (XF/rasterizer/TEV/PE) is fully idle |

| SR value | State |
|----------|-------|
| `0x000c` | FIFO drained AND pipeline done — **target state after each frame** |
| `0x0004` | FIFO drained but pipeline still running — stall if persistent |
| `0x0008` | Pipeline done but CP FIFO not flagged empty (unusual) |
| `0x0000` | Pipeline active, FIFO has data (or very early transitional) |

After `cp_write(CP_REG_CTRL, 0)`, expect SR→0x0008 (CmdIdle=1, ReadIdle=0) when the
pipeline drains naturally.  SR=0x000c is the steady state while GP is enabled and the
FIFO is empty with all pipeline work done.

## Onboard diagnostic logging

**The kernel is booting with `init=/init-diag.sh` in the bootargs.**  This script:

1. Mounts `/proc` and `/sys`.
2. Remounts rootfs read-write.
3. Writes `/dmesg.txt` start/uptime markers.
4. Sleeps 20 seconds (captures multiple frame 360 windows).
5. Appends `dmesg`, `/proc/bus/input/devices`, and `/dev/input/` output.
6. Blinks the Wii slot LED 10 times when logging is complete.
7. Calls `exec /bin/sh -l` on `/dev/console`.

Script location on rootfs: `/media/anolis/WII-LINUX-NGX1/init-diag.sh`

To retrieve results: boot Wii, wait ~25 seconds for LED blink, power off, move SD card
to host, read `/media/anolis/WII-LINUX-NGX1/dmesg.txt`.

## Build and deploy

```sh
make ARCH=powerpc CROSS_COMPILE=powerpc-linux-gnu- -j$(nproc)
cp arch/powerpc/boot/dtbImage.wii /media/anolis/BOOTWII/gumboot/zImage.ngx
sync
```

BOOTWII (FAT, ~40 MB) and WII-LINUX-NGX1 (ext2, ~338 MB) are separate partitions on
the same SD card (sdd on the host).

## Key files

```text
drivers/video/fbdev/gcn-gx.c   ← all GX register programming
drivers/video/fbdev/gcn-gx.h   ← register addresses and bit definitions
drivers/video/fbdev/gcnfb.c    ← vi_dispatch_vtrace calls gcn_gx_blit_fb_rgb565
arch/powerpc/boot/dts/wii.dts  ← /memreserve/ for FIFO, tex buf, XFB
```

## Suggested next steps

### Step 1: Boot build `313e82b6` and check dmesg

**Expected if hypothesis is correct:**
- f0–f3 all show `SR=000c`
- No "pipeline did not go idle" warning
- No "first_stall" warning
- Screen goes black (EFB is zero, XFB copy writes YUYV black)

**If still SR=0x0004 at f2 with copy present:**
- The CmdIdle stall is not a missing draw-done fence.
- Next suspect: EFB pixel format.  mini leaves GX with unknown EFB format.
  Try setting XF 0x100F (EFB format) explicitly: `gx_load_xf_reg(0x100F, 0)`
  (GX_PF_RGB8_Z24).
- Or: reduce scissor to 4×4 pixels to test if stall is proportional to pixel count.
  If 4×4 doesn't stall but full-screen does, the issue is per-pixel processing time
  (possibly EFB tile cache eviction or PE write bandwidth).

### Step 2: Re-enable texture fetch

Once f0–f3 all drain with SR=000c:

1. Switch `gcn_gx_blit_fb_rgb565` from `gx_setup_texcoord_parse_state` back to
   `gx_setup_2d_state` + `gx_setup_texture_rgb565` (binds `gx_tex_buf` as TEX0).
2. Keep `gcn_gx_copy_efb_to_xfb` and `gx_submit_cmds`.
3. Fill `gx_tex_buf` with solid red at frame 360 (as in previous color-cycle diagnostic).
4. Expected: frame-360 XFB becomes non-black (solid red or YUYV version of red).

### Step 3: Enable tiling

If Step 2 works (XFB shows solid red at f360):
1. Add `gx_tile_rgb565(vfb, gx_tex_buf, width, height)` at the top of
   `gcn_gx_blit_fb_rgb565`.
2. Remove color-cycle override.
3. Expected: terminal console text visible on screen via GX path.

### Step 4: Cleanup

Once terminal content renders correctly via GX:
- Remove all `do_log` / `frame_log` / `pr_info` diagnostic logging from `gx_submit_cmds`.
- Remove stall_bytes dump code.
- Remove XFB readback logging (`xfb0`, `xfb1`).
- Remove `gx_log_next_submit` mechanism.
- Remove `gx_draw_pos_quad` (unused diagnostic function).
- Consider registering a PE FINISH ISR instead of polling / udelay.

## Critical discoveries (chronological)

### 1. The original "freeze after H" was usually a display freeze

When `gx_accel_ready=true`, the RGB565 path in `gcnfb.c` takes the GX blit path.  If
that blit does not update XFB, VI keeps scanning the last software-transcoded frame.
The CPU can still be alive and boot into a shell.

### 2. The GP was not seeing command bytes until cache flush was added

`gx_submit_cmds()` must flush `gx_fifo_buf` before enabling GP reads.

```c
flush_dcache_range((unsigned long)gx_fifo_buf,
                   (unsigned long)gx_fifo_buf + fifo_pos);
```

### 3. PI FIFO control is required

```c
pi_write(PI_REG_FIFO_CTRL, PI_FIFO_CTRL_EN);
```

### 4. CP register access must be big-endian

Use `in_be16`/`out_be16`, not `ioread16`/`iowrite16`.

```c
static inline void cp_write(int reg, u16 val) { out_be16(cp_regs + reg, val); }
static inline u16  cp_read(int reg)            { return in_be16(cp_regs + reg); }
```

### 5. The command FIFO must live in MEM1 (bit-23 hardware limitation)

CP FIFO address registers drop bit 23 of the physical address.  Any buffer with bit 23
set is silently misaddressed.  Fix: use `/memreserve/ 0x01684000` in DTS.

Same constraint applies to `gx_tex_buf` — the texture fetch unit cannot address MEM2.
Fix: `/memreserve/ 0x01200000` in DTS.

### 6. FIFO drain polling is unreliable with LINKEN + PI_FIFO_CTRL_EN

After enabling the GP with `CP_CR_LINKEN | CP_CR_GPRESET` and writing `PI_FIFO_CTRL_EN`,
the PI can continue advancing `CP_WT` after GP drains user-submitted commands.  This makes
`RD == WT` polling loop spin forever.  Fix: fixed `udelay(10000)` then read RD/WT once.

### 7. EFB→XFB copy confirmed reaching the display (vertical-bars breakthrough)

EFB-clear + copy showed vertical bars across the screen.  Confirmed: copy IS executing,
VI IS scanning the XFB.  Bars were caused by the stride bug (§8).

### 8. EFB→XFB copy stride: 32-byte cache line units, not 16-byte

`BP_DISP_COPY_DST` stores stride in **32-byte** units:
```c
gx_load_bp_reg((BP_DISP_COPY_DST << 24) | ((width * 2) >> 5));
```

### 9. COPY_CTRL_CLEAR is clear-AFTER-copy, not clear-before

`COPY_CTRL_CLEAR` copies EFB contents first, then fills EFB with clear colour.  It does
not fill XFB with the clear colour.  Do not use it for colour fills.

### 10. Three BP registers are pixel-discard traps at hardware reset

All three must be set explicitly in any GX 2D init sequence:

| Register | Reset value | Trap |
|----------|-------------|------|
| BP 0x40 (ZMODE) | 0x00 | Z-compare may reject all pixels |
| BP 0x41 (BLENDMODE) | 0x00 | `colorupdate=0` → PE silently discards all pixels |
| BP 0xF3 (ALPHA_COMPARE) | 0x00 | `comp0=NEVER` → alpha test rejects all pixels |

Any one of these being wrong produces a completely black EFB with no error.

### 11. Stage-0 TEV order is BP 0x25, not BP 0x28

libogc `tevRasOrder[0]` = BP `0x25`.  BP `0x28` is a later stage.

### 12. EHCI spinlock BUG does not permanently kill DI1

At `t ≈ 0.5s` the EHCI USB 2.0 controller BUG holds a spinlock for ~2.5 seconds.  GX
blit counter stops during this window (DI1 IRQ blocked), then resumes.  DI1 fires at
~23-24 Hz (every-other-field VI behaviour, not 60 Hz).

### 13. raschan must be 7 (GX_COLOR_NULL) when numcolchans=0

BP 0x25 bits[9:7] = raschan.  Value 0 = GX_COLOR0A0 requires colour channel 0 to exist.
With genMode numcolchans=0, TEV waits for a colour token from a channel that never fires.
Set raschan=7 (GX_COLOR_NULL) when not using a colour channel.

### 14. XF 0x1040 encoding: (srcrow<<7)|type

Type 0 = MTX3x4, type 1 = MTX2x4.  For TEX0 with 2D blit: `(4<<7)|1 = 0x201`.
MTX3x4 with uninitialized TEXMTX0 causes a perspective divide by q=garbage → rasterizer
stall.  **TEXMTX0 (XF 0x0078–0x007F) must be explicitly initialized to identity.**

### 15. GP idle-wait required between frames

After `cp_write(CP_REG_CTRL, 0)`, poll SR bit 3 (CmdIdle) before returning from
`gx_submit_cmds`.  Without this, the next frame's FIFO register reprogramming races
the still-active downstream pipeline, causing SR=0x0000 at the next frame's pre-log
and `RDoff=0x0000` (CP never starts reading).

```c
int t = 2000;
while (t-- && !(cp_read(CP_REG_STATUS) & 0x0008))
    udelay(10);
```

### 16. BP 0x65 draw-done fence — hypothesis (not yet confirmed)

Without BP 0x65 queued after the draw, the PE never fires PEFinish, and CmdIdle (SR bit
3) may stay 0 indefinitely even after the FIFO drains.  The position-only build (no copy,
no BP 0x65) drained all frames.  The TEX0-parse build (no copy, no BP 0x65) stalled at
frame 2.  Re-enabling `gcn_gx_copy_efb_to_xfb()` (which includes BP 0x65) is the current
test.  **Boot result pending.**

## Known pitfalls

- **Do not interpret a frozen display as a CPU crash** — screen can be static while CPU
  boots normally.  Check keyboard/shell response before assuming a hard hang.
- Do not remove `flush_dcache_range()` for the FIFO or texture buffer.
- Do not revert CP accessors to `ioread16/iowrite16` — must be `in_be16`/`out_be16`.
- Do not leave `CP_CR_LINKEN` enabled between frames.
- `GFP_DMA` does not guarantee MEM1 on this platform; use `/memreserve/` + `__va(phys)`.
- BP `0x4D` (dispCopyDst) is in **32-byte** units — `(width * 2) >> 5`, not `>> 4`.
- PE_CTRL_STAT is at byte offset 0x00 from PE base, not 0x02.
- PE FINISH (BP 0x65) may not leave a persistent polling-visible bit; hardware fires a
  CPU interrupt.  Use `udelay` or poll CmdIdle rather than polling PE_CTRL_STAT bits.
- CP FIFO and texture buffer addresses: hardware drops bit 23 of physical addresses.
- `COPY_CTRL_CLEAR` (BP 0x52 bit 11) clears EFB **after** the copy, not before.
- XF texgen type 0 (MTX3x4) requires a valid W component; type 1 (MTX2x4) does not.
  Always use MTX2x4 for 2D blits.  Always init TEXMTX0 before enabling texgen.
- raschan in BP 0x25 bits[9:7] must be 7 (GX_COLOR_NULL) when numcolchans=0.
- `pos=320/352` may not change when adding small BP commands due to 32-byte alignment padding.
