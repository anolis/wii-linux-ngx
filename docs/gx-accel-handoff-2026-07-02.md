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

### Session 2026-07-03 changes (not yet committed)

All changes listed here are live in the deployed build but not yet in a git commit.

- **Fixed PE register offset**: `PE_REG_CTRL_STAT = 0` (byte offset 0x00), not 1.
  Previous code wrote to pe_regs[1] = PE_TOKEN_VALUE, doing nothing useful.
- **Fixed PE clear write**: `out_be16(pe_regs + PE_REG_CTRL_STAT, 0x0003)` — clears
  status bits 2-3 (PEToken/PEFinish) while preserving enable bits 0-1.
- **Reduced sync delay**: 8ms → 2ms. Boot log proves copy completes in <1ms.
- **Added BP 0x66 ×2 (BPMEM_TX_INVALIDATE)** in `gx_setup_texture_rgb565`:
  Forces the TMU to re-fetch from main memory each frame instead of serving the
  first frame's cached data.  Sent twice for reliability (same as libogc).
- **Added multi-frame diagnostic logging** (4 frames, controlled by `frame_log` counter):
  Logs CP SR, RD, WT before/after GP enable, plus XFB readback for frames 0-3 and 360.
- **Added solid color cycling test** (frames 360-900) in `gcn_gx_blit_fb_rgb565`:
  Fills `gx_tex_buf` with solid red/green/blue to verify color output independently
  of `vfb_mem` content.  Active while `frame_count` is in the 360-900 range.
- **Fixed BP 0x41 (BLENDMODE) missing**: Added `gx_load_bp_reg(0x41000018)` to
  `gx_setup_2d_state` — see §11.
- **Fixed BP 0xF3 (ALPHA_COMPARE) missing**: Added `gx_load_bp_reg(0xF33F0000)` to
  `gx_setup_2d_state` — see §13 (★ most recent fix, not yet boot-tested).

**Current deployed build: `b1c85bf6`**

## Onboard diagnostic logging

**The kernel is booting with `init=/init-diag.sh` in the bootargs.**  This script:

1. Mounts `/proc` and `/sys`.
2. Remounts rootfs read-write.
3. Sleeps 20 seconds (captures the full color-cycling test window).
4. Writes `dmesg` output to `/dmesg.txt` on the rootfs partition.
5. Appends `/proc/bus/input/devices` and `/dev/input/` listing for USB input debugging.
6. Calls `sync`, then `exec /bin/sh -l` on `/dev/console` (interactive shell available after ~20s).

Script location on the rootfs (WII-LINUX-NGX1 partition):

```text
/media/anolis/WII-LINUX-NGX1/init-diag.sh
```

Script contents:

```sh
#!/bin/sh
mount -t proc none /proc
mount -t sysfs none /sys
mount -o remount,rw /
sleep 20
dmesg > /dmesg.txt
echo "--- /proc/bus/input/devices ---" >> /dmesg.txt
cat /proc/bus/input/devices >> /dmesg.txt 2>&1
echo "--- /dev/input ---" >> /dmesg.txt
ls -la /dev/input/ >> /dmesg.txt 2>&1
sync
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

**Expected result for build `b1c85bf6`**: At frame 360 (`t ≈ 15s`), `xfb0` should be a non-black YUYV value (approximately `0x515A51F0` for solid red under BT.601).  The display should visibly cycle red → green → blue over ~18 seconds from boot.

## CP status register (SR) field meanings

| SR value | Meaning |
|----------|---------|
| `0x0008` | GP stopped / idle (CP_CTRL = 0) |
| `0x000c` | GP enabled + FIFO empty (idle, all commands consumed) |
| `0x0004` | GP enabled + FIFO not empty (still processing) |
| `0x0000` | Transitional / uncertain |

## Current pipeline state (build `b1c85bf6`)

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

`gcnfb.c` still runs `vi_transcode_RGB565` unconditionally as a safety net.  The GX blit runs after it each frame and overwrites the XFB if GX output is correct.

## Suggested next steps

1. **Boot `b1c85bf6` and check dmesg.txt** (auto-written at t=20s by init-diag.sh):
   - `f360 xfb0` should be non-black (e.g. `~0x515A51F0` for red).
   - User should observe solid red → green → blue on the display between t≈15s and t≈50s.

2. **If color cycling confirms end-to-end pipeline works**:
   - Remove the color cycling test block in `gcn_gx_blit_fb_rgb565`.
   - Remove the 4-frame CP SR logging in `gx_submit_cmds`.
   - Remove the XFB readback `pr_info` calls.
   - Verify actual terminal content (fbcon text) renders correctly via GX.

3. **Remove the SW transcode safety net** once GX output is visually confirmed correct.
   In `gcnfb.c` RGB565 path, remove the `vi_transcode_RGB565` call.

4. **Commit the session's changes** — currently all sitting as uncommitted edits on `feature/gcn-gx-accel`.

5. **Register a PE FINISH ISR** (future improvement): replace `udelay(2000)` with an interrupt-driven wait on the PE FINISH signal, freeing ~2ms of CPU time per frame.

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
