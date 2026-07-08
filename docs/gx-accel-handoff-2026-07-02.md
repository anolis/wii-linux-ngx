# Wii GX acceleration handoff - 2026-07-03

This report summarises the current state of the direct GX framebuffer acceleration work in `wii-linux-ngx`.  It is updated in place each session.

## Current update - 2026-07-05

Current diagnostic focus: isolate GX display-copy semantics while keeping the VI
output visually stable with the CPU green fallback in `gcnfb.c`.

Fresh `dmesg.txt` from the previous deployed image showed:

```text
gcn-gx: f0 diag=no-submit xfb0=26835780 xfb1=10752387 xfbc=1a7d1093
gcnfb: f0 post-gx-pre-cpu-fill fb0=26835780 fb1=10752387 fbc=1a7d1093
gcnfb: f0 cpu-fill-green pattern=a52ba515
gcnfb: f0 post-cpu-fill fb0=a52ba515 fb1=a52ba515 fbc=a52ba515
gcn-gx: f1 diag=no-submit xfb0=a52ba515 xfb1=a52ba515 xfbc=a52ba515
```

Interpretation: the CPU-visible `fb_mem` mapping and VI scanout are good.  The
frame-0 GX copy was not green before the CPU fallback because `GX_CopyDisp` with
`clear=true` copies the old EFB contents into XFB first, then clears EFB after
the copy.  The old assumption that copy-clear immediately writes the clear color
to XFB was wrong.

Latest result from the frame-separated clear/copy test:

```text
gcn-gx: f0 diag=clear-f0-copy-f1 xfb0=178c2f76 xfb1=10743f83 xfbc=1a7d1088
gcnfb: f0 post-gx-pre-cpu-fill fb0=178c2f76 fb1=10743f83 fbc=1a7d1088
gcnfb: f0 cpu-fill-green skipped
gcn-gx: f1 diag=clear-f0-copy-f1 xfb0=90369122 xfb1=90369022 xfbc=72487238
gcnfb: f1 post-gx-pre-cpu-fill fb0=90369122 fb1=90369022 fbc=72487238
gcnfb: f1 cpu-fill-green pattern=a52ba515
```

Interpretation: GX submissions drained and frame 1's copy changed XFB data, but
it still did not produce the expected green `a52ba515`.  Later frames were green
only from the CPU fallback.  This means copy-clear is not a useful EFB seed for
the current diagnostic path.

Current test in `gcn_gx_blit_fb_rgb565()`:

Latest result from the no-channel constant-TEV primitive test:

```text
gcn-gx: f0 pre: SR=0008 RD=0000 WT=01a0 pos=416
gcn-gx: f0 post: SR=000c RDoff=01a0 WToff=01a0
gcn-gx: f0 diag=solid-primitive-copy xfb0=178c2f76 xfb1=10782377 xfbc=1a811087
gcnfb: f0 post-gx-pre-cpu-fill fb0=178c2f76 fb1=10782377 fbc=1a811087
gcnfb: f0 cpu-fill-green pattern=a52ba515
```

Interpretation: the FIFO drained, but constant-TEV output did not produce a
controlled uniform EFB/XFB result.  Later green frames were still only the CPU
fallback.

Current test in `gcn_gx_blit_fb_rgb565()`:

Latest result from the split direct draw/copy test:

```text
gcn-gx: f0 pre: SR=0008 RD=0000 WT=0180 pos=384
gcn-gx: f0 post: SR=000c RDoff=0180 WToff=0180
gcn-gx: f0 diag=draw-f0-copy-f1 xfb0=00800080 xfb1=00800080 xfbc=00800080
gcnfb: f0 post-gx-pre-cpu-fill fb0=00800080 fb1=00800080 fbc=00800080
gcnfb: f0 cpu-fill-green skipped
gcn-gx: f1 pre: SR=0008 RD=0000 WT=0020 pos=32
gcn-gx: f1 post: SR=000c RDoff=0020 WToff=0020
gcn-gx: f1 diag=draw-f0-copy-f1 xfb0=178c2f76 xfb1=10782377 xfbc=1a7e238f
gcnfb: f1 post-gx-pre-cpu-fill fb0=178c2f76 fb1=10782377 fbc=1a7e238f
gcnfb: f1 cpu-fill-green pattern=a52ba515
```

Interpretation: visually the screen was uniform green, but that came from the
CPU fallback after frame 1.  The decisive logs show frame 0 became uniform
`00800080` before software touched XFB, despite submitting only the primitive
draw and no explicit display copy.  Frame 1's explicit copy still did not
produce the requested green primitive output.  This is progress: direct
primitive state can correlate with a uniform black/zero output, but the colour
path is still wrong or the output being sampled is not the intended EFB colour.

Current test isolates output colour by sending direct vertex **white** while
preserving this split frame structure.  If frame 0 remains `00800080`, the
channel/TEV colour is being forced to zero.  If frame 0 or frame 1 becomes a
white-derived YUYV value, the direct vertex colour path can work and the
previous green case failed through colour-specific state, copy timing, or the
green fallback masking the result.

Previous test in `gcn_gx_blit_fb_rgb565()`:

Latest result from the same-frame direct-green draw+copy test:

```text
gcn-gx: f0 pre: SR=0008 RD=0000 WT=01a0 pos=416
gcn-gx: f0 post: SR=000c RDoff=01a0 WToff=01a0
gcn-gx: f0 diag=direct-green-copy xfb0=168d2e77 xfb1=10753e83 xfbc=1a7d1088
gcnfb: f0 post-gx-pre-cpu-fill fb0=168d2e77 fb1=10753e83 fbc=1a7d1088
gcnfb: f0 cpu-fill-green pattern=a52ba515
```

Interpretation: direct vertex colour did not produce controlled XFB output when
the primitive and copy were submitted in the same FIFO.

Current test in `gcn_gx_blit_fb_rgb565()`:

1. Frame 0: configure direct vertex colour: `genMode` one colour channel,
   TEV `d=RASC`, ras channel `GX_COLOR0A0`, `XF 0x1008/0x1009 = 1`,
   `VCD_LO=0x2200`, and `VAT0=0x40016008`.
2. Draw a full-screen direct RGBA8 green quad into EFB, but do not copy yet.
3. `gcnfb.c` samples `fb_mem`, then intentionally skips the CPU fallback on
   frame 0 so software does not touch XFB/EFB-visible contents.
4. Frame 1: submit only `gx_copy_efb_to_xfb(clear=false)`, then sample before
   resuming the CPU green fallback.

Expected result: frame 1 `post-gx-pre-cpu-fill` should change to a uniform
green-derived YUYV value if primitive writes are merely late relative to
same-FIFO copy.  If frame 1 remains stale/noisy, ordinary primitive writes still
are not producing controlled EFB contents under the current state.

Previous test in `gcn_gx_blit_fb_rgb565()`:

1. Frame 0: submit `gx_set_copy_clear_rgb(0, 255, 0)` plus
   `gx_copy_efb_to_xfb(clear=true)`.
2. `gcnfb.c` samples `fb_mem` as `post-gx-pre-cpu-fill`, then intentionally
   skips the CPU green fallback on frame 0.
3. Frame 1: submit `gx_copy_efb_to_xfb(clear=false)`.
4. `gcnfb.c` samples `fb_mem` again, then resumes the CPU green fallback for
   visual stability.

Expected result: if frame 1 `post-gx-pre-cpu-fill` is `a52ba515`, then the
EFB clear and EFB-to-XFB copy path are working across a VI frame.  If frame 1 is
still stale/noisy, either the clear did not affect EFB as expected or the copy
path is not reading the cleared EFB contents.

Result from the immediate two-submit test that preceded this:

```text
gcn-gx: f0 diag=clear-then-copy xfb0=ad22ca5e xfb1=ad289152 xfbc=8040802e
gcnfb: f0 post-gx-pre-cpu-fill fb0=ad22ca5e fb1=ad289152 fbc=8040802e
gcnfb: f0 cpu-fill-green pattern=a52ba515
gcn-gx: f1 diag=clear-then-copy xfb0=a52ba515 xfb1=a52ba515 xfbc=a52ba515
```

Interpretation: the immediate second copy did not observe the green clear.
Later frames were green only because software filled XFB after frame 0, so the
test was still ambiguous.

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

**Previous tested image**: `f08056d` code, rebuilt/copied as SHA-256
`6e760faaa1465f0eb4a4aa5391592b566fb9af4f720f760d0b3238f8ed770d84`.
XF=1 + 0x200 + all quad texcoords=(0,0).  Result: still stalls with SR=0004
starting at frame 2.

**Current deployed image**: SHA-256
`d1be38b8227c4a48e19ad6bb0454cbae1a25f7caf7ecc2d10590e04b8f4d8920`.
This is the XF-from-position diagnostic: XF 0x103F=1, XF 0x1040=0x000
(`sourcerow=GX_TG_POS`, projection=0), VCD TEX0 disabled, position-only quad.
Result: drains cleanly through frame 360.

## Current gcn_gx_blit_fb_rgb565 pipeline (diagnostic)

```c
gx_setup_texcoord_parse_state(width, height);    // XF=1, src=POS, texenable=0
gx_draw_pos_quad(width, height);                  // DIAGNOSTIC: no TEX0 vertex payload
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
| 1 | 1 | DIRECT | all=(0,0) + 0x200 | ✗ 0004 at f2 | zero gradient does not help |
| 1 | 1 | none, src=POS | pos-only | **✓ 000c** | XF output itself is safe |

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

The f08056d test set all 4 vertex texcoords to (0,0), giving dS/dx = dS/dy = 0
everywhere.  That still stalled with SR=0004 at frame 2.

Conclusion: the stall is upstream of non-zero LOD derivative computation.  Merely
enabling XF texgen output into the rasterizer is enough to trigger the persistent
downstream non-idle state.

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

#### Test: XF=1 + 0x200 + all texcoords=(0,0) → SR=0004 at f2

Commit `f08056d559ff`, rebuilt/copied image hash:

```text
6e760faaa1465f0eb4a4aa5391592b566fb9af4f720f760d0b3238f8ed770d84
```

dmesg:

```text
f0 pre:  SR=0008 RD=0000 WT=0180 pos=384
f0 post: SR=000c RDoff=0180 WToff=0180
f0 tex0=00000000 xfb0=00800080 xfb1=00800080
f1 post: SR=000c RDoff=0180 WToff=0180
f1 tex0=00000000 xfb0=00800080 xfb1=00800080
f2 post: SR=0004 RDoff=0180 WToff=0180
pipeline did not go idle after submit (SR=0x0004)
f2 tex0=00000000 xfb0=00800080 xfb1=00800080
f3 pre:  SR=0000 RD=0000 WT=0180 pos=384
f3 post: SR=0004 RDoff=0180 WToff=0180
first_slow f13 SR=0000 RDoff=0120 WToff=0180 PIoff=0180 pos=384
first_stall f13 SR=0000 RDoff=0120 WToff=0180 PIoff=0180 pos=384
```

Important distinction: frame 2 fully drained the FIFO (`RDoff == WToff == 0x0180`)
but did not reach command-idle (`SR=0004`, bit 3 clear).  The hard CP read-pointer
stall at frame 13 is a cascaded failure after repeatedly submitting into a
downstream pipeline that never became idle.

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

### Step 1: Test whether XF generating any texcoord output is sufficient

The f08056d zero-gradient test landed in Case B: zero-gradient TEX0 still gives
SR=0004 at frame 2.  The next diagnostic should remove VCD TEX0 entirely and ask
XF to generate texcoord 0 from position instead:

- `XF 0x103F = 1`
- `XF 0x1040 = 0x004` (`sourcerow=GX_TG_POS`, `stq=1`, projection=0)
- `VCD TEX0 = none`
- `VAT = position-only`
- `gx_draw_pos_quad()`

This tests whether TEX0 vertex data is needed to trigger the stall, or whether
XF producing any texcoord output to rasterizer/SU is sufficient.

This diagnostic is now deployed as image hash:

```text
d1be38b8227c4a48e19ad6bb0454cbae1a25f7caf7ecc2d10590e04b8f4d8920
```

Boot result:

```text
f0 post:   SR=000c RDoff=0160 WToff=0160
f1 post:   SR=000c RDoff=0160 WToff=0160
f2 post:   SR=000c RDoff=0160 WToff=0160
f3 post:   SR=000c RDoff=0160 WToff=0160
f360 post: SR=000c RDoff=0160 WToff=0160
```

Interpretation: XF emitting generated texcoord output is safe when the source is
position and the FIFO is position-only.  The frame-2 stall requires either
`sourcerow=GX_TG_TEX0` specifically or the presence of a TEX0 vertex attribute
while texgen output is enabled.

Next split test: keep `sourcerow=GX_TG_POS` but re-add VCD/VAT TEX0=DIRECT and
all-zero TEX0 payload.  If that drains, TEX0 payload is harmless and the bug is
specific to XF sourcing from TEX0.  If it stalls, the bug requires a TEX0 vertex
attribute to be present while XF texgen output is enabled.

That split test is now deployed as image hash:

```text
56af7eeedab61fd8e34df69de75adc2ee6a3338d64c399a71e33985f4bdd1604
```

Current diagnostic state:

- `XF 0x103F = 1`
- `XF 0x1040 = 0x000` (`sourcerow=GX_TG_POS`, projection=0)
- `VCD TEX0 = DIRECT`
- quad vertices include direct TEX0 payload, currently all zero
- TEV texture fetch disabled, EFB->XFB copy still enabled

Boot result:

```text
f0 post: SR=000c RDoff=0180 WToff=0180
f1 post: SR=000c RDoff=0180 WToff=0180
f2 post: SR=0004 RDoff=0180 WToff=0180
pipeline did not go idle after submit (SR=0x0004)
f3 pre:  SR=0000 RD=0000 WT=0180 pos=384
first_stall f13 SR=0000 RDoff=0120 WToff=0180 PIoff=0180 pos=384
```

Interpretation: the frame-2 failure does not require XF sourcing TEX0.  TEX0
attribute presence is enough to trigger it when texgen output is enabled, even
when XF 0x1040 sources generated texcoord 0 from position and the TEX0 payload is
all zero.  The frame-2 FIFO drains completely; the later frame-13 read-pointer
stall is again a cascaded failure after the downstream pipeline remains non-idle.

Next split test: remove the TEX0 vertex attribute again, but set XF 0x1040 back
to `0x200` (`sourcerow=GX_TG_TEX0`).  If that drains, source selection alone is
safe and the bug specifically requires a TEX0 attribute while texgen output is
enabled.  If it stalls, either source selection or missing TEX0 source state can
trigger the backend hang.

That split test is now deployed as image hash:

```text
afa87b4dcda807e9eb0d41336d785c7c4cd71a466939bbdc0cf0cdf67726b20c
```

Current diagnostic state:

- `XF 0x103F = 1`
- `XF 0x1040 = 0x200` (`sourcerow=GX_TG_TEX0`, projection=0)
- `VCD TEX0 = none`
- quad vertices are position-only
- TEV texture fetch disabled, EFB->XFB copy still enabled

Boot result:

```text
f0 post:   SR=000c RDoff=0160 WToff=0160
f1 post:   SR=000c RDoff=0160 WToff=0160
f2 post:   SR=000c RDoff=0160 WToff=0160
f3 post:   SR=000c RDoff=0160 WToff=0160
f360 post: SR=000c RDoff=0160 WToff=0160
```

Visual result: white/pixel-noise output, consistent with copying EFB contents
generated from missing/undefined TEX0 input.  The important hardware result is
that the pipeline drains cleanly through frame 360.

Interpretation: `sourcerow=GX_TG_TEX0` by itself is safe when no TEX0 vertex
attribute is present.  The frame-2 downstream stall specifically requires a
direct TEX0 vertex attribute while XF texgen output is enabled.  A viable
production path should avoid direct TEX0 attributes and generate texcoords from
position instead.

Next practical test: source texcoord 0 from position (`XF 0x1040=0x000`), remove
TEX0 from VCD/VAT, load TEXMTX0 as a scale matrix mapping pixel XY to normalized
ST, enable TEV texture fetch, bind `gx_tex_buf`, and draw a position-only quad.
This tests whether the full texture path can work without the bad TEX0 vertex
attribute.

That practical texture-fetch test is now deployed as image hash:

```text
c1fc6e2a0539204e23cd6b0d7e5edcea7aae1abbbfc95de6bc41bc9f97752c7d
```

Current diagnostic state:

- `XF 0x103F = 1`
- `XF 0x1040 = 0x000` (`sourcerow=GX_TG_POS`, projection=0)
- `TEXMTX0 = scale(position.xy -> normalized st)`
- `VCD TEX0 = none`
- quad vertices are position-only
- `gx_tex_buf` is filled with solid RGB565 red/green/blue cycling every 180 frames
- TEV texture fetch enabled, texmap 0 bound, EFB->XFB copy enabled

Boot result:

```text
f0 post:   SR=000c RDoff=01c0 WToff=01c0 tex0=f800f800
f1 post:   SR=000c RDoff=01c0 WToff=01c0 tex0=f800f800
f2 post:   SR=000c RDoff=01c0 WToff=01c0 tex0=f800f800
f3 post:   SR=000c RDoff=01c0 WToff=01c0 tex0=f800f800
f360 post: SR=000c RDoff=01c0 WToff=01c0 tex0=001f001f
```

Visual result: vertical stripes/noise.  No frame-2 stall.  `xfb0` remained near
video black rather than the expected solid red/blue YUYV values, so the failure
has moved to texture coordinate/object correctness.

Comparison against local `/home/anolis/repos/libogc` found mismatches in this
test:

- `GX_TG_POS` texgen uses `stq=1`, so `XF 0x1040` should be `0x004`, not `0x000`.
- Selecting `GX_TEXMTX0` for texcoord0 requires matrix-index low bits
  `GX_TEXMTX0 << 6` (`30 << 6`) written to CP `0x30` and XF `0x1018`.
- Non-mipmap `GX_InitTexObj` default `texMode0` is `0x90`
  (`0x10` mag-filter bit plus `0x80` non-mipmap min filter); the old `0x250`
  was not libogc's default object state.
- RVL default `texRegion[0]` encodes TMEM cache-size metadata in BP `0x8C/0x90`
  and uses odd-bank base `0x08000`; the hand-written `0x8C000000/0x90004000`
  did not match libogc.

Deployed follow-up build `e881d745b437ae72f9ec2c123abe4713a29020467946238fb7c83e08b4bfaa15`
applied the first two texgen fixes plus the mistaken `0x80` texMode0 value.
Hardware result was still vertical stripes/noise.  Next build corrects
texMode0 to `0x90` and changes BP `0x8C/0x90` to libogc RVL texRegion[0]
values `0x8C0D8000/0x900D8400`.

Deployed image `fcdb36554396dd6a145e2c15dbb68f8bc3d1f65a819d970821b7cac1aa101641`
contains the `0x90` texMode0 and RVL texRegion[0] fixes.  Awaiting hardware
result.

Hardware result was still the same vertical stripes/noise, with the FIFO still
draining and `xfb0/xfb1` unchanged.  Next test explicitly programs BP `0x43`
to `0x43000040`, matching libogc's `GX_SetZCompLoc(GX_TRUE)` plus
`GX_SetPixelFmt(GX_PF_RGB8_Z24, GX_ZC_LINEAR)`, to rule out mini leaving the PE
in an incompatible EFB pixel format.

Deployed image `8cae3eb7abd85699b1149ce98886fa263f098454883cad650e444183b70a0fff`
contains the explicit BP `0x43000040` PE-control write.  Awaiting hardware
result.

Hardware result was still the same vertical stripes/noise.  The log still
showed FIFO drain and unchanged XFB samples.  Next diagnostic bypasses texture
fetch and drawing entirely: it programs `GX_SetCopyClear`-style BP `0x4F/0x50/
0x51` solid red/green/blue colours and calls display-copy with `clear=1`.  If
EFB->XFB copy is actually writing the XFB, the display should become a solid
colour by the second frame.  If it remains striped, focus on copy destination,
copy control, or XFB/VI memory format rather than texture or raster state.

Deployed image `6311481a4c33228e84a95b8a26349ffc90990fbad1e40593d64522dfae75e4ab`
contains the copy-clear diagnostic.  Awaiting hardware result.

Hardware result: solid red, green, and blue appeared.  This proves CP/FIFO,
display-copy execution, copy destination, and VI-visible XFB writes are working.
The fault is before display-copy: either rasterization/PE draw into EFB or TMU
texture sampling/TEV state.

Next diagnostic removes TMU but keeps rasterization: draw a full-screen direct
RGBA8 vertex-colour quad (`VCD_LO=0x2200`, `VAT0=0x40016008`,
`XF 0x1008=1`) with `numchans=1`, `numtexgens=0`, TEV `PASSCLR`, then perform
normal non-clear EFB->XFB copy.  If solid colours appear, rasterization works
and the remaining bug is texture sampling.  If stripes return, focus on
position transform, vertex/color channel state, or PE raster writes.

Deployed image `6539a7899b263d47938348a28243cfe64974e6c01435f01f2ab722578fb31abe`
contains the vertex-colour raster diagnostic.  Awaiting hardware result.

Hardware result: vertical bars/noise returned.  Log showed FIFO drain
(`RDoff=WToff=0x0180`) but XFB remained stale, so primitive commands are being
consumed but no quad reaches EFB.  Comparison against libogc found that
`GX_SetCurrentMtx(GX_PNMTX0)` writes both CP `0x30` and XF `0x1018`; our
`gx_load_identity_pos_mtx0()` only wrote XF `0x1018`.  Next build writes
CP `0x30=0` before XF `0x1018=0`, so position vertices should use PNMTX0
instead of stale matrix-index state.

Deployed image `cd8ef02f159b6579785e07b1ac02eb8ca59201acfee9bed2ea896ef18c04238e`
contains the CP `0x30=0` matrix-index fix while keeping the vertex-colour
raster diagnostic active.  Awaiting hardware result.

Hardware result: still vertical bars/noise.  The rootfs log was unavailable
when the SD came back, but the visible result means the CP current-matrix fix
was not sufficient.  Next build keeps the vertex-colour raster diagnostic and
adds two more libogc init states that affect raster visibility:
`GX_SetScissorBoxOffset(0,0)` as BP `0x59000000` and
`GX_SetClipMode(GX_CLIP_ENABLE)` as XF `0x1005=1`.

Deployed image `6f8477b829ecbf0baa992c908146dc941fb9d27ff153c4a22a06310041c67574`
contains those explicit clip/scissor state writes while keeping the
vertex-colour raster diagnostic active.  Awaiting hardware result.

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
of texcoord values (pixel-space, normalized [0,1], uniform all-zero) or XF 0x1040
projection bit (0x200 vs 0x201).

Pattern:
- f0: SR=000c, pixels rendered to EFB (xfb=00800080)
- f1: SR=000c, pixels rendered to EFB
- f2: SR=0004, pipeline never idles — persistent stall
- f3+: cascaded failure (SR=0000 at pre-log)

The "exactly frame 2" pattern suggests a hardware state that accumulates over 2 renders.
Degenerate texcoords (all=(0,0)) did not fix it, so non-zero gradient/LOD derivative
math is not sufficient to explain the stall.

### 18. Vertex-colour raster path still consumes FIFO but does not update EFB

The vertex-colour diagnostic drains cleanly, but the XFB samples remain stale and the
display shows the old vertical bars/noise:

- f0/f1/f2/f3 and f360 all report `RDoff == WToff == 0x01a0`
- `vcol` cycles to blue at f360
- XFB samples stay at the same mini/noise values after f1

This means CP/FIFO command consumption is not the problem for this test.  The remaining
failure is between primitive setup and EFB colour writes: viewport/projection/clip,
colour-channel state, raster state, or PE/EFB write masks.

Current test after this note: explicitly program libogc-style field state in
`gx_setup_vertex_color_state()`:

```c
gx_load_bp_reg(0x44000003); /* GX_SetFieldMask(GX_TRUE, GX_TRUE) */
gx_load_bp_reg(0x68000000); /* GX_SetFieldMode(GX_FALSE, GX_FALSE) */
```

Hypothesis: copy-clear can update/clear EFB while ordinary raster writes may be blocked
if mini left one or both field masks disabled.

Result: still vertical bars/noise.  Fresh dmesg showed the same clean drain and stale
XFB samples:

- f360 `RDoff == WToff == 0x01a0`
- f360 `vcol=0000ff`
- XFB samples remained unchanged after f1

Field mask/state is not sufficient.

Next test: issue a proven `GX_CopyDisp(clear=true)` with black clear colour before the
vertex-colour draw, then run the ordinary non-clear display copy.  Expected outcomes:

- red/green/blue: vertex-colour raster writes work after an explicit EFB clear
- black: pre-clear works but primitive raster writes still do not update EFB
- vertical bars/noise: clear-before-draw ordering is not working or the clear did not run
  in this combined command stream

Result: black.  Fresh dmesg:

- command stream grew to `pos=480`
- f0/f1/f2/f3/f360 all drained (`RDoff == WToff == 0x01e0`)
- XFB samples became stable video black (`10801080 127f107f`)

This proves the pre-clear executes inside the combined command stream and the final
display copy sees the cleared EFB.  The vertex-colour primitive still does not visibly
modify EFB.  One ambiguity remains: the primitive could be drawing black because the
raster colour/TEV/channel state is wrong, or it could be drawing no fragments.

Next test: pre-clear EFB to green, then draw the same vertex-colour quad:

- red/green/blue: vertex colour path works
- green: primitive produces no visible EFB writes
- black: primitive writes pixels, but colour/TEV state outputs black

Deployed image `9451ecf1105020d9efd42e20e5c0b88654ac5a8ca2b071c90028723124ded437`
contains the green-preclear diagnostic (commit `550766c192b1`).

Result: green.  Fresh dmesg confirmed a stable, uniform green XFB readback
(`xfb0=90369122 xfb1=90369022`) identical at f0 (vcol=red) and f360
(vcol=blue) — the vertex colour never showed up in the output, definitively
proving the vertex-colour primitive produces **no visible EFB writes at all**
(not "writes black"): the clear colour survives the primitive untouched.

### 2026-07-04 session (picks up from the green-preclear result)

Two register mismatches were found by cross-referencing the vertex-colour
diagnostic against `/home/anolis/repos/libogc` `gx.c`/`gx.h` bit-for-bit,
the same technique that resolved the earlier texture-path bugs:

**Mismatch 1 — XF 0x100e/0x1010 (chan0 colour/alpha control) encoding.**
libogc's `GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX,
GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE)` — the exact call `GX_Init()` makes
for colour0 by default — computes register value `0x401`.  Our driver had
`0x201`.  Decoding both: bit0 (matsrc=vertex) matches in both, but bits
9:10 differ — `0x201` encodes `attn_fn=GX_AF_SPEC` (specular) while `0x401`
correctly encodes `attn_fn=GX_AF_NONE`.  Fixed to `0x401` in isolation
first (commit pending) to test whether this alone was the "no write" cause.

Deployed image `6b332902ff63725f6b2544e8078f0b23096864317370ff5b29bab53839fabb39`
contains only this chan-ctrl fix (clip register untouched, still the old
value).

Result: still green.  This mismatch was real (confirmed against libogc) but
not sufficient by itself to produce visible EFB writes.

**Mismatch 2 — XF 0x1005 GX_CLIP_ENABLE polarity is inverted.** libogc
defines `GX_CLIP_ENABLE = 0` and `GX_CLIP_DISABLE = 1`;
`GX_SetClipMode(mode)` writes `mode & 1` directly to XF 0x1005.  Our driver
wrote `gx_load_xf_reg(0x1005, 1)` with a comment claiming this was
`GX_CLIP_ENABLE` — it was actually `GX_CLIP_DISABLE`.  Fixed to write `0`.

Deployed image `038b5a2ddee676d88db719ed6ab9ffe066c112cba2ef4eddd0060a3c9e364f5a`
contains both the chan-ctrl fix and the clip-polarity fix.

Result: **black**, not green — a real change in behaviour.  Fresh dmesg
showed a stable but non-uniform XFB corner readback (`xfb0=b52e8675
xfb1=c749e386`, unchanged between f0 (vcol=red) and f360 (vcol=blue)), i.e.
still not tracking the vertex colour, but visually reported as black
full-screen.  Per the green/black discriminator table above: this is the
"black" bucket — **the primitive is now visibly writing to the EFB**
(progress from the green "no writes" state), but the output colour is
wrong (black) instead of the vertex RGBA colour.

Interpretation: the clip-polarity bug was actually enabling
`GX_CLIP_DISABLE` before the fix; something about the (previously
uninvestigated) interaction between correct clipping and this pipeline
state was masking the primitive's EFB writes entirely.  With clip now
genuinely enabled, rasterization proceeds but colour output is black.

Re-verified against libogc, bit-for-bit, with no discrepancies found in:
VAT0 colour0 format (`0x40016008` = RGBA8 direct, correct), BP 0x25 TEV
order (`rascolor=GX_COLOR0A0`→colid 0, correct), BP 0xC0/0xC1 TEV
colour/alpha combiners (`PASSCLR` formula, byte-exact match), XF 0x1008
VtxSpecs and XF 0x1009 SETNUMCHAN (byte-exact match), and genMode
numcolchans/numtevstages fields.  libogc's own doc comment for
`GX_SetChanCtrl` confirms: "When the channel enable is set to GX_FALSE,
the material colour source ... is passed through as the channel's output
colour" — so `matsrc=GX_SRC_VTX` with `enable=0` should pass the vertex
colour through untouched, which every register we can compare against
libogc says should work.

Next test isolates vertex-attribute colour parsing from the
channel/TEV/PE pipeline: switch `matsrc` from `GX_SRC_VTX` to `GX_SRC_REG`
and drive channel 0 from a hardcoded bright-red XF 0x100c material-colour
register instead of the per-vertex attribute (vertex payload is still
sent in the FIFO but now ignored by the channel unit).

- solid red: the channel/TEV/PE pipeline is fine; the bug is specifically
  in vertex CLR0 attribute parsing (VCD/VAT/byte layout despite the
  bit-exact match found above — possibly a CP-side parsing quirk not
  visible from static register comparison).
- still black: the bug is downstream of the material-colour source itself,
  in the channel/TEV/PE path — despite every statically comparable register
  matching libogc, so look at dynamic/hardware-specific state (PE
  colour-update masks, dst alpha, or an register we haven't cross-checked
  yet).

Deployed image `f319d2350945c5f8939a60c446533fae16420769eefca051d2a23c07c60a4074`
contains the `matsrc=GX_SRC_REG` + hardcoded red XF 0x100c isolation test
(chan-ctrl and clip fixes both still in place).

Result: **green**, not solid red or black.  Fresh dmesg showed the XFB
corner readback (`xfb0=90369122 xfb1=90369022`) exactly matching the very
first green-preclear test byte-for-byte — the clean, uniform "no visible
writes" signature — unlike the previous (`matsrc=GX_SRC_VTX`) test, whose
corner readback was non-uniform (`b52e8675`/`c749e386`).

Interpretation: switching the material-colour source from `GX_SRC_VTX` to
`GX_SRC_REG` did not just change the output colour — it reverted the
primitive to producing **zero visible fragment writes**, the same
"no-op" failure mode as before the clip-polarity fix.  This is the
opposite of the naive expectation (that matsrc only picks which colour
value feeds the same downstream pipeline).  Net conclusion:
`matsrc=GX_SRC_VTX` is the configuration that actually perturbs the EFB;
`GX_SRC_REG` regresses to no-op.  Reverted back to `GX_SRC_VTX`
(`0x401`, no XF 0x100c write) since it is the only configuration making
observable progress.

The real open question is now: how much of the screen does the "black"
result actually cover, and is it truly `(0,0,0)` output or something else?
The only hardware evidence so far is 2 corner pixels
(`xfb[0]`/`xfb[1]`, i.e. the very first 2 pixels of row 0), which do not
decode to black under either YUYV byte-order assumption (both give
moderately bright, saturated colours) — yet the user's whole-screen visual
report was "black".  This discrepancy is unresolved: either the corner
pixels are a rendering artifact unrepresentative of the rest of the frame
(e.g. a scissor/viewport boundary effect at exactly `(0,0)`), or the
whole-screen black report is dominated by mostly-black content with a
small bright corner the viewer didn't register.

Added a third XFB sample point (`xfbc`, screen-centre pixel) to the
`gcn_gx_blit_fb_rgb565` diagnostic `pr_info` so the next boot's dmesg
directly shows whether the centre of the frame is black, coloured, or
something else — resolving the ambiguity without guessing from the corner
alone.

Deployed image `4e269bd386e2146a8d8abd8c4dc8a4508af609b7d58703e796d30069b4e376a2`
contains the reverted `matsrc=GX_SRC_VTX` config plus the new `xfbc`
centre-pixel sample (no register-state changes versus the earlier "black"
test, `b784b920565b`, other than the added log field).

Result: **black** again (expected — no register changes).  Fresh dmesg:

```
f0   vcol=ff0000 xfb0=b1317972 xfb1=c352b786 xfbc=157d157d
f360 vcol=0000ff xfb0=b1317972 xfb1=c352b786 xfbc=157d157d
```

`xfbc=157d157d` decodes (either YUYV byte order, since U≈V≈0x7d here) to
roughly RGB(17,24,16) — genuinely near-black, and completely unchanged
between f0 (vcol=red) and f360 (vcol=blue).  **This confirms the
whole-screen "black" report**: the primitive is drawing across the
screen interior, but outputs an almost-zero colour regardless of the
vertex RGBA input.  The corner samples (`xfb0`/`xfb1`) differ between
this boot and the previous "black" boot (`b1317972`/`c352b786` vs
`b52e8675`/`c749e386`) even though the driver code is unchanged between
those two boots' logging point — i.e. the corner pixel is non-deterministic
across reboots (mini leftover memory / a scissor-edge artifact at exactly
pixel (0,0)), while the centre pixel is deterministically near-black.
The corner samples are therefore a red herring; trust `xfbc`, not
`xfb0`/`xfb1`, for judging overall fill colour from now on.

Next hypothesis: `GX_Init()` itself defensively initializes XF 0x100a
(chan0 ambient colour) and XF 0x100c (chan0 material colour) to
`BLACK={0,0,0,0}` / `WHITE={255,255,255,255}` (register values
`0x00000000` / `0xFFFFFFFF`) even for its own default
`matsrc=GX_SRC_VTX, enable=GX_DISABLE` channel.  Our driver has never
written either register in the VTX path — they're left at whatever mini
left behind.  The GX_SetChanCtrl doc comment says these should be
irrelevant when `enable=0`, but that's a software API guarantee, not a
verified hardware one.  Test whether real silicon depends on them being
initialized by adding both writes with GX_Init()'s own default values.

- solid red/green/blue (tracking vcol): confirms the missing
  ambient/material registers were the cause: the vertex-colour channel
  computation depends on them being sane even when nominally bypassed by
  `matsrc=GX_SRC_VTX`.
- still black: rule out ambient/material register content entirely; the
  bug is elsewhere in the channel/TEV/PE path (next candidates: dst-alpha
  config, dither, or a raster interpolator enable bit not yet identified).

Deployed image `f940fac719275f6558776ce1539d786e76996e8f870ee08fc223b449caeaf977`
contains both XF 0x100a/0x100c initialization writes.

Result: **still black**. Fresh dmesg:

```
f0   vcol=ff0000 xfb0=b1308271 xfb1=b653c078 xfbc=1f781f75
f360 vcol=0000ff xfb0=b1308271 xfb1=b653c078 xfbc=1f781f75
```

`xfbc=1f781f75` decodes to roughly RGB(19,26,16) — essentially the same
near-black as the previous test's `157d157d`≈(17,24,16).  Ambient/material
register content is **not** the fix, but the small non-zero shift between
these two otherwise-identical tests (ambient/material previously
uninitialized garbage vs. now BLACK/WHITE) is a real, reproducible signal:
if `matsrc=GX_SRC_VTX, enable=GX_DISABLE` were a pure hardware passthrough
of the vertex colour as libogc's doc comment claims, XF 0x100a/0x100c
content could not affect the output at all.  It measurably did (by a
small amount), which means the "disabled channel" path on this specific
hardware is not the simple passthrough the software API documents.

Next test bypasses that undocumented shortcut entirely: explicitly enable
lighting (`enable=1`) with zero lights (`litmask=GX_LIGHTNULL`) and a
white ambient colour, so the hardware computes
`matColor * (ambColor + 0 lights) = vertexColor * white = vertexColor`
through the same code path real lit geometry uses, rather than the
disabled-channel special case.

`GX_SetChanCtrl(GX_COLOR0A0, GX_ENABLE, GX_SRC_REG, GX_SRC_VTX,
GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE)` → register value `0x403`
(`matsrc=1, enable=1, ambsrc=0(REG), attn_fn=NONE`).  XF 0x100a (ambient)
set to WHITE (`0xFFFFFFFF`) so the multiply is a no-op; XF 0x100c
(material) left at WHITE too (unused while `matsrc=GX_SRC_VTX`).

- red/green/blue (tracking vcol): the disabled-channel passthrough path
  was the actual hardware bug; going through the lit-with-zero-lights path
  works correctly.
- still black: rule out the channel-enable path entirely; the bug must be
  downstream in TEV/PE (dst-alpha, dither, or an EFB write-mask register
  not yet identified) or upstream in vertex delivery itself (re-examine
  VCD/VAT dynamically, e.g. by dumping the actual FIFO bytes at the
  vertex payload offset via the existing stall-dump logging).

Deployed image `0e4239bff13586d754fe8363fe458452aec3aa818095d20ea56fa15ec6f22903`
contains the enable=1/zero-lights/white-ambient diagnostic.

Result: **still black**. Fresh dmesg:

```
f0   vcol=ff0000 xfb0=b537ae6e xfb1=d888728f xfbc=1d791d77
f360 vcol=0000ff xfb0=b537ae6e xfb1=d888728f xfbc=1d791d77
```

`xfbc=1d791d77` ≈ RGB(21,28,19) — essentially the same near-black as
both previous tests (`157d157d`≈(17,24,16), `1f781f75`≈(19,26,16)).  Three
completely different colour-channel configurations
(`matsrc=VTX,enable=0` passthrough; `matsrc=REG,enable=0`; and
`matsrc=VTX,enable=1` lit-with-zero-lights) have now all produced nearly
identical near-black output regardless of the vertex colour.  That
convergence across such different configurations is itself a strong
signal: it is unlikely that three different, individually-correct (per
libogc) channel setups would all coincidentally break the same way.  More
likely, the colour-channel/rasterizer output is not the thing reaching
the EFB at all, and something downstream (TEV, PE, or the copy) is
involved, or the true bug is further upstream than the channel registers
(e.g. vertex delivery itself).

Next test bypasses the colour-channel/rasterizer stage entirely: change
the TEV colour combiner's `d` input from `GX_CC_RASC` (rasterized colour,
value 10) to `GX_CC_ONE` (hardware constant 1.0, value 12), independent
of any channel or vertex state — `a=b=c=GX_CC_ZERO(15), d=GX_CC_ONE(12)`
→ BP 0xC0 = `0xC008FFFC`.  This should render solid white if TEV/PE
themselves are healthy, with zero dependency on the colour channel.

- solid white: TEV/PE are fine; the bug is 100% isolated to
  colour-channel/rasterizer colour generation upstream of TEV — focus
  there exclusively (possibly a raster interpolator enable bit, or the
  vertex CLR0 attribute genuinely never reaching the rasterizer despite
  matching VAT/VCD bit layout).
- still black/other: the bug is not in the colour channel at all; look at
  TEV output stage config, PE writes, dst-alpha, or EFB→XFB copy
  addressing/format for this specific (post-raster, non-clear) code path.

Deployed image `c98af827a860f1daa31a829e5fd1ef328604aeec668f6142c53de3a19e183f4e`
contains the CC_ONE TEV bypass diagnostic. Awaiting hardware result.

Result: **green**.  The SD rootfs was not mounted cleanly on return (reader showed the
card device as `0B`), so no fresh dmesg was available for this boot.  The visual result is
still decisive for the current discriminator: the proven green pre-clear survived to the
final copy, so the CC_ONE primitive stream did not produce visible EFB writes.  This does
not fit the simple "colour-channel outputs black" model, because CC_ONE should be
independent of the colour channel.

Important nuance: that CC_ONE test still left `numcolchans=1`, `raschan=GX_COLOR0A0`, and
sent a direct CLR0 vertex payload even though TEV colour no longer consumed RASC.  It may
therefore still depend on a raster colour token or channel-side state in ways we did not
intend to test.

Next test removes that dependency completely:

- genMode `numcolchans=0`
- XF `VtxSpecs`/`SETNUMCHAN` set to zero colour channels
- TEV order `raschan=GX_COLOR_NULL`, texture disabled
- TEV colour `d=GX_CC_ONE`
- TEV alpha `d=GX_CA_ZERO`
- VCD/VAT and draw payload changed to position-only (`gx_draw_pos_quad`)

Expected result:

- white: TEV/PE and primitive raster are healthy when raster colour is fully removed; the
  previous green result was caused by lingering colour-channel/raster-token state.
- green: even a position-only primitive with constant-white TEV does not modify EFB; focus
  on primitive assembly/rasterization state, viewport/scissor, cull/front-face, or PE write
  state that differs from copy-clear.
- black/other: the primitive is modifying EFB, but the constant TEV configuration is not
  producing the expected colour; inspect TEV/output-stage encoding more deeply.

Deployed image `1a8c2b1f75a02b1140975b9d5689b5719c58d86f231608ea5de6b5dd3ab56041`
to `/media/anolis/BOOTWII/gumboot/zImage.ngx` after the card reappeared as `/dev/sdd1`
(`BOOTWII`) and `/dev/sdd2` (`WII-LINUX-NGX`).  Awaiting hardware result.

Result: **green again**. Fresh dmesg:

```
f0   pre/post: pos=480 RDoff=WToff=01e0, xfbc=72487238
f360 pre/post: pos=480 RDoff=WToff=01e0, xfbc=72487238
```

The centre XFB sample is stable green and the FIFO drains completely.  This rules out
direct CLR0 parsing, colour-channel state, and raster-colour TEV input as the sole cause:
even position-only vertices with zero colour channels and constant-white TEV do not
overwrite the green EFB pre-clear.

Next test keeps the same position-only / zero-colour-channel / constant-white TEV state,
but changes the primitive from `GX_QUADS` (4 vertices) to `GX_TRIANGLES` (two triangles,
6 vertices covering the same rectangle).

- white: quad primitive assembly was the issue; use triangles for the eventual blit.
- green: primitives still produce no visible EFB writes; focus on viewport/projection,
  scissor/clip, cull/front-face, or PE/raster state.

Deployed image `0b3b81e92ffad549535fa9cf7711b8e6806866abc7239d5c898559ddb11e8942`
contains this `GX_TRIANGLES` primitive diagnostic.  Plain `cp` was attempted first, but
BOOTWII was mounted read-only; deployment required a one-time `pkexec mount -o remount,rw`
followed by the same copy/sync/checksum flow.

Result: **green still**. Fresh dmesg:

```
f0   pre/post: pos=480 RDoff=WToff=01e0, xfbc=7e417e30
f360 pre/post: pos=480 RDoff=WToff=01e0, xfbc=7e417e30
```

`GX_TRIANGLES` did not change the outcome.  The command stream drains completely and the
centre XFB sample remains the green pre-clear.  This rules out the `GX_QUADS` primitive
assembly path as the reason primitives fail to overwrite EFB.

Follow-up audit found the deployed "no colour channels" diagnostic was not actually
internally consistent: XF `VtxSpecs`/`SETNUMCHAN` were set to zero colour channels and
TEV order used `GX_COLOR_NULL`, but BP `genMode` still wrote `0x00000010`, i.e.
`numcolchans=1`.  That leaves the rasterizer expecting one colour channel while XF
advertises none, a plausible reason the constant-white primitive never reaches EFB.

Next test changes only BP 0x00 genMode from `0x00000010` to `0x00000000` so all active
state agrees on `numcolchans=0`:

- white: the mismatched genMode/XF colour-channel count was blocking raster output.
- green: the no-write failure is elsewhere; continue with viewport/scissor/cull/front-face
  isolation.

Deployed image `5938e69bf97e56893c1207b7b3060f558ba423e4cfcc817deccdcd068fd86914`
contains this genMode/XF channel-count consistency fix.  Awaiting hardware result.

Result: **still green**. Fresh dmesg:

```
f0   pre/post: pos=480 RDoff=WToff=01e0, xfbc=72487238
f360 pre/post: pos=480 RDoff=WToff=01e0, xfbc=72487238
```

The genMode/XF colour-channel count mismatch was real but not the blocker: the corrected
stream drains fully and the centre XFB sample remains the green pre-clear.

Next test isolates viewport/projection edge cases.  Keep the same position-only,
zero-colour-channel, constant-white TEV state, but replace the pixel-space orthographic
projection with identity orthographic projection and draw one oversized clip-space
triangle: `(-4,-4)`, `(4,-4)`, `(0,4)`.

- white: the previous pixel-space ortho/projection or rectangle coverage was wrong.
- green: even an oversized clip-space primitive does not write EFB; move on to scissor,
  cull/front-face, PE/raster state, or an XF/CP state bit outside projection.

Deployed image `e0e8c2e19e89f943946e2005be9791994c1f99c11af8ebae6ff7daa522477e58`
contains this identity-projection oversized-triangle diagnostic.  Awaiting hardware result.

Result: **green still**. Fresh dmesg:

```
f0   pre/post: pos=448 RDoff=WToff=01c0, xfbc=72487238
f360 pre/post: pos=448 RDoff=WToff=01c0, xfbc=72487238
```

The identity-projection oversized triangle drained fully and still did not overwrite the
green pre-clear.  This rules out the pixel-space orthographic projection constants and
ordinary rectangle edge coverage as the immediate cause.

Next test changes only scissor setup in the active constant-white primitive diagnostic:
instead of programming a screen-sized scissor using GX's normal `+342` offset convention,
write a raw full-range scissor (`BP 0x20 = 0`, `BP 0x21 = 0x7ff/0xfff`) and keep
`BP 0x59` scissor-box offset at zero.

- white: computed scissor or scissor offset was clipping all primitive pixels.
- green: scissor is not the reason primitives fail to write EFB.

Deployed image `6985136a253ba3e3f1fd757ac3e02acb29567792313cf7b5f4565802aa6bbc60`
contains this raw full-range scissor diagnostic.  Awaiting hardware result.

Result: **green still**. Fresh dmesg:

```
f0   pre/post: pos=448 RDoff=WToff=01c0, xfbc=72487238
f360 pre/post: pos=448 RDoff=WToff=01c0, xfbc=72487238
```

The raw full-range scissor did not change the outcome.  This rules out our computed
scissor rectangle and the normal +342/scissor-offset convention as the cause of the
primitive no-write path.

Next test keeps the same full-range scissor and identity-projection oversized triangle,
but emits the triangle twice with opposite winding in the same draw call (6 vertices total).

- white: hidden/stale cull or front-face state was rejecting one winding.
- green: cull/front-face is not the easy explanation; move deeper into PE/raster state or
  vertex/XF state outside projection/scissor.

Deployed image `0f924bd08b7928dca2d8802ecda5df392724e4ab36789e85878c2b91335fdd1e`
contains this opposite-winding diagnostic.  Awaiting hardware result.

Result: **green still**. Fresh dmesg:

```
f0   pre/post: pos=480 RDoff=WToff=01e0, xfbc=72487238
f360 pre/post: pos=480 RDoff=WToff=01e0, xfbc=72487238
```

Drawing both windings in the same command stream did not change the outcome.  This rules
out a simple hidden/stale cull or front-face state rejecting the triangle winding.

Next test changes the position vertex format from XY to XYZ and sends `z=0` for every
vertex, while keeping the same two opposite-winding oversized triangles.  This checks
whether the XF/raster path is silently unhappy with 2-component positions despite the VAT
encoding.

- white: the raster path requires/works with XYZ positions in this setup.
- green: position component count is not the blocker; move deeper into PE/raster state or
  untested XF/CP state.

Deployed image `62e1ad060cf68d7ed0d709e0ddad75bbe5d05c88e0eb03795643193fac2f4611`
contains this XYZ-position diagnostic.  Awaiting hardware result.

Result: **green still**. Fresh dmesg:

```
f0   pre/post: pos=512 RDoff=WToff=0200, xfbc=72487238
f360 pre/post: pos=512 RDoff=WToff=0200, xfbc=72487238
```

Changing position vertices from XY to XYZ did not help.  The no-channel/constant-white
TEV diagnostic continues to drain fully while leaving the green EFB pre-clear untouched.

Next test returns to the direct `CLR0` + TEV `PASSCLR` path, because earlier versions of
that path produced real full-screen near-black EFB writes.  It keeps the recent geometry
workarounds: full-range scissor, identity projection, oversized opposite-winding
triangles, and XYZ positions.  Each vertex now carries direct RGBA8 colour again.

- red/green/blue tracking `vcol`: the direct-colour path is healthy with the newer
  geometry state; the no-channel constant-TEV path was the invalid branch.
- black/near-black: primitive writes still happen, but colour/TEV/channel output remains
  wrong; continue debugging output-state encoding from the known-writing path.
- green: the older full-screen black write depended on some older viewport/projection/
  scissor detail; reintroduce those one at a time.

Deployed image `9bfae070b9489a7cf13fdf974cfb6eb07a3f64d24e060a51613924d2f96e6484`
contains this direct-CLR0/PASSCLR-with-current-geometry diagnostic.  Awaiting hardware
result.

Result: **green still**. Fresh dmesg:

```
f0   pre/post: pos=544 RDoff=WToff=0220, xfbc=72487238
f360 pre/post: pos=544 RDoff=WToff=0220, xfbc=72487238
```

Direct `CLR0` + TEV `PASSCLR` did not regain the older near-black full-screen write when
combined with the newer geometry workarounds.  That points back at a geometry/state
difference rather than the no-channel constant-TEV branch alone.

Next test restores the older known-writing geometry baseline: normal GX-style scissor
with the 342 offset, pixel-space orthographic projection, direct XY+RGBA8 vertices, and
`GX_QUADS`.  The green pre-clear remains in place.

- black/near-black: the older writing baseline is recovered; then bisect which geometry
  change broke visible primitive writes.
- red/green/blue tracking `vcol`: even better, the restored path fixed colour as well.
- green: the earlier black-writing observation depended on some other state not yet
  restored; compare against the exact known-black commit.

Deployed image `bd5ebef7c1295010f4453c3b8e925ae005782e432f4f6551b3082cf720877fe9`
contains this restored pixel-space direct-colour geometry baseline.  Awaiting hardware
result.

Result: **green still**. Fresh dmesg:

```
f0   pre/post: pos=480 RDoff=WToff=01e0, xfbc=72487238
f360 pre/post: pos=480 RDoff=WToff=01e0, xfbc=72487238
```

Restoring the obvious pixel-space geometry baseline was not sufficient.  A direct diff
against known-black commit `1cf82d033072` showed the remaining important mismatches:
current code still used the later enabled-lighting channel control (`0x403`) plus
ambient/material writes, and called the renamed position draw helper instead of the
original `gx_draw_color_quad`.

Next test restores the exact known-black colour path: XF chan0 colour/alpha control
`0x401`, no ambient/material writes, and the original `gx_draw_color_quad()` call, while
keeping the green pre-clear and current logging.

- black/near-black: confirms the known-black baseline is reproducible; then bisect the
  channel-control changes from there.
- green: the missing difference is elsewhere in command submission or another state not
  visible in the local setup diff; compare the full FIFO bytes against `1cf82d033072`.

Deployed image `eeadda11112ebf963caf219adccbe7ed7b065a21df14c94024da969281b942e3`
contains this exact known-black colour-path restore.  Awaiting hardware result.

Result: **green still**. Fresh dmesg:

```
f0   pre/post: pos=480 RDoff=WToff=01e0, xfbc=72487238
f360 pre/post: pos=480 RDoff=WToff=01e0, xfbc=72487238
```

At this point the active source diff against `1cf82d033072` in `gcn-gx.c` is only comments
and an unused helper.  No other tracked files differ except this handoff document.  To
separate source-state drift from build/runtime/environment drift, a temporary worktree was
created at `/tmp/wii-known-black` and checked out at exact commit `1cf82d033072`:

```
git worktree add -f /tmp/wii-known-black 1cf82d033072
cp /home/anolis/repos/wii-linux-ngx/.config /tmp/wii-known-black/.config
CCACHE_TEMPDIR=/tmp CCACHE_DIR=/home/anolis/repos/wii-linux-ngx/.ccache \
  make ARCH=powerpc CROSS_COMPILE=powerpc-linux-gnu- -j$(nproc)
```

Deployed image `e4b1551f7eeaaae3c569d0699ab02cb6ff145a664685ff7aa0e6e5547245bc51`
is built from exact historical commit `1cf82d033072`, the commit that previously produced
the confirmed near-black centre sample (`xfbc=157d157d`).  Awaiting hardware result.

- black/near-black: the historical source still reproduces; inspect generated FIFO bytes
  or compiler/build differences between HEAD and this worktree.
- green: the old black observation is not reproducible on the current runtime/card/setup;
  treat the green pre-clear result as the reliable baseline and stop chasing
  `1cf82d033072` as known-good.

Result: **green still**. Fresh dmesg from the exact historical build:

```
f0   pre/post: pos=480 RDoff=WToff=01e0, xfbc=90369022
f360 pre/post: pos=480 RDoff=WToff=01e0, xfbc=90369022
```

This invalidates `1cf82d033072` as a currently reproducible known-black baseline.  The
historical `xfbc=157d157d` result was real in the handoff log, but it is not reproducible
now from the same source commit built with the current `.config` and booted on the current
card/rootfs.  Stop using that commit as a control point.  Current reliable facts are:

- EFB copy-clear works and can generate visible colours.
- Command submission drains fully.
- Primitive command streams consume FIFO bytes but do not visibly overwrite the copy-clear
  result under the current runtime setup.

Next test checks a possible ordering/race bug in the diagnostic itself.  `GX_CopyDisp` with
`clear=true` clears EFB **after** the copy operation.  The previous diagnostics placed:

```
copy-clear green -> draw primitive -> final copy
```

inside one FIFO.  If the copy-clear operation is delayed internally relative to following
primitive work, the green clear could wipe out the primitive before the final copy, making
all primitive tests falsely appear green.

The new diagnostic submits the copy-clear as a separate FIFO first, waits through the usual
`gx_submit_cmds()` idle path, then submits the primitive draw plus final copy as a second
FIFO:

```
submit 1: copy-clear green
submit 2: draw direct-colour quad -> final copy
```

- red/green/blue or black/near-black: the single-FIFO diagnostic was self-invalidating;
  the copy-clear was wiping later primitive output.
- green: the primitive still does not visibly modify EFB even when the clear is isolated.

Deployed image `dbf638513479453469894f554eba8ae91dd89c6cf7c4db72c8d06be5e3a8fdd7`
contains this split-submit copy-clear/draw diagnostic.  Awaiting hardware result.

Result: **green still** visually. Fresh dmesg:

```
submit 1 f0: pos=64  RDoff=WToff=0040
submit 2 f1: pos=416 RDoff=WToff=01a0, xfbc=7e417e30
f360 submit 2: pos=416 RDoff=WToff=01a0, xfbc=7e417e30
```

The split-submit diagnostic rules out the single-FIFO `COPY_CTRL_CLEAR` ordering bug:
isolating the copy-clear did not make the primitive colour appear.  The center sample did
change from the earlier green signatures (`72487238`/`90369022`) to `7e417e30`, but that
still decodes as green-ish YUYV and remains unchanged between red and blue `vcol` phases.
So the primitive path still does not produce visible vertex-colour output.

Next diagnostic keeps the split-submit isolation but removes the vertex-colour dependency
again:

```
submit 1: copy-clear green
submit 2: 0 colour channels, TEV constant white, position-only quad -> final copy
```

This uses `GX_COLOR_NULL` for raschan, direct XY positions only, `numcolchans=0`, and a
constant-white TEV colour.  It retests the no-channel/constant-white primitive path now
that the copy-clear is isolated in a separate FIFO submit.

- white: primitive coverage and PE writes work when colour-channel state is removed.
- green: the primitive path still does not visibly modify EFB; move to lower-level FIFO
  byte comparison / XF or PE state diagnostics.

Deployed image `aa4b69755b31a6c62d742cd09924427a9e6de5ee3a25329c0d9de7a5929912fc`
contains this split-submit constant-white primitive diagnostic.  Awaiting hardware result.

Result: **green** visually.  This rules out vertex colour/channel state as the reason
primitive output is not visible: even a position-only quad with constant-white TEV did not
overwrite the proven green copy-clear.

Next diagnostic removes the recurring clear.  The previous split-submit tests still issued
the green copy-clear every frame, so a late or reordered primitive write could be masked by
the next frame's clear.  The new image seeds EFB/XFB with green on frame 0 only:

```
frame 0: copy-clear green
all frames: 0 colour channels, TEV constant white, position-only quad -> final copy
```

- white after the first frame: primitive writes were happening, but repeated clear/copy
  was masking them.
- green forever: the primitive path is still not modifying EFB, even without recurring
  clear interference.

Deployed image `4fadd40dfdbaa1595d79d632d3e5357a3d45ec11d80d7aad572ec3c0a086bde9`
contains the one-shot-clear diagnostic.

Result: screen started green, then developed visible spots/noise. Fresh dmesg:

```
f0 clear submit:     pos=64  RDoff=WToff=0040
f1 primitive+copy:   pos=384 RDoff=WToff=0180, xfbc=72487238
f2 primitive+copy:   pos=384 RDoff=WToff=0180, xfbc=72487238
f3 primitive+copy:   pos=384 RDoff=WToff=0180, xfbc=72487238
f360 primitive+copy: pos=384 RDoff=WToff=0180, xfbc=72496939
```

Interpretation: the primitive-only frames are no longer completely invisible; repeated
green clears were masking or dominating the result.  FIFO still drains cleanly, and the
center XFB sample changes slightly by f360, but output is noisy/partial rather than stable
white.  This points at primitive/raster/PE completion or state, not CP submission.

Next diagnostic splits the primitive submit and final copy submit:

```
frame 0: copy-clear green
all frames:
  submit A: 0 colour channels, TEV constant white, position-only quad
  submit B: final EFB->XFB copy, clear=false
```

- white/stable: same-FIFO primitive+copy was racing the raster/PE backend.
- green with spots/noise: primitive writes themselves are partial/unstable; continue into
  geometry/viewport/scissor or PE state.
- green unchanged: the visual noise may have been unrelated or intermittent.

Deployed image `4d748314afcf6f3b939dbac45f9ebf351b6d702bb8e192a90b7c12875dea0c1e`
contains the split primitive/copy diagnostic.

Result: **green with some noise** visually. Fresh dmesg:

```
f0 clear submit: pos=64  RDoff=WToff=0040
f1 primitive:    pos=352 RDoff=WToff=0160
f2 copy:         pos=32  RDoff=WToff=0020, xfbc=72487238
f360 copy:       pos=32  RDoff=WToff=0020, xfbc=72496939
```

Interpretation: splitting primitive and copy submits did not make the output stable white,
so same-FIFO copy timing is not sufficient to explain the noisy/partial primitive result.
The primitive submit and copy submit both drain cleanly.  Continue on primitive assembly,
geometry/viewport/scissor, or PE state.

Next diagnostic keeps the one-shot green seed and split primitive/copy submits, but
replaces the full-screen `GX_QUADS` rectangle with two explicit `GX_TRIANGLES`:

```
frame 0: copy-clear green
all frames:
  submit A: constant-white position-only rectangle as two triangles
  submit B: final EFB->XFB copy, clear=false
```

- stable white: the issue was GX_QUADS primitive assembly or quad-specific setup.
- green with noise: primitive assembly type is not the main cause; continue toward
  viewport/projection/scissor or PE update state.

Deployed image `15a132d9e4694a93a472110a6f17ee7ed5658df97cabbf1b45e4764a2bb733a2`
contains the two-triangle rectangle diagnostic.

Result: **green and then noise** visually.  Rootfs `dmesg.txt` was not mounted at the
usual path when the card returned, but this visual result matches the one-shot/split-submit
noise pattern and rules out `GX_QUADS` as the main cause.

Next diagnostic keeps the one-shot green seed and split primitive/copy submits, but removes
the pixel-space rectangle projection path:

```
frame 0: copy-clear green
all frames:
  submit A: identity orthographic projection + oversized clip-space triangles
  submit B: final EFB->XFB copy, clear=false
```

The primitive submit uses two opposite-winding triangles with vertices `(-4,-4)`, `(4,-4)`,
and `(0,4)`, so it should cover the viewport regardless of rectangle edge math and should
also survive stale cull/front-face state.

- stable white: the pixel-space projection or rectangle coverage math was wrong.
- green with noise: projection/coverage is not the main cause; continue toward PE state,
  scissor/field mask, or a missing low-level raster initialization bit.

The clip-space image caused a no-HDMI/no-signal boot report.  Card checks showed no
filesystem corruption: read-only `fsck.vfat` and `e2fsck -fn` both passed, boot files were
present, and `zImage.ngx` matched the local image.  The SD was rolled back to commit
`78706171323d` in a temporary worktree and deployed as checksum
`772473dcdfb06786e3ba4be83b2caaa3867e87446c9b2b269e8b16dd85ee15fd`.

Rollback result: **green and then larger than normal pixel noise**.  This confirms the
card is bootable and the noise scale changes with primitive/projection setup.  That makes
the noise more likely to be tied to raster/PE state than random card corruption.

Next diagnostic asks whether the noisy output obeys a shape.  It returns to the booting
pixel-space two-triangle rectangle setup, but shrinks the scissor to a centered half-width,
half-height box:

```
frame 0: copy-clear green
all frames:
  submit A: constant-white pixel-space rectangle as two triangles, centered scissor box
  submit B: final EFB->XFB copy, clear=false
```

- noise/white confined to the centered box: primitive rasterization is controllable; build
  outward from this state.
- noise still covers the whole screen: the visible noise is not obeying scissor/primitive
  coverage and may be copy/PE side effect.
- no noise: the noise depends on full-screen setup or edge coverage.

Result: **green screen with slightly smaller noise**.  Fresh dmesg showed clean FIFO drain
through f360 and the centre XFB sample changing slightly by f360:

```
f360 pre:  SR=0008 RD=0000 WT=0020 pos=32
f360 post: SR=000c RDoff=0020 WToff=0020
f360 diag=white xfb0=90369122 xfb1=90369022 xfbc=7e427531
```

This is a weak positive signal: the centered half-screen scissor affected the noise scale,
but the visual result was not obviously shaped.  Next diagnostic tightens the scissor to a
narrow centered vertical stripe while keeping the same one-shot green seed and pixel-space
two-triangle primitive.  Expected result:

- vertical stripe of noise/white: scissor controls primitive output; proceed with raster/PE
  state from this known-controlled shape.
- green with reduced/no noise: the primitive output obeys scissor but remains unstable or too
  sparse to see clearly at narrow coverage.
- full-screen noise: the visible corruption is not governed by primitive scissor coverage.

Result: **full-screen slightly large noise**, smaller than the first full-screen noise but not
shaped into the narrow vertical stripe.  Fresh dmesg again showed clean FIFO drain through
f360 and the same centre XFB signature as the half-screen scissor test:

```
f360 pre:  SR=0008 RD=0000 WT=0020 pos=32
f360 post: SR=000c RDoff=0020 WToff=0020
f360 diag=white xfb0=90369122 xfb1=90369022 xfbc=7e427531
```

This rules against primitive scissor coverage being the mechanism that shapes the visible
noise.  Next diagnostic removes the primitive submit entirely after the one-shot green seed:

```
frame 0: copy-clear green
all frames:
  submit only final EFB->XFB copy, clear=false
```

- stays green: the primitive/state submit is the source of the noise.
- develops noise anyway: repeated copy or EFB persistence is corrupting the output without
  any primitive draw.

Result: **still has noise**.  Fresh dmesg confirmed the copy-only diagnostic was active:
frame 0 submitted the 64-byte green copy-clear, then frames 1+ submitted only the 32-byte
EFB->XFB copy.  The centre sample still changed by f360:

```
f1 pre:    SR=0008 RD=0000 WT=0020 pos=32
f360 pre:  SR=0008 RD=0000 WT=0020 pos=32
f360 diag=white xfb0=ad2b9146 xfb1=90379022 xfbc=7e427531
```

This proves primitive commands are not required for the visible noise.  Next diagnostic
submits **nothing** after the one-shot green copy-clear:

```
frame 0: copy-clear green
all later frames: no GX submit; only CPU-side XFB sample logging
```

- stays green: repeated EFB->XFB copy was reading unstable/stale EFB contents.
- develops noise: visible corruption is outside the repeated GX command stream, likely VI/XFB
  scanout, XFB memory overlap, or something else writing the XFB.

Result: **vertical bars with noise**.  Fresh dmesg from the no-submit build showed the CPU
readback of `__va(xfb_phys)` was completely stable from frame 0 through f360 even though the
visible output was not:

```
f0   diag=no-submit xfb0=158d2e77 xfb1=10753e83 xfbc=1a7d1088
f360 diag=no-submit xfb0=158d2e77 xfb1=10753e83 xfbc=1a7d1088
```

That means primitive commands and repeated copy commands are both unnecessary for the
visible bars/noise.  Next diagnostic bypasses GX entirely in the RGB565 accelerated IRQ
path and CPU-fills the actual `fb_mem` ioremap used by the known-good software transcode
with a solid RGB565-green converted through `rgbrgb16toycbycr()`.

- stable green: VI scanout and `fb_mem` are correct; the GX copy path was writing or exposing
  the wrong XFB contents.
- bars/noise: VI is not scanning the `fb_mem` address/stride/page we think, or something else
  is changing VI scanout state after setup.

Result: **stable green**.  The SD was pulled before the normal 20-second dmesg append, but
the visual result is decisive: CPU-filling `fb_mem` from the DI1 path makes the display
stable.  This proves VI scanout, the `fb_mem` ioremap, and the YUYV green pattern are good.

Next diagnostic gives GX one chance to run the existing green copy-clear, samples the real
`fb_mem` mapping immediately after that GX submit, then CPU-fills green so the visible output
stays readable:

```
DI1:
  gcn_gx_blit_fb_rgb565()  # frame 0 green copy-clear, later frames no-submit
  log fb_mem sample as post-gx-pre-cpu-fill
  CPU-fill fb_mem green
  log fb_mem sample as post-cpu-fill
```

- `post-gx-pre-cpu-fill` is not green while `post-cpu-fill` is green: GX is not writing the
  scanned XFB correctly.
- both are green: the earlier bars were from not keeping `fb_mem` refreshed, or from reading
  the wrong alias in gcn-gx diagnostics.

Result: the CPU fallback stayed visibly green, but the pre-fallback samples proved GX did
not write the requested direct primitive colour into the scanned XFB.  Same-frame direct
green draw+copy consumed the FIFO (`RDoff == WToff`) but produced stale/noisy-looking XFB
samples before the CPU fill:

```
gcn-gx: f0 pre: SR=0008 RD=0000 WT=01a0 pos=416
gcn-gx: f0 post: SR=000c RDoff=01a0 WToff=01a0
gcn-gx: f0 diag=direct-green-copy xfb0=168d2e77 xfb1=10753e83 xfbc=1a7d1088
gcnfb: f0 post-gx-pre-cpu-fill fb0=168d2e77 fb1=10753e83 fbc=1a7d1088
gcnfb: f0 cpu-fill-green pattern=a52ba515
```

Split draw/copy across frames was then tested to rule out an ordering/race issue where
the copy runs before the primitive lands in EFB.  Frame 0 drew a direct green primitive
with CPU fill skipped; frame 1 copied EFB to XFB and then resumed CPU green fill:

```
gcn-gx: f0 diag=draw-f0-copy-f1 xfb0=00800080 xfb1=00800080 xfbc=00800080
gcnfb: f0 post-gx-pre-cpu-fill fb0=00800080 fb1=00800080 fbc=00800080
gcnfb: f0 cpu-fill-green skipped
gcn-gx: f1 diag=draw-f0-copy-f1 xfb0=178c2f76 xfb1=10782377 xfbc=1a7e238f
gcnfb: f1 post-gx-pre-cpu-fill fb0=178c2f76 fb1=10782377 fbc=1a7e238f
```

Repeating the split test with a white primitive produced the same result: frame 0 sampled
as uniform `00800080`, while the frame-1 EFB copy did not become white:

```
gcn-gx: f0 diag=draw-white-f0-copy-f1 xfb0=00800080 xfb1=00800080 xfbc=00800080
gcnfb: f0 post-gx-pre-cpu-fill fb0=00800080 fb1=00800080 fbc=00800080
gcnfb: f0 cpu-fill-green skipped
gcn-gx: f1 diag=draw-white-f0-copy-f1 xfb0=158d2f77 xfb1=10782377 xfbc=1a7d1088
gcnfb: f1 post-gx-pre-cpu-fill fb0=158d2f77 fb1=10782377 fbc=1a7d1088
gcnfb: f1 cpu-fill-green pattern=a52ba515
gcnfb: f1 post-cpu-fill fb0=a52ba515 fb1=a52ba515 fbc=a52ba515
```

Current diagnostic, committed after the white result, is a no-submit control:

```
frame 0: submit no GX commands; gcnfb still skips CPU fill
frame 1: copy whatever EFB already contains to XFB, then resume CPU green fill
```

- frame 0 still reads `00800080`: the split primitive frame-0 black sample was just initial
  XFB state/current scanout contents, not evidence that the primitive path rendered black.
- frame 0 reads stale boot/noise data instead: the primitive draw submit was correlated with
  the uniform black sample even though colour was not correct.
- frame 1 gives the baseline EFB->XFB copy result without any frame-0 primitive.

Result: visual output was stable green from the CPU fallback, and the frame-0 no-submit
sample was still `00800080`:

```
gcn-gx: f0 diag=no-submit-f0-copy-f1 xfb0=00800080 xfb1=00800080 xfbc=00800080
gcnfb: f0 post-gx-pre-cpu-fill fb0=00800080 fb1=00800080 fbc=00800080
gcnfb: f0 cpu-fill-green skipped
gcn-gx: f1 diag=no-submit-f0-copy-f1 xfb0=b497eb3a xfb1=d55e8677 xfbc=a682d382
gcnfb: f1 post-gx-pre-cpu-fill fb0=b497eb3a fb1=d55e8677 fbc=a682d382
gcnfb: f1 post-cpu-fill fb0=a52ba515 fb1=a52ba515 fbc=a52ba515
```

This proves the previous split-primitive frame-0 black value was baseline XFB/current scanout
state, not evidence that primitive output reached EFB.  Frame 1 remains a stale/noisy
baseline copy from EFB with no primitive.

Current diagnostic is a four-phase differential EFB test:

```
frame 0: GX copy with clear=true after setting copy-clear colour to green; gcnfb skips CPU fill
frame 1: copy seeded EFB to XFB; then CPU-fill XFB green
frame 2: draw direct RGBA8 white primitive into EFB only; then CPU-fill XFB green
frame 3: copy EFB to XFB; then CPU-fill XFB green
```

- frame 1 pre-CPU sample green and frame 3 pre-CPU sample green: EFB copy-clear seeding
  works, but the direct primitive did not overwrite EFB.
- frame 1 pre-CPU sample green and frame 3 pre-CPU sample white: direct primitive rendering
  works; the remaining issue is likely state/order around the full textured path.
- frame 1 not green: the assumed copy-clear EFB seed path is not actually controlled in this
  build, so debug copy-clear/copy timing before chasing primitive state.

Result: frame 1 was not green, so the one-shot copy-clear seed assumption is invalid in
the current code path:

```
gcn-gx: f0 diag=clear-green-f0-copy-f1-draw-white-f2-copy-f3 xfb0=216c698c xfb1=2c7b7d9f xfbc=a08e3b66
gcnfb: f0 post-gx-pre-cpu-fill fb0=216c698c fb1=2c7b7d9f fbc=a08e3b66
gcnfb: f0 cpu-fill-green skipped
gcn-gx: f1 diag=clear-green-f0-copy-f1-draw-white-f2-copy-f3 xfb0=ad22c95e xfb1=ad269152 xfbc=7e417e30
gcnfb: f1 post-gx-pre-cpu-fill fb0=ad22c95e fb1=ad269152 fbc=7e417e30
gcn-gx: f2 diag=clear-green-f0-copy-f1-draw-white-f2-copy-f3 xfb0=a52ba515 xfb1=a52ba515 xfbc=a52ba515
gcn-gx: f3 diag=clear-green-f0-copy-f1-draw-white-f2-copy-f3 xfb0=ad22c95e xfb1=ad269152 xfbc=7e427531
```

Frame 2 is CPU fallback green, not GX primitive output.  Frame 3 returns to the same
green-ish/noisy copy signature as frame 1.  Do not use one-shot copy-clear as an EFB seed.

Current diagnostic retests the historical solid-colour path directly:

```
frames 0-3: gx_set_copy_clear_rgb(0,255,0) + GX copy with clear=true
later frames: no GX submit; CPU green fallback only
```

The key distinction from the failed four-phase test is that every early frame uses
`clear=true`, matching the old diagnostic that visually produced solid red/green/blue.

- f1/f2/f3 pre-CPU samples become `a52ba515` or otherwise uniform green: repeated
  `clear=true` copies are a special working path; `clear=false` copy after clear is the
  suspect.
- f1/f2/f3 remain the same noisy signatures: the historical copy-clear colour result is not
  reproducible under the current runtime/logging setup, and primitive tests need a different
  controlled XFB/EFB baseline.

Result: repeated `clear=true` is reproducible and controlled, but it does **not** match the
CPU fallback's `a52ba515` RGB565-green conversion.  Frames 1-3 produce a stable GX full-green
copy signature:

```
gcn-gx: f0 diag=repeated-green-copy-clear xfb0=178c2f76 xfb1=10782377 xfbc=1a7d1088
gcnfb: f0 post-gx-pre-cpu-fill fb0=178c2f76 fb1=10782377 fbc=1a7d1088
gcnfb: f0 cpu-fill-green skipped
gcn-gx: f1 diag=repeated-green-copy-clear xfb0=90369122 xfb1=90369022 xfbc=72487238
gcnfb: f1 post-gx-pre-cpu-fill fb0=90369122 fb1=90369022 fbc=72487238
gcnfb: f1 post-cpu-fill fb0=a52ba515 fb1=a52ba515 fbc=a52ba515
gcn-gx: f2 diag=repeated-green-copy-clear xfb0=90369122 xfb1=90369022 xfbc=72487238
gcnfb: f2 post-gx-pre-cpu-fill fb0=90369122 fb1=90369022 fbc=72487238
gcn-gx: f3 diag=repeated-green-copy-clear xfb0=90369122 xfb1=90369022 xfbc=72496939
gcnfb: f3 post-gx-pre-cpu-fill fb0=90369122 fb1=90369022 fbc=72496939
```

Important correction: `90369122/90369022` should be treated as controlled GX green from
full RGB `{0,255,0}` after EFB copy conversion.  `a52ba515` is only the CPU fallback's
YUYV value for its RGB565-green diagnostic pattern.  This means the old frame-separated
clear/copy result at the top of this document was not a failure; it was likely GX green
with different conversion math plus non-uniform centre/stale areas.

The failed four-phase test is now more specific: frame 1 used `clear=false` after frame 0
`clear=true` and produced `ad22...`, while repeated `clear=true` produces controlled
`9036...`.  Next useful test is:

```
frame 0: clear=true green
frame 1: clear=true green again, but also draw/copy variants later only after confirming
         whether clear=false or clear=true is required to observe the cleared EFB
```

Treat `clear=false` copy after a previous clear as suspect until a dedicated test proves
it can read the clear result.

Current diagnostic avoids `clear=false` and uses the known repeated `clear=true` path as
the readout:

```
frame 0: clear=true green   # copies stale EFB, then clears EFB green
frame 1: draw direct white primitive into EFB only
frame 2: clear=true green   # copies EFB before clearing it green again
```

Interpretation:

- frame 2 pre-CPU sample is GX green (`9036...`): the direct white primitive did not
  overwrite EFB, even when read out by the working `clear=true` path.
- frame 2 pre-CPU sample is white-derived YUV: primitive writes work; the earlier failures
  were due to `clear=false` copy/readback or test ordering.
- frame 2 is stale/noisy: even the repeated `clear=true` readout depends on more state than
  expected; return to pure copy-clear colour cycling before testing primitives.

Result: frame 2 read back the GX-green signature, not white:

```
gcn-gx: f0 diag=green-clear-f0-draw-white-f1-clear-read-f2 xfb0=178d2e76 xfb1=10792377 xfbc=1a7d1088
gcnfb: f0 post-gx-pre-cpu-fill fb0=178d2e76 fb1=10792377 fbc=1a7d1088
gcn-gx: f1 diag=green-clear-f0-draw-white-f1-clear-read-f2 xfb0=178d2e76 xfb1=10792377 xfbc=1a7d1088
gcnfb: f1 post-gx-pre-cpu-fill fb0=178d2e76 fb1=10792377 fbc=1a7d1088
gcn-gx: f2 diag=green-clear-f0-draw-white-f1-clear-read-f2 xfb0=90369122 xfb1=90369022 xfbc=7049703a
gcnfb: f2 post-gx-pre-cpu-fill fb0=90369122 fb1=90369022 fbc=7049703a
```

This proves the direct RGBA8 vertex-colour primitive did not overwrite EFB, even when the
result is read out through the working `clear=true` path.

Current diagnostic removes vertex colour and channel state entirely while keeping the same
working `clear=true` readout:

```
frame 0: clear=true green
frame 1: position-only quad with 0 colour channels, raschan=GX_COLOR_NULL, TEV d=GX_CC_ONE
frame 2: clear=true green readout
```

- frame 2 is white-derived YUV: primitive coverage/PE writes work; the direct colour/channel
  path was the problem.
- frame 2 remains GX green (`9036...`): even constant TEV output does not write EFB, so the
  failure is earlier/lower than colour channels, likely primitive coverage, position
  transform, rasterization, or PE write state.

Result: frame 2 did not become a clean white-derived output.  The centre sample remained
the known GX-green signature while the leading words were mixed:

```
gcn-gx: f0 diag=green-clear-f0-const-white-f1-clear-read-f2 xfb0=658fe880 xfb1=a542cf89 xfbc=eb8691a0
gcnfb: f0 post-gx-pre-cpu-fill fb0=658fe880 fb1=a542cf89 fbc=eb8691a0
gcn-gx: f1 diag=green-clear-f0-const-white-f1-clear-read-f2 xfb0=658fe880 xfb1=a542cf89 xfbc=eb8691a0
gcnfb: f1 post-gx-pre-cpu-fill fb0=658fe880 fb1=a542cf89 fbc=eb8691a0
gcn-gx: f2 diag=green-clear-f0-const-white-f1-clear-read-f2 xfb0=952ccf72 xfb1=7e5a7f8a xfbc=90369022
gcnfb: f2 post-gx-pre-cpu-fill fb0=952ccf72 fb1=7e5a7f8a fbc=90369022
```

This rules out vertex colour/channel state as the main blocker.  Primitive output is still
not reliably covering/writing EFB.

Current diagnostic keeps constant-white TEV and the working `clear=true` readout but changes
geometry/projection:

```
frame 0: clear=true green
frame 1: position-only oversized clip-space triangles, both windings, identity projection
frame 2: clear=true green readout
```

The goal is to remove pixel-space orthographic projection and stale front-face/cull winding
as variables.  If frame 2 still reads GX green in the centre, focus on raster/PE write state
rather than colour or projection setup.

Operational note: `/init-diag.sh` on the SD rootfs was briefly reduced from `sleep 20` to
`sleep 10`, but that cut off the f360 marker, which appears around 15 seconds.  It has
been restored to `sleep 20` so `/dmesg.txt` captures both early frames and f360.  This
edit was made directly on `/media/anolis/WII-LINUX-NGX1`; it is not part of the kernel
repo image.

### Host capture and SSH setup

A USB HDMI capture card is available as:

```
USB3.0 Video: USB3.0 Video (usb-0000:02:00.0-1)
  /dev/video0  video capture
  /dev/video1  UVC metadata
```

OBS works when set to the highest exposed capture mode.  For automation, repo tool
`tools/gx_capture_probe.py` can probe modes and save PPM frames without OpenCV.  A visible
preview plus sampled frame capture also works with ffmpeg SDL output:

```
mkdir -p /tmp/gx-capture-live
timeout 20s ffmpeg -hide_banner -loglevel warning \
  -f v4l2 -input_format mjpeg -video_size 1280x720 -framerate 60 -i /dev/video0 \
  -filter_complex 'split=2[rawview][rawsnap];[rawview]format=yuv420p[view];[rawsnap]fps=1/2,format=rgb24[snapout]' \
  -map '[view]' -f sdl 'Wii capture preview' \
  -map '[snapout]' -frames:v 10 /tmp/gx-capture-live/frame-%03d.ppm
```

This produced ten usable `1280x720` frames with stable metrics.  If OBS is open, ffmpeg
will fail with `Device or resource busy`.

The SD rootfs has been configured locally for Wi-Fi and key-based root SSH during debug
boots.  Do not commit wireless credentials or generated WPA files to the repo.  The only
repo-relevant detail is that `/init-diag.sh` on the SD now mounts `/dev`, starts `wlan0`,
waits briefly for DHCP, logs interface/route state to `/dmesg.txt`, starts `/usr/sbin/sshd`,
then continues the existing 20-second dmesg capture, LED blink, and console shell.

Rootfs backups were created with suffix `20260705-121632`.

### 2026-07-08 session: wifi/ssh dead end, resume GX bisection

The Wi-Fi/SSH effort above did not reach a working state.  Root cause, confirmed via
`init-diag.sh`'s status logging and `wpa_supplicant -dd` debug output: `wpa_supplicant`
authenticates and associates cleanly with the target AP (`status_code=0`), but the AP's
EAPOL Message 1 of the 4-way handshake never arrives -- zero `RX message`/`RX EAPOL` lines
in the debug log, and independently `ifconfig wlan0` shows `RX packets:0` for the entire
boot session despite management-frame-level auth/assoc clearly working.  `wpa_supplicant`
self-times-out after 10s and deauths with `reason=15` (4-Way Handshake timeout), repeating
across ~5 attempts.

Ruled out: wrong credentials (retested with cleartext password), AP-side ACL/config (works
for every other device on the same AP), weak signal (`-48dBm`), and `wpa_supplicant`
driver backend choice (`wext` alone failed differently -- scan itself never worked; both
plain `nl80211` and the `nl80211,wext` fallback chain from this rootfs's own
`/etc/wpa_supplicant/functions.sh` default scan/associate fine but hit the identical
zero-RX-data-frames wall).  Hardware is `BCM4318 rev.2` (`ssb: Found chip with id 0x4318,
rev 0x02`) via the `b43` driver in PIO mode (forced by SDIO transport, not the `pio`
module parameter), firmware `666.2`.  No confirmed root cause; this looks like a
kernel-level `b43`/`mac80211` data-frame RX bug specific to this chip, which would need
real driver-level debugging, not a config change, to take further.  Shelved in favour of
resuming the GX bisection; the physical SD-card-swap test loop remains the working method
for now.

`gcn_gx_init()` was temporarily commented out of `vifb_do_probe()` in `gcnfb.c` during the
wifi work (to keep the console visible instead of stuck on the GX diagnostic pattern).  It
has been restored (uncommitted revert nets to a no-op diff against this doc's prior state).

Current test resumes Test A from the session's test-strategy plan: the constant-white
diagnostic (`gx_setup_constant_white_state`, `gcn-gx.c`) draws an identity-projection
triangle at clip-space `+/-4.0`, far outside the standard `+/-1` clip volume, while
`GX_CLIP_ENABLE` is genuinely on (`XF 0x1005 = 0`, per the earlier polarity fix).  Every
other permutation tried across the whole bisection (color/TEV/scissor/winding/
quad-vs-triangle/XY-vs-XYZ) has failed to produce a visible EFB write -- consistent with
the XF clipper silently discarding the whole triangle before rasterization, a structural
cause rather than any single raster-state register.

Test: flip `XF 0x1005` from `0` to `1` (`GX_CLIP_DISABLE`), changing nothing else in the
diagnostic.

- Frame 2 (`clear=true` readout) becomes white-derived instead of the known `9036...`
  GX-green signature: confirmed -- the oversized clip-space geometry was being clipped
  away the whole time.  Next: build a production path with geometry that stays inside the
  real clip volume (a correct pixel-to-NDC projection, not permanently-disabled clipping).
- Frame 2 remains the GX-green signature: clip-volume rejection is ruled out.  Move to a
  static FIFO-byte-level audit against `libogc`'s `GX_Begin`/`GX_End` emission next, since
  every raster-state permutation has now been exhausted and the remaining candidates are a
  primitive-encoding bug (consumes FIFO bytes cleanly but describes zero real geometry) or
  something not yet identified in PE/copy addressing.

Commit `6002aa187cda` contains this diagnostic.  Deployed image SHA-256:

```text
e02ea422dde38ba0021fe166c27edcdb2d73b5cfc1a8edbe3b076bcd6b61fb00
```

Result: **still green**, ruling out clip-volume rejection.  Tested on a second,
independently-imaged SD card (see the wifi retest note below); the deployed
`BOOTWII/gumboot/zImage.ngx` checksum on that card was confirmed byte-for-byte identical
to the checksum above before trusting this result.  Frame 360 samples were contaminated by
the CPU-fallback pattern as expected (`xfbc=a52ba515`, the documented CPU-fill signature,
not a GX result -- see the `a52ba515` vs `9036...` distinction earlier in this doc); the
valid pre-contamination sample is frame 2:

```text
gcn-gx: f2 diag=green-clear-f0-const-white-f1-clear-read-f2 xfb0=ad23ca5d xfb1=91309039 xfbc=7e417e30
gcnfb: f2 post-gx-pre-cpu-fill fb0=ad23ca5d fb1=91309039 fbc=7e417e30
```

`xfbc=7e417e30` matches the documented "still green" signature family from the earlier
split-submit copy-clear/draw diagnostic (same exact value logged there and interpreted as
green-ish YUYV, unchanged across vertex-colour phases).  `GX_CLIP_DISABLE` did not produce
a white-derived readout.  Test A is ruled out: the oversized clip-space geometry was never
being rejected by the XF clipper.

Next step per the test-strategy plan: Test B, a static FIFO-byte-level audit of the
primitive draw call in `gx_draw_pos_quad` (`gcn-gx.c`) against `libogc`'s `GX_Begin`/
`GX_End` emission for the same `GX_TRIANGLES` configuration, since CP/GP has now been shown
to cleanly consume every primitive submission across every raster-state permutation tried
while never once producing a visible EFB write -- consistent with a primitive-encoding bug
(wrong opcode, vertex count, or VAT/VCD byte-layout mismatch causing the GP to parse the
command stream as describing zero real geometry) rather than any raster/PE/clip state.
This needs no new hardware boot; it can be done by inspecting the emitted bytes and
comparing against `/home/anolis/repos/libogc/libogc/gx.c`.

### Test B: FIFO-byte/encoding audit result, and a new structural-gap finding

Performed the static audit against `libogc`.  The primitive draw command itself
(`gx_draw_pos_quad`) checks out byte-for-byte: opcode `0x90` matches `GX_TRIANGLES`
exactly, the 16-bit big-endian vertex count matches `GX_Begin`'s `wgPipe->U16 = vtxcnt`,
and the per-vertex payload (two `f32`s, no color/tex data) matches `GX_Position2f32`
exactly, consistent with the currently active VAT0 (`pos cnt=XY, type=F32`) and empty VCD.
`GX_End()` is a no-op in libogc (no closing/padding token required).  Test B as originally
scoped -- a primitive-encoding bug -- is ruled out.

Widened the audit while there: `gx_submit_cmds()` is the identical shared function used for
both the (working) copy-clear submission and the (failing) primitive-only submission, so
the bug cannot be in the FIFO submission mechanism itself -- that would break copy-clear
too.  It has to be something specific to the primitive-draw FIFO's content.

Checked `GX_SetCullMode`/genMode bits `[15:14]` against our `genMode = 0x00000000`: cull is
explicitly `GX_CULL_NONE` (hw value 0) in our code, not a leftover "mini" state -- ruled
out, no test needed.

Found a genuine structural gap by diffing `__GX_InitGX()`'s full default-state sequence
against every BP register this driver has ever written: **BP 0x42** (`peCMode1`,
destination alpha) is never written anywhere in `gcn-gx.c`.  It sits directly between BP
0x41 (BLENDMODE, a confirmed pixel-discard trap per Known pitfalls below) and BP 0x43
(PE_CONTROL, which we do set) in the same PE control-register block.  libogc's
`__GX_InitGX()` explicitly initializes it via `GX_SetDstAlpha(GX_DISABLE, 0)`.  This is a
gap, not just an untested value -- every raster-state permutation tried across this entire
bisection shares it, since it has simply never been part of any diagnostic's init sequence.

Test: add `gx_load_bp_reg(0x42000000)` (dst-alpha disabled, matching libogc's default)
to `gx_setup_constant_white_state`, changing nothing else.  Also reverted `XF 0x1005` back
to `GX_CLIP_ENABLE` (Test A's ruled-out clip-disable is no longer needed) to keep this a
clean, single-variable test against the last confirmed baseline.

- Frame 2 becomes white-derived: found it -- an uninitialized dst-alpha register was gating
  pixel writes.  Next: understand the exact mechanism and set this correctly across all
  code paths, not just this diagnostic.
- Frame 2 remains the "still green" signature: ruled out.  Continue the `__GX_InitGX()`
  diff for other completely-untouched registers (candidates not yet checked: fog registers
  BP ~0xEE-0xFF, Z-texture BP ~0xF5 region though Z-compare is already disabled so this is
  lower priority, TEV swap-mode table BP 0xF6-0xFD).

Commit `d7af01f74d3a` contains this diagnostic.  Deployed image SHA-256:

```text
5561e87c75d4e8a25adc63156072d549c0c198240f5be50263f87b4c9a708d55
```

Result (checksum-verified on card): **new, different result -- neither predicted branch.**
Frame 2 (pre-CPU-fallback, the valid sample):

```text
gcn-gx: f2 diag=green-clear-f0-const-white-f1-clear-read-f2 xfb0=b52f8975 xfb1=d8807396 xfbc=1f781f75
gcnfb: f2 post-gx-pre-cpu-fill fb0=b52f8975 fb1=d8807396 fbc=1f781f75
```

`xfbc=1f781f75` is not the "still green" family (`9036...`/`7241...`/`7e41...`) seen across
every single test in this entire bisection -- it decodes (Y~=31, near-neutral chroma) to the
**"near-black"** signature, an exact hex match to a result logged much earlier in this
project (the `matsrc=VTX` ambient/material test) from a completely different colour-channel
configuration.  This is a real, measurable behaviour change: every prior test of this exact
no-channel/`CC_ONE` diagnostic (including the just-ruled-out clip-disable test) read back
"still green" (primitive produces zero effect).  BP 0x42 is the first change in this entire
session to make the primitive visibly write to EFB at all through this diagnostic path.

However the colour is wrong: TEV (`a=b=c=GX_CC_ZERO`, `d=GX_CC_ONE`, add, scale=1, bias=0,
clamp=on -- re-verified bit-for-bit against libogc's exact field layout this session) should
mathematically output pure white, not near-black.  This is the same unexplained "near-black
convergence regardless of intended colour" the project's history flagged early on (three
different colour-channel configs producing the same near-black result) before the
investigation pivoted to chasing the "no writes at all" problem -- this result reopens that
thread rather than resolving it.

Next test: swap TEV `d` from `GX_CC_ONE(12)` to `GX_CC_HALF(13)`, a one-bit register change,
to determine whether TEV programming is being respected at all:

- Mid-gray output (visibly distinct luma from both white and the near-black signature): TEV
  programming is respected; the near-black result is a real, separate bug in how this
  formula evaluates on real hardware, not a downstream override.
- Unchanged near-black readout regardless of the TEV `d` change: the TEV-computed colour is
  being overridden/attenuated by something entirely downstream -- redirect focus to the PE
  write path or the EFB->XFB copy/YUYV conversion, not TEV/raster state.

Commit `27c0bcf9eeec` contains this diagnostic.  Deployed image SHA-256:

```text
fc66b633a50b858bbaaf9d93523a1a20898e2c12a0645be39f17389ea5ffa2e1
```

Result (checksum-verified on card): **TEV programming is respected.**  Frame 2:

```text
gcn-gx: f2 diag=green-clear-f0-const-white-f1-clear-read-f2 xfb0=90369122 xfb1=ad2e903a xfbc=8040802e
gcnfb: f2 post-gx-pre-cpu-fill fb0=90369122 fb1=ad2e903a fbc=8040802e
```

`xfbc=8040802e` decodes to Y~=128 (mid-tone) -- a third, distinct value from both the
"still green" family and the near-black `1f781f75` from `d=GX_CC_ONE`.  This confirms the
"downstream override" branch is ruled out: TEV's `d` input genuinely changes the readout,
so the near-black result from `d=GX_CC_ONE` is a real, formula-specific behaviour, not
something ignoring TEV entirely.

Open puzzle: the relationship is inverted from naive expectation.  `d=GX_CC_ONE` (1.0)
produced *darker* output (Y~=31) than `d=GX_CC_HALF` (0.5, Y~=128) -- more should be
brighter, not less, if this were a simple linear scale.

Re-verified BP 0xC1 (alpha combiner) bit-for-bit this session (it had only been decoded
loosely during the CC_HALF analysis): all four inputs (`a`,`b`,`c`,`d`) are genuinely
`GX_CA_ZERO(7)`, formula ADD, scale=1, bias=0, clamp=on, output to TEVPREV -- confirmed
correct, matching the "alpha=ZERO" intent exactly.  Alpha is ruled out as a factor in the
brightness puzzle.

Leading hypothesis for the inversion: TEV's internal fixed-point math clamps or wraps
specifically at/above the 1.0 boundary, rather than `GX_CC_ONE`'s input-select encoding
being wrong (already verified against libogc: value 12, matches exactly).  Test: reach the
same *intended* final brightness (1.0) via a different computational path -- keep
`d=GX_CC_HALF` (0.5, confirmed safe) but change TEV scale from `GX_CS_SCALE_1` to
`GX_CS_SCALE_2` (2x), so `0.5 * 2 = 1.0` through the scale multiplier instead of selecting
the ONE constant directly.

- Near-black again: confirms a hardware/fixed-point clamp or overflow at the 1.0 boundary
  regardless of which path reaches it.
- Genuine bright/white output: isolates the bug specifically to `GX_CC_ONE`'s input-select
  path, not general 1.0-valued TEV output.

Commit `c35cc26f7099` contains this diagnostic.  Deployed image SHA-256:

```text
ee48fb8fcb816bdcecaa3075df572dc4a89651f1baf271f8ccea11a1e30d105a
```

Result (checksum-verified on card): **neither predicted branch.**  Frame 2:

```text
gcn-gx: f2 diag=green-clear-f0-const-white-f1-clear-read-f2 xfb0=90369122 xfb1=91389022 xfbc=72487238
gcnfb: f2 post-gx-pre-cpu-fill fb0=90369122 fb1=91389022 fbc=72487238
```

`xfbc=72487238` is back to the exact "still green" signature -- the primitive produced
**zero visible effect**, matching the original pre-BP-0x42 baseline.  Full picture across
the three TEV variants tried this session, all otherwise identical (no-channel, position-
only, oversized clip-space triangles, clip enabled):

| `d` input | scale | Result |
|-----------|-------|--------|
| `GX_CC_ONE` (1.0) | 1x | near-black, `xfbc=1f781f75` (writes, wrong colour) |
| `GX_CC_HALF` (0.5) | 1x | mid-tone, `xfbc=8040802e` (writes, roughly-correct colour) |
| `GX_CC_HALF` (0.5) | 2x | still green, `xfbc=72487238` (no write at all) |

This complicates the "1.0-boundary clamp" hypothesis: a pure value-clamp bug should still
produce some visible (if wrong) pixel output, as the `d=ONE` case did, not complete
invisibility matching the original no-write signature.  The scale-field bit math was
re-checked against `GX_SetTevColorOp`'s exact shift/mask (`tevscale`=bits[21:20],
`tevregid`=bits[23:22]) and is correct -- `0xC018FFFD` does encode what was intended, so
this isn't a simple encoding mistake.

Open question for next session: why does reaching intended brightness 1.0 via
`HALF`+`scale=2x` produce *no write at all*, while reaching a lower intended brightness
(0.5) via `HALF`+`scale=1x` writes correctly, and reaching 1.0 directly via the `ONE`
constant writes but with wrong colour?  Three genuinely different outcomes from three
closely related configurations -- this needs a fresh look rather than another blind
single-bit variation.  Candidates: re-confirm `HALF`+`scale=1x` still reproduces the
mid-tone result (control/sanity check, rule out build or card-state drift); try reaching
1.0 via `GX_TB_ADDHALF` bias instead of scale (`0.5 + 0.5 = 1.0` through yet another
independent path); or step back from single-bit TEV micro-variation entirely and consider
whether this fine a grain of detail is the best use of further hardware cycles right now.

User raised a good hypothesis to check before trusting any of these three results further:
could this be silently falling through to the `fbdev` software transcode path
(`vi_transcode_RGB565`) rather than genuinely exercising the GX code path?  Checked
directly and ruled out: `gcnfb: probe done vfb_format=0x50424752 gx=1` confirms
`gx_accel_ready` was true, no "GX accel unavailable" fallback message appears anywhere in
the log, and -- more decisively -- the `gcn-gx: fN diag=...` lines themselves are only ever
printed from inside `gcn_gx_blit_fb_rgb565`, which the software fallback path never calls
at all.  The scale=2x "still green" result was a genuine GX-hardware outcome, not a
software-fallback artifact.

Chose the control/sanity check first: re-deployed the exact `d=GX_CC_HALF, scale=1x`
configuration that gave the mid-tone result, completely unchanged, to confirm it's still
reproducible before trusting any of the three results further.

Commit `1fe09353a70a` contains this control test.  Deployed image SHA-256:

```text
9ef71ac8461d51016c94457d316cf89dc73104ae10ed487564e4b327ccd1d8cd
```

## MAJOR FINDING: non-determinism confirmed -- treat prior single-boot conclusions cautiously

Result (checksum-verified identical to the boot that produced mid-tone): **did not
reproduce.**  Frame 2:

```text
gcn-gx: f2 diag=green-clear-f0-const-white-f1-clear-read-f2 xfb0=ad22c95e xfb1=ad269052 xfbc=72487238
gcnfb: f2 post-gx-pre-cpu-fill fb0=ad22c95e fb1=ad269052 fbc=72487238
```

`xfbc=72487238` -- the "still green" signature, not the `8040802e` mid-tone this exact
checksum-verified binary produced last time.  **The exact same command stream, sent to the
same hardware, produced a different visible outcome on a different boot.**  Frame 0/1's
`xfb0`/`xfb1` corner samples also differ from the previous boot of this same binary
(`24845781`/`12703f95` here vs different values before) -- consistent with genuine run-to-
run variance, not a fluke read.

This is more significant than the TEV brightness puzzle itself.  It means the underlying
assumption behind every binary test in this bisection -- same registers in, same result
out -- does not reliably hold on this hardware/setup.  Practical implications:

- The confusing three-way TEV pattern from the last two sessions (`ONE`/1x=near-black,
  `HALF`/1x=mid-tone, `HALF`/2x=invisible) may not be three genuinely different causal
  outcomes at all -- it could be the same non-determinism surfacing differently across
  configs that don't actually matter causally.
- Every single-boot "still green" / "ruled out" conclusion earlier in this document should
  now be held more loosely.  A single negative result no longer safely means "this
  register/config doesn't matter" -- it could just be this run's roll of whatever is
  non-deterministic.
- Before any further register-level bisection is trustworthy, the non-determinism itself
  needs characterizing: is it a genuine hardware/timing race (e.g. in `gx_submit_cmds`'s
  fixed `udelay` waits), leftover "mini" memory state that varies boot-to-boot, or
  something else?  Candidate next step: reboot the *same* binary several times in a row
  (no code changes) and tabulate how often each signature appears, to characterize the
  failure rate/pattern before trusting any single result again.

### Follow-up: the "non-determinism" was a sampling artifact, not real hardware randomness

Repeated the `d=GX_CC_HALF, scale=1x` control binary (checksum
`9ef71ac8461d51016c94457d316cf89dc73104ae10ed487564e4b327ccd1d8cd`) 4 more times: **green
every time**, all visually confirmed on the TV.  Combined with the original mid-tone
reading never having been visually confirmed (only read from the dmesg log), this already
made the mid-tone result suspect.

Checked whether the near-black result (BP 0x42 + `d=GX_CC_ONE`, the one that started this
whole thread) had ever been visually confirmed either.  It had not -- the user's only
directly-observed black screen was much earlier in this project's history (around the
`matsrc=VTX`/ambient-material era), before the proven green/red/blue copy-clear cycle test;
every boot since then has visually shown green with "not much change."  Both the
near-black and mid-tone readings were therefore based purely on decoding 2-3 sampled pixel
values (`xfb0`/`xfb1`/`xfbc`) from the dmesg log, never independently confirmed against the
actual screen.

Used the project's existing HDMI capture rig (`/dev/video0`, confirmed present) to get an
actual full-frame ground-truth capture of the current (`d=GX_CC_HALF, scale=1x`) diagnostic
state instead of trusting point samples.  Result: **a single, completely uniform green
fill across the entire frame** -- no black patch, no gray patch, no visible variation
anywhere on screen.

This resolves the "non-determinism" scare: it was not genuine hardware/timing randomness.
The whole screen has been uniformly, stably green (matching the "still green" / no visible
write signature) across all of this session's tests.  The two "different" log readings
(near-black `1f781f75`, mid-tone `8040802e`) were most likely noise or anomalies in the
2-3 sampled pixel values themselves -- not evidence that the primitive ever produced a
real, visible, distinguishable colour change on screen.  **Retract those two results as
unreliable.**  The reliable, ground-truth-confirmed state for every TEV variant tried this
session (`ONE`/1x, `HALF`/1x, `HALF`/2x) is: still green, no visible primitive write.

Methodology change going forward: any claimed "different" result from register-level pixel
sampling should be confirmed with a full-frame HDMI capture (this rig, or
`tools/gx_capture_probe.py`) before being logged as a real finding -- 2-3 point samples
have now demonstrably produced false-different readings twice in one session.  This also
means the earlier corner-pixel unreliability finding (`xfb0`/`xfb1` at the (0,0)
scissor/viewport boundary) may extend to the centre sample (`xfbc`) too, at least under
some conditions -- treat single-pixel dmesg samples as a fast first look, not proof, from
now on.

### Fresh angle: hardware perf counters, and comparing against a known-working reference

Checked whether `GX_Init()`'s other one-time perf-counter-related writes (`CP 0x20`,
`XF 0x1006`, `BP 0x23`/`0x24`/`0x67`, found while widening the earlier register-gap audit)
were relevant.  They are not -- confirmed via `GX_SetGPMetric()` in libogc that these are
pure hardware performance-counter configuration (triangle/vertex/clock counts for
profiling), with zero effect on actual rendering output.  Ruled out by direct reasoning.

This did surface something genuinely useful though: `GX_PERF0_TRIANGLES_PASSED`, a
hardware counter that directly answers "did our triangles survive culling and reach the
rasterizer" -- completely independent of colour output, sidestepping the whole TEV/colour
question and the pixel-sampling reliability problem above entirely.  Added: select this
counter (BP 0x23, libogc's exact encoding `0x23009E7F`) and clear it before the frame-1
primitive draw, then read back the 32-bit count from CP registers 32/33 (byte offsets
0x40/0x42, matching libogc's `GX_ReadGPMetric()` indexing) after the draw completes.
Commit `1b460675a7b6`, deployed image SHA-256:

```text
47018c77d89411e094485e5a73e0fac6bac5ea51f329681cc45e21cc3866dee4
```

Separately, fetched the actual devkitPro `wii-examples` canonical GX triangle demo
(`graphics/gx/triangle/source/triangle.c`) as a known-working reference to compare against,
per the session's new direction of copying a proven-correct path rather than continuing
piecemeal register hypotheses.  Notable difference found: the working example enables
Z-testing (`GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE)`) with `GX_POS_XYZ` vertices, while our
diagnostic explicitly disables Z (`BP 0x40 = 0x40000000`) with XY-only vertices.  Confirmed
our copy-clear already writes `BP 0x51 = 0x00ffffff` (Z-clear to far/max) so there's no
obvious confound in testing Z-enabled with XYZ positions -- this exact combination (Z
enabled *and* XYZ positions together) has not been tried; earlier XYZ-position tests in this
project used Z disabled.  Also noted (not yet acted on): the working example uses `INDEX8`
addressing via `GX_SetArray`/CP array-base-and-stride registers, not our `DIRECT` addressing
-- not considered relevant since direct mode is a distinct, valid path, but flagged in case
it becomes relevant later.

Result (checksum-verified on card): **inconclusive, not trustworthy.**

```text
gcn-gx: f1 perf0=triangles_passed count=3907289
```

`count=3907289` is physically implausible for a draw of 2 triangles.  This was the first-ever
use of this readback mechanism in this driver, with no prior verified control test, so there
is no way to know whether the `CP_REG_CLR` clear write, the CP register 32/33 offsets, or the
`BP 0x23` selector encoding is actually correct versus reading an unrelated counter (a clock-
cycle count would plausibly be in the millions).  Do not read anything into this number either
way.  If this mechanism is revisited, it needs a proper control first: confirm the counter
reads exactly 0 with the clear applied and *no* draw submitted, before trusting any non-zero
reading as meaningful.  Deprioritized in favour of the Z-enable + XYZ-position test below.

Deployed the Z-enable + XYZ-position test described above.  Commit `fd2ab314bb5c`, deployed
image SHA-256:

```text
1c4147662bc1e8f786e64728f59ef0c5a8a836ff89edab6b796f06de7e20861b
```

Changes: `BP 0x40 = 0x40000017` (`GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE)`), `VAT0 =
0x40000009` (`GX_POS_XYZ` instead of XY), `gx_draw_pos_quad` now emits `Z=0` per vertex to
match.

First boot attempt produced a suspiciously short `dmesg.txt` (35 bytes, only the initial
`init-diag: start` marker) and the user reported the console boot-looping during a phone
call.  Given the severity, the change was reverted (`779ba45c3e88`) to restore a known-safe
state, then un-reverted (`36b41231e447`, confirmed byte-identical source to `fd2ab314bb5c`
via `git diff`) for a clean retry, since a single short log during an unattended boot-loop
observation wasn't conclusive proof this specific change caused a crash.

Retry result (same checksum-verified binary, second independent boot): **no hang** -- full
254-line log, completed normally.  This strongly suggests the earlier short log was an
early card pull during the phone-call session, not a real crash caused by this change.

```text
gcn-gx: f1 perf0=triangles_passed count=0
gcn-gx: f2 diag=green-clear-f0-const-white-f1-clear-read-f2 xfb0=3556cfa7 xfb1=b33eda66 xfbc=8040802e
```

`triangles_passed=0` is a far more plausible reading than the earlier `3907289` (likely
garbage from a boot that was mid-crash) -- consistent with, though not proof of, "triangles
still never reach the rasterizer."  The perf-counter mechanism still has no independent
zero-draw control test, so this remains suggestive rather than confirmed.

`xfbc=8040802e` reappeared -- the exact same "mid-tone" value retracted earlier this session
as sampling noise.  Per the methodology correction, this was **not** trusted at face value:
asked the user to check the actual screen, and it was confirmed **green**, matching every
other visual observation this session and confirming `xfbc=8040802e` really is sampling
noise, not a real state, for a second independent time.

**Conclusion: Z-enable + XYZ positions, matching the actual working devkitPro reference
exactly, does not fix the bug.**  The primitive still produces no visible EFB write.

### Perf-counter mechanism validation: zero-draw control test

Before trusting the `GX_PERF0_TRIANGLES_PASSED` counter for anything further, it needs
independent validation.  Deployed a zero-draw control: submit only the counter select+clear
(`BP 0x23009E7F` + `cp_write(CP_REG_CLR, 4)`), with the triangle draw itself skipped
entirely, then read back.  If the mechanism (clear write, CP register 32/33 offsets, BP 0x23
selector encoding) is correct, this **must** read exactly 0, since zero triangles were ever
submitted.  Any other value proves the mechanism itself is unreliable, independent of what
any future non-zero reading might otherwise seem to show.

Commit `222fc7d531da`, deployed image SHA-256:

```text
d6c9f71883e1541c44c8b29b89452eea15e4cc63a0a9113e800bcb2787f23973
```

Result (checksum-verified on card): **mechanism validated.**

```text
gcn-gx: f1 perf0=triangles_passed count=0 (zero-draw control)
```

`count=0` with zero triangles submitted, exactly as required.  The clear write
(`cp_write(CP_REG_CLR, 4)`), the CP register 32/33 offsets, and the `BP 0x23` selector
encoding are all behaving correctly.

**This means the earlier `count=0` reading from the actual Z-enable+XYZ triangle draw
(previous section) is now trustworthy, not merely plausible.**  Combined: this is a
validated, colour-independent, non-pixel-sample confirmation that our 2 submitted
triangles genuinely do not pass culling / reach the rasterizer's fill stage -- consistent
with, and now much more solidly evidencing, the "no visible EFB write" signature across
this entire bisection.  Cull mode is confirmed `GX_CULL_NONE` (should not reject *any*
triangle regardless of winding), so if the counter is measuring post-cull survival, the
triangles must be getting rejected earlier in the pipeline than the cull stage --
candidates: degenerate/zero-area triangle detection in primitive assembly, or a vertex
transform (viewport/projection) producing a collapsed result despite the source vertex
data being genuinely non-degenerate (three distinct clip-space points, verified in the
FIFO byte audit).  This is a promising new, validated angle for the next session: verify
what the rasterizer's triangle setup unit actually receives after the position transform,
rather than continuing to vary raster/colour state.

### Direct clip-stage test with the validated counter: GX_PERF0_CLIP_VTX

Used the now-validated mechanism to directly test the XF clip stage, rather than relying on
the earlier clip-disable test (which was never independently confirmed with full-frame
capture before this session's methodology correction).  Selected `GX_PERF0_CLIP_VTX`
(libogc's exact encoding, `XF 0x1006 = 0x0000016b` -- this metric is selected via an XF
write, not BP, per `GX_SetGPMetric()`) and read back after the real 6-vertex draw, with clip
currently enabled (`XF 0x1005 = 0`, confirmed in the active diagnostic).

- 6 vertices clipped: the XF clip stage is rejecting everything, contradicting the earlier
  (unverified) clip-disable result -- would need to redo that test properly with the
  validated counter instead of pixel sampling.
- 0 vertices clipped: rules out clipping as the rejector for real this time; look at
  primitive/triangle-setup assembly instead.

Commit `fd164bf18193`, deployed image SHA-256:

```text
e161a598cc810c3d91e1d05a3bea29562ec03ebece6f8456dbe54c60d8be25d2
```

Result (checksum-verified on card): **`count=0`.**

```text
gcn-gx: f1 perf0=clip_vtx count=0
```

Zero vertices clipped, with clip genuinely enabled.  This is a real, validated (not
pixel-sampled) result: **the XF clip stage is definitively not the rejector.**  Combined
with the validated `triangles_passed=0`, both cull mode and clip are now ruled out with
real evidence, not inference.

Next: test `GX_PERF0_TRIANGLES` (total triangles seen by the GP, unconditional of any
pass/fail/cull gate -- libogc encoding `BP 0x2300AE7F`) to check the most fundamental
checkpoint: does the GP even recognize our FIFO submission as 2 valid triangles at all,
before asking whether they pass or fail anything.  0 here would point at a structural
primitive-recognition issue (despite the earlier byte-exact FIFO audit); 2 here would mean
triangles are correctly recognized but rejected by some gate distinct from cull and clip.

Commit `a67016814c88`, deployed image SHA-256:

```text
f563775c2a7c128d9d0bbb36b219dbf7f824fbe141074c672690b108fccf6e96
```

Awaiting hardware result.

### Wifi retest on independent hardware (same session)

Separately, the wifi/ssh dead end from earlier this session was retested on a second,
freshly-imaged SD card (independent rootfs from the one used throughout the rest of this
doc).  Wifi failed identically: `wpa_supplicant` reached `ASSOCIATED` then bounced back to
`SCANNING`/`DISCONNECTED` without ever reaching `COMPLETED`, matching the original card's
symptom exactly.  This rules out "something specific to the first card's rootfs/config" and
is further (not conclusive) evidence the wifi issue is a genuine kernel-level `b43` driver
bug rather than a config or corruption issue local to one SD card.  Wifi remains shelved;
resuming the GX bisection (Test B above) is the active thread.

Since wifi is confirmed to need real driver-level effort rather than a config fix, the
`wait_for_wlan0`/carrier-poll/`wpa_supplicant`/`dhclient` block was removed from
`/init-diag.sh` on the SD card (not tracked in this repo) to bring the boot-to-shell cycle
back down from ~2.5 minutes worst-case to the original ~20-25s.  The script now just does
the `sleep 20` dmesg capture, LED blink, and console shell, matching the pre-wifi-detour
form described earlier in this doc.

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
