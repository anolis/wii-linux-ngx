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
CCACHE_TEMPDIR=/tmp CCACHE_DIR=/tmp/wii-ccache \
  make ARCH=powerpc CROSS_COMPILE=powerpc-linux-gnu- zImage -j$(nproc)
mount /media/anolis/BOOTWII
cp arch/powerpc/boot/zImage /media/anolis/BOOTWII/gumboot/zImage.ngx
sync
sha256sum arch/powerpc/boot/zImage \
  /media/anolis/BOOTWII/gumboot/zImage.ngx
umount /media/anolis/BOOTWII
```

Do not use `pkexec`, mount the device node directly, or use the old temporary
`/tmp/bootwii-mnt` path.  The host already has an `/etc/fstab` entry for UUID
`CFC6-4C2F` with `user,rw,uid=1000,gid=1000`, so the commands above mount,
write, and unmount as user `anolis` without a password.  This procedure was
write-tested successfully on 2026-07-20.

BOOTWII (FAT, ~40 MB) and WII-LINUX-NGX1 (ext3, ~359 MB) are separate
partitions on the same SD card.  Device letters are not stable; identify them
by filesystem label rather than assuming `/dev/sdc` or `/dev/sdd`.

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

Result (checksum-verified on card): **`count=0` again.**

```text
gcn-gx: f1 perf0=triangles_total count=0
```

The GP does not recognize our submission as any valid triangles at all, unconditional of
pass/fail/cull/clip.  More fundamental than either previous result.

Important gap noticed in the earlier "validation": the zero-draw control only proved
"0 draws in -> 0 count out" -- it cannot distinguish a genuinely working counter from a
counter that is simply stuck at 0 regardless of input, since both give the identical result
for a zero-draw case.  A real positive control is needed.  Next: select `GX_PERF0_VERTICES`
(libogc encoding `XF 0x1006 = 0x0000014a`, value `0`) with the same 6-vertex draw.  If this
reads 6, that is genuine positive proof the counter mechanism works (individual vertices are
being processed by the GP), even if triangle *assembly* specifically is not happening -- a
real, useful distinction.  If this also reads 0, either the counter mechanism itself isn't
trustworthy after all, or no vertex processing is happening at all (a much deeper issue).

Commit `1e7257591707`, deployed image SHA-256:

```text
8fc005dc3b8aff89b529bf0ce27eaaef283773369db1ef9a7a4b1adef52f8fb6
```

Result (checksum-verified on card): **`count=0` -- negative result on the positive
control.**

```text
gcn-gx: f1 perf0=vertices_total count=0
```

Zero, despite a genuine 6-vertex draw submitted.  This fails the positive control.

## RETRACTION: the entire perf-counter diagnostic line this session is unvalidated

Since `GX_PERF0_VERTICES` reads 0 even for a real draw with real vertex data, the same
readback mechanism (`CP_REG_CLR` clear write, CP register 32/33 offsets, `BP 0x23`/`XF
0x1006` selector encodings) could equally be producing false zeros for every other metric
tried this session.  **Retract as unreliable, not confirmed findings:**
`triangles_passed=0`, `clip_vtx=0`, `triangles_total=0`.  None of the conclusions drawn from
them (cull ruled out, clip ruled out, primitive recognition broken) are supported anymore --
they may simply reflect a counter that never increments at all, regardless of what's
submitted.

This should have been caught earlier: the positive control (does the counter show a
non-zero value for known real activity) should have been run *before* drawing conclusions
from the mechanism, not after four rounds of results built on top of it.  The zero-draw
control alone (proving "0 in -> 0 out") was insufficient and this session initially
over-trusted it as full validation.

**Where this actually leaves the bug:** back to the pre-perf-counter state.  The FIFO-byte
encoding audit (Test B, statically verified byte-exact against libogc, not hardware-sampled)
remains the one genuinely solid piece of evidence from deep in this bisection.  Colour/pixel
sampling requires full-frame HDMI capture to trust (rig confirmed working earlier this
session).  The GX_PERF0 counter mechanism needs real hardware debugging (e.g. checking
whether `GX_DrawDone()`-style PE interrupt synchronization is required before the counter
value is actually latched/readable, which this driver's polling-only `gx_submit_cmds()`
never provides) before it can be trusted for anything again -- not assumed working from a
zero-draw check alone.

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

### MAJOR FINDING: BP 0x65 is not PE_DONE -- this driver has never issued a real draw-done fence

While debugging the failed `GX_PERF0_VERTICES` positive control, found that this driver's
long-standing "PE draw-done fence" (`gx_copy_efb_to_xfb`, written after every copy,
documented in Known pitfalls below and in this doc's own BP register table) is wrong.

**BP 0x65 is not `PE_DONE`.**  Per YAGCD, BP 0x65 is `TX_LOADTLUT1` (texture LUT load
config: rid/count/tmem-offset fields) -- independently confirmed against libogc's own
`GX_InitTlutRegion()`, which builds a register value with `tmem_addr`/`tlut_sz` fields and
address byte `0x65` in the top byte, matching YAGCD exactly.  The real `PE_DONE` register is
**BP 0x45** (libogc's actual `GX_DrawDone()`/`GX_SetDrawDone()` both write
`BP 0x45 = 0x00000002`), which this driver has **never written anywhere, for any
submission**, in this entire ~80-commit investigation.  Every `0x65000002` write this whole
project has been silently reconfiguring (mostly harmlessly, since no TLUT/palette textures
are in use) the texture LUT unit, not signalling draw completion.

This may explain more than just the perf-counter failure: if the PE requires a genuine
`PE_DONE` completion event to flush/commit pending writes in some scenarios, this could
plausibly connect to the original "primitive never visibly writes EFB" mystery too, not just
the perf counters -- worth keeping in mind for the next hardware test regardless of what the
immediate perf-counter test shows.

Test: added the correct `BP 0x45 = 0x00000002` fence after the draw in the
`GX_PERF0_VERTICES` diagnostic, keeping everything else identical to isolate this one
variable.  The old (wrong) `BP 0x65` write in `gx_copy_efb_to_xfb` is left untouched for now
to keep this a clean single-variable test, but is now known to be a no-op for draw-done
purposes and should be revisited (likely replaced with the correct BP 0x45, or removed if a
real completion-wait strategy is designed instead).

- Non-zero `vertices_total` (ideally 6): confirms `PE_DONE` completion signalling was the
  missing piece for the perf-counter mechanism -- re-run the earlier retracted tests
  (`triangles_passed`, `clip_vtx`, `triangles_total`) with the corrected fence to get
  trustworthy answers this time, and consider whether the same fix might affect the
  visible-EFB-write mystery.
- Still 0: rules out missing `PE_DONE` signalling as the explanation for the failed
  positive control; the counter readback mechanism needs a different kind of debugging
  (e.g. re-verify the CP register 32/33 offset assumption itself, or that
  `CP_REG_CLR`/`_cpReg[2]=4` genuinely means "clear perf counters" and not something else).

Commit `ae3f8a5315bb`, deployed image SHA-256:

```text
1707fc5ec82fddc9067cb8dd58df41ea9ecbbb5f9dad1fa950a0c2b2edc6700e
```

Result (checksum-verified on card): **`count=0` again, still fails the positive control.**

```text
gcn-gx: f1 perf0=vertices_total count=0 (with real PE_DONE)
```

Adding the correct `BP 0x45` `PE_DONE` fence did not change the outcome.  This rules out
"missing draw-done completion signalling" as the explanation for the perf-counter positive
control failure specifically.  The `BP 0x65`-is-`TX_LOADTLUT1` register-identity fix itself
remains independently verified and correct (via YAGCD and libogc's own
`GX_InitTlutRegion()`) and stays in place going forward -- it just didn't happen to be the
missing piece for the counter mechanism.

The `GX_PERF0` counter readback mechanism (`CP_REG_CLR` clear write, CP register 32/33
offsets, `BP 0x23`/`XF 0x1006` selector encodings) remains unvalidated after this attempt.
Remaining candidates if this is picked back up: verify `CP_REG_CLR`/`_cpReg[2]=4` genuinely
means "clear perf counters" on real hardware rather than something else entirely (it was
never independently confirmed, only assumed from the libogc name); re-verify the CP register
32/33 offset assumption itself against real hardware documentation rather than inferring it
from libogc's `_cpReg[]` array indexing; or check whether a separate "perf counters enabled"
global bit exists that hasn't been set anywhere.  Given the diminishing returns after this
many attempts, this specific mechanism is being set aside rather than continuing to guess at
increasingly obscure hardware plumbing without new evidence.

Follow-up correction: the previous test intentionally left the old `gx_copy_efb_to_xfb()`
`BP 0x65` write untouched so the `GX_PERF0_VERTICES` retry changed only one variable.  That
is no longer useful after the test ruled out PE_DONE as the perf-counter fix.  The copy path
now emits the real draw-done register, `BP 0x45 = 0x00000002`, after every EFB copy and the
stale in-code comments have been corrected.  This is a baseline hygiene fix, not a new
hypothesis result yet; deploy it once by itself before trying any further TEV/fog/register
candidate so future tests are not built on a known-wrong completion fence.

Built image SHA-256 for this baseline correction:

```text
52349c3203758688baedb231a933b01547f716db14dd0b540fa77ae9822db4ab
```

## 2026-07-17: reset to the confirmed copy-clear baseline

The live RGB565 blit path had accumulated the failed three-frame
constant-white primitive and `GX_PERF0_VERTICES` experiment.  That made the
deployed behavior more complicated than the small set of operations actually
confirmed on hardware.

Commit `297284f0515f` removes that experiment from the active path.  Every
RGB565 refresh now performs only the known-good sequence:

1. Set the copy-clear color to solid green.
2. Execute an EFB-to-XFB display copy with `clear=true`.
3. Submit and drain the GX FIFO.

The path intentionally ignores `vfb` for now.  It is a hardware reference
point, not a functional framebuffer implementation.  Primitive drawing,
`GX_PERF0` counters, and CPU-side XFB pixel samples are not treated as working
or diagnostic evidence in this baseline.

Build command:

```text
CCACHE_TEMPDIR=/tmp CCACHE_DIR=/tmp/wii-ccache ARCH=powerpc \
  CROSS_COMPILE=powerpc-linux-gnu- make zImage -j8
```

Built and checksum-verified on the SD card at `gumboot/zImage.ngx`:

```text
9f5d12e771d14ef1abb24fba3d39efc9dd4b56d2f11fe2385b7f6a5dc1e7d502
```

Expected hardware result: a stable, uniform green display.  Record the visual
result before introducing the next single-variable primitive test.

Observed result on 2026-07-20: the display initially alternated between full
green and full black, then stopped flickering and remained green around the
time the boot diagnostics wrote to the SD card.  Inspection found a confound
outside `gcn-gx.c`: the RGB565 IRQ path still called
`vi_gx_then_cpu_fill_green_diag()`, which performed a CPU YUYV green fill after
every GX call except the first.  Therefore the eventual stable green cannot be
credited to repeated GX copy-clear, and the initial flicker cannot be cleanly
separated from the CPU/GX overwrite race.

The next build removes the old immediate green CPU fallback and its unreliable
pixel sampling.  For the first 300 refreshes, `gcnfb.c`
runs only the unchanged repeated GX green copy-clear path.  Starting at frame
300, it performs a CPU YUYV **red** fill after every GX call.  This separates
the writers by both time and color: green or green/black during the first
interval is GX behavior, while stable red afterward positively identifies the
CPU writer and provides an unmistakable transition marker.

Fresh `dmesg.txt` from the previous green/green build confirms that the GX and
CPU writers were racing rather than that GX eventually stabilized.  All four
logged GX submissions drained fully with `RDoff=0040` and `WToff=0040`.  Before
the CPU fill, frames 1, 2, 3, and 360 all retained the same non-green values:

```text
gcnfb: f1 post-gx-pre-cpu-fill fb0=b531da65 fb1=d8827286 fbc=1d791d77
gcnfb: f360 post-gx-pre-cpu-fill fb0=b531da65 fb1=d8827286 fbc=1d791d77
```

The CPU then replaced the sampled XFB with uniform green `a52ba515` each time.
This explains the observed green/black flicker as VI seeing alternating GX and
CPU writes.  The later `dmesg.txt` write was only correlated in time; CPU green
had already started at 0.44 seconds.  The same log contains an unrelated EHCI
bad-spinlock/DMA-debug warning during boot, but the kernel recovered and GX
continued to submit and drain afterward.

Built image SHA-256, checksum-verified after deployment to
`gumboot/zImage.ngx`:

```text
2d115b2a3a5dafa379fdf2c691752730f9539fa1e9b3aacb861b3355d94ea603
```

Hardware result: **solid green, then green/red flicker, then solid red**.

This is a successful positive control for both writers.  The first 300
refreshes were GX-only, so their stable green output proves that repeated GX
copy-clear and EFB-to-XFB copy are stable without a competing CPU writer.  The
flicker began only when the delayed CPU red fill joined at frame 300, directly
confirming that the earlier green/black behavior was an XFB overwrite race.
The final stable red validates the CPU YUYV fill and VI scanout independently.

The confirmed working boundary is now: CP FIFO submission/drain, GX copy-clear,
EFB-to-XFB display copy, CPU XFB writes, and VI scanout.  The remaining graphics
failure is downstream primitive rasterization.  Future primitive tests should
use the stable GX green copy-clear as their background/readout and must not add
a CPU fallback writer or infer full-frame results from sparse pixel samples.

The returned log independently confirms clean GX command processing: frames
0-3 all report `RDoff=0040 WToff=0040`, with no slow, stall, timeout, or
pipeline-idle warning.  The delayed CPU writer enabled at `13.134929s`, so 300
refresh callbacks took about 13 seconds in this boot rather than the initially
estimated five seconds.

## 2026-07-21: PE-finish polling positive control

The next test corrects a concrete PE MMIO register error before attempting
another primitive. The driver defined PE interrupt status as word index 0
(`PE+0x00`), but both libogc (`_peReg[5]`) and YAGCD identify it as word index 5
(`PE+0x0a`). YAGCD also confirms bit 1 as finish interrupt enable and bit 3 as
the write-one-to-clear finish status generated by `BP 0x45 = 2`.

This build keeps the confirmed GX-only green copy-clear path and removes the
completed delayed CPU-red diagnostic. Before each submission it acknowledges
stale PE-finish status at the corrected register, submits the known-working
copy-clear/copy/`BP 0x45` sequence, polls bit 3 for up to 20 ms, acknowledges
the result, and logs the first four frames. Interrupt enables remain off: the
call runs inside the VI interrupt handler, so waiting for a separately serviced
PE interrupt there would deadlock.

Positive-control criterion:

```text
gcn-gx: f0 PE finish=1 wait_us=<bounded value> status=<bit 3 set>
```

If finish is observed consistently while the display stays solid green, the
register mapping and completion signal are validated. The following test can
then split primitive drawing and EFB copying at a real PE-finish fence instead
of treating CP FIFO drain as raster completion. A timeout means the polling
mechanism itself is still unvalidated and must not be used to reinterpret old
primitive results.

Built image SHA-256:

```text
6ba6b9ad4f7fc22415b4a239a403023ae5a19ec89753b43b155c756d979ed4d7
```

Hardware result: **solid green, but the PE-finish positive control failed.**
The first four known-good copy submissions all drained completely and reported
`SR=000c RDoff=0040 WToff=0040`, but PE interrupt status remained `0000` for
the full 20 ms polling window each time:

```text
gcn-gx: PE finish positive control timed out (PE=0000)
gcn-gx: f0 PE finish=0 wait_us=20000 status=0000
gcn-gx: f1 PE finish=0 wait_us=20000 status=0000
gcn-gx: f2 PE finish=0 wait_us=20000 status=0000
gcn-gx: f3 PE finish=0 wait_us=20000 status=0000
```

This confirms the corrected register access does not destabilize the known
copy-clear path, but it does **not** validate polling with PE finish interrupts
disabled. Do not use this result to infer that `BP 0x45` failed or that PE
finish cannot be polled. Libogc enables finish signalling (bit 1) as part of
its PE initialization before relying on the event. The next single-variable
control will enable that PE source while leaving hwirq 10 masked at the
Flipper PIC, then poll the same latched PE status from VI IRQ context.

### Enabled-source retry

The follow-up changes only PE interrupt-status bit 1: finish signalling is now
enabled when the PE is initialized and when stale finish status is cleared
before each submission. Flipper PIC hwirq 10 remains masked and no PE IRQ
handler is registered, so the existing VI IRQ-context poll cannot deadlock on
a nested interrupt. The command stream, green copy-clear visual baseline,
20 ms polling window, and first-four-frame logging are unchanged.

Success still requires `PE finish=1` on the known-good copy path. If status
remains zero, the next step must validate the event via the Flipper interrupt
cause register or a real PE IRQ handler outside the VI callback; it must not
assume that a fixed delay is equivalent to raster completion.

Built image SHA-256:

```text
be141e82d1ba37851c196919d9aef1cd59acac83d7f493abf222b37ffbe522b3
```

Expected visual result: unchanged solid green.

Hardware result: **positive control passed via token-value readback.** The PE
token-status and finish-status bits did not latch (`status=0003`, enable bits
only), but PE word index 7 advanced to the exact new per-frame marker in about
330 microseconds on all four logged frames:

```text
gcn-gx: f0 PE token_status=0 finish=0 token=0001 expected=0001 wait_us=330 status=0003
gcn-gx: f1 PE token_status=0 finish=0 token=0002 expected=0002 wait_us=330 status=0003
gcn-gx: f2 PE token_status=0 finish=0 token=0003 expected=0003 wait_us=330 status=0003
gcn-gx: f3 PE token_status=0 finish=0 token=0004 expected=0004 wait_us=330 status=0003
```

Every submission also drained to `RDoff=0060 WToff=0060` with `SR=000c`.
This validates PE+0x0e token-value polling as a real, bounded downstream command
marker and independently confirms that BP writes after the EFB copy execute.
The non-latching status bits are not needed for this mechanism and remain
unsuitable as diagnostics.

The next primitive test should use two submissions per frame: draw the
contrasting primitive, append `BP 0x45` plus a fresh draw-sync token, submit and
wait for the token value; only then issue EFB-to-XFB copy-clear plus another
fresh token. This directly tests whether the old draw-and-copy command stream
was copying EFB before raster completion.

### Token-fenced direct-colour primitive

This test applies the validated PE token marker as a real boundary. Each RGB565
refresh now performs two separately submitted command buffers:

1. Configure one direct RGBA8 vertex-colour channel with TEV `GX_PASSCLR`, draw
   a full-screen red quad, emit `BP 0x45`, append token A, and wait until
   `PE+0x0e == token A`.
2. Only after A is observed, copy EFB to XFB with `clear=true`, restoring the
   known green EFB background, append token B, and wait for token B.

The direct-colour state is intentionally texture-free and includes the two
independently verified hygiene fixes accumulated after the old direct-colour
test: destination alpha disabled via BP 0x42 and the real BP 0x45 draw-done
command. CPU XFB writing remains disabled. Submission diagnostics are labelled
`draw` and `copy` so each token and FIFO drain can be checked separately.

Visual result is decisive: red means the primitive changed EFB before the
fenced copy; green means the primitive still produced no visible EFB pixels.
An initial transient is not significant because the first copy-clear seeds the
green background for subsequent frames.

Built image SHA-256:

```text
9cc481b9256df4c913fe175fed91f46337c0299b93c799141403418db287a476
```

Hardware result: **solid green; both tokens advanced, but no primitive pixels
were visible.** The two independently labelled streams both drained fully:

```text
gcn-gx: f0 draw PE token=0001 expected=0001 wait_us=10
gcn-gx: f0 draw post: RDoff=01a0 WToff=01a0
gcn-gx: f1 copy PE token=0002 expected=0002 wait_us=330
gcn-gx: f1 copy post: RDoff=0060 WToff=0060
```

The pattern repeated for tokens 3 and 4. This rules out the copy merely
overtaking unconsumed draw commands, but it also corrects the interpretation of
the prior token positive control: **token-value advancement is a validated BP
command-progress marker, not a validated raster-completion fence.** The draw
token arrived in only 10 microseconds while the actual EFB copy held the token
for roughly 330 microseconds. `BP 0x45` followed by a token therefore does not
block token progression until the PE finish event is serviced.

Next test: register Flipper PIC hwirq 10 as the real PE-finish IRQ and split the
experiment across VI callbacks. First validate the handler against known-good
copy completion. Then submit the red draw and return from VI IRQ context. Only
after the separate PE-finish handler fires should a later VI callback copy EFB
to XFB. This avoids the impossible nested-interrupt wait and tests the actual
completion mechanism used by libogc.

### Asynchronous PE-finish IRQ test

This test maps Flipper PIC hwirq 10 through the default IRQ domain and registers
a real PE-finish handler. The handler acknowledges finish at `PE+0x0a`,
increments a shared completion count, and returns. No VI handler spins waiting
for it. The RGB565 diagnostic advances across retraces:

1. Submit a known-good green copy-clear and return from VI.
2. Require the PE-finish count to advance, positively validating hwirq 10.
3. Submit a second copy so the displayed and EFB baselines are both green;
   require its independent finish event.
4. Submit the full-screen red direct-colour quad plus `BP 0x45`, then return.
5. Only after the separate PE IRQ increments the count does a later VI callback
   copy EFB to XFB and stop the test.

The token marker remains on each submission for command-progress diagnostics,
but it is not used as raster completion. Expected final display is red if the
primitive wrote EFB, or green if it did not. Failure to validate the very first
known-copy finish IRQ invalidates the IRQ mechanism and must stop interpretation
before the primitive phase.

Built image SHA-256:

```text
da235b7e262d35aabcbc01557466d4fe129c922c9f86510291a5f6e282165003
```

Hardware result: **solid green; the asynchronous PE-finish mechanism passed all
positive controls, and the draw itself raised a finish IRQ before readout.** The
known seed copy raised IRQ count 1, the independent green-baseline copy raised
count 2, the red primitive submission raised count 3, and only then did a later
VI callback submit the EFB readout copy, which raised count 4:

```text
gcn-gx: f0 seed post: RDoff=0060 WToff=0060
gcn-gx: PE finish IRQ count=1 status=0003
gcn-gx: seed PE finish IRQ validated at count=1
gcn-gx: f1 green post: RDoff=0060 WToff=0060
gcn-gx: PE finish IRQ count=2 status=0003
gcn-gx: green seed complete; submitting red draw
gcn-gx: f2 draw post: RDoff=01a0 WToff=01a0
gcn-gx: PE finish IRQ count=3 status=0003
gcn-gx: draw PE finish observed; copying EFB readout
gcn-gx: f3 readout post: RDoff=0060 WToff=0060
gcn-gx: PE finish IRQ count=4 status=0003
```

Every FIFO drained fully, and the draw's command-progress token arrived in 10
microseconds while its separate PE-finish interrupt arrived asynchronously.
The final readout remained solid green. This definitively rules out the prior
draw and EFB-copy sharing one stream, token advancement, FIFO drain, or a fixed
delay being too early. The direct-colour primitive reaches PE completion but
does not produce visible EFB pixels. Continue with state/primitive diagnosis;
do not spend another test on draw-versus-copy ordering.

### Draw-sync token positive control

This test keeps the solid-green copy-clear command stream and appends libogc's
exact `GX_SetDrawSync()` sequence after the existing `BP 0x45` command:

```text
BP 0x48 = per-frame 16-bit token
BP 0x47 = the same per-frame token
```

PE token and finish signalling are enabled while their Flipper PIC lines stay
masked. The poll now waits for either PE token-status bit 2 or the new per-frame
token value at PE word index 7 (`PE+0x0e`). A different token is used each
frame so stale state cannot pass the control. The first four frames log token
status, finish status, actual token, expected token, and elapsed polling time.

Success criterion: `token_status=1` and/or `token=expected` before 20 ms. That
would validate an independently observable PE event path and prove the two
draw-sync BP writes progressed downstream. If the token value advances while
finish remains clear, the token path becomes the leading completion diagnostic
for a split draw-then-copy primitive test. If neither changes, inspect the
Flipper interrupt-cause register or service the events outside VI IRQ context
before treating any PE status mechanism as trustworthy.

Built image SHA-256:

```text
21e323a64f4ee5702033e71d6254136920d1ca7e217a4277310f882a6d6b6507
```

Expected visual result: unchanged solid green.

### Correct stage-0 TEV order register

Reviewing Dolphin's `BPMemory.h` exposed a foundational register-address
error: BP `0x25` is `BPMEM_RAS1_SS0` (indirect texture-coordinate scale), not
the stage-0 TEV order register. `BPMEM_TREF`, which contains the paired TEV
stage order entries, starts at BP `0x28`.

This was independently confirmed in libogc. `GX_Init()` assigns BP addresses
`0x25` through `0x2f` to the eleven-element `tevRasOrder[]` backing array, but
`GX_SetTevOrder()` deliberately selects `reg = 3 + (stage >> 1)`. Stage 0
therefore writes array entry 3, BP `0x28`. The earlier project conclusion that
array entry 0 represented stage 0 was incorrect.

The active asynchronous direct-colour diagnostic now changes only its claimed
stage-0 order write from `BP 0x25 = 0` to `BP 0x28 = 0`: texture sampling
disabled and raster channel 0 selected. The validated green copy controls,
direct RGBA8 red quad, real PE-finish IRQ boundary, and delayed EFB readout are
unchanged. A red final display means the stale/unknown BP `0x28` state was the
reason all prior primitives were invisible; green means this real register bug
was not sufficient by itself.

Built image SHA-256:

```text
1fbcea946af61d4355f39fd9f3a89d2bfff9cd489cb05d64556dbea8eb9ffcae
```

Hardware result: **the final display changed from the established bright green
control to visibly dark green.** The returned log confirms that the intended
image ran and completed every asynchronous phase normally:

```text
gcn-gx: PE finish IRQ count=1 status=0003
gcn-gx: seed PE finish IRQ validated at count=1
gcn-gx: PE finish IRQ count=2 status=0003
gcn-gx: green seed complete; submitting red draw
gcn-gx: f2 draw post: SR=000c RDoff=01a0 WToff=01a0
gcn-gx: PE finish IRQ count=3 status=0003
gcn-gx: draw PE finish observed; copying EFB readout
gcn-gx: f3 readout post: SR=000c RDoff=0060 WToff=0060
gcn-gx: PE finish IRQ count=4 status=0003
```

This is not the intended red, but it is the first visually observed change
caused solely by correcting stage-0 routing to BP `0x28`. It confirms stale or
unknown TREF state affected the primitive path. Dolphin decoding then exposed
a second concrete error in the active state: `BP C1 = 0x08fff5` selects TEV
raster and texture swap table 1 in bits 0-3 while leaving alpha input `d` as
`GX_CA_ZERO`. Exact libogc `GX_PASSCLR` with swap table 0 encodes the alpha
combiner as `BP C1 = 0x08ffd0`; that is the next single-register test.

### Correct stage-0 alpha and swap-selector encoding

The active direct-colour path now changes only BP C1 from `0x08fff5` to
`0x08ffd0`. Decoded field by field, the new value is exactly libogc's stage-0
`GX_PASSCLR` alpha state:

```text
rswap=GX_TEV_SWAP0, tswap=GX_TEV_SWAP0
a=b=c=GX_CA_ZERO, d=GX_CA_RASA
op=ADD, bias=ZERO, scale=1, clamp=true, dest=TEVPREV
```

The old value put `5` in the low swap-selector nibble, selecting swap table 1
for both raster and texture colour, while the actual `d` field remained 7
(`GX_CA_ZERO`). Since the swap selector also controls the raster RGB consumed
by the C0 `GX_CC_RASC` input, this error can explain the dark-green result even
though destination alpha is disabled.

Built image SHA-256:

```text
35043de72bc98415d8d47e6ccd2a279b77d3c5fc08f3e84cb9e61d7bb3a6dd06
```

Hardware result: **bright green**, matching the established green background
rather than the preceding test's dark green. The intended binary completed all
four PE-finish phases with full FIFO drains, including the red draw's separate
finish event before readout.

Correcting C1 therefore removed the darkening caused by the malformed swap-1
selection, but it did not produce the intended red. This is still not a clean
negative for raster output: C1 now selects swap table 0, but the driver has
never initialized BP F6/F7's swap-table-0 channel mapping and inherits unknown
state from Mini. A stale swizzle can transform a red vertex into green and make
a successful full-screen write indistinguishable from the green background.
The next test explicitly programs table 0 to RGBA identity while leaving the
correct C1 selector and every other state value unchanged.

### Initialize TEV swap table 0 to identity

The direct-colour setup now writes libogc's swap-table-0 channel mapping before
the corrected BP `0x28` stage order:

```text
BP F6 low nibble = 0x4  (R=RED,   G=GREEN)
BP F7 low nibble = 0xE  (B=BLUE,  A=ALPHA)
```

The full writes are `F6000004` and `F700000E`. Their konst-selection fields are
zero, which is irrelevant because this one-stage `GX_PASSCLR` configuration
does not use konst inputs. C1 continues to select swap table 0. No geometry,
TEV formula, PE, copy, or completion state changes. Solid red is the expected
result if prior inherited swap-table state was transforming the red primitive;
green means the draw still does not produce a distinguishable EFB result.

Built image SHA-256:

```text
ee5ae16a80759d712024814f9f5304008e2c3f779c5938d6fb9041b34f63b075
```

Hardware result: **bright green again.** The returned log confirms the exact
image completed both green controls, the red draw's independent PE-finish IRQ,
and the delayed readout copy with all FIFOs drained. Explicit identity
swizzling did not expose red output.

This rules out inherited TEV swap-table state as the remaining explanation for
the green frame. Pause register-by-register hardware tests here. The next work
is an isolated FIFO differential apparatus: build devkitPro's known-working GX
triangle with the official devkitPPC container, capture one rendered frame in
Dolphin's FIFO logger, decode it using Dolphin's own register definitions, and
compare the resulting CP/BP/XF stream against this driver's emitted bytes.

### Establish a known-good libogc FIFO reference

Added `tools/gx-fifo-reference`, a minimal libogc program which intentionally
matches the active Linux diagnostic: direct XY/F32 positions, direct RGBA8
colors, one raster color channel, no texture generators, one `GX_PASSCLR` TEV
stage, an orthographic 640x480 projection, and a full-screen red quad over a
green copy-clear background.

The program was built with the official `devkitpro/devkitppc:latest` container
and run visibly in Dolphin. It rendered solid red. Dolphin's FIFO recorder then
captured one complete frame, reporting 204 FIFO bytes, zero memory-update bytes,
and one frame. Reopening that `.dff` directly in Dolphin's FIFO Player again
rendered the red EFB, providing an independently replayed positive control.

Capture SHA-256:

```text
1f56e7adce3cfd9831e027b5ce5c659698b2daf6dc5cbe5ef7286cad74a057f2
```

The checked-in `decode_dff.py` follows Dolphin's packed `FileHeader` and
`FileFrameInfo` definitions and decodes both the complete initial BP/CP/XF
snapshot and every recorded FIFO command. Its golden output is
`reference-red-quad.txt`. This distinction matters because the 204-byte frame
contains only values changed after recording began; state written once by
`GX_Init()` or `configure_gx()` exists only in the initial arrays.

The first differential is highly constraining. The reference and Linux driver
match on the primitive opcode/count/payload, CP VCD/VAT, identity position
matrix, XF viewport and projection, channel count/control, TEV C0/C1, stage-0
TEV routing, PE pixel format, and draw-done command. The reference's draw-time
stream starts with:

```text
BP 40 = 0x00000e  (Z disabled, compare function ALWAYS, update disabled)
BP 41 = 0x00311c  (blend/logic disabled, dither and color/alpha writes enabled,
                   source ONE, destination ZERO, logic COPY)
BP 43 = 0x000040  (RGB8/Z24, Z compare before texture)
```

The driver currently uses `BP 40 = 0x000000`, `BP 41 = 0x000018`, and the
matching `BP 43 = 0x000040`. Although disabled blend and depth modes make some
of the differing fields look nominally irrelevant, the exact known-good PE
state is now the smallest evidence-based next hardware test. Do not resume the
older unbounded register search; first make the active direct-color diagnostic
match the reference's BP 0x40 and 0x41 values exactly.

### Test exact libogc draw-time PE state

The active direct-color diagnostic now changes only two BP payloads to match
the validated red libogc capture byte for byte:

```text
BP 40: 0x000000 -> 0x00000e
BP 41: 0x000018 -> 0x00311c
```

The first value retains disabled Z testing and Z writes but selects the
`GX_ALWAYS` compare function. The second retains disabled blending and logic
operations with color and alpha writes enabled, while adding libogc's dither,
source-ONE, destination-ZERO, and logic-COPY fields. BP 0x43 was already the
matching `0x000040`. No CP, XF, geometry, TEV, copy, FIFO, or asynchronous
PE-finish behavior changed.

Built image SHA-256:

```text
16f68552ab170373702e8116d0f139dd187bab29a127e11383e3793ebbc6d0c5
```

Expected result: solid red means the raster backend requires one or more of
these nominally inactive PE fields to carry libogc's canonical values. Solid
green means the remaining cause is in state absent from the driver's setup but
present in the DFF initial snapshot, rather than the already matched frame
payload. Any nonuniform result must be confirmed visually before attribution.

Hardware result: **solid green.** Matching libogc's complete draw-time BP 0x40
and BP 0x41 values did not make the red primitive visible. This rules out the
remaining PE-state differential contained in the recorded 204-byte frame.

The comparison therefore moves to initialization state stored only in the DFF
snapshot. The strongest missing command is libogc's unconditional
`XF 0x1000 = 0x0000003f` write in `__GX_InitRevBits()`. Dolphin names 0x1000
`XFMEM_ERROR` but does not emulate its behavior, so describing the write as an
error clear remains a hardware hypothesis rather than a confirmed semantic.
The driver never writes this register. An inherited XF fault remains consistent
with CP FIFO consumption and functional BP copies alongside absent primitive
output, making this the next isolated test.

### Test libogc XF 0x1000 initialization

The active direct-color setup now adds one command before its existing XF
vertex state:

```text
XF 0x1000 = 0x0000003f
```

This is byte-exact with the unconditional write in libogc
`__GX_InitRevBits()` and with the validated DFF initial snapshot. The prior
exact PE-state values remain in place; no other BP, CP, XF, geometry, copy, or
completion behavior changed.

Built image SHA-256:

```text
2235039082ad18edb708b6cd2ffeebdc37bd2b26c7fbae6c98d886f4c7365668
```

Expected result: red supports an inherited XF error/state latch as the cause of
discarded primitives. Green rules out this missing libogc initialization write
and moves the differential to the other initial XF state, beginning with
`XF 0x1012 = 1` and the channel-1 defaults.

Hardware result: **solid green.** The libogc `XF 0x1000 = 0x3f` write did not
make the red primitive visible. It is ruled out as an isolated fix.

Do not continue testing DFF snapshot registers individually. The current DFF's
204-byte frame is only a post-configuration delta, while its initial arrays do
not preserve command order and cannot safely be replayed wholesale because
some BP registers have side effects. The reference program should instead call
its complete `configure_gx()` routine inside each frame before drawing. A new
validated FIFO capture will then contain the exact ordered public libogc setup,
primitive, fence, and copy sequence. Compare or replay that complete stream in
Linux as one bounded control.

### Capture complete ordered libogc draw state

The reference program now calls `configure_gx()` inside every frame before its
red quad. A new one-frame Dolphin capture contains 564 FIFO bytes and zero
memory updates. Unlike the original 204-byte capture, it includes the complete
ordered public configuration sequence, primitive, BP 0x45 draw-done command,
and EFB-to-XFB copy.

Capture SHA-256:

```text
8b42cc84e28b8ab09e53f981f4c1b197ec57fa0ee0029704f78d1a819f1aa302
```

Positive control: launching the new `.dff` directly in Dolphin's FIFO Player
rendered the expected solid red frame. The capture is therefore independently
replayable and suitable as the ordered ground truth. Its structured decode is
checked in as `reference-red-quad-full-state.txt`.

The ordered comparison also corrected a false lead: libogc explicitly writes
CP 0x30 and XF 0x1018 before the draw, but the driver's
`gx_load_identity_pos_mtx0()` already writes both registers with position
matrix index 0. Current position-matrix selection is not missing.

The next hardware test should emit the capture's complete setup and red draw in
the same order, then use the driver's physical XFB address for the final copy.
This is a bounded replay test, not another individual-register experiment.

### Replay the validated 564-byte libogc frame on hardware

The active diagnostic retains its two independent green copy controls, then
copies the validated full-state frame into the MEM1 command FIFO verbatim. A
build-time assertion fixes its length at 564 bytes. A host-side structured
comparison confirmed that every embedded byte equals the independently replayed
DFF frame.

Only the three-byte payload of BP 0x4b at frame offset `0x22c` is replaced with
the Wii driver's physical XFB address. The captured 640x480 viewport, sample
pattern, scissor, complete public draw state, CP/XF matrix indices, red quad,
BP 0x45 fence, 32-byte flush, and final copy-clear command retain their original
order and encoding. The separate old draw/readout submissions are removed from
this test so they cannot overwrite the replay result.

Built image SHA-256:

```text
178dd40fbc5e55baa4f84be5ef1ed9462ae67fd467e979c3ab03b7247ae91b05
```

Expected result: red proves the missing behavior is represented somewhere in
the ordered libogc public command sequence and permits a command-range bisect.
Green means even the captured sequence cannot draw under Linux's inherited GX
environment, shifting the investigation to initialization outside the frame:
GX/CP/PE reset, one-time `GX_Init()` commands, or FIFO execution context.

Hardware result: **solid green.** The exact ordered 564-byte frame that replays
red in Dolphin still did not produce a visible primitive on Wii Linux. This is
decisive evidence that the fault is outside the public draw-state stream.

The strongest untested difference is PI `FIFO_RESET` at byte offset `0x18`
(32-bit PI register index 6). Libogc's Wii `GX_AbortFrame()` pulses this register
to 1 and back to 0; Dolphin independently names it `PI_FIFO_RESET` and models a
write of bit 0 as resetting the gather pipe, CP FIFO registers, and video FIFO
buffer. The Linux driver has never written this register and therefore retains
Mini's downstream GX/FIFO state across every test. The next build performs one
reset pulse before programming the driver's FIFO, then runs the unchanged
validated replay.

### Test PI FIFO reset before GX initialization

The driver now disables CP reads and reproduces libogc's Wii abort pulse before
programming any FIFO state:

```text
PI_FIFO_RESET (0x0c003018) = 1
wait 10 us
PI_FIFO_RESET (0x0c003018) = 0
wait 10 us
```

Memory barriers order both MMIO writes. The delays are deliberately longer than
libogc's short timebase waits. After the pulse, the existing FIFO initialization,
two green copy controls, and byte-exact 564-byte red replay run unchanged.

Built image SHA-256:

```text
40cd0acd23db470e380b148f36863805995d07ca52311d72785b15f664f2ed1b
```

Expected result: red means Mini left the GP/FIFO backend in a state that only a
PI reset clears. Green rules out the missing abort/reset pulse and leaves the
one-time FIFO `GX_Init()` command preamble as the next bounded differential.

Hardware result: **black screen / apparent hard failure.** The system did not
reach the normal SD-card diagnostic write, unlike every recent green display
test. Treat PI `FIFO_RESET` as unsafe at this point in the Linux takeover
sequence. The pulse has been removed completely; do not retry it without a
separately designed recovery sequence that restores every reset CP/PI state and
provides an external serial progress channel.

Proceed only with ordinary FIFO commands from libogc's one-time `GX_Init()`
preamble. Those commands do not reset live MMIO state and can be placed before
the already validated 564-byte replay.

### Test libogc's one-time GX command preamble before exact replay

This build leaves the reverted PI reset completely absent and prepends only
ordinary FIFO commands from libogc's Wii `GX_Init()` path to the unchanged,
validated 564-byte red-frame replay. The preamble preserves libogc source order
and contains:

- BP bus-clock writes `0x0f0000ff`, `0x690004ed`, `0x0f0000ff`, and
  `0x46000273`.
- CP registers `0x80` through `0x87` initialized to `0x80000000`.
- XF registers `0x1000=0x3f`, `0x1012=1`, and later `0x1006=0`.
- BP `0x58=0x0f`, CP `0x20=0`, and BP `0x23`, `0x24`, `0x67`, and `0x0f`
  cleared.
- The 16 BP writes emitted by Wii `__GX_SetTmemConfig(2)`.

The two known-good green copy controls still run first. The preamble and full
red frame are then submitted as one contiguous FIFO stream, followed by the
existing token completion diagnostic. No MMIO reset, texture upload, CPU XFB
write, or alternate readout path is introduced.

Built image SHA-256:

```text
e6e3ccb41784dccba32de468c27f018a8c194f0e2d9efed991614fb498c8f289
```

Expected result: red means a one-time low-level GX command omitted by the Linux
driver is required before public draw state can rasterize. Green rules out this
captured subset of libogc's FIFO preamble and shifts attention to low-level
MMIO initialization or state not represented by ordinary commands. Black or a
missing SD diagnostic write means one of these preamble commands is unsafe in
the inherited Mini/Linux GX environment and requires a command-range bisect.

Hardware result: **solid green.** The complete command stream drained without
the hard-failure behavior caused by PI `FIFO_RESET`, but the primitive still did
not visibly alter the green EFB clear. This rules out the tested ordinary-command
subset of libogc's one-time `GX_Init()` preamble. Keep the preamble available as
known-safe initialization, but investigate low-level CP/PI/PE MMIO state next.

### Correct the off-by-one PI FIFO register map

The low-level comparison found a longstanding concrete bug in this driver. It
defined PI FIFO BASE/END/WPTR as 32-bit indices `2/3/4` (offsets
`0x08/0x0c/0x10`) and invented a `PI_FIFO_CTRL` register at index `5`
(`0x14`). Both libogc and Dolphin independently define the real mapping as:

```text
PI + 0x0c (index 3): FIFO_BASE
PI + 0x10 (index 4): FIFO_END
PI + 0x14 (index 5): FIFO_WPTR
PI + 0x18 (index 6): FIFO_RESET
```

There is no FIFO enable/control register at `0x14`; that address is the real
write pointer. Consequently every old submission wrote the intended base into
an undefined `0x08` register, the intended end into the real BASE, the intended
write pointer into the real END, and finally the value `1` into the real WPTR.
The init-time safety redirect likewise wrote FIFO_END instead of WPTR. Old
`PIoff` logs read the real FIFO_END register and must not be treated as write-
pointer observations.

This test changes only those three indices to `3/4/5` and removes the bogus
`PI_FIFO_CTRL_EN` write. CP FIFO programming, LINKEN/GPRESET, the known-safe
libogc command preamble, two green controls, and exact 564-byte red replay remain
unchanged.

Built image SHA-256:

```text
42b81188b2f4877632921f0357c105a27c9c3e9483c526cdcd9a4008d1c864da
```

Expected result: red means the malformed PI FIFO configuration allowed command
tokens and copy operations through but prevented primitive rasterization. Green
means the correction is still required for correctness but is not the EFB-write
fix. Black or a missing diagnostic write indicates that restoring real PI/CP
link semantics exposes another takeover-order bug.

Hardware result: **solid green**, with normal diagnostic completion. The PI map
correction is retained as an independently verified register fix.

However, the fresh log exposed a more important test-apparatus failure. The
replay submission was only 224 bytes (`WT=0x00e0 pos=224`) and emitted a warning
at `gcn-gx.c:1383`. That line is the `width != 640 || height != 480` guard in
`gx_load_reference_red_frame()`. The active framebuffer is the driver's default
overscan-compensated `576x432` mode, so the loader returned without appending any
of the 564 captured bytes. The 224-byte stream is exactly the 206-byte libogc
preamble plus the driver's 10-byte token marker and alignment padding.

**Retract the hardware conclusions from both "Replay the validated 564-byte
libogc frame on hardware" and "Test libogc's one-time GX command preamble before
exact replay."** Neither test actually submitted the captured frame. The green
result does validate that the preamble itself is accepted, but it says nothing
about whether the full captured draw works on Wii hardware. The warning from IRQ
context also triggered later kernel corruption diagnostics, so the dimension
guard must not be exercised again.

The next build must select the framebuffer driver's existing `nostalgic` mode,
which uses the native 640x480 dimensions expected by the independently validated
capture. Keep the 564-byte command stream unchanged; a valid replay submission
with the preamble and token padding must report `pos=800` (`0x0320`).

### Run the first valid 640x480 exact-frame replay

The built-in Wii command line previously used `video=gcnfb:tv=auto`, but the
driver registers and calls `fb_get_options()` as `gcn-vifb`; the stale name did
not select any driver options. This build changes only that argument to
`video=gcn-vifb:tv=auto,nostalgic`. The driver's existing `nostalgic` mode uses
the native 640x480 VI/framebuffer geometry matching the captured libogc frame.

No GX command bytes changed. The corrected PI map, two green controls, 206-byte
libogc preamble, exact 564-byte red frame, and token completion marker remain as
in the prior build.

Built image SHA-256:

```text
7f1f00fed07a726ee8ea89997771470d244b82d5e402a7b097a592e915f31c2f
```

This test is valid only if the fresh log contains both `mode is 640x480x16` and
`f2 replay ... WT=0320 pos=800`, with no warning at the replay loader. Red then
proves the validated libogc stream works on hardware. Green with a complete
800-byte drain is the first legitimate evidence that state outside the capture
still differs. Any other mode or byte count must be treated as another apparatus
failure rather than a GX result.

Hardware result: **solid red.** The fresh log satisfies every validity check:

```text
gcn-vifb: mode is 640x480x16
gcn-gx: f2 replay pre:  ... WT=0320 pos=800
gcn-gx: f2 replay PE ... token=0003 expected=0003
gcn-gx: f2 replay post: ... RDoff=0320 WToff=0320
gcn-gx: libogc replay PE finish observed
```

This is the first definitive primitive-render success on Wii Linux. The CP
consumed the complete stream, the PE observed its markers, the red quad visibly
replaced the green clear, and the final EFB-to-XFB copy displayed it correctly.
The GX hardware, direct MEM1 FIFO submission, corrected PI mapping, primitive
encoding, raster pipeline, and display copy path are therefore all operational.

Next isolate whether the 206-byte one-time libogc preamble is required: retain
the exact 564-byte frame and 640x480 mode but omit only
`gx_load_libogc_init_preamble()`. The expected padded submission becomes 576
bytes (`0x0240`, including the driver's token marker). Red means the required
state is wholly inside the captured frame; green means it is in the preamble and
that 206-byte range can be bisected.

### Test exact red frame without the libogc preamble

This build removes only the call to `gx_load_libogc_init_preamble()` from the
validated red path. It retains native 640x480 mode, the corrected PI register
map, both green copy controls, all 564 captured frame bytes, runtime XFB address
patch, and the driver's PE token marker. The preamble helper remains in source
for later range tests but emits no commands in this build.

Built image SHA-256:

```text
2816e412f543d6c7e3bbb6365b541163509a9ee73f1bed5a3ebb75e92646e5b6
```

Validity requires `mode is 640x480x16`, no loader warning, and replay
`WT=0240 pos=576` followed by `RDoff=0240 WToff=0240`. Red proves the 564-byte
public frame contains every command required for rasterization. Green proves at
least one command in the omitted 206-byte one-time preamble is required and
makes that range the next bisect target.

Hardware result: **red again**, with a thin green sliver at the left edge that
was also present in the successful preamble build. The fresh log validates the
test exactly:

```text
gcn-vifb: mode is 640x480x16
gcn-gx: f2 replay pre:  ... WT=0240 pos=576
gcn-gx: f2 replay PE ... token=0003 expected=0003
gcn-gx: f2 replay post: ... RDoff=0240 WToff=0240
gcn-gx: libogc replay PE finish observed
```

The one-time 206-byte libogc preamble is conclusively unnecessary for primitive
rasterization. All required state is contained in the decoded 564-byte public
frame. Treat the narrow green edge as a later viewport/scissor coverage issue;
it does not affect the red primitive positive control.

The next work should compare the working capture's decoded command groups with
the driver's generated direct-color setup, then isolate differences inside the
564-byte frame. Do not return to PI reset, undocumented register probing, or the
one-time preamble unless new evidence specifically requires it.

### Reproduce the historical BP 0x59 scissor-offset encoding

The strongest concrete difference between the proven frame and the driver's
generated direct-color setup is BP `0x59` (`SCISSOR_OFFSET`). The working capture
does not rewrite it and its initial snapshot contains `0x5902acab`. The driver
explicitly emits `0x59000000`, claiming that value represents logical offset
`(0,0)`.

Libogc's `GX_SetScissorBoxOffset()` independently proves that claim wrong. It
encodes each axis as `(offset + 0x156) >> 1`; logical `(0,0)` therefore produces
raw X=Y=`0xab` and register payload `0x02acab`. Dolphin documents the same 342
bias and divide-by-two representation.

This test prepends only the driver's historical `0x59000000` write to the
otherwise proven no-preamble 564-byte red frame. No other command or mode changes.

Built image SHA-256:

```text
817496400e4474cd34fe730eb98022e1e48deea3eba9c171b867013c6c493d1e
```

Validity requires 640x480 mode and replay `WT=0260 pos=608` with a complete
drain. Green or severe clipping reproduces the historical primitive failure with
one known-bad register and identifies the root cause. Red means BP 0x59 is still
a real encoding bug but not sufficient to explain the old failures; compare BP
0x28 and TEV KSEL/swap state next.

Hardware result: **mostly green, with a red rectangle confined to roughly the
bottom-right sixth of the screen.** The fresh log validates the run at 640x480,
`WT=0260 pos=608`, the expected token and PE-finish IRQ, and a complete drain to
`RDoff=WToff=0x0260`.

This is a decisive controlled reproduction. One BP `0x59=0` write changed the
same proven frame from almost fully red to severely shifted/clipped while every
other GX command remained byte-identical. The driver's historical
`GX_SetScissorBoxOffset(0,0)` encoding is therefore a root cause of invisible or
partial primitives. At the former 576x432 framebuffer geometry, the same
342-pixel coordinate displacement can leave little or no visible raster output.

Next replace the replay with the driver's generated direct-color setup and draw,
but correct BP 0x59 to `0x5902acab`. Preserve the validated PI mapping, 640x480
mode, green controls, draw-done ordering, and completion diagnostics. Red will
prove the generated path is operational with this fix; green will expose an
additional generated-state difference, with BP 0x28 and TEV KSEL/swap values the
next strongest candidates.

### Test generated direct-color path with corrected scissor offset

This build stops using the captured frame for the active test and restores the
driver's generated `gx_setup_vertex_color_state()` plus
`gx_draw_color_quad()` path. Its BP 0x59 write is corrected from
`0x59000000` to libogc-equivalent `0x5902acab`; no other generated-state value is
changed, so known differences such as BP 0x28 and TEV KSEL/swap upper fields
remain available as follow-up variables.

The draw and readout are separate PE-fenced submissions. After the two green
controls, the driver submits generated state, a direct RGBA8 red quad, and BP
0x45. A later VI callback observes the draw's PE-finish IRQ before submitting
the EFB-to-XFB copy-clear. This prevents a premature copy from hiding a valid
primitive.

Built image SHA-256:

```text
ca39cf9b3d43b925f981788b05ffa64bcc002105d919cad1cf02cb2ae2f4d0cb
```

Validity requires 640x480 mode, a generated `draw` submission at
`WT=01a0 pos=416` with complete drain and finish observation, followed by a
`copy` submission at `WT=0060 pos=96` with complete drain. Red proves the
corrected BP 0x59 encoding is sufficient to revive the generated direct-color
path. Green means at least one additional generated-state difference remains.

Hardware result: **solid green.** The fresh log validates every sequencing and
length control: 640x480 mode, draw `WT=01a0 pos=416`, expected token, complete
draw drain, draw PE-finish IRQ, copy `WT=0060 pos=96`, expected token, complete
copy drain, and final copy PE-finish IRQ. BP 0x59 is necessary but not sufficient;
at least one additional generated-state mismatch remains.

BP 0x28's apparent `0x000000` versus captured `0x049000` mismatch is not the
next useful test: libogc packs two TEV stages into that register, and the stage-0
fields used here are zero in both values. The nonzero captured bits describe
unused stage 1.

The strongest omitted working-frame group is now BP 0x01 through 0x04. Libogc's
`GX_SetCopyFilter(aa=false, ...)` writes `0x666666` to all four sample-position
registers; the generated path writes none of them and inherits Mini's unknown
values. Add exactly those four writes before the generated draw. Keep all other
state and split draw/copy sequencing unchanged.

### Add libogc's non-AA sample-position group

This build adds exactly four BP writes to `gx_setup_vertex_color_state()`:

```text
BP 0x01 = 0x666666
BP 0x02 = 0x666666
BP 0x03 = 0x666666
BP 0x04 = 0x666666
```

These are the values emitted by the proven frame's
`GX_SetCopyFilter(aa=false, ...)` call. The corrected scissor offset and every
other generated draw/copy command remain unchanged.

Built image SHA-256:

```text
d6af40f4dfc44470ce2ddc070cceadf613f7b114733bfa5beab540d283f237ed
```

Validity requires generated draw `WT=01c0 pos=448`, its token/finish and complete
drain, followed by copy `WT=0060 pos=96` and complete drain. Red identifies the
missing sample-position initialization as the second generated-path blocker.
Green rules it out and leaves TEV KSEL/swap preservation or an actively harmful
extra generated command as the next comparison target.

Hardware result: **solid green.** The fresh log validates draw
`WT=01c0 pos=448`, expected token, PE-finish IRQ, and complete drain, followed by
copy `WT=0060 pos=96`, expected token, PE-finish IRQ, and complete drain. The
sample-position group is accepted but does not restore generated raster output.

Before changing another state register, test the remaining structural difference
from the successful capture: that frame keeps draw, BP 0x45, a 32-byte NOP gap,
and EFB copy in one contiguous FIFO submission. The generated diagnostic splits
draw and copy across separate CP disable/reprogram/enable cycles despite waiting
for PE finish. Keep all generated state unchanged and combine those operations.
This distinguishes a CP/FIFO submission-boundary effect from another state-value
mismatch.

### Combine generated draw and copy in one FIFO submission

This build changes only submission topology. After generated state and the red
quad, it emits BP 0x45, exactly 32 zero/NOP bytes, the same green copy-clear
state used by the split test, and the EFB-to-XFB copy before calling
`gx_submit_cmds()` once. The now-unneeded intermediate VI phase and second CP
restart are removed. Corrected scissor state, sample positions, and every other
generated register value remain unchanged.

Built image SHA-256:

```text
8e3f347238cd04527f83068f1999913a1fe4cfea601e9d962bafe4185783297b
```

Validity requires one `drawcopy` submission at `WT=0220 pos=544`, expected token
and finish IRQ, and complete drain to `RDoff=WToff=0x0220`. Red means disabling
and reprogramming CP between draw and copy lost or invalidated the EFB result
despite PE finish. Green rules out submission topology and returns the search to
specific generated-state differences or harmful extra commands.

Hardware result: **solid green.** The fresh log validates one contiguous
`drawcopy` submission at `WT=0220 pos=544`, expected token, PE-finish IRQ, and
complete drain to `RDoff=WToff=0x0220`. Splitting draw and copy across CP restarts
was not the cause.

Return to the proven 564-byte red frame for controlled challenge tests. The
generated path writes BP 0x30=`width-1` and BP 0x31=`height-1` even though it has
zero texture generators; the successful frame does not write either register.
Earlier claims that these SU-size writes were required came from the invalid
pre-640x480 test era and are not reliable. Prepend only these two generated
values to the proven frame. A green result directly identifies them as harmful;
red rules them out without conflating the other generated differences.

### Challenge the proven red frame with BP 0x30/0x31

This build restores the byte-exact no-preamble red frame and prepends only:

```text
BP 0x30 = 639
BP 0x31 = 479
```

These match the generated 640x480 direct-color path. The frame subsequently
sets zero texture generators but does not overwrite either SU-size register.
All generated draw commands and the combined generated copy are removed from
the active test, returning display behavior to the proven positive-control path
apart from these ten challenge bytes.

Built image SHA-256:

```text
8fff0212261866cc68007b7d507659980476e8202f451ef102fef64e41b98ee5
```

Validity requires one `susize` submission at `WT=0260 pos=608`, expected token
and finish IRQ, and complete drain. Green proves these generated-only SU-size
writes suppress the otherwise valid primitive. Red rules them out and moves the
challenge method to the generated matrix-index/extra-texture-matrix group.

Hardware result: **red with the same narrow green sliver at the left edge as the
unchallenged positive control.** The fresh log validates 640x480 mode,
`susize WT=0260 pos=608`, expected token, PE-finish IRQ, and complete drain to
`RDoff=WToff=0x0260`. BP 0x30/0x31 are harmless for this zero-texgen draw and are
ruled out as the generated failure.

A numeric audit found that generated `f32_div_u16(2,640)` and `(2,480)` truncate
to projection words `0x3b4ccccc` and `0x3b888888`, one ULP below libogc's rounded
`0x3b4ccccd` and `0x3b888889`. This is a real precision bug to fix before final
driver work, but it shifts edges inward by far below one pixel and is not a
credible explanation for a completely green 640x480 frame.

Next challenge the proven frame with the generated TEV paired-register payloads:
BP 0x28=`0`, BP 0xf6=`4`, and BP 0xf7=`0x0e`. These preserve the active stage-0
order and swap selectors but clear paired unused-stage/KSEL fields. Red confirms
those inactive differences are harmless; green shows hardware depends on one of
the supposedly inactive preserved fields.

### Challenge the proven frame with generated TEV paired fields

The active path again uses the exact 564-byte frame, but after copying it into
the FIFO buffer patches these payloads in place without changing command order
or stream length:

```text
BP 0x28: 0x049000 -> 0x000000
BP 0xf6: 0x018064 -> 0x000004
BP 0xf7: 0x01806e -> 0x00000e
```

The generated values retain stage-0's texture-disabled/color-0 order and
identity raster swap selectors while clearing paired stage-1 and konst-select
fields. The prior SU-size challenge bytes are removed.

Built image SHA-256:

```text
9d3cff9017797d808adce8817ae8bba454aefc3ccda8154e6b897622b11d6353
```

Validity requires `tevpairs WT=0240 pos=576`, expected token and finish IRQ, and
complete drain. Red rules out all three paired-register differences together.
Green means one of the fields thought inactive is required and this three-value
group must be split.

Hardware result: **red.** The fresh log validates 640x480 mode,
`tevpairs WT=0240 pos=576`, expected token, PE-finish IRQ, and complete drain to
`RDoff=WToff=0x0240`. Generated BP 0x28, 0xf6, and 0xf7 payloads are all harmless
for this one-stage `GX_PASSCLR` draw and are ruled out together.

Next challenge the proven frame with the generated matrix helper's effective
differences: patch CP 0x30 and XF 0x1018 matrix-index A values to zero, and
prepend its extra eight-register identity texture matrix at XF 0x0078. The
position-matrix index remains PNMTX0 in both paths; this tests whether clearing
the otherwise-unused texture-matrix index fields or writing the extra matrix has
an undocumented effect.

### Challenge the proven frame with generated matrix state

The active path starts with the generated helper's extra 2x4 identity texture
matrix at XF 0x0078, then submits the exact 564-byte proven-red frame after
patching both matrix-index A payloads to the generated value of zero:

```text
CP 0x30:   0x3cf3cf00 -> 0x00000000
XF 0x1018: 0x3cf3cf00 -> 0x00000000
XF 0x0078: 1, 0, 0, 0, 0, 1, 0, 0 (prepended)
```

The position-matrix selection remains PNMTX0. The previous TEV-pair patches are
removed, so this isolates the generated matrix helper's effective differences
as one group.

Built image SHA-256:

```text
38a1c7bdebe89d23cc2f7210cce2c66830faf4f2cbb2f8dcc0cd1063282c711f
```

Validity requires `matrix WT=0280 pos=640`, expected token and finish IRQ, and
complete drain. Red rules out the zero matrix indices and extra XF 0x0078 matrix
together. Green means this group suppresses the proven primitive and must be
split into separate challenges.

Hardware result: **red.** The fresh log validates 640x480 mode,
`matrix WT=0280 pos=640`, token `0x0003`, PE-finish IRQ, and complete drain to
`RDoff=WToff=0x0280`. The generated zero matrix-index A values and extra XF
0x0078 identity texture matrix are harmless for this direct-color draw and are
ruled out together.

Next perform a fresh byte-level comparison between the generated direct-color
setup and the proven frame, then challenge the smallest remaining coherent
state group. Do not split this now-ruled-out matrix group merely because it was
grouped: a red positive-control result proves every change in the group is
compatible with the working frame.

### Omit matrix-index B like the generated helper

The fresh comparison found that the proven frame explicitly loads matrix-index
B through CP 0x40 and XF 0x1019, both with payload `0x00f3cf3c`, while
`gx_load_identity_pos_mtx0()` initializes only matrix-index A. This build
reproduces the generated helper's omission exactly by replacing the complete
commands with equal-length NOPs in the otherwise-proven frame:

```text
frame 0x1a5..0x1aa: CP 0x40 0x00f3cf3c -> six NOP bytes
frame 0x1ab..0x1b3: XF 0x1019 0x00f3cf3c -> nine NOP bytes
```

Unlike writing zero into those registers, omitting the commands preserves the
hardware values inherited after the same green seed used by the generated-path
test. Command positions and total frame length remain unchanged. All patches
from the previous matrix-A/extra-XF challenge are removed.

Built image SHA-256:

```text
0e9598c3d2a5ae43ff38dd52e04456c9579fad0ec5ff8e3a358595aa5935549b
```

Validity requires `nomtxb WT=0240 pos=576`, expected token and finish IRQ, and
complete drain. Green identifies the missing matrix-index B initialization as a
generated-path blocker. Red proves the omission is harmless for this draw under
the same inherited hardware state.

Hardware result: **red.** The fresh log validates 640x480 mode,
`nomtxb WT=0240 pos=576`, token `0x0003`, PE-finish IRQ, and complete drain to
`RDoff=WToff=0x0240`. Omitting both CP 0x40 and XF 0x1019 matrix-index B loads is
harmless under the same inherited state and cannot explain the generated path's
green output.

Next challenge the proven frame with the generated projection helper's two
known one-ULP truncations: XF 0x1020 `0x3b4ccccd -> 0x3b4ccccc` and XF 0x1022
`0xbb888889 -> 0xbb888888`. This is not a credible full-frame failure mechanism,
but it is the smallest remaining exact mismatch and should be closed before
testing larger omitted-state groups.

### Challenge the proven frame with generated projection precision

The active path restores the exact proven frame except for two four-byte
projection payloads, patched in place to the values emitted by
`f32_div_u16()`:

```text
frame 0x12a, XF 0x1020: 0x3b4ccccd -> 0x3b4ccccc
frame 0x132, XF 0x1022: 0xbb888889 -> 0xbb888888
```

These are the generated `2/640` and `-2/480` projection coefficients. No
command is added, removed, or reordered; the prior matrix-index B omissions are
restored to the proven bytes.

Built image SHA-256:

```text
1c73bc84e94cd8ea95e95a979177bf8def5f23d44ba13759e51ea877ecfac438
```

Validity requires `projulp WT=0240 pos=576`, expected token and finish IRQ, and
complete drain. Red rules out these precision differences as expected. Green
would show that even the one-ULP inward edge shift unexpectedly suppresses the
primitive and would make rounding in `f32_div_u16()` the immediate fix.

Hardware result: **red.** The fresh log validates 640x480 mode,
`projulp WT=0240 pos=576`, token `0x0003`, PE-finish IRQ, and complete drain to
`RDoff=WToff=0x0240`. The generated projection coefficients remain a real
rounding defect to fix eventually, but they do not suppress primitive output
and are ruled out as the current blocker.

Next audit state inherited by the proven frame against state explicitly written
by the generated path, then challenge one omitted draw-relevant group. Avoid
copy-only BP registers (copy clear, filter, and scale) unless the audit finds a
specific route by which they can gate rasterization.

### Omit miscellaneous frame-only BP setup

The on-hardware exact replay does not restore the DFF initial snapshot, so XF
state present only in that snapshot cannot distinguish it from the generated
path: both inherit the same Mini/green-seed state. The fresh ordered-stream
audit instead found six explicit pre-draw commands in the proven frame that
neither the generated setup nor the green seed emits. This build replaces each
complete five-byte BP command with NOPs in place:

```text
frame 0x05b: BP 0x4e = 0x000100 (display-copy Y scale)
frame 0x07e: BP 0x53 = 0x30a208 (display-copy filter)
frame 0x083: BP 0x54 = 0x00820a (display-copy filter)
frame 0x088: BP 0x22 = 0x000606 (line/point size)
frame 0x08d: BP 0x0f = 0x000000
frame 0x097: BP 0x0f = 0x000000
```

The proven frame's copy-clear values are retained because the generated path
inherits the same green values from the seed. The post-draw EFB-to-XFB copy is
also unchanged. Projection words are restored to the exact proven values from
the previous test, and total stream length remains unchanged.

Built image SHA-256:

```text
3a160c913de7a95dd916d9aff0b08029d9a852305b89225e18632b97af0fff89
```

Validity requires `nomisc WT=0240 pos=576`, expected token and finish IRQ, and
complete drain. Red rules out all six omitted setup commands together. Green
shows at least one is unexpectedly required before primitive rasterization and
the group must be bisected.

Hardware result: **red.** The fresh log validates 640x480 mode,
`nomisc WT=0240 pos=576`, token `0x0003`, PE-finish IRQ, and complete drain to
`RDoff=WToff=0x0240`. BP 0x4e, 0x53, 0x54, 0x22, and both BP 0x0f writes are
all unnecessary for the working direct-color primitive and are ruled out
together.

Next remove only redundant draw-state rewrites from the proven frame: the
second TEV color-environment write, the second and third TEV alpha-environment
writes, and repeated BP 0x40/0x41/0x43 values. Retain the first authoritative
write of every register and retain the pre-copy BP 0x45 fence. Red proves the
duplicates do not provide an undocumented state-commit effect; green requires
bisecting them.

### Omit redundant draw-state rewrites

The active path restores every command from the proven frame except these eight
redundant five-byte BP writes, which are replaced by NOPs in place:

```text
frame 0x0ab: second BP 0xc0 TEV color environment
frame 0x0b0: second BP 0xc1 TEV alpha environment
frame 0x0b5: third  BP 0xc1 TEV alpha environment
frame 0x0cd: second BP 0x40 Z mode
frame 0x0d2: second BP 0x41 blend mode
frame 0x0d7: third  BP 0x41 blend mode
frame 0x0dc: fourth BP 0x41 blend mode
frame 0x0eb: second BP 0x43 PE control
```

The first identical write of every affected register remains, so final visible
register state is unchanged. BP 0x45 `PE_DONE` at frame offset 0x1e7 and all
post-draw copy commands remain intact. The prior miscellaneous omissions are
restored to the proven bytes, and total stream length remains unchanged.

Built image SHA-256:

```text
d0ff47c9c22d6fff64e1a6a73000e58e7d0e58c2773de8cb352d5cb61c432f1c
```

Validity requires `nodup WT=0240 pos=576`, expected token and finish IRQ, and
complete drain. Red rules out all redundant writes as hidden state-commit
operations. Green means repetition or one write's later position matters and
the group must be bisected.

Hardware result: **red.** The fresh log validates 640x480 mode,
`nodup WT=0240 pos=576`, token `0x0003`, PE-finish IRQ, and complete drain to
`RDoff=WToff=0x0240`. Repeated BP 0xc0/0xc1 TEV state and BP 0x40/0x41/0x43 PE
state have no hidden commit effect and are ruled out together.

Next replace only the frame's pre-copy BP 0x45 `PE_DONE` command at offset
0x1e7 with five NOPs. Retain the following 32 NOP bytes and every copy command.
Red proves the internal draw-done marker is not what makes the exact frame
visible; green identifies the fence itself as required even though the earlier
generated split-submission test used the same register.

### Omit the pre-copy PE_DONE command

This build restores the exact proven frame except for bytes `0x1e7..0x1eb`,
where `BP 0x45 = 0x000002` is replaced with five NOPs. The following 32 NOP
bytes remain in place, as do all draw state, vertex data, post-draw copy state,
the copy execute command, and the submit helper's final BP 0x48/BP 0x47 token
marker. The submit helper does not append a separate BP 0x45 finish marker.
The prior duplicate-state omissions are fully restored.

Built image SHA-256:

```text
fe821ded8ba86745a94b5c458a2e1ec5b5e9ca954ceb7ffe7941c171995ff1ee
```

Validity requires `nofence WT=0240 pos=576`, expected final token value, and
complete drain. No third finish IRQ should occur because this test removes the
submission's only BP 0x45 command. Red proves the frame's internal BP 0x45
command is not required to make its primitive visible before the copy. Green
makes that fence the distinguishing mechanism despite the earlier generated
fence test.

Hardware result: **red.** The fresh log validates 640x480 mode,
`nofence WT=0240 pos=576`, token value `0x0003`, and complete drain to
`RDoff=WToff=0x0240`. As expected after removing the only BP 0x45 command, no
third PE-finish IRQ occurred and the state machine remained in `WAIT_DRAW`.
This independently confirms that BP 0x48/BP 0x47 token completion does not
generate a PE-finish interrupt. The red frame proves BP 0x45 is unnecessary for
primitive visibility or ordering before this copy.

Next restore BP 0x45 and physically remove only the 32 NOP bytes at frame
offsets 0x1ec..0x20b. Red rules out post-draw FIFO padding/pacing; green shows
that the otherwise no-op byte gap is required between primitive completion and
copy execution.

### Remove the post-draw 32-NOP block

The loader now emits bytes `0x000..0x1eb` of the exact proven frame followed
immediately by original bytes `0x20c..0x233`, physically omitting only the 32
NOP bytes at `0x1ec..0x20b`. This reduces the replay from 564 to 532 bytes. The
BP 0x45 command at `0x1e7` is restored, and the XFB-address patch follows the
compacted copy command to emitted offset `0x20c`.

No state or payload value changes. With the submit helper's ten token bytes and
final DMA alignment, the FIFO length becomes 544 bytes.

Built image SHA-256:

```text
956ddf4c31185b034eaeca3a4e785f04ea752018428210b4b7122ad20b5da7e1
```

Validity requires `nonops WT=0220 pos=544`, expected token value, restored
third PE-finish IRQ, and complete drain. Red rules out post-draw NOP pacing.
Green means the byte gap between BP 0x45 and copy-state writes is required on
hardware despite containing no semantic commands.

Hardware result: **red.** The fresh log validates 640x480 mode,
`nonops WT=0220 pos=544`, token `0x0003`, restored third PE-finish IRQ, and
complete drain to `RDoff=WToff=0x0220`. The 32-NOP block after BP 0x45 has no
pacing or synchronization effect and is ruled out.

Next restore the post-draw NOP block and physically remove only the first 32
NOPs at frame offsets 0x0f..0x2e, immediately after initial BP 0x43. Red rules
out the capture's remaining internal NOP pacing; green identifies a required
delay or write-gather boundary before the subsequent setup commands.

### Remove the initial 32-NOP block

The loader emits original bytes `0x000..0x00e` followed immediately by original
bytes `0x02f..0x233`, physically omitting only the initial 32 NOP bytes at
`0x00f..0x02e`. The post-draw NOP block from the previous test is fully
restored. This again produces a 532-byte replay and moves the XFB-address patch
to emitted offset `0x20c` without changing its source command.

Every semantic command and payload remains byte-exact with the proven frame.
With the submit token and final alignment, total FIFO length remains 544 bytes.

Built image SHA-256:

```text
51c9442318c178645fde6b9ceabf7cd5d65b891d43ed08c35defb2431d530174
```

Validity requires `noinitnop WT=0220 pos=544`, expected token and third finish
IRQ, and complete drain. Red rules out the initial NOP block as pacing. Green
means a no-op delay or gather boundary after the initial BP 0x43 is required.

Hardware result: **red.** The fresh log validates 640x480 mode,
`noinitnop WT=0220 pos=544`, token `0x0003`, third PE-finish IRQ, and complete
drain to `RDoff=WToff=0x0220`. Neither captured 32-NOP block provides required
pacing, synchronization, or a write-gather boundary.

Next restore the untouched 564-byte frame and prepend the complete generated
`gx_setup_vertex_color_state()` output. Red proves that generated setup commands
do not poison hidden state and shifts the failure toward commands/order absent
from the generated-only stream. Green proves at least one generated write is
harmful despite later proven state overwriting all ordinary shared registers,
giving a bounded setup stream to bisect.

### Prefix the proven frame with the complete generated setup

The active path restores the original untouched 564-byte frame and emits the
complete 370-byte `gx_setup_vertex_color_state(640, 480)` stream immediately
before it. This includes every generated BP, CP, XF, position-matrix,
texture-matrix, viewport, projection, and vertex-format write, including the
corrected BP 0x59 value and all previously challenged differences. The proven
frame then overwrites shared visible state before issuing its known-good draw
and copy.

The resulting byte count before alignment is:

```text
370 generated setup + 564 proven frame + 10 token marker = 944 bytes
944 padded to the next 32-byte boundary = 960 bytes (0x03c0)
```

Built image SHA-256:

```text
42514106468d29f48629b5ffb5aec9ecfc443560b4511321b98ee705f6466e0e
```

Validity requires `preset WT=03c0 pos=960`, expected token and third finish IRQ,
and complete drain. Red proves the complete generated setup does not poison
hidden pipeline state. Green proves at least one generated command has a
persistent harmful effect and makes this 370-byte prefix the next bisect range.

Hardware result: **full-width red with no historical green sliver at the left
edge.** The fresh log validates 640x480 mode, `preset WT=03c0 pos=960`, token
`0x0003`, third PE-finish IRQ, and complete drain to `RDoff=WToff=0x03c0`.
Therefore none of the 370 generated setup bytes poisons persistent hidden state;
the generated-only failure is instead caused by a missing command or command
ordering before its draw. The disappearance of the sliver shows that some
generated prefix state also corrects exact-frame edge coverage. Correct BP 0x59
is the leading explanation given the earlier bad-offset clipping result, but
this grouped test does not isolate attribution.

Next return to the generated-only draw/copy path and re-emit its vertex-consumer
state immediately before the primitive in the same order libogc deferred it in
the proven frame: gen mode; VCD; XF VtxSpec; VAT; XF channel count/controls and
texgen count; then CP/XF matrix-index A. Red identifies late ordered state
commit as the missing mechanism. Green rules out this complete 95-byte tail and
leaves earlier raster-state ordering as the next target.

### Re-emit the libogc-ordered vertex-consumer tail

The active path restores the previously green generated-only `drawcopy`
topology: generated setup, direct RGBA8 red quad, BP 0x45, 32 NOPs, green copy
clear state, and clear-enabled EFB-to-XFB copy in one submission. The only added
commands are this 95-byte tail immediately before the primitive:

```text
BP 0x00 = 0x000010
CP 0x50 = 0x00002200; CP 0x60 = 0
XF 0x1008 = 1
CP 0x70 = 0x40016008; CP 0x80 = 0x80000000; CP 0x90 = 0
XF 0x1009 = 1; XF 0x100e = 0x401; XF 0x1010 = 0x401
XF 0x103f = 0
CP 0x30 = 0; XF 0x1018 = 0
```

This reproduces the proven frame's deferred vertex-consumer ordering using the
generated helper's already-challenged matrix-index A value. Matrix-index B is
not added because its complete omission has already passed a red controlled
challenge. The byte count is 608 before the submit token, 618 afterward, and
640 after DMA alignment.

Built image SHA-256:

```text
5bb0ca116973141900bab2d6959b739a7aed7f849e27efdbbd1f6019f21da482
```

Validity requires `tail WT=0280 pos=640`, expected token and third finish IRQ,
and complete drain. Red identifies late ordered vertex-state emission as the
missing generated-path mechanism and requires reducing the tail. Green rules
out the entire tail and shifts ordering work to earlier raster/XF state.

Hardware result: **solid green.** The fresh log contains the unique test
markers and validates `tail WT=0280 pos=640`, token value `0x0003` after 410
microseconds, the third PE-finish IRQ, and complete drain to
`RDoff=WToff=0x0280`. The entire 95-byte late vertex-consumer tail is therefore
ruled out as the missing generated-path requirement. Command submission,
downstream BP execution, and finish signalling all completed normally; the
primitive still made no visible EFB write.

Next decode both command streams mechanically and compare their ordered BP,
CP, and XF writes. The generated setup is proven non-poisoning and the complete
late vertex tail is proven insufficient, so the next test should isolate a
specific missing command or an earlier raster/XF ordering difference rather
than adding another broad state group.

### Challenge the projection Z-translation word

A mechanical decode of the complete proven frame exposed a projection mismatch
that the earlier projection-precision challenge did not cover. The proven
frame's seven-word `XF 0x1020..0x1026` load sets word 5 (`XF 0x1025`, the Z
translation coefficient) to `0xbf800000` (`-1.0`). The generated
`gx_setup_vertex_color_state()` path instead writes `0x00000000` there. The
earlier challenge changed only the one-ULP-different X and Y scale words at
frame offsets `0x12a` and `0x132`, so its red result says nothing about this
larger Z-translation difference.

This build restores the untouched proven 564-byte frame and changes only its
four bytes at offset `0x13e` from `bf 80 00 00` to `00 00 00 00`. All other
state, command ordering, vertex data, fence, copy, and XFB-address patching are
unchanged. The ruled-out late-tail helper is removed from the active source.

Built image SHA-256:

```text
df8a4d765d9c491d8c735c0e2232dc5f46cd17eacdd1419966a8b5ac084f6a98
```

Validity requires `projz WT=0240 pos=576`, token `0x0003`, third PE-finish IRQ,
and complete drain. Red rules out the generated Z-translation value. Green
identifies this exact projection word as necessary for primitive visibility and
directly motivates correcting the generated projection helper.

Hardware result: **solid green.** The fresh log validates the unique
projection-Z test markers, `projz WT=0240 pos=576`, token `0x0003` after 410
microseconds, the third PE-finish IRQ, and complete drain to
`RDoff=WToff=0x0240`. Changing only `XF 0x1025` from the proven frame's `-1.0`
to the generated path's `0.0` is sufficient to suppress the otherwise proven
red primitive. This identifies the projection Z-translation word as a real
generated-path blocker; correct the generated orthographic projection to emit
`-1.0` before returning to that path.

### Retest the generated path with corrected projection Z

The active diagnostic returns to the generated-only contiguous `drawcopy`
topology that previously produced solid green: generated direct-colour setup,
red RGBA8 quad, BP 0x45, 32 NOPs, green copy-clear state, and clear-enabled
EFB-to-XFB copy in one submission. Captured FIFO bytes are not used.

The only generated-state correction is projection word `XF 0x1025`, changed
from `0x00000000` to the proven frame's `0xbf800000` (`-1.0`). This directly
tests the blocker isolated by the preceding one-word challenge without adding
the ruled-out late vertex-state tail or changing any other draw/copy state.

Built image SHA-256:

```text
d0c032659fd26ab11874ca38c43dfde511dae8de41cf1bae2e77c2d1c2230b83
```

Validity requires `drawcopy WT=0220 pos=544`, token `0x0003`, third PE-finish
IRQ, and complete drain. Red proves the driver-generated direct-colour path is
operational and closes the primitive-rendering reverse-engineering phase.
Green means the projection fix is necessary but a second generated-path blocker
remains.

Hardware result: **solid red.** The fresh log validates the generated-only
`drawcopy` stream at `WT=0220 pos=544`, token `0x0003` after 720 microseconds,
the third PE-finish IRQ, and complete drain to `RDoff=WToff=0x0220`. No captured
FIFO bytes were used. This confirms that correcting `XF 0x1025` from `0.0` to
`-1.0` fixes the driver-generated primitive path and closes the direct-colour
EFB-write investigation.

Next retain the corrected projection and move to a controlled RGB565 texture
test. Start with a CPU-generated tiled pattern in the reserved texture buffer,
then use the generated texture state, textured quad, and validated EFB-to-XFB
copy. A recognizable pattern provides a positive control for tiling, texture
addressing, cache invalidation, texcoord generation, and TEV sampling before
connecting the live Linux virtual framebuffer.

### Test a generated tiled RGB565 texture

The active diagnostic fills the reserved MEM1 texture buffer directly in GX
4x4-tiled RGB565 layout with four full-screen quadrants: red at top left, green
at top right, blue at bottom left, and white at bottom right. It flushes the
entire 640x480 texture before submission.

The test keeps the now-proven generated direct-colour setup and corrected
`XF 0x1025=-1.0` projection. It adds one position-derived 2x4 texcoord generator
through `GX_TEXMTX0`, enables texmap 0 in stage-0 BP `0x28`, changes TEV stage 0
to texture colour/alpha passthrough, binds the MEM1 buffer as RGB565, invalidates
the texture cache twice, and draws the same proven XY+RGBA8 quad. The raster
colour remains in the vertex format but TEV ignores it. Draw, fence, green
copy-clear state, and EFB-to-XFB copy remain one contiguous submission.

Built image SHA-256:

```text
c5c280d9a75882de75a4da41982fb1c444420e82c3ba7011675d7d5ed9973971
```

Validity requires `texquad WT=02a0 pos=672`, token `0x0003`, third PE-finish
IRQ, and complete drain. Four correctly placed colour quadrants validate the
tiler layout, MEM1 texture addressing, cache flush/invalidate, position texgen,
TMU fetch, and TEV texture passthrough as a group. Solid green means no visible
textured write; any other pattern should be described precisely before reducing
the setup or connecting the live framebuffer.

Hardware result: **all four quadrants rendered correctly.** The fresh log
validates `texquad WT=02a0 pos=672`, token `0x0003` after 800 microseconds, the
third PE-finish IRQ, and complete drain to `RDoff=WToff=0x02a0`. This is a
successful positive control for the complete generated RGB565 texture path:
4x4 tiling, MEM1 addressing, CPU cache flush, GX texture-cache invalidation,
position-derived texcoords, TMU fetch, TEV passthrough, EFB rendering, and
EFB-to-XFB display copy.

Next preserve this exact command path and replace only the generated quadrant
source with the live linear RGB565 virtual framebuffer passed by `gcnfb`. Tile
and submit it continuously after the diagnostic startup so visible console
updates validate real framebuffer integration and repeated-frame stability.

### Render the live Linux RGB565 framebuffer continuously

The active path removes the generated quadrant filler and uses the existing
`gx_tile_rgb565()` conversion on the `vfb_mem` pointer passed by `gcnfb`. Each
frame is tiled into the same validated MEM1 texture buffer, flushed, bound, and
rendered through the unchanged position-texgen/TMU/TEV/EFB/XFB command path.

After the two startup green controls, `live0` submits the first framebuffer
image and waits for its PE-finish IRQ. The `GX_DIAG_DONE` state then submits a
fresh `live` frame on every subsequent VI callback. This deliberately retains
the complete diagnostic state setup, BP 0x45/NOP gap, cache invalidation, and
clear-enabled copy while testing integration and stability; reduction and
performance cleanup come after visible live output is established.

Built image SHA-256:

```text
4170bb24fc6100c830e89adf487c4227a4d2b613448e1389c2e9468e335ad8cd
```

Validity requires both `live0 WT=02a0 pos=672` with token `0x0003` and a later
`live WT=02a0 pos=672` with token `0x0004`, finish IRQs, and complete drains.
Success is a correctly oriented, correctly coloured Linux console that visibly
updates when text is entered. A static snapshot, corruption, wrong quadrant-like
layout, or green screen must be reported separately.

Hardware result: **live rendering works, but is not yet stable.** Boots one and
three displayed the correctly oriented Linux console; the third showed a
blinking cursor, proving repeated framebuffer updates. The second boot instead
showed about 20 repeated vertical copies of the console while still responding
to keyboard input. On the successful third boot, the display stopped updating
when `init-diag.sh` wrote its log and called `sync`. The first boot also appeared
to fail around USB-keyboard hotplug.

The fresh third-boot log validates `live0 WT=02a0 pos=672`, token `0x0003`,
finish IRQ 3, and complete drain, followed by `live WT=02a0 pos=672`, token
`0x0004`, finish IRQ 4, and complete drain. No FIFO slow/stall warning was
recorded. The log snapshot necessarily ends before the post-write freeze because
the script is writing that same snapshot.

The current implementation performs the entire 640x480 tiling pass, cache
flush, command construction, token wait, and GP-idle wait from the VI hard-IRQ
handler. The first live frame spends about 9 ms tiling/building before the FIFO
pre-log and then remains in submission long enough for the logged handler path
to exceed a 16.7 ms frame period. This is not acceptable interrupt latency and
fits both the SD-I/O and USB-hotplug failures.

Next preserve the exact validated texture command path but have the VI IRQ only
queue coalesced work. Perform tiling and GX submission from workqueue context;
if a frame is already queued/running, allow the next callback to coalesce rather
than blocking hard IRQs. Re-run the existing 20-second log-write/sync sequence
and keyboard hotplug as stress tests.

### Move live RGB565 rendering out of the VI hard IRQ

The active test preserves the byte-for-byte generated RGB565 texture, draw,
fence, clear, and EFB-to-XFB submission path that produced a working console.
Only its scheduling changes. `gcn_gx_blit_fb_rgb565()` now snapshots the stable
framebuffer parameters and queues one `work_struct`; the worker performs the
tiling pass, cache flush, command construction, token polling, and FIFO drain.
Repeated VI callbacks coalesce while work is pending instead of extending hard
IRQ latency or starting concurrent GX submissions.

Worker-run markers at runs 1, 60, and 300 provide a positive control that
deferred rendering continues. Module teardown first disables acceleration and
then synchronously cancels pending work. The diagnostic phase is reset during
initialization so a future unload/reload starts from the known seed sequence.

Built image SHA-256:

```text
760459b00ec1f02cda0323a4144f6ff2c488eadd9a758ede30a24e19e7d0c5a9
```

Validity requires the same `live0 WT=02a0 pos=672` and subsequent
`live WT=02a0 pos=672` token/finish/drain results as the IRQ-context image,
plus worker progress markers. Success requires the console to remain live
through the 20-second `dmesg` write, `sync`, slot-LED blink, and USB keyboard
activity. Repeated columns, a post-write freeze, or FIFO/token errors are
separate failures and must be recorded precisely.

Hardware result: **failed, with a new actionable kernel diagnostic.** The
console contained recognizable but severely blurred text and a blinking cursor,
then visibly stopped updating around the diagnostic SD write. The script itself
continued through `after blink` and `before shell`, proving this remained a
display-path failure rather than a CPU crash.

The worker positive controls passed: run 1 at 0.385 seconds, run 60 at 5.117
seconds, and run 300 at 13.125 seconds. `live0` and the following `live`
submission both reached their expected PE tokens and drained to
`RDoff=WToff=0x02a0`. However, the first live submission overlapped built-in
EHCI initialization and exposed `BUG: spinlock bad magic` in `ehci_halt()` at
0.494 seconds. The affected lock was the EHCI object's lock at `0xd2d139e4`,
not the GX work-parameter lock. PID 1 then reported a suspected lockup for about
2.6 seconds before initialization continued. EHCI subsequently also emitted a
DMA-debug warning while unmapping an unallocated address-zero transfer.

Do not treat this run as a clean evaluation of system-workqueue stability or
image quality. The immediate next test must suppress GX work until
`system_state == SYSTEM_RUNNING`, leaving the worker, command stream, and
single-XFB behavior unchanged. If that removes the EHCI diagnostics, boot-time
driver initialization is a required scheduling boundary. The blur separately
motivates vblank presentation to a back XFB after worker rendering is stable.

### Gate deferred GX rendering on completed kernel initialization

The active test adds only a boot-state gate to the preceding workqueue image.
VI callbacks return without queueing GX work while `system_state` is
`SYSTEM_BOOTING`. The first callback after `kernel_init()` has completed all
built-in and asynchronous init work, freed init memory, and set
`SYSTEM_RUNNING` starts the unchanged seed/green/live worker sequence.

Expected markers are one `deferring RGB565 worker until SYSTEM_RUNNING` line
during framebuffer probe and one `SYSTEM_RUNNING; enabling RGB565 worker` line
after all built-in driver probes. No GX seed or live submission may overlap the
EHCI probe. The existing worker-run and FIFO/token/drain criteria remain
unchanged. A clean test must contain neither the EHCI `spinlock bad magic`
failure nor its address-zero DMA-debug warning. This isolates boot ordering; it
does not attempt to fix the known blur from copying into the visible XFB outside
vertical retrace.

Built image SHA-256:

```text
bbf7629e619aa6abefb44a2681373b2974bee77998e08c811f1006791797f991
```

Hardware result: **the boot gate worked as ordered, but did not fix either
visible symptom.** The display remained blurry and stopped visibly updating
immediately after the `random: nonblocking pool is initialized` line. The
fresh file begins at uptime 4.11, records the script's `after sleep` marker at
24.16, and reaches `after blink` and `before shell` at 28.52/28.55 seconds.
Thus the apparent freeze precedes the diagnostic SD write and remains confined
to presentation; userspace and the kernel continue running.

The gate markers validate that no GX work ran before `SYSTEM_RUNNING` at 3.716
seconds. Nevertheless, EHCI hit the same bad lock at 0.438 seconds and the same
address-zero DMA warning before the gate opened. This proves those EHCI
diagnostics are independent of GX execution and must not be used to evaluate
the graphics worker. After the gate opened, runs 1, 60, and 300 and the first
four token/drain checks all passed.

The next test should use the two physical XFB pages already allocated and
tracked by `gcnfb`: the worker renders only to the non-visible page, marks that
page complete after the PE token/FIFO drain, and the VI DI1 handler flips to it
during vertical retrace before queueing the next back-page render. This removes
the current race where the VI scans the same XFB that GX is asynchronously
rewriting. Add later worker milestones around the observed 18-28 second window
so a scheduling stop can be distinguished from a presentation failure.

Correction from the established software-fallback control: the console also
appeared to stop after the random-pool message when `init-diag.sh` was used with
software transcoding, while removing the diagnostic init script restored normal
console operation. Therefore the post-random apparent freeze is a property of
the temporary init environment, not evidence that GX or its workqueue stopped.
Do not use it as a graphics acceptance criterion. The severe blur remains a
real GX presentation defect and still motivates hidden-XFB rendering plus a
vertical-retrace flip.

### Present completed frames through the hidden XFB at vertical retrace

The active graphics test changes the worker/VI contract without changing the
validated RGB565 GX command stream. `gcnfb` now passes its non-visible physical
XFB page to the worker. After the PE token and FIFO drain prove the copy has
completed, the worker publishes that page but does not immediately reuse it.
At the next VI DI1 interrupt, `gcnfb` switches the VI framebuffer registers to
the completed page during vertical retrace and queues the next render against
the opposite page.

An explicit busy/ready state prevents a queued or running worker from
overwriting either the page currently scanned by VI or a completed page waiting
to be presented. The first four flips log their alternating physical addresses.
Worker milestones 450, 600, and 750 extend diagnostics through the former
18-28 second ambiguity, although normal `/sbin/init` is restored by commit
`bb9223f340cd` and the diagnostic-script pause is no longer an acceptance
criterion.

Built image SHA-256:

```text
cf7131417ba2e6d2042ad02bccd513c758b151ce2a77f0a953cb81582216be6d
```

Validity requires alternating `present` addresses matching `gcnfb`'s two page
addresses, continued `WT=02a0` token/drain success, and a normally booted getty.
The visual success criterion is a sharp, stable, correctly oriented console
with a responsive blinking cursor and keyboard input. Any repeated columns,
blur, stale half-frame, page alternation failure, or new FIFO warning is a real
graphics failure.

Hardware result: **page ownership and presentation are stable; image sharpness
and userspace console setup remain unresolved.** The display booted normally,
kept a blinking cursor, and did not freeze. Shift+PageUp/PageDown continued to
scroll the kernel console, but ordinary typing produced no login input. The
image remained severely blurry.

The persistent normal-init log validates exact page alternation for the first
four presentations: `0x0172e000`, `0x01698000`, `0x0172e000`, then
`0x01698000`. Both initial live submissions reached their PE tokens and drained
to `RDoff=WToff=0x02a0`. Worker milestones continued through runs 60, 300, 450,
600, and 750 at approximately 5.8, 13.8, 18.8, 23.8, and 28.9 seconds. This is
positive evidence that the worker and vblank page-flip protocol remain active
well beyond the former apparent-freeze interval. No presentation or FIFO
failure was found.

The missing typing is independently explained by the root filesystem's
`auth.log`: every getty for `/dev/tty1` through `/dev/tty6` exits with `No such
file or directory`, while `.config` has `CONFIG_DEVTMPFS` disabled. The working
scrollback keys are handled by the kernel console and therefore do not prove a
getty is attached. Fix this separately by enabling `CONFIG_DEVTMPFS` and
`CONFIG_DEVTMPFS_MOUNT`; do not attribute it to GX.

The next graphics-only sharpness test should clear bit 4 of texture mode BP
`0x80`, changing the active value from `0x80000090` (linear magnification) to
`0x80000080` (nearest magnification). Do not combine that one-bit sampler test
with changes to the validated tiling, FIFO stream, copy filter, or XFB
presentation path.

### Restore virtual-terminal device nodes with devtmpfs

The active apparatus test enables `CONFIG_DEVTMPFS` and
`CONFIG_DEVTMPFS_MOUNT` in the tracked Wii defconfig and the build
configuration. The preceding normal-init boot proved that all six gettys were
failing because `/dev/tty1` through `/dev/tty6` did not exist. Kernel console
scrollback still worked because it does not require a userspace getty, which
explains the otherwise contradictory keyboard observation.

No GX source, command, filter, scheduling, or presentation value changes in
this image. Success requires a visible `tty1` login prompt that accepts typed
input, with no repeated getty failures in `auth.log`. The existing blurry but
stable alternating-XFB output should remain unchanged. This is an apparatus
repair, not the pending texture-filter sharpness test.

Built image SHA-256:

```text
cf1e10470ca3293b37719724bf5e34507b533a1fa7994ff35543a1f133e5f54a
```

Hardware result: **devtmpfs/getty passed; graphics outcome was mixed across two
boots of the identical image.** On the successful boot, `devtmpfs: mounted`
appeared at 3.73 seconds, `auth.log` recorded `ROOT LOGIN on '/dev/tty1'`, and
the user confirmed that the console was sharp and keyboard input was
responsive. No new getty `No such file or directory` failures occurred. This
validates the apparatus fix and the tracked configuration change.

The first boot of the same checksum remained dark green and was reset after
about 30 seconds without a login test. Its GX diagnostics were nevertheless
indistinguishable from the successful boot at the current level: both reached
the first live texture frames, alternated the first four XFB presentations,
drained every logged FIFO, and continued through worker run 750. The first
boot's log ends abruptly after run 750; the second ends in an orderly halt
after the successful login.

Do not apply the planned nearest-magnification change yet. A sharp result with
the unchanged `0x80000090` texture mode proves blur is not a deterministic
consequence of that bit. Reboot this exact image several more times and record
whether each run reaches a visible sharp console or remains on the green seed.
The identical GX logs for opposite visual outcomes suggest that the next useful
diagnostic, if the green outcome repeats, is VI framebuffer-register readback
after each presentation rather than another raster-state change.

Two further boots of the same checksum refine the result. One again produced a
stable, sharp console with working keyboard input. The next produced a stable
but visibly blurry console, including a recognizable Tux logo at the login
screen. Together with the earlier dark-green boot, the identical binary has
now produced three visual states while continuing to render and accept input.
This confirms that sharpness is nondeterministic and retracts the proposed
one-bit magnification-filter test as the immediate next step.

Source comparison then found that the active texture-cache invalidation does
not in fact match libogc despite its comment. This driver writes
`0x66000000` twice. Libogc's `GX_InvalidateTexAll()` writes its texture-state
flush (`BP 0x0F`), then `0x66001000`, then `0x66001100`, then the texture-state
flush again. Stale or partially invalidated TMEM is consistent with green,
sharp, and blurry results from the same main-memory texture stream. Test that
exact four-command sequence before adding VI readback or changing filtering.

### Match libogc's complete texture-cache invalidation

The active test replaces the incorrect pair of `0x66000000` writes with
libogc's exact `GX_InvalidateTexAll()` command sequence:

```text
BP 0x0F = 0x000000
BP 0x66 = 0x001000
BP 0x66 = 0x001100
BP 0x0F = 0x000000
```

The first and last writes are libogc's `__GX_FlushTextureState()` with the
zero indirect-texture mask. The two BP 0x66 payloads are the actual full-cache
invalidate commands. The preceding code's claim that two zero-payload writes
matched libogc was factually wrong. No texture mode/filter, TMEM region, vertex,
copy, worker, XFB, or VI value changes in this image.

Built image SHA-256:

```text
37244facf4f73cf324b435e28c5fd57ff2e28edff03977d43bfc5bfb4229ef47
```

Validity still requires the initial token/FIFO drains, alternating XFB
presentations, and continued worker milestones. Because the previous checksum
intermittently produced dark green, sharp, and blurry output, success requires
at least three consecutive sharp and responsive console boots. Any green seed
that persists after live submissions or any blurry console is a failure of
this cache-invalidation hypothesis.

Hardware result: **inconclusive for texture invalidation; all three boots had
a display-only presentation failure before the changed commands mattered.**
The last visible line was `fifo_init: done`, but persistent logs prove that the
kernel continued normally: root and devtmpfs mounted, normal init ran, the GX
seed and live phases reached their PE tokens and drained, XFB presentation
markers alternated, worker milestones reached run 750, and at least one run
ended through an orderly userspace reboot.

The known green seed copy is submitted and presented before
`gx_setup_texture_rgb565()` emits the corrected invalidation sequence. It also
failed to become visible. Therefore these runs neither validate nor reject the
libogc invalidation fix. They instead reproduce the existing intermittent
disconnect between successful software `present` markers and what VI scans.

The next diagnostic must correlate three facts for each of the first four
presentations: the completed physical XFB address, uncached words read directly
from that XFB page after GX completion, and hardware readback of VI TFBL/BFBL
immediately after `vi_set_framebuffer()`. This separates failure to write XFB,
failure to latch the VI page address, and failure downstream of correctly
latched registers. Keep the corrected cache sequence in place until that
earlier presentation failure is isolated.

### Correlate completed XFB contents with VI hardware readback

The active diagnostic keeps the corrected libogc cache invalidation and all GX
and VI writes unchanged. For each of the first four completed pages,
`vi_gx_present_rgb565()` now reads back VI TFBL and BFBL immediately after
`vi_set_framebuffer()`. It also reads the first, middle, and last 32-bit words
of that physical XFB through the existing uncached `fb_mem` mapping.

Built image SHA-256:

```text
171ea7aec4629594910073971564f0eb84eaeec6246a4c5bf5e3ea65db895354
```

Each `GX present diag` line reports the completed XFB, raw TFBL/BFBL, and three
XFB words. Presentation 2 is the strongest positive control: it copies the
known uniform green EFB produced by the seed clear, so all three XFB samples
must be the same valid YUYV word. Presentations 3 and 4 should contain live
console pixels and normally produce nonuniform samples. TFBL/BFBL must change
with the alternating XFB address. If the display remains stale, these values
distinguish among an unwritten XFB, a VI register that failed to latch, and a
failure after both memory and VI state are correct.

Hardware result: **VI page programming passed, but the XFB sample mechanism
failed its positive control and is not usable as pixel evidence.** The screen
remained a visually uniform dark green. The first return of the card appeared
to contain no fresh log because the ext3 root filesystem had journal and inode
damage; an offline `e2fsck -f -y` recovered the appended kernel log and repaired
the filesystem. The image on the boot partition still matched the deployed
SHA-256 above exactly.

The recovered first four readbacks were:

```text
diag 1 xfb=0172e000 TFBL=100b9700 BFBL=000b9728 XFB=b2a59247/a4a5bd3a/6493ca84
diag 2 xfb=01698000 TFBL=100b4c00 BFBL=000b4c28 XFB=a296e35b/2f6e2f6a/eb67d481
diag 3 xfb=0172e000 TFBL=100b9700 BFBL=000b9728 XFB=a296e35b/2f6e2f6a/eb67d481
diag 4 xfb=01698000 TFBL=100b4c00 BFBL=000b4c28 XFB=a296e35b/2f6e2f6a/eb67d481
```

The TFBL/BFBL values decode to the exact requested top-field address and the
corresponding one-line-offset bottom-field address for both alternating pages.
This is a successful positive control for the VI register writes and rules out
failure to program the selected page. By contrast, presentation 2's three XFB
words are nonuniform despite the known uniform-green copy, and presentations
2 through 4 return the same three words from different pages while the visual
frame is uniform. This project has previously demonstrated false conclusions
from sparse CPU XFB samples. Treat these words as a failed measurement, not as
proof that the displayed page contains those pixels.

The active generated path still leaves display-copy state inherited from Mini.
In particular, the initial seed copies run before BP 0x43/0x44/0x68 and sample
positions are initialized, while all copies omit BP 0x4e and BP 0x53/0x54.
The next test should initialize that coherent display-copy group once before
the first seed using the values from the independently proven libogc capture.
This directly targets boot-to-boot persistence of green/sharp/blurry output
without changing tiling, texture addressing, TEV, geometry, page ownership, or
VI programming.

### Initialize deterministic display-copy state before the seed

The active test writes the complete known display-copy state once at the start
of the first seed submission, before any EFB clear or EFB-to-XFB copy:

```text
BP 0x42 = 0x000000  destination alpha disabled
BP 0x43 = 0x000040  RGB8/Z24 EFB, linear Z
BP 0x44 = 0x000003  update both fields
BP 0x68 = 0x000000  field mode disabled
BP 0x01-0x04 = 0x666666  centered non-AA samples
BP 0x53 = 0x30a208  libogc vertical-filter coefficients 0-3
BP 0x54 = 0x00820a  libogc vertical-filter coefficients 4-6
BP 0x4e = 0x000100  display-copy Y scale 1.0
```

These values are byte-exact with the independently captured libogc frame that
rendered red both on Wii hardware and in Dolphin FIFO Player. The generated
live path already wrote BP 0x42-0x44, BP 0x68, and BP 0x01-0x04, but only after
the initial seed copies; it never wrote BP 0x4e or BP 0x53/0x54. Consequently,
copy behavior and filtering could retain Mini state across every frame and
vary between boots. No texture, TEV, geometry, cache invalidation, XFB
ownership, or VI register value changes in this test.

Built image SHA-256:

```text
8daa81f756ad3dc33342e9b2ecc522228d89e4bf1a0e5e15884f7202f8a02b9c
```

The added eleven BP writes increase only the first `seed` submission from
`WT=0060` to `WT=0080`; the following `green` submission remains `WT=0060`
and live texture submissions remain `WT=02c0`. All must reach their PE token
and drain to `RDoff=WToff`. The primary result is visual reproducibility: run
the exact checksum at least three times. A consistently sharp console supports
the missing-state hypothesis; consistently filtered/blurry output still proves
the boot-to-boot variation was removed and permits a later explicit no-filter
test. Any remaining mix of green, sharp, and blurry results rules this group
out as the nondeterminism source.

Hardware result: **failed; the display showed approximately twenty repeated
vertical copies of the console.** The log validates the intended image and the
entire asynchronous pipeline. The first seed grew to `WT=0080`, the following
green copy remained `WT=0060`, and the first two live submissions remained
`WT=02c0`. Every PE token arrived, every FIFO drained to `RDoff=WToff`, VI
TFBL/BFBL alternated between the expected physical pages, and worker milestones
continued through run 750. The display-copy initialization group is therefore
accepted as deterministic state but ruled out as sufficient to fix the live
texture corruption. Sparse XFB sample words remain non-authoritative.

Source inspection then found a concrete error in `gx_setup_texture_rgb565()`.
On Wii, libogc's `__GXDefTexRegionCallback()` sends RGB565 format 4 through the
non-CI/non-CMPR branch to `texRegion[mapid+8]`. For map 0, `GX_Init()` builds
that region from `_gxtexregionaddrtable[16] = 0x00000000` and
`_gxtexregionaddrtable[24] = 0x00080000`. `GX_InitTexCacheRegion()` therefore
encodes BP 0x8c as `0x0d8000` and BP 0x90 as `0x0dc000`.

The driver instead writes BP 0x90 as `0x0d8400`, which is the odd-bank encoding
for address `0x00008000` used by `texRegion[0]`, not the RGB565 region. Its
comment incorrectly claims that indexed region is the RVL RGB565 default. The
safe libogc initialization preamble already contains the correct
`0x900dc000`, but every live `GX_LoadTexObjPreloaded` equivalent immediately
overwrites it with the wrong value. The next hardware test changes only
`0x900d8400` to `0x900dc000` and corrects the comment.

### Select libogc's RGB565 odd TMEM bank

The active test changes only BP 0x90 in `gx_setup_texture_rgb565()` from
`0x900d8400` to `0x900dc000`. This selects the `0x80000` odd TMEM bank from
libogc's `texRegion[mapid+8]`, which its default region callback uses for
RGB565. The old `0x08000` bank belongs to `texRegion[mapid]`, selected for CI
and CMPR formats, and was based on an incorrect source reading.

Built image SHA-256:

```text
9ab3968b2faaaa60ac4ce8deb5f07b8a5e7375d74091170bd774b5255123b773
```

Command counts and FIFO lengths are unchanged: seed must remain `WT=0080`,
green `WT=0060`, and live frames `WT=02c0`, with PE tokens, complete FIFO
drains, and alternating VI pages. A correctly oriented single console image
without repeated columns is success. Green, blur, or repeated columns are
separate failures and must be reported exactly; because recent presentation
is nondeterministic, a successful first boot must be repeated before treating
the bank correction as stable.

Hardware result: **the RGB565 TMEM-bank correction passed.** Three consecutive
boots of the exact checksum produced one coherent, correctly oriented console
image; the approximately twenty repeated vertical copies did not recur. Boot
one was visibly blurry but functional. Boots two and three were clear, showed
the login prompt, and accepted keyboard input. `auth.log` records a successful
root `tty1` login in all three boots, and the third ended through userspace
shutdown after the user issued `sync` and `shutdown now`.

All three kernel logs satisfy the unchanged hardware controls: seed
`WT=0080`, green `WT=0060`, live frames `WT=02c0`, expected PE tokens, complete
`RDoff=WToff` drains, correct alternating TFBL/BFBL page values, and worker
progress through run 750. Keep BP 0x90=`0x0dc000`; the old indexed-texture bank
was a real source-level error and is now hardware-validated for live RGB565.

Do not attribute the remaining one-in-three blur to the fixed TMEM bank or to
the vertical copy filter without another controlled test. The identical image
was clear twice while writing the same filter coefficients. Every boot also
reports `NTSC 480i (initial guess)`, so field timing or external deinterlacer
lock is now a plausible presentation-only cause. Audit the driver's active
`nostalgic`/scan-mode selection before changing texture or copy state again.

Primary references:

- `https://github.com/devkitPro/libogc/blob/master/libogc/gx.c`
- `https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/VideoCommon/BPMemory.h`
- `https://hitmen.c02.at/files/yagcd/yagcd/chap5.html#sec5.2`

### Read VI scan-mode inputs without changing video timing

The active test keeps the hardware-validated RGB565 TMEM-bank correction and
all GX, XFB, and VI writes unchanged. It adds one probe-time diagnostic in
`vi_detect_tv_mode()` reporting raw `VI_DCR` and `VI_SEL`, plus the decoded
component-cable and non-interlaced bits and `force_scan` value.

Built image SHA-256:

```text
0b9b4b7fca56356944412b0bde89c1c04383d51c00b261b8c315d840392785cd
```

This is a positive-control prerequisite for any 480p experiment. A reported
`component=1` permits the existing `progressive` option to select the driver's
480p timing path. A reported `component=0` explains the current 480i selection
and means that merely adding `progressive` to the bootargs will not change the
mode because the driver deliberately gates 480p on `VI_SEL` bit 0. Record the
visual result separately, but do not use this no-behavior-change image to draw
new conclusions about GX rendering or filtering.

Hardware result: **the component-output prerequisite passed, and scan-mode
detection was ruled out as the source of intermittent blur.** Four consecutive
boots produced the visual sequence blurry, blurry, clear, clear. All four
reported the identical raw state `DCR=0001 SEL=0001`, decoded as interlaced
input state (`nin=0`) with the component-output detect bit set and no forced
scan mode. The diagnostic prints `component=-1` because
`has_component_cable` is a signed one-bit C bitfield; it is the driver's
truthy representation of the decoded value 1, not a differing hardware value.

Every boot selected NTSC 480i and retained healthy PE-token and FIFO-drain
controls. Therefore neither VI mode detection nor the GX command stream
correlates with the blur. `SEL=0001` confirms that the existing `progressive`
option can pass its component-cable gate. The next controlled test should add
only `progressive` to the `gcn-vifb` boot option and verify NTSC 480p selection,
a clear stable console, and unchanged GX completion controls.

### Force the existing NTSC 480p output path

The active test adds only `progressive` to the device-tree command line,
changing `video=gcn-vifb:tv=auto,nostalgic` to
`video=gcn-vifb:tv=auto,nostalgic,progressive`. The prior four-boot diagnostic
proved `VI_SEL` bit 0 is set, so the driver's existing cable gate should permit
NTSC 480p. No driver implementation, GX command, texture state, display-copy
state, XFB ownership, or framebuffer geometry changes in this image.

Built image SHA-256:

```text
4905937e877ade497501fa1c4aee6f1604a66c77fa5f2f9526569d131d734ed1
```

The boot log must report `force_scan=2`, select `NTSC 480p`, and configure a
non-interlaced 640x480 framebuffer. The GX controls must retain seed
`WT=0080`, green `WT=0060`, live `WT=02c0`, successful PE tokens, complete
FIFO drains, and continued worker milestones. Visually, run the exact image at
least three times. A consistently clear and responsive console supports 480i
or external deinterlacer lock as the remaining presentation problem. Loss of
signal, malformed output, or continued blur rejects the current progressive
path and requires reverting only this boot option.

Hardware result: **failed; progressive output does not fix the presentation
defect.** The four reported visual outcomes were green, blurry, blurry,
blurry. Three complete logs persisted and all three show `force_scan=2`,
`NTSC 480p`, and progressive VI page programming where TFBL and BFBL point to
the same scanout address. Each also retained seed `WT=0080`, green `WT=0060`,
live `WT=02c0`, successful PE tokens, complete FIFO drains, alternating XFB
pages, and continued worker execution. The initial green boot ended before a
complete rotated log persisted, so it is visual evidence only.

This rules out 480i field timing and external deinterlacer lock as the primary
cause of the repeatable blur. Revert only the `progressive` boot option. The
next texture-only test should then clear texMode0's magnification-filter bit,
changing BP 0x80 from `0x80000090` (linear) to `0x80000080` (nearest). That
one-bit test was proposed before the TMEM-bank correction but never executed
against the now-working `0x900dc000` RGB565 region.

### Use nearest magnification for the 1:1 framebuffer texture

The active test reverts the failed `progressive` boot option to restore the
known NTSC 480i baseline, then changes only texMode0's magnification-filter bit:
BP 0x80 moves from `0x80000090` (linear) to `0x80000080` (nearest). Clamp
wrapping, the non-mipmap minification mode, corrected RGB565 odd TMEM bank,
full texture-cache invalidation, tiling, TEV, geometry, display copy, XFB
ownership, and worker scheduling remain unchanged.

Built image SHA-256:

```text
a864d7946e2bc6c0571049a5be38dfb704fb5a094278e111996731f53f5fbc60
```

The log must return to `force_scan=0` and `NTSC 480i`, with seed `WT=0080`,
green `WT=0060`, live `WT=02c0`, successful PE tokens, complete FIFO drains,
alternating XFB pages, and continued worker milestones. Test at least three
boots. Consistently sharp and responsive text validates nearest sampling for
the 1:1 framebuffer path. Any green, repeated, malformed, or blurry result
means the magnification bit is not sufficient and must be reported separately.

Hardware result: **failed; nearest magnification does not remove the
nondeterminism.** Five consecutive boots of the exact checksum produced green,
clear working console, blurry console, clear working console, then blurry
console. Four complete nearest-filter logs persisted. Every one returned to
`force_scan=0` and NTSC 480i, reached the expected seed, green, and live FIFO
lengths, observed each PE token, drained to `RDoff=WToff`, and alternated VI
pages. `auth.log` also records successful root logins during this test series.

Because one fixed BP 0x80 value produced all three visual states, texture
magnification is not the cause. Keep nearest as a reasonable 1:1 presentation
choice for now, but do not treat it as a stability fix. The next direct-debug
step is to extend the existing libogc reference program with a known tiled
RGB565 texture, capture one complete textured frame in Dolphin, validate it in
FIFO Player, and compare its ordered BP/CP/XF stream with the generated Linux
path. There is currently only a direct-color reference capture, so further
texture-register guesses lack a positive-control stream.

### Match libogc's post-texture-matrix identity index

The new `tools/gx-texture-reference` program renders a deterministic 640x480
tiled RGB565 grid through the same position-generated texture pipeline shape as
the Linux driver. Its live DOL output and a one-frame Dolphin FIFO replay both
showed the identical correct grid, validating capture SHA-256
`df9d2ee358b625886d0fd76ffe60f2c5e4b48f5aaed095a9c0d4062c70bea09c`
as a positive-control texture stream. The capture contains 679 FIFO bytes and
one texture-memory update.

Ordered comparison exposed a concrete active mismatch. Libogc emits XF 0x1050
as `0x0000003d`. This follows directly from its source:
`GX_SetTexCoordGen()` supplies `GX_DTTIDENTITY=125`, and
`GX_SetTexCoordGen2()` stores `postmtx - GX_DTTMTX0`, where
`GX_DTTMTX0=64`; therefore the encoded index is 61 (`0x3d`). The Linux driver
instead writes `0x3f`, selecting an undefined post-texture-matrix index. That
can transform otherwise identical texture coordinates through inherited data
and is a direct mechanism for green, clear, and blurry boot variation.

The active test changes only the live RGB565 path's XF 0x1050 value from
`0x3f` to `0x3d`. It deliberately retains the preceding test's BP 0x80 value,
the corrected TMEM bank, texture invalidation, tiling, TEV, geometry, display
copy, XFB ownership, and worker scheduling. A second capture mismatch exists
in BP 0x80 (`0x100` in the validated nearest-filter capture versus `0x80` in
Linux), but it must remain unchanged until this post-matrix test is evaluated.

Built image SHA-256:

```text
59ca63e5b8f46f198380044bdb8eeb131d6bad92d201374de598059851cc9fbf
```

Test at least five boots because the prior checksum produced all three visual
states within five runs. Every boot must retain the established NTSC 480i,
FIFO-length, PE-token, drain, alternating-page, and worker controls.
Consistently clear, responsive consoles validate the post-matrix correction.
Any green or blurry boot means at least one additional mismatch remains.

Hardware result: **failed as a complete stability fix, but keep the corrected
post-matrix index.** Five consecutive boots of the exact checksum produced the
visual sequence blurry, clear, approximately twenty repeated clear columns,
approximately twenty repeated clear columns, then clear. The five matching
rotated kernel logs all selected NTSC 480i and reported identical raw VI mode
inputs. Each reached seed `WT=0080`, green `WT=0060`, and live `WT=02c0`,
observed PE tokens 1 through 4, drained every FIFO to `RDoff=WToff`, and kept
the RGB565 worker running. No logged control correlates with the three visual
outcomes.

XF 0x1050=`0x3d` remains the source-verified and capture-verified encoding for
`GX_DTTIDENTITY`; do not revert it to the undefined `0x3f` value. Its failure
to stabilize the output means at least one other active-state mismatch remains.
The next isolated correction is BP 0x80: the validated libogc reference frame
emits texMode0 payload `0x000100` for explicit nearest magnification, nearest
minification, and disabled edge LOD, while Linux currently emits `0x000080`
(nearest magnification but linear minification, with the diagonal-LOD bit
clear). Change only that register before revisiting broader matrix-index or
TEV defaults.

### Match libogc's complete nearest texture mode

The active test keeps the capture-verified XF 0x1050=`0x3d`, corrected RGB565
odd TMEM bank, complete texture-cache invalidation, tiling, TEV, geometry,
display copy, XFB ownership, VI mode, and worker scheduling unchanged. It
changes only BP 0x80 texMode0 from payload `0x000080` to `0x000100`.

This is not another generic nearest-filter guess. The validated libogc
RGB565-texture capture emits `0x000100` after an explicit
`GX_InitTexObjLOD(..., GX_NEAR, GX_NEAR, ..., GX_DISABLE)` call. Dolphin's BP
layout and libogc's register construction agree that the previous `0x000080`
selected nearest magnification but retained linear minification and left the
diagonal-LOD bit clear. The new value exactly matches the positive-control
stream's nearest magnification, nearest minification, no-mipmap, edge-LOD-off
state.

Built image SHA-256:

```text
acac0e5f733620b331666d651241624487d3b96e7627052aaa19cdf0d274f300
```

Test five consecutive boots. Record each visual state in order, distinguishing
clear, blurry, green, and repeated-column output. The kernel logs must retain
NTSC 480i, seed `WT=0080`, green `WT=0060`, live `WT=02c0`, PE tokens 1
through 4, complete FIFO drains, alternating XFB pages, and continued worker
execution. Five clear, responsive consoles pass the correction. Any malformed
boot means texMode0 was another real stream mismatch but is not sufficient to
explain the remaining nondeterminism.

Hardware result: **strong improvement, but not a stability pass.** Five
consecutive boots of the exact checksum produced green, clear, clear, clear,
then clear. Only the final two boots persisted new rotated kernel logs; both
correspond to clear output and retain identical NTSC 480i inputs, expected
seed/green/live FIFO lengths, PE tokens 1 through 4, complete FIFO drains, and
continued worker execution. The initial green boot is visual evidence only, so
there is no failed log to correlate with a command or timing control.

Keep BP 0x80=`0x000100`: it exactly matches the independently replayed libogc
texture stream and improved this five-boot visual distribution from the prior
test's two malformed, one blurry, and two clear boots to one green and four
clear boots. It is nevertheless insufficient by itself because the first boot
never advanced visually from the known green copy-clear result.

The next source audit found an active prerequisite omitted by both the Linux
setup and the earlier frame-only comparison. Libogc `GX_Init()` calls
`GX_LoadTexMtxImm(identity, GX_DTTIDENTITY, GX_MTX3x4)`, which writes the 3x4
identity matrix to XF `0x05f4..0x05ff`. The validated frame selects that matrix
with XF 0x1050=`0x3d`, but does not reload it each frame because libogc already
initialized it. Linux now selects the same index without ever defining those
12 XF words. Initialize only that matrix next; do not simultaneously force XF
0x1012 (`DUAL_TEX`) so the test remains isolated.

### Initialize the GX_DTTIDENTITY post-transform matrix

The active test keeps BP 0x80=`0x000100`, XF 0x1050=`0x3d`, the corrected
RGB565 odd TMEM bank, texture invalidation, tiling, TEV, geometry, display
copy, XFB ownership, VI mode, and worker scheduling unchanged. It adds only
libogc's missing 3x4 `GX_DTTIDENTITY` matrix load:

```text
XF 0x05f4..0x05ff =
    1, 0, 0, 0,
    0, 1, 0, 0,
    0, 0, 1, 0
```

The address is source-derived from `GX_DTTIDENTITY=125`,
`GX_DTTMTX0=64`, and libogc's
`0x0500 + ((texidx - GX_DTTMTX0) << 2)` calculation. Libogc performs this
load during `GX_Init()`, so the validated per-frame FIFO capture selects index
`0x3d` but does not contain the initialization command itself. Linux previously
selected the same index while leaving all 12 matrix words inherited. The new
path reloads them every live frame. XF 0x1012 (`DUAL_TEX`) deliberately remains
unchanged: whether inherited post-transform execution is enabled or disabled,
an identity post-transform should now leave the already-normalized coordinates
unchanged.

Built image SHA-256:

```text
d54945870ead21b8e96b820b3f6590860e6eb07f4889998e617edecbdf74f03c
```

The 53-byte XF command changes the padded live FIFO length from `WT=02c0` to
`WT=02e0`; seed remains `WT=0080` and green remains `WT=0060`. Test five
consecutive boots and record each visual outcome in order. Every persisted log
must observe PE tokens 1 through 4, complete FIFO drains, alternating XFB
pages, and continued worker execution. Five clear, responsive consoles pass
the missing-matrix correction. Any green, blurry, or repeated-column boot means
another inherited state remains relevant.

Hardware result: **the missing matrix initialization is correct but not a
stability fix.** Five consecutive boots again produced the exact visual
sequence green, clear, clear, clear, clear. This is identical to the preceding
texMode0 image's sequence. Only the final boot persisted a fresh kernel log;
it corresponds to clear output and validates the new live `WT=02e0`, PE tokens
1 through 4, complete `RDoff=WToff` drains, and continued worker execution.
The green boot remains visual evidence only.

Keep the explicit XF `0x05f4..0x05ff` identity load because Linux must not
select an undefined inherited post-transform matrix, but do not attribute the
four clear boots to it. Two successive images now reproduce green on the first
boot followed by four clear boots. Stop changing GX state until the operational
variable is isolated: test this exact checksum across fully cold AC-disconnected
power-ons separately from software/reset-button warm reboots. If cold boots are
green and warm boots are clear, the remaining defect is inherited startup or
takeover state rather than random per-frame command behavior.

Cold-power result: **there is no simple cold-green/warm-clear split.** Three
boots with AC disconnected for 30 seconds between runs produced green,
extremely blurry, then blurry output. The third run was followed by an
unexpected kernel restart that the console appeared to attribute to the SD
filesystem; the restarted output was also blurry. Thus a warm restart does not
reliably produce clear output.

Only one new log persisted from this sequence. It corresponds to blurry output
and retains the expected live `WT=02e0`, PE tokens 1 through 4, complete FIFO
drains, alternating XFB pages, and worker progress through run 300. A host-side
read-only `e2fsck -fn /dev/sdc2` subsequently passed all five phases without
finding an error. The kernel's warning is that it mounts the ext3 volume as an
unchecked ext2 filesystem; it is not evidence of current on-disk corruption.
The EHCI `spinlock bad magic`/lockup and DMA-debug warnings also recur in older
logs and boot continues past them, so keep that independent USB-host defect out
of the GX state experiment.

Resume the remaining validated-stream comparison. The next isolated active
state is XF 0x1012 (`DUAL_TEX`): libogc initializes it to 1 and the validated
capture's initial XF snapshot contains 1, while the active Linux path never
writes it. Now that XF `0x05f4..0x05ff` is explicitly identity, force only
`DUAL_TEX=1` so the selected post-transform path cannot vary with inherited
state.

### Enable the initialized post-transform path

The active test adds exactly one command to the live RGB565 setup:

```text
XF 0x1012 = 0x00000001  (DUAL_TEX enabled)
```

Libogc writes this value during `GX_Init()`, Dolphin models bit 0 as the enable
for the post-transform selected by XF 0x1050, and the validated capture's
initial XF snapshot contains 1. Linux previously left it inherited. The prior
test now guarantees that selected index `0x3d` points to an explicitly loaded
3x4 identity matrix, so enabling this path cannot intentionally alter S/T/Q;
it only removes inherited enable-state variation. BP 0x80, both texture
matrices, matrix indices, TMEM regions, invalidation, TEV, geometry, copy, XFB,
VI, and worker behavior remain unchanged.

Built image SHA-256:

```text
355a3f69dd9eec15c4fc69d59869ea2afac9a8faee109bc294704b5e3985bf67
```

The 9-byte command changes the padded live FIFO length from `WT=02e0` to
`WT=0300`; seed remains `WT=0080` and green remains `WT=0060`. Test five
ordinary boots and record the exact visual sequence. Every persisted log must
observe PE tokens 1 through 4, complete drains, alternating XFB pages, and
continued worker execution. Five clear consoles pass. Any green, blur, or
repeated columns reject inherited `DUAL_TEX` as the last stability defect.

Hardware result: **failed as the final stability fix.** Five ordinary boots of
the exact checksum produced green, blurry, clear, clear, then clear. No fresh
`WT=0300` kernel log persisted from the sequence, so this result is visual-only
and cannot be correlated with a completion or timing control.

Keep XF 0x1012=`1` because it matches libogc initialization and the validated
capture, and because its selected post-transform matrix is now explicitly
identity. Do not claim the value improved stability: the distribution remains
within the green/blurry/clear variation already observed without it.

Stop expanding the register-difference search for the next test. Replace only
the live console texture data with the same deterministic tiled RGB565
four-quadrant/grid pattern used by `tools/gx-texture-reference`, while retaining
the complete current hardware state and copy path. A fixed pattern is a better
positive control than a framebuffer being modified concurrently by fbcon. It
will show whether blur and malformed output remain when texture bytes are
constant, and its sharp color boundaries expose coordinate, tiling, cache, and
filter errors directly.

### Render a deterministic tiled RGB565 reference texture

The active test retains the complete `WT=0300` GX command stream from the
preceding image, including BP 0x80=`0x000100`, explicit base and post-transform
matrices, XF 0x1012=`1`, corrected TMEM regions, cache invalidation, TEV,
geometry, copy, XFB, VI, and worker behavior. It changes only the source bytes
written into `gx_tex_buf`: instead of reading the concurrently updated fbcon
framebuffer, the worker regenerates the validated reference program's exact
4x4-tiled RGB565 pattern before every frame.

The expected full-screen image is top-left red, top-right green, bottom-left
blue, and bottom-right white, with black grid lines every 32 pixels and yellow
diagonals. The pattern-generation order and RGB565 constants match
`tools/gx-texture-reference/source/main.c`. It exercises the same texture DMA,
cache invalidation, coordinate generation, TEV, EFB draw, and XFB copy as the
console path while removing framebuffer races and changing source pixels.

Built image SHA-256:

```text
fb24597f05b165001df40e7e0c37d68241a78c6e35700e325b52d9034a3bd66d
```

Test five ordinary boots and report each image precisely. The FIFO controls
remain seed `WT=0080`, green `WT=0060`, and live `WT=0300`. A sharp, correctly
oriented reference pattern on all five boots proves the remaining console
variation originates before texture upload or in presentation of changing text.
Green means no visible primitive write. Blur with sharp color boundaries but
soft grid lines points to filtering or VI presentation. Repeated or displaced
quadrants/grid lines identify coordinate, tiling, or texture-cache behavior.

Hardware result: **the deterministic texture control passed the GX pipeline
and materially narrowed the defect.** Five ordinary boots rendered the expected
reference pattern with the visual sequence blurry, clear, clear, clear, clear.
There were no solid-green results and no repeated or displaced columns. No
fresh `WT=0300` log persisted, so the result is visual-only, but all five boots
visibly prove texture sampling, coordinate generation, TEV, EFB drawing, and
XFB copying occurred.

The first-boot blur can still be VI/display lock, but the disappearance of green
and repeated-column failures means changing from live fbcon bytes to a complete
deterministic texture affected the unstable path. This test changed three
things together: source bytes became constant, every texture word was generated
by the reference fill, and that fill performs more CPU work before submission
than the simple console tiler. Do not attribute the improvement to only one yet.

The next control keeps the reference fill as a texture-buffer warm-up and timing
load, then immediately overwrites every tile with the live fbcon through the
existing `gx_tile_rgb565()` before the unchanged cache flush and `WT=0300`
submission. Stable console output implicates first-touch/cache state or timing.
Return of green/columns implicates live source data or the second overwrite.

### Prefill the texture buffer before restoring live console pixels

This test keeps the deterministic reference generator from the successful
pattern control, but uses it only as a full-buffer warm-up. Each frame first
writes the complete four-quadrant reference pattern into `gx_tex_buf`, then
immediately overwrites every 4x4 tile with the live fbcon RGB565 data through
the original `gx_tile_rgb565()` path. The cache flush and the complete GX
command stream remain unchanged, including expected live FIFO `WT=0300`.

This preserves the reference build's full texture-memory first touch, cache
population, and additional CPU work before submission while restoring the
actual console bytes presented to GX. It changes no GX register, FIFO command,
texture address, or synchronization behavior.

Built image SHA-256:

```text
8b6bec7859aee84142d398b3cc8da7e00a26d2c0d67584c42ce896f07caf6479
```

Test five ordinary boots and report the ordered visual result of each boot.
Stable live console output without green or repeated columns implicates texture
buffer first-touch/cache state or the extra pre-submit CPU delay. Return of
green, blur, or repeated columns implicates the live source data, concurrent
fbcon updates, or the second full-buffer overwrite. Any persisted log should
show the unchanged live FIFO endpoint `WT=0300`; visual results remain valid
but must be explicitly labeled visual-only when no fresh log persists.

Hardware result: **green, blurry, blurry, clear, clear.** The available
`dmesg.txt` was stale and reported `WT=02a0`, not this image's expected
`WT=0300`, so this five-boot sequence is visual-only. Restoring live fbcon data
also restored the failure distribution despite retaining the successful
reference build's full-buffer first touch and extra CPU work. This rejects the
simple texture-allocation warm-up or pre-submit-delay explanation.

Together with the deterministic pattern result, this narrows the unstable input
to the live source path: bytes being modified concurrently by fbcon, source
cache visibility while the worker reads `vfb_mem`, or the second overwrite of
the tiled texture. Do not change GX state for the next control. Snapshot the
linear live framebuffer into private kernel memory first, then tile only from
that immutable snapshot. This separates concurrent source mutation during the
relatively slow tiled conversion from the already validated texture/GX path.

### Tile live pixels from an immutable linear snapshot

The active test preserves the complete preceding texture and GX path, including
the reference prefill and expected live FIFO `WT=0300`. It allocates a CPU-only
linear RGB565 snapshot with `vzalloc()` during GX initialization. For each live
frame, the worker copies `vfb_mem` into that private buffer once and performs
the 4x4 texture tiling exclusively from the snapshot. The GX texture address,
cache flush, register state, draw, EFB copy, XFB selection, and VI behavior are
unchanged.

Built image SHA-256:

```text
7b6ab583fb205b0633d098d29b286fed24e7aa71d3af3dbcba042942dc09f7f5
```

Test five ordinary boots and report the ordered visual result. Five stable,
clear consoles would implicate fbcon mutating the linear source while the tiled
converter traverses it. Green, blur, or repeated columns would reject that
specific race and shift attention to source-cache visibility, the live pixel
values themselves, or presentation timing. A fresh log must show `WT=0300`;
otherwise classify the sequence as visual-only.

Hardware result: **green, clear, blurry, blurry, clear.** The only available
`dmesg.txt` remained the stale July 20 log with `WT=02a0`, so the sequence is
visual-only. An immutable linear snapshot did not improve the distribution.
Concurrent fbcon mutation while `gx_tile_rgb565()` traverses the source is
therefore ruled out as the cause of green and blurry boots.

The contrast with the stable deterministic control now points at changing
texture contents or the live values, but the static reference pattern did not
positively validate cache invalidation: stale texture-cache lines would still
produce the expected image when every frame contains identical bytes. The next
control must change deterministic texture bytes on a slow, visible schedule
while retaining the known-good reference pattern and unchanged GX commands.

### Alternate deterministic texture contents every four seconds

The active test removes the completed live-snapshot control and returns to the
known-good deterministic RGB565 reference texture. It regenerates the normal
four-quadrant/grid pattern every frame, then bitwise-inverts every RGB565 word
during alternating 120-worker-run intervals. At the observed roughly 30 worker
runs per second, the display should switch about every four seconds between the
normal red/green/blue/white pattern and an unmistakable cyan/magenta/yellow/
black inverse with white grid lines and blue diagonals.

The cache flush, exact libogc `GX_InvalidateTexAll()` command sequence, texture
address, GX register state, FIFO `WT=0300`, draw, EFB copy, XFB, and VI paths
remain unchanged. Unlike the static reference control, this is a positive
control for GX observing changed texture bytes at the same physical address.

Built image SHA-256:

```text
4c9239eb0deb7f3a2ddb8b9d5c6a4e4b8e3cf4cd72246dd8a5116ab6e6b53289
```

Run five ordinary boots, leaving each boot running for at least 12 seconds.
Report whether each boot cleanly alternates normal/inverted at approximately
four-second intervals, stays on one pattern, becomes green, or shows partial,
blurry, or repeated updates. Five clean alternations validate texture cache
invalidation for changing deterministic bytes and narrow the live failure to
the actual fbcon values or another source-side distinction. Static or partial
transitions implicate cache invalidation or CPU-to-GX visibility directly.

Hardware result: **green; top half only alternating clearly; top half only
alternating clearly; full frame alternating clearly; full frame alternating
clearly.** The available `dmesg.txt` was still the stale July 20 `WT=02a0`
file, so this sequence is visual-only.

This is a partial positive control. Two full-frame boots prove the existing
flush plus `GX_InvalidateTexAll()` path can make changed deterministic bytes
visible at the same physical texture address. The green and two half-frame
boots prove it is not reliable across boots or across the complete texture.
The defect therefore does not require fbcon or live pixel values; the changing
deterministic texture reproduces it directly.

The next control should stop depending on same-address invalidation. Reserve a
second complete MEM1 texture buffer and bind alternating physical addresses for
the normal and inverted four-second phases. Keep texture contents, timing,
commands, and all downstream state unchanged. If double-buffering makes every
transition complete, stale or partially invalidated same-address texture cache
state is the cause. If green or half-frame output remains, investigate texture
DMA/address range or downstream presentation rather than cache identity.

---

## Known pitfalls

- **Do not interpret a frozen display as a CPU crash** — screen can be static while CPU
  boots normally.  Check keyboard/shell response before assuming a hard hang.
- Do not remove `flush_dcache_range()` for the FIFO or texture buffer.
- Do not revert CP accessors to `ioread16/iowrite16` — must be `in_be16`/`out_be16`.
- Do not leave `CP_CR_LINKEN` enabled between frames.
- `GFP_DMA` does not guarantee MEM1 on this platform; use `/memreserve/` + `__va(phys)`.
- BP `0x4D` (dispCopyDst) is in **32-byte** units — `(width * 2) >> 5`, not `>> 4`.
- PE interrupt status is word index 5, byte offset `0x0a` from PE base. The old
  word-index-0/byte-offset-`0x00` definition was incorrect.
- Do not block waiting for a PE IRQ handler from the VI IRQ handler. Polling the
  latched PE-finish status failed positive controls both with finish signalling
  disabled (`PE=0000`) and enabled (`PE=0002`); do not treat it as a fence.
- **BP 0x65 is `TX_LOADTLUT1` (texture LUT load config), not `PE_DONE`** -- this was
  wrongly documented as the draw-done fence throughout this project until 2026-07-09.
  The real `PE_DONE` register is **BP 0x45** (`0x00000002`, matching libogc's
  `GX_DrawDone()`/`GX_SetDrawDone()` exactly).  See the dedicated write-up above for the
  full discovery and its possible connection to the primitive-EFB-write mystery.
- CP FIFO and texture buffer addresses: hardware drops bit 23 of physical addresses.
- `COPY_CTRL_CLEAR` (BP 0x52 bit 11) clears EFB **after** the copy, not before.
- XF 0x1040 bit[0]=1 (projection=STQ) requires 3 valid matrix rows; with only 2 rows
  loaded the Q component is garbage → perspective divide stall.  Use 0x200 (bit0=0).
- Stage-0 TEV order is BP `0x28`, not BP `0x25`. BP `0x25` is indirect
  texture-coordinate scale. The old BP `0x25` stage-order finding is retracted.
- raschan in BP 0x28 bits[9:7] must be 7 (GX_COLOR_NULL) when numcolchans=0.
- `pos=320/352` may not change when adding small BP commands due to 32-byte alignment padding.
- suSsize (BP 0x30) and suTsize (BP 0x31) must be set even when texenable=0 — the SU
  uses them for LOD gradient computation regardless of whether the TMU fetches the texture.
