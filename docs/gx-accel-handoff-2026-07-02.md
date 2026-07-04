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
