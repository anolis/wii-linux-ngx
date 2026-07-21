# GX FIFO reference capture

This program renders a full-screen red quad through libogc using the same
direct XY/F32 and RGBA8 vertex format as `gcn-gx.c`. Its purpose is to produce
a known-good Dolphin FIFO capture for command-by-command comparison with the
Linux driver.

Build it with devkitPro's official container:

```sh
docker run --rm \
  --user "$(id -u):$(id -g)" \
  -e HOME=/tmp \
  -v "$PWD:/work" \
  -w /work/tools/gx-fifo-reference \
  devkitpro/devkitppc:latest make
```

The output is `gx-fifo-reference.dol`. Run it in Dolphin and confirm the frame
is solid red before trusting any FIFO capture made from it. The capture must
include one complete draw and EFB-to-XFB copy frame.

`reference-red-quad.dff` is the validated capture made from this program. It
contains one 204-byte frame and replays as a solid red EFB in Dolphin's FIFO
Player. Its SHA-256 is:

```text
1f56e7adce3cfd9831e027b5ce5c659698b2daf6dc5cbe5ef7286cad74a057f2
```

Decode both the initial GX state snapshot and the recorded frame with:

```sh
./decode_dff.py reference-red-quad.dff
```

The checked-in `reference-red-quad.txt` is the expected decoder output. The
initial BP/CP/XF arrays matter: the 204 frame bytes contain only state changed
after recording began, not the complete setup performed by `GX_Init()` and
`configure_gx()`.

`reference-red-quad-full-state.dff` comes from the variant which calls
`configure_gx()` inside every frame. Its one frame contains 564 ordered FIFO
bytes and zero memory updates, including the complete public libogc draw setup,
quad, draw-done fence, and display copy. Direct FIFO Player replay was visually
confirmed red. Its SHA-256 is:

```text
8b42cc84e28b8ab09e53f981f4c1b197ec57fa0ee0029704f78d1a819f1aa302
```

`reference-red-quad-full-state.txt` is its golden decoder output. This is the
preferred reference for ordered command-stream comparison and hardware replay.

Source references:

- <https://github.com/devkitPro/wii-examples/tree/master/graphics/gx/triangle>
- <https://github.com/devkitPro/libogc/blob/master/libogc/gx.c>
- <https://github.com/dolphin-emu/dolphin/tree/master/Source/Core/VideoCommon>
