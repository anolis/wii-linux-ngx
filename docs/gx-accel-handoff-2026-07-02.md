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
f08056d559ff gcn-gx: test XF=1+0x200 with degenerate tex=(0,0) to isolate LOD stall
6cf3192219c8 gcn-gx: test GENMODE=1 + XF=0 + VCD TEX0=DIRECT to isolate stall source
630232fafe12 gcn-gx: test GENMODE=1 with XF=0 and pos-only to isolate GENMODE effect
fd93b5112d1b gcn-gx: fix rasterizer stall — normalize texcoords to [0,1] in fullscreen quad
aef34ffc988f gcn-gx: strip texcoord from GENMODE+VCD, use pos-only quad to isolate TEV
d2aabae562d7 gcn-gx: set XF 0x103F=0 (disable texgen output) to isolate stall source
```

**Current deployed image**: `f08056d` — XF=1 + 0x200 + all quad texcoords=(0,0). **Awaiting boot result.**

## Current gcn_gx_blit_fb_rgb565 pipeline (diagnostic)

```c
gx_setup_texcoord_parse_state(width, height);    // XF=1 enabled, 0x200, texenable=0
gx_draw_fullscreen_quad(width, height);           // DIAGNOSTIC: all texcoords=(0,0)
gcn_gx_copy_efb_to_xfb(xfb_phys, width, height); // BP 0x65 draw-done fence included
gx_submit_cmds();                                  // flush dcache, enable GP, wait drain
```

No tiling, no texture bind. EFB content = CC_ZERO (zero luma). Screen goes black.
`gcnfb.c` RGB565 path: **GX-exclusive** (`gx_accel_ready=true`).

---

## Stall isolation table (all results from hardware)

| GENMODE | XF 0x103F | VCD TEX0 | Vertex texcoords | SR result | Notes |
|---------|-----------|----------|-----------------|-----------|-------|
| 0 | 0 | none | pos-only | **✓ 000c** | pos-only, all frames clean through f360 |
| 1 | 0 | none | pos-only | **✓ 000c** | GENMODE=1 alone does not stall |
| 1 | 0 | DIRECT | [0,1] | **✓ 000c** | VCD TEX0 through CP/XF without XF output → no stall |
| 1 | 1 | DIRECT | [0,576] (pixel) | ✗ 0004 at f2 | original stall |
| 1 | 1 | DIRECT | [0,1] (norm) + 0x201 | ✗ 0004 at f2 | |
| 1 | 1 | DIRECT | [0,1] (norm) + 0x200 | ✗ 0004 at f2 | projection bit irrelevant |
| 1 | 1 | DIRECT | all=(0,0) + 0x200 | **PENDING** | current build |

**Confirmed root of stall**: XF 0x103F ≥ 1 (texgen output enabled to rasterizer).
With XF=0, all modes clean. With XF=1, stall always appears at frame 2 regardless of texcoord values.

---

## The frame-2 stall: what is known

### Symptom

- f0 post: SR=000c, xfb=00800080 (CC_ZERO rendered, our output overwrites EFB)
- f1 post: SR=000c, xfb=00800080 (same)
- f2 post: SR=0004, stall. xfb unchanged from f1 content
- f3 pre:  SR=0000 (CP never starts — cascaded failure from f2 stall)

With XF=0 all frames are clean including f360 (the last logged frame, ~6 seconds in).

### What the stall is NOT

- **Not a missing draw-done fence (BP 0x65)**: `gcn_gx_copy_efb_to_xfb` queues BP 0x65 after every copy. Stall persists.
- **Not XF perspective divide**: changing XF 0x1040 from 0x201 (bit0=1) to 0x200 (bit0=0) had no effect. Both stall identically at f2.
- **Not texcoord magnitude**: pixel-space [0,576] and normalized [0,1] both stall at f2.
- **Not VCD TEX0 data flowing through CP/XF**: with XF=0, VCD TEX0=DIRECT + [0,1] texcoords work fine (confirmed SR=000c all frames).
- **Not GENMODE=1 alone**: GENMODE=1 + XF=0 works.

### Leading hypothesis (not yet confirmed)

The stall is in the rasterizer's **texcoord interpolation / LOD derivative unit (SU/TSTT)**.

With XF=1, the rasterizer receives texcoords from XF and must compute LOD derivatives for each span. suSsize=575 and suTsize=431 have been set. But something in this computation either:

(a) Has a numerical issue that manifests after 2 frames of accumulation, OR  
(b) Stalls on ANY non-zero texcoord gradient via some hardware bug.

Current test (f08056d): all 4 vertex texcoords = (0,0), giving dS/dx = dS/dy = 0 everywhere. LOD = log₂(0) = −∞ → clamped to LOD=0. No interpolation step-error possible.

- **If this boots SR=000c**: the stall is in LOD derivative computation with non-zero gradients. Next step: restore normalized texcoords but try suSsize=0 (1×1 scale).
- **If still SR=0004 at f2**: the stall is upstream in XF output path or rasterizer setup, independent of texcoord values.

### Secondary hypothesis (if degenerate texcoords don't fix it)

The stall may be caused by a missing XF register initialization that mini left in a specific state:
- XF 0x1009 (XF_SETNUMCHAN / numcolchans in XF) — not written by our driver; mini may have left numchans=1
- Some XF color channel register causing color output that backs up the rasterizer after 2 frames

---

## Session history (2026-07-03, picks up from previous handoff)

### Previous state at session start

The previous handoff left BP 0x65 draw-done hypothesis as "pending boot test."

### Build sequence this session

#### Confirmed: GENMODE=1 + XF=0 + VCD TEX0=DIRECT → SR=000c

Commit `6cf3192219c8`. dmesg:
```
f0 post: SR=000c  xfb0=178d2e76 (mini EFB content — our render skipped pixels with XF=0)
f1 post: SR=000c  xfb0=127f107f (≈video black — something changed between f0 and f1)
f2 post: SR=000c  xfb0=127f107f
f360 post: SR=000c  xfb0=127f107f  ← all frames clean through 360
```
**Key fact**: With XF=0, the rasterizer does not receive texcoords. The render for f0
apparently skips pixel output entirely (EFB stays at mini's content), then f1 and beyond
write near-video-black. All frames clean through f360.

#### Test: XF=1 + 0x200 + normalized [0,1] texcoords → SR=0004 at f2

Commit not included; in-tree during session. dmesg:
```
f0 post: SR=000c  xfb0=00800080 (our CC_ZERO, Y=0)
f1 post: SR=000c  xfb0=00800080
f2 post: SR=0004  ← stall
f3 pre:  SR=0000  ← cascaded failure
```
With XF=1, f0 and f1 DO render pixels (xfb=00800080 = CC_ZERO output confirmed). Stall is at f2.

With previous 0x201 value the outcome was identical. The projection/bit0 change had no effect.

#### Test: XF=1 + 0x200 + all texcoords=(0,0) → PENDING

Commit `f08056d559ff`. Deployed. Result not yet read.

---

## XF register bit layouts (hardware-verified)

### XF 0x1040 — TexMtxInfo for texcoord gen 0

Actual hardware bit layout (from Dolphin XFMemory.h / YAGCD):

```
bit[0]    = projection (0=ST output, no divide; 1=STQ output, perspective divide)
bits[3:1] = inputform + texgentype (0 = regular matrix multiply, AB11 input form)
bits[11:7] = sourcerow
  0 = GX_TG_POS
  1 = GX_TG_NRM
  4 = GX_TG_TEX0   ← correct for 2D blit
  5 = GX_TG_TEX1
```

Encoding formula: `(sourcerow << 7) | projection_and_type_bits`

| Value | Meaning |
|-------|---------|
| 0x200 | src=TEX0, projection=0 (ST, no divide), type=regular — **currently deployed** |
| 0x201 | src=TEX0, projection=1 (STQ, with divide) — **causes stall with 2×4 matrix** |
| 0x280 | src=TEX1, type bits=0 — **old wrong value, never use** |

**WARNING**: 0x201 with a 2×4 TEXMTX0 (only 2 rows) causes the XF to request a Q
component from an uninitialized 3rd row → Q≈0/NaN → rasterizer perspective divide by
zero → SR=0x0004 stall. Always use 0x200 with GX_MTX2x4.

However, this was NOT the root cause of the frame-2 stall (changing to 0x200 produced
identical stall behavior — the stall is elsewhere).

### XF 0x103F — numTexCoordGens

| Value | Effect |
|-------|--------|
| 0 | No texcoord output from XF — rasterizer receives nothing. CONFIRMED SAFE. |
| 1 | Texcoord 0 output enabled — rasterizer receives texcoords from XF. CAUSES F2 STALL. |

### XF 0x0078–0x007F — TEXMTX0

Hardware reset value undefined. Must be initialized before enabling texgen (XF 0x103F≥1):

```c
/* 2×4 identity: maps (s,t,1,0) → (s,t) unchanged */
gx_load_xf_regs_n(0x0078, 8);
wg_f32_bits(F32_ONE);  wg_f32_bits(F32_ZERO); wg_f32_bits(F32_ZERO); wg_f32_bits(F32_ZERO);
wg_f32_bits(F32_ZERO); wg_f32_bits(F32_ONE);  wg_f32_bits(F32_ZERO); wg_f32_bits(F32_ZERO);
```

### XF 0x1050 — post-texcoord gen

Set to 0x3F (disable all post-transforms / GX_DTTIDENTITY).

---

## BP register quick reference

| BP addr | Name | Value used | Notes |
|---------|------|------------|-------|
| 0x40 | ZMODE | 0x40000000 | Z disabled |
| 0x41 | BLENDMODE | 0x41000018 | colorupdate=1, alphaupdate=1, no blend |
| 0xF3 | ALPHA_COMPARE | 0xF33F0000 | comp0=ALWAYS, comp1=ALWAYS |
| 0x00 | GENMODE | 0x00000001 | 1 texgen, 1 TEV stage, 0 colchans |
| 0x20 | SCISSOR_TL | computed | (342,342) in GX internal coords |
| 0x21 | SCISSOR_BR | computed | (342+width, 342+height) |
| 0x30 | suSsize | `0x30\|(width-1)` | rasterizer texcoord S scale (width-1) |
| 0x31 | suTsize | `0x31\|(height-1)` | rasterizer texcoord T scale (height-1) |
| 0x25 | TEV_ORDER stage0 | 0x25000380 | texenable=0, raschan=7=COLOR_NULL |
| 0xC0 | TEV COLOR stage0 | 0xC008FFFF | all CC_ZERO = output 0 (diagnostic path) |
| 0xC1 | TEV ALPHA stage0 | 0xC108FFF0 | all CA_ZERO = output 0 (diagnostic path) |
| 0x49 | DISP_COPY_TL | 0x49000000 | copy source top-left (0,0) |
| 0x4a | DISP_COPY_WH | computed | copy source width/height |
| 0x4d | DISP_COPY_DST | `(w*2)>>5` | **32-byte** units, not 16 |
| 0x4b | DISP_COPY_ADDR | `phys>>5` | XFB physical addr right-shifted 5 |
| 0x52 | DISP_COPY_CTRL | execute | no CLEAR flag |
| 0x65 | PE_DONE | 0x65000002 | draw-done fence; queued after every copy |

---

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
| `0x0000` | Pipeline active, FIFO has data (or early transitional / post-crash) |

---

## Suggested next steps

### Step 1: Read f08056d boot result and branch accordingly

Boot `f08056d` (XF=1 + 0x200 + all texcoords=(0,0)).

**Case A — SR=000c all frames**: zero-gradient texcoords eliminate the stall. LOD derivative computation with non-zero gradients is the root cause. Next: restore [0,1] texcoords but set suSsize=0 (suTsize=0). If that also fixes it, the suSsize scale causes a bad LOD. If not, something about the gradient itself (e.g. fixed-point precision accumulation) is the issue.

**Case B — SR=0004 at f2 still**: zero-gradient doesn't help. The stall is not in LOD computation. It's in XF output or rasterizer setup for texcoords. Next: try XF=1 + src=POS (sourcerow=0 instead of TEX0=4) + VCD TEX0=NONE + pos-only vertices. Tests if TEX0 vertex data is needed to trigger the stall, or if XF generating any texcoord output is sufficient.

### Step 2: Expand to full texcoord path

Once SR=000c with XF=1 + any working texcoord config:

1. Restore [0,1] texcoords in `gx_draw_fullscreen_quad`.
2. Switch `gx_setup_texcoord_parse_state` to use texenable=1 (BP 0x25 → 0x250003C0) and CC_TEXC in TEV combiners (0xC008FFF8 / 0xC108FFC0).
3. Call `gx_setup_texture_rgb565(gx_tex_buf, width, height)` before the draw to bind the texture.
4. Add solid-color fill at frame 360 to visually confirm texture fetch.

### Step 3: Enable tiling

Once texture fetch works:
1. Add `gx_tile_rgb565(vfb, gx_tex_buf, width, height)` at top of `gcn_gx_blit_fb_rgb565`.
2. Remove color-cycle override.
3. Expected: terminal console text visible on screen via GX path.

### Step 4: Cleanup

Once terminal renders correctly via GX:
- Remove all `do_log` / `frame_log` / `pr_info` diagnostic logging from `gx_submit_cmds`.
- Remove stall_bytes dump code, XFB readback logging, `gx_log_next_submit` mechanism.
- Remove `gx_draw_pos_quad` and `gx_setup_texcoord_parse_state` (diagnostic-only code).
- Remove `gx_setup_2d_state` if it gets merged back with the main path.
- Consider registering a PE FINISH ISR instead of polling / udelay.

---

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
the same SD card (`/dev/sdd` on the host, automounts to `/media/anolis/BOOTWII` and
`/media/anolis/WII-LINUX-NGX1`).

## Key files

```text
drivers/video/fbdev/gcn-gx.c   ← all GX register programming
drivers/video/fbdev/gcn-gx.h   ← register addresses and bit definitions
drivers/video/fbdev/gcnfb.c    ← vi_dispatch_vtrace calls gcn_gx_blit_fb_rgb565
arch/powerpc/boot/dts/wii.dts  ← /memreserve/ for FIFO, tex buf, XFB
```

---

## Critical discoveries (chronological, sessions combined)

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

### 14. XF 0x1040: bit[0] is projection, NOT the MTX2x4/MTX3x4 type flag

Despite earlier documentation saying `(srcrow<<7)|type` with type=1=MTX2x4:

- bit[0] = **projection** flag (0=ST output / no perspective divide, 1=STQ / divide by Q)
- Using 0x201 (bit0=1) with a 2×4 TEXMTX0 requests STQ output but only 2 matrix rows
  exist → Q comes from garbage 3rd row → divide by zero → stall.
- Correct value for 2D blit (TEX0 source, no perspective): **0x200**

However, 0x201 vs 0x200 did NOT change the frame-2 stall behavior — the root cause
is elsewhere. The 0x200 fix is correct but insufficient alone.

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

### 16. XF 0x103F=0 ("no texgen output") vs GENMODE numtexcoordgens=1 mismatch is safe

With XF 0x103F=0 and GENMODE=1 (rasterizer expects 1 texcoord), the rasterizer handles
the missing texcoord gracefully — it does NOT stall.  All frames run clean through f360.

With XF=0, f0's render apparently does not output pixels to EFB (xfb stays at mini's
content). Starting from f1, pixels are written (xfb=127f107f ≈ video black from CC_ZERO).
Exact reason for f0 skip with XF=0 is unknown but irrelevant for the production path.

### 17. XF texgen output (XF 0x103F=1) causes frame-2 stall — root cause still open

Enabling XF texgen output (XF 0x103F=1) causes SR=0x0004 at exactly frame 2, regardless
of texcoord values (pixel-space, normalized [0,1], or uniform 0x200 vs 0x201).

Pattern:
- f0: SR=000c, pixels rendered to EFB (xfb=00800080)
- f1: SR=000c, pixels rendered to EFB
- f2: SR=0004, pipeline never idles — persistent stall
- f3+: cascaded failure (SR=0000 at pre-log)

The "exactly frame 2" pattern suggests a hardware state that accumulates over 2 renders.
Current investigation: degenerate texcoords (all=(0,0)) to test if texcoord gradient
computation is the trigger.

---

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
- XF 0x1040 bit[0]=1 (projection=STQ) requires 3 valid matrix rows; with only 2 rows
  loaded the Q component is garbage → perspective divide stall.  Use 0x200 (bit0=0).
- raschan in BP 0x25 bits[9:7] must be 7 (GX_COLOR_NULL) when numcolchans=0.
- `pos=320/352` may not change when adding small BP commands due to 32-byte alignment padding.
- suSsize (BP 0x30) and suTsize (BP 0x31) must be set even when texenable=0 — the SU
  uses them for LOD gradient computation regardless of whether the TMU fetches the texture.
