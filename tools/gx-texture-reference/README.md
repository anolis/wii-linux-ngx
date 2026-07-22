# GX RGB565 texture reference

This libogc program is a positive-control generator for the Wii Linux GX
framebuffer path. It draws a deterministic 640x480 tiled RGB565 texture using
the same high-level pipeline shape as `gcn-gx.c`:

- position-generated texture coordinates through `GX_TEXMTX0`;
- direct XY/F32 and RGBA8 vertex attributes;
- one RGB565 texture on map 0 with nearest sampling;
- one TEV stage using `GX_REPLACE`;
- a full-screen quad followed by `GX_DrawDone()` and `GX_CopyDisp()`.

Build with devkitPro's official container:

```sh
docker run --rm \
  --user "$(id -u):$(id -g)" \
  -e HOME=/tmp \
  -v "$PWD:/work" \
  -w /work/tools/gx-texture-reference \
  devkitpro/devkitppc:latest make
```

Run `gx-texture-reference.dol` in Dolphin and verify the test pattern before
recording a FIFO log. Record a frame only after the pattern is stable. The
capture is useful only after it also replays correctly in Dolphin FIFO Player.
Decode the capture with the sibling reference tool:

```sh
../gx-fifo-reference/decode_dff.py reference-rgb565-texture.dff
```

`reference-rgb565-texture.dff` is the validated one-frame capture from this
program. Both the live DOL and FIFO Player render the same four-quadrant grid.
The capture contains 679 FIFO bytes and one texture-memory update. Its SHA-256
is:

```text
df9d2ee358b625886d0fd76ffe60f2c5e4b48f5aaed095a9c0d4062c70bea09c
```
