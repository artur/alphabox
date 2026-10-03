# The ATI Radeon 7500 (RV200)

The `radeon` class emulates the Radeon 7500 (RV200, 1002:5157), the AGP
card HP shipped in the ES47, ES80 and GS1280. Configuration, consoles and
guests are in [peripherals.md](peripherals.md) and
[openvms.md](openvms.md#graphics); this page is the engines: what is
emulated, where each behaviour comes from, how it is checked and what is
not.

Sources live in `src/devices/video/radeon/`. The documents and driver
sources they were written from are collected, with their URLs, in
`lab/docs-radeon/INDEX.md`. ATI never published an R100 register
reference or 3D documentation. What exists:

- the **Rage 128 Pro Register Reference Guide** (RRG-G04500-C, 2000): the
  R100's 2D engine, datapath, colour compare and scissors are the Rage
  128's;
- AMD's **Radeon R5xx Acceleration** guide (v1.5, 2010), chapter 6: the
  PM4 packet formats (types 0-3 and every 2D type-3 packet) that the R100
  microcode already used;
- the open drivers: Linux's radeon DRM (`r100.c`: the command-stream
  checker), Mesa's classic r100 driver (the 3D and TCL state as a driver
  programs it), X.org's xf86-video-ati (2D acceleration, Render through
  the 3D engine, the cursor).

Where the model follows a driver rather than a document, or infers, the
source says so in its comments.

## What is emulated

**2D engine** (`RadeonEngine.cpp`): every ROP3; solid, 8x8 mono (with
`BRUSH_Y_X`), 8x1/1x8 mono, 32x1 and 32x32 mono and 8x8 colour brushes;
memory, mono-memory and host-data sources (colour, mono MSB/LSB first,
FG/BG and FG/leave-alone, byte-aligned rows, big-endian host data);
blits in all four directions; Bresenham lines with `DST_LAST_PEL` and dash
patterns (`DST_LINE_PATCOUNT`); the scissor rectangle; the write mask;
colour compare (source, destination or both; never, equal, not-equal and
the flip function); 8, 16 and 32 bpp destinations; the side effects of a
`DP_GUI_MASTER_CNTL` write (default pitch/offset and scissors, the
directions in `DP_CNTL`, `GMC_WR_MSK_DIS` and `GMC_CLR_CMP_CNTL_DIS`
loading `DP_WRITE_MSK`/`CLR_CMP_MSK` and clearing `CLR_CMP_CNTL`).

**Command processor** (`RadeonCP.cpp`): the ring buffer (with the read
pointer written back), indirect buffers, the PIO queue, scratch
registers written back to memory; type-0 (consecutive and one-register),
type-1 and type-2 packets; the type-3 2D packets PAINT, PAINT_MULTI,
BITBLT, BITBLT_MULTI, TRANS_BITBLT, HOSTDATA_BLT, POLYLINE,
POLYSCANLINES, NEXTCHAR, PLY_NEXTSCAN, SET_SCISSORS (with the settings
block, every brush packet, `BRUSH_Y_X`); the 3D packets below.

**Display** (`RadeonDisplay.cpp`): extended modes, the 8-bit palette,
the hardware cursor (mono AND/XOR, and 64x64 ARGB blended as
premultiplied alpha).

**3D engine** (`Radeon3D.cpp`, `RadeonTcl.cpp`, `RadeonRaster.cpp`):

- vertex input: `3D_DRAW_IMMD`, `3D_DRAW_VBUF`, `3D_DRAW_INDX` and their
  `_2` forms, `3D_RNDR_GEN_INDX_PRIM`, `3D_LOAD_VBPNTR` (up to 16
  arrays), the `SE_VF_CNTL`/`SE_PORT_DATA` register path; 16- and 32-bit
  indices; every `SE_VTX_FMT` field the drivers use (position, W,
  normal, float or packed colours, specular and fog, four texture sets
  with Q);
- primitives: points, lines (strips, loops; one pixel or
  `SE_LINE_WIDTH` wide), triangle lists, fans, strips, rectangle lists,
  quads, quad strips, polygons;
- TCL (when `VF_CNTL<9>` asks and `SE_CNTL_STATUS` does not bypass it):
  the model-view-projection, model-view and inverse-transpose matrices
  from the vector memory (`SE_TCL_VECTOR_INDX_REG`/`DATA_REG`, scalars
  likewise), clipping to the view volume and the six user clip planes,
  the viewport, OpenGL fixed-function lighting (eight lights: infinite,
  local, spot; range attenuation; material from the registers or the
  vertex colours; separate or combined specular; local viewer), fog
  factors (exp, exp2, linear, from eye depth or range), texture
  coordinates from the inputs or through the texture matrices with
  texture generation (object, eye, normal, reflection);
- setup and rasteriser: sub-pixel snapping, OpenGL or Direct3D pixel
  centres, face culling and point/line fill modes, the top-left rule,
  solid/flat/Gouraud shading per attribute, perspective-correct
  texturing, the clip rectangle, the polygon stipple and the line
  pattern;
- textures (three units): I8, AI88, RGB332, ARGB1555, RGB565, ARGB4444,
  ARGB8888, RGBA8888, Y8, both YUV 4:2:2 orders (with YUV-to-RGB),
  DXT1, DXT2/3, DXT4/5; power-of-two mip chains and non-power-of-two
  images (`PP_TEX_SIZE`/`PP_TEX_PITCH`); nearest, linear, mip-nearest
  and mip-linear filters, LOD bias, the maximum level; the eight wrap
  modes and the border colour;
- the combiners (`PP_TXCBLEND`/`PP_TXABLEND`, three stages): add,
  subtract, add-signed, blend, dot3, complemented arguments, the texture
  factor, x2/x4 scale and clamping; the specular colour sum; fog;
- the alpha test, stencil (eight functions, six operations, masks), Z
  (16-, 24- with stencil and 32-bit integer), blending (the GL factors,
  add and subtract), the logic op, the plane mask; colour buffers in
  ARGB8888, RGB565, ARGB1555, ARGB4444, RGB332, Y8 and RGB8.

## How it is checked

`ALPHABOX_RADEON_SELFTEST=1` (or `=exit` to end the emulator with the
result) runs at the end of the card's `init()`, before the machine
starts, and puts the card back as it found it. It drives the engines as a
driver does -- register writes through the MMIO BAR, packets through the
CP ring in VRAM -- and compares each result with a reference written
independently in `RadeonSelfTest.cpp` (its own ROP3 truth table, line
properties, bit expansion, a double-precision barycentric rasteriser,
texture decoders, combiner equations, transform and lighting):

```
cd lab/radeon-selftest      # an ES40 config with pci0.2 = radeon and gui = sdl
SDL_VIDEO_DRIVER=dummy ALPHABOX_RADEON_SELFTEST=exit alphabox run
```

It prints `%RADEON-I-SELFTEST: <check> ok|FAILED` per check and `PASS`
or `FAIL`. 42 checks: 24 on the 2D engine, the CP and the cursor, 18 3D
scenes. The scenes are written as PNGs to `ALPHABOX_RADEON_SELFTEST_DIR`
(default `$ALPHABOX_WORK/radeon-3d`), each with a `-cmp.png` (frame,
reference, differing pixels in white):

| Frame | Proves |
|---|---|
| 01-gouraud | Gouraud triangle list, fan and strip; top-left coverage |
| 02-flat-solid-cull | flat shading (provoking vertex), `RE_SOLID_COLOR`, back-face culling |
| 03-texture-filter-wrap | nearest and bilinear; wrap, mirror, clamp-to-edge, clamp-to-border |
| 04-texture-perspective | perspective-correct texture coordinates (W0 = 1/w) |
| 05-texture-mipmap | mip level selection, nearest and linear between levels |
| 06-texture-formats | the 14 texture formats, DXT included |
| 07-combiners | modulate, add, add-signed, subtract, blend, dot3, x2 with the texture factor, two stages |
| 08-blend-alphatest-fog | five blend modes, the alpha test, vertex fog, the specular sum |
| 09-zbuffer, 10-stencil | 16- and 24-bit Z with LESS and writes; stencil REPLACE then EQUAL, plane mask 0 |
| 11-primitives | quads, quad strips, polygons, lines, points, wide lines; the IMMD, IMMD_2, VBUF (two arrays), INDX, RNDR_GEN_INDX_PRIM and register-port walks |
| 12-stipple-pattern | the polygon stipple with offsets, the line pattern |
| 13-tcl-lit-cube | TCL: matrices, two lights (directional with specular, local with attenuation), linear fog, Z, culling |
| 14-tcl-texmatrix-ucp | TCL: the texture matrix, a user clip plane |
| 15-tcl-near-clip | TCL: clipping at the near plane (a vertex behind the eye) |
| 16-render-composite | X.org's Render composite as `R100PrepareComposite` programs it: OVER, an A8 mask, non-power-of-two textures |
| 17-misc-state | LOD bias, mirror-once and GL clamp, the logic op and plane mask, float colours, 32-bit indices, an indexed `RNDR_GEN_INDX_PRIM` |

Colour formats (565, 1555, 4444, 332, RGB8) are checked pixel by pixel
without a frame.

The references are written from the same sources as the model, so a
scene that passes shows that the engine does what the drivers' reading of
the hardware says, consistently, through the guest-visible interfaces --
not that the silicon does it. No Alpha guest drives the 3D engine today
(Windows 2000 has no Radeon driver; OpenVMS's DECwindows uses only 2D;
Linux does not run on Marvel), so nothing in the 3D engine has been seen
by a guest.

Guest-verified: OpenVMS 8.4 DECwindows (the CP ring, indirect buffers,
solid and 8x8 mono pattern fills, mono host data in `HOSTDATA_BLT`, the
mono cursor), unchanged by this work: before and after it the login
screen's last frame is identical, and the server touches the same
registers and CP operations (their counts vary with timing; evidence in
`lab/radeon-3d/decw-check/`).

## Where the documents disagree, and inferences

- **Colour compare polarity.** The Rage 128 guide and the R5xx PM4
  chapter say function 4 writes the pixels *equal* to the reference.
  X.org's radeon driver gets "skip pixels equal to the transparent
  colour" from function 4 on the Radeon, while its r128 driver uses
  function 5 for the same, and says the Radeon's compare is the opposite
  of the r128's. The model follows the Radeon driver: 4 keeps equal
  pixels from being written. The destination function is taken to have
  the same polarity [inference].
- **PAINT and the scanline packets**: the guide does not say whether a
  bottom-right corner or a span's end is inclusive; exclusive is assumed
  (the Windows RECTL the packets were made for).
- **Face winding**: `FFACE_CULL_DIR` counter-clockwise means as the
  screen shows it (Mesa's GL front faces, with Y flipped by the
  viewport, and the bit swapped when Mesa renders unflipped to a
  texture).
- **The ARGB cursor** blends premultiplied colours: X.org loads ARGB
  cursor images (premultiplied in X) unchanged and skips premultiplying
  its mono-derived images only because their pixels are opaque or
  clear.
- Inferred from register names, not from a driver: the register-port
  draw, the solid shading of specular and fog, the bypass setup's
  `STn_PRE_MULT_1_OVER_W0`, the face fill modes 1/2 (points/lines), the
  polygon stipple's bit order and offsets, the YUV byte orders (from
  Mesa's YCbCr formats), the view-volume clip (exact, no guard band).

## Not emulated

Said once at run time when a guest asks for them: tiled colour buffers
and textures (macro/micro tiling), cube maps and 3D textures, the texture
chroma key, table fog, floating-point and W depth formats, HyperZ (Z
compression, hierarchical Z, `3D_CLEAR_ZMASK`/`3D_CLEAR_HIZ`), the
3-vertex point/line lists, TRI_FLAG and sprite primitives, anti-aliased
lines and polygons, `INDX_BUFFER`. Silently not modelled: dithering,
polygon offset (`SE_ZBIAS_*`), vertex blending, two-sided lighting, dual
cones, the specular threshold, the 2D source scissors, the endian swaps
of texture and colour surfaces, the CP microcode (kept, not executed),
the R5xx-only packets (PRED_EXEC, COND_EXEC, WAIT_SEMAPHORE, WAIT_MEM,
MPEG_INDEX), `LOAD_PALETTE`, `CNTL_SMALLTEXT`.
