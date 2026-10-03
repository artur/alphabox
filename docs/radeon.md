# The ATI Radeon 7500 (RV200)

The `radeon` class emulates the Radeon 7500 (RV200, 1002:5157), the AGP
card HP shipped in the ES47, ES80 and GS1280. Configuration, consoles and
guests are in [peripherals.md](peripherals.md) and
[openvms.md](openvms.md#graphics); this page is the engines: what is
emulated, where each behaviour comes from, how it is checked and what is
not.

Sources live in `src/devices/video/radeon/` (the family's common code)
and its generation directories (`r100/`; see
[Layout](#layout-one-family-a-directory-per-generation)). The documents
and driver sources they were written from are collected, with their
URLs, in `lab/docs-radeon/INDEX.md`. ATI never published an R100 register
reference or 3D documentation. What exists:

- the **Rage 128 Pro Register Reference Guide** (RRG-G04500-C, 2000): the
  R100's 2D engine, datapath, colour compare and scissors are the Rage
  128's;
- AMD's **Radeon R5xx Acceleration** guide (v1.5, 2010), chapter 6: the
  PM4 packet formats (types 0-3 and every 2D type-3 packet) that the R100
  microcode already used;
- the open drivers: Linux's radeon DRM (`r100.c`: the command-stream
  checker, the CP and GART set-up; the legacy `radeon_cp.c` and
  `radeon_state.c`), Linux's radeonfb (the PLLs), Mesa's classic r100
  driver (the 3D and TCL state as a driver programs it; `radeon_tile.c`,
  the family's micro tile layout), X.org's xf86-video-ati (2D
  acceleration, Render through the 3D engine, the cursor);
- the card's own BIOS (`roms/video/radeon7500`, disassembled in
  `lab/docs-radeon/rom.dis`): its PLL block and its waits.

Where the model follows a driver rather than a document, or infers, the
source says so in its comments.

## Layout: one family, a directory per generation

The Radeons from the R100 to the R300 share most of the card: the PCI
identity, the BIOS and VGA plumbing, the register file, the display
(CRTC, PLLs, cursor), the memory controller and its GART, the command
FIFO and the engine thread, the CP and its PM4 packets, the 2D engine.
What each generation has of its own is the 3D engine (and its TCL). So:

```
src/devices/video/radeon/       the family's common code
  Radeon.hpp/.cpp               CRadeon: PCI header, BARs, init(), state file
  RadeonChip.hpp, RadeonChips.cpp
                                the chip rows (radeon::ChipInfo) and the
                                generations (radeon::Generation)
  RadeonMicrocode.hpp/.cpp      the known CP microcode images of every
                                generation, by CRC, and their packet sets
  RadeonEngine3D.hpp            CRadeonEngine3D (a generation's 3D engine, as
                                the common code calls it), CRadeonEngineBus
                                (the card, as an engine reaches it)
  RadeonRegs.hpp                the common registers
  RadeonControl.cpp             the register aperture
  RadeonTiming.cpp              PLLs, pixel clock, CRTC timing
  RadeonMemory.cpp              framebuffer aperture, VRAM
  RadeonDisplay.cpp             extended modes, cursor, palette
  RadeonEngine.cpp              the 2D engine
  RadeonQueue.cpp               the command FIFO, the engine thread, busy status
  RadeonCP.cpp                  the CP: ring, indirect buffers, PIO queue,
                                micro-engine RAM, packet dispatch
  RadeonGart.cpp                the MC's view of the bus: AGP window, PCI GART
  RadeonSelfTest.hpp/.cpp       the self-test's common checks and the means
                                the generations' scenes use
  RadeonSelfTestQueue.cpp       its FIFO, CP, GART and clock checks
  r100/                         the R100 generation (R100, RV100, RV200, ...)
    RadeonR100_3D.hpp/.cpp      radeon::r100::CRadeonR100_3D: ports, packets,
                                vertex fetch, assembly, clipping; gen_r100
    RadeonR100Tcl.cpp           transform and lighting
    RadeonR100Raster.cpp        rasteriser and pixel pipeline
    RadeonR100Regs.hpp          its registers and packet fields
    RadeonR100SelfTest.cpp      its self-test scenes (01-23)
```

The common code is the radeon/ root rather than a `common/` beside the
generations, as in the other families (sym53c8xx/, i8255x/): the
generations are what is added, so they are the subdirectories.

How the pieces meet:

- **The chip row** (`RadeonChips.cpp`) is everything common code may ask
  about a part: PCI device and subsystem IDs, command FIFO depth, PIO
  queue depth, ME RAM size, the clocks, whether its microcode has the
  R200 packets, the CP microcode family a driver loads on it
  (`cp_microcode`, which `RadeonMicrocode.cpp`'s known-image rows are
  matched against) -- and its **generation**. The `radeon` class's `chip` key
  names the row (`"rv200"`, the default); `model` (`agp`/`pci`) picks the
  board's subsystem ID within it.
- **The generation** (`radeon::Generation`, defined in its directory:
  `gen_r100`) holds what the generation's register map decides in common
  code -- the range of registers that go through the command FIFO
  (0x1400-0x3fff on the R100) and RBBM_STATUS's 2D and 3D busy bits --
  and the factory that builds its 3D engine.
- **The 3D engine** (`CRadeonEngine3D`) is called by the common code only:
  register writes and reads after the card's own registers have had their
  turn, type-3 packets the CP does not know, reset, its block of the state
  file, the pixels it wrote (the busy-time model), its self-test scenes. It
  reaches the card only through `CRadeonEngineBus`: the register file,
  VRAM, the bus-master translation (framebuffer, AGP window, PCI GART), the
  part's row. Nothing in the common code names a generation.
- **Threads.** An engine runs under the card's execution lock
  (`m_exec_mx`), on the engine thread that drains the FIFO and the CP, or
  inline on a CPU's thread (`ALPHABOX_RADEON_SYNC=1`, the state file, the
  self-test). Every interface call is made with the lock held, so an engine
  keeps its state without locks, starts no threads and never waits for the
  CPU.
- **The state file.** The card's block, then the engine's (its own magic
  first: the R100's is `AR3D`), then the micro-engine's. A generation's
  `restore()` returns false and leaves the file where it was when the block
  is not its own, and the card then resets the engine.

### Adding a generation (or a part)

A part of an existing generation is a row in `kChips` (`RadeonChips.cpp`)
with its facts and the generation's `Generation`; its name becomes a
`chip` value. A new generation, say the R200:

1. `src/devices/video/radeon/r200/`, files named `RadeonR200*` (headers
   are included unqualified: names must not collide with r100/'s), code in
   `namespace radeon::r200`.
2. Add the directory to `CMakeLists.txt`'s `file(GLOB ...)` list and to
   `target_include_directories`, and re-run the configure step of every
   lane.
3. The engine: a class deriving from `CRadeonEngine3D` that keeps a
   `CRadeonEngineBus`, implementing every method; its own state-file magic.
   Follow the threading rule above.
4. `const radeon::Generation gen_r200` in its directory: the FIFOed
   register range, the RBBM_STATUS busy bits, the factory; declare it
   `extern` beside `gen_r100` in `RadeonChips.cpp`.
5. The rows: device and subsystem IDs, FIFO and PIO depths, ME RAM size,
   clocks, `r200_cp_packets = true` for parts whose microcode has the R200
   packets, `cp_microcode = "R200"`, `&gen_r200`. The generation's
   microcode images go into `RadeonMicrocode.cpp`'s table (CRC, word
   width, dispatch-table packet set; [radeon-microcode.md](radeon-microcode.md)
   and `test/tools/radeon_me_dis.py --table` give them) -- the R200's and
   R300's are already there.
6. Its self-test scenes: `selftest_scenes()` in `r200/RadeonR200SelfTest.cpp`,
   using `radeon::SelfTest` and the helpers in `RadeonSelfTest.hpp`; the
   common checks run before and after them.
7. If the generation changes something the common code does (a register
   the R100 does not have, a different CP packet, a different 2D engine
   detail), the difference goes into the row or the `Generation` as a fact,
   and the common code asks for it -- never a test of the part's name.
8. Document it here and in `es40.cfg`'s `chip` list, add the row's name to
   `docs/peripherals.md`, and run the self-test with `chip = "<it>"`.

## What is emulated

**The command FIFO and the engine** (`RadeonQueue.cpp`). A CPU write to a
rendering-engine register -- 0x1400 and up, the Rage 128 guide's "GUI
registers (FIFOed)" -- goes into a 64-entry command FIFO (the part's
row, `RadeonChips.cpp`; Linux's `r100_gui_wait_for_idle` waits for 64).
One engine thread takes the entries in order and, when the FIFO is
empty, reads the CP's ring a packet at a time. RBBM_STATUS shows the
free entries (`CMDFIFO_AVAIL` <6:0>), `CP_CMDSTRM_BUSY` <16> while the
ring has unread dwords, the 2D or 3D blocks' busy bits and `GUI_ACTIVE`
<31> while anything is queued, executing or still running on the
engine's timeline: every command is charged a clock and every pixel half
a clock of the engine clock (180 MHz, from the card's BIOS PLL block; two
pixel pipes), the next command starting when the last ended, so the
engine reads busy for its modelled time however fast the host drew
[inference: the R100's per-command costs are not documented]. With the
FIFO full a write waits until an entry is free (the chip holds the bus
write; nothing is dropped). A CPU read of a queued register waits for the
writes queued before it (the chip takes a register write long before the
next bus read; OpenVMS's server writes `DEFAULT_PITCH_OFFSET` and reads
it straight back) [inference]; the status registers read at once.
`WAIT_UNTIL` stalls the FIFO: the idle conditions hold by construction
(one command at a time, the timeline orders the rest), the CRTC
conditions wait for the next vertical blank [inference: no
`CRTC_GUI_TRIG_VLINE`]. The destination caches
(`RB2D_DSTCACHE_CTLSTAT`, `RB3D_DSTCACHE_CTLSTAT`, `RB3D_ZCACHE_CTLSTAT`,
`DSTCACHE_CTLSTAT`): the engine writes memory directly, so a flush or a
purge is a point in the FIFO, `DC_BUSY` <31> reading set from the write
until the engine has reached it and the work before it is over.
`GEN_INT_STATUS` `GUI_IDLE` <19> latches when the engine goes idle and
interrupts with `GEN_INT_CNTL` <19>. `RBBM_SOFT_RESET`'s engine bits
finish the command running, reset the blocks and hold the engine while
set (the FIFO keeps its entries). The state file waits for the engine to
drain. `ALPHABOX_RADEON_SYNC=1` brings back the synchronous engine (every
command inside its register write, the engine always idle), for A/B
runs.

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
loading `DP_WRITE_MSK`/`CLR_CMP_MSK` and clearing `CLR_CMP_CNTL`); the
source scissor (`SRC_SC_RIGHT`/`BOTTOM`, `SRC_SC_BOTTOM_RIGHT`, loaded
from `DEFAULT_SC_BOTTOM_RIGHT` unless `GMC_SRC_CLIPPING`, RRG 3-147 and
3-165): a source pixel outside it is not read and its destination not
written [inference: the guide names the scissor, not what it does].

**Command processor** (`RadeonCP.cpp`), on the engine's thread: the
ring buffer -- read from `CP_RB_RPTR` towards `CP_RB_WPTR` after the
write that publishes it has returned, the host's copy written back to
`CP_RB_RPTR_ADDR` every 2^`RB_BLKSZ` quadwords and when the ring runs
dry (unless `RB_NO_UPDATE`), `CP_RB_RPTR_WR` taking effect on the next
`CP_RB_WPTR` write while `RB_RPTR_WR_ENA` (R5xx guide 5.8), `BUF_SWAP`
for buffers in host memory --, indirect buffers, the primary PIO queue
(`CP_CSQ_APER_PRIMARY`, 64 dwords [inference], its room in
`CP_CSQ_CNTL` <7:0>), scratch registers written back to memory (with
`SCRATCH_SWAP`); `CP_STAT`, `CP_CSQ_STAT` from the queues. Which streams
run is the CSQ mode's (`CP_CSQ_CNTL` <31:28>, or `CP_CSQ_MODE`'s
enables): the ring when the primary stream bus masters, the PIO queue
when it is pushed, indirect buffers when the indirect stream bus
masters. The micro-engine: ATI never documented its instruction set, so
the packets are interpreted natively, but there is no CP without
microcode -- until every one of the 256 ME RAM entries has been loaded
(`CP_ME_RAM_ADDR`, then `DATAH` (8 bits) / `DATAL` pairs, R5xx guide
5.12) with something other than zeros, neither the ring nor the PIO
queue is read and the read pointer stays put. Any complete image is
taken; its CRC-32 is looked up in the table of every CP microcode image
the open sources published (`RadeonMicrocode.cpp`,
[radeon-microcode.md](radeon-microcode.md)) and its name printed, or
"unknown microcode". OpenVMS 8.4's DECwindows server loads `b86caa2a`,
ATI's R100 image -- the one Linux loads from `radeon/R100_cp.bin`, which
never changed from its first publication in 2001. A known image's
dispatch table says which type-3 packets it handles: one it sends to
NOP's handler is skipped. An image for another generation is accepted
with a warning (`%RADEON-W-UCODE`). The image is in the state file. Packets: type-0
(consecutive and one-register), type-1 and type-2; the type-3 2D
packets PAINT, PAINT_MULTI, BITBLT, BITBLT_MULTI, TRANS_BITBLT,
HOSTDATA_BLT, POLYLINE, POLYSCANLINES, NEXTCHAR, PLY_NEXTSCAN,
SET_SCISSORS (with the settings block, every brush packet,
`BRUSH_Y_X`); LOAD_PALETTE (R5xx guide 6.2.2.12; kept for the 2D
scaler, which is not modelled); the 3D packets below. The R200
microcode's packets -- the `_2` draws, `3D_CLEAR_HIZ`, `INDX_BUFFER` --
do nothing on the RV200: the legacy DRM refuses them unless the R200
microcode is loaded ("safe but r200 only", `radeon_state.c`), and the
R100 image's dispatch table has no handler for them; they are
implemented for a part whose row says so, or when the R200 image is
loaded.

**The memory controller's view of the bus** (`RadeonGart.cpp`): an
address the CP or the 3D engine fetches is the framebuffer
(`MC_FB_LOCATION`), the AGP window (`MC_AGP_LOCATION`, reaching the bus
at `AGP_BASE`), or the card's own PCI GART: `AIC_CNTL`
`PCIGART_TRANSLATE_EN`, the range `AIC_LO_ADDR`..`AIC_HI_ADDR`, a page
table at `AIC_PT_BASE` of one dword a 4 KB page holding its bus address,
itself in the framebuffer or in host memory (Linux
`r100_pci_gart_enable`, `r100_pci_gart_set_page`; the legacy DRM's
`radeon_set_pcigart`). The AGP window's addresses are bus addresses in
the host bridge's AGP aperture; on the Alpha machines that have an AGP
port (the Titan's, the Marvel IO7's port 3) Linux builds the aperture
from the port's scatter-gather window (`titan_agp_*`, `marvel_agp_*`),
which the chipset model translates already. Every access is translated
afresh (Linux notes the chip caches one translation with no way to
flush it) [inference]; an address in none of the ranges is not decoded.

**Clocks** (`RadeonTiming.cpp`): the pixel clock is the PPLL's, ref x
FB_DIV / (REF_DIV x POST_DIV) (radeonfb: `PPLL_REF_DIV` <9:0>,
`PPLL_DIV_n` <10:0> and the post divider code <18:16>, the set chosen by
`CLOCK_CNTL_INDEX` <9:8>, the CRTC on the PPLL when `VCLK_ECP_CNTL` <1:0>
is 3), with the 27 MHz reference from the card's BIOS PLL block. With
`PPLL_CNTL` `ATOMIC_UPDATE_EN` new dividers reach the PLL only through
`PPLL_REF_DIV`'s atomic update bit, which reads 1 until it completes, a
microsecond later [inference: radeonfb says most chips "pass at the very
first test"]. A VGA mode's clock is the divider set the VGA clock select
names [inference]. The CRTC's frame lasts total pixels / pixel clock:
the current line, `CRTC_STATUS`'s vertical blank, `CRTC_CRNT_FRAME`, the
VBLANK and VLINE interrupts (`CRTC_VLINE_CRNT_VLINE` <11:0>) follow, and
stay continuous across mode changes; a rate outside 10-400 Hz (no PLL
set up yet) is taken as 60 Hz. `PLL_TEST_CNTL` <31:24>, the counter the
BIOS times its microsecond delays with (0x7154: 27 counts, 0x715d: 135),
counts reference clocks from what was written and stops at 255
[inference]. `MC_STATUS` `MEM_PWRUP_COMPL_A/B` <1:0> clear for two
microseconds after each `MEM_SDRAM_MODE_REG` write, which the BIOS polls
for (0x6c94) [inference: the time]; `MC_IDLE` <2> is the engine's idle.

**Display** (`RadeonDisplay.cpp`): extended modes, the 8-bit palette,
the hardware cursor (mono AND/XOR, and 64x64 ARGB blended as
premultiplied alpha).

**3D engine**, the R100 generation's (`r100/`: `RadeonR100_3D.cpp`,
`RadeonR100Tcl.cpp`, `RadeonR100Raster.cpp`):

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
- vertex blending (`SE_TCL_UCP_VERT_BLEND_CTL` BLEND_OP_COUNT,
  POSITION/NORMAL_BLEND_OP_ENABLE, WGT_MINUS_ONE: the weighted sum of the
  MODELPROJECT/MODELVIEW/IT_MODELVIEW matrices `SE_TCL_MATRIX_SELECT_n`
  names, with the vertex's blend weights) [inference: no driver programs
  it on the R100]; two-sided lighting (`LIGHT_TWOSIDE`: the vertex lit
  again with its normal reversed, the triangle's facing choosing the
  set; Mesa leaves it to the chip only when the front and back materials
  are equal); the TCL unit's own culling (`CULL_FRONT`, `CULL_BACK`,
  `CULL_FRONT_IS_CCW`);
- setup and rasteriser: sub-pixel snapping, OpenGL or Direct3D pixel
  centres, face culling and point/line fill modes, the top-left rule,
  solid/flat/Gouraud shading per attribute, perspective-correct
  texturing, the clip rectangle, the polygon stipple and the line
  pattern; polygon offset (`SE_CNTL` `ZBIAS_ENABLE_POINT/LINE/TRI`,
  `SE_ZBIAS_FACTOR` x the depth slope + `SE_ZBIAS_CONSTANT`, as Mesa's
  `radeonPolygonOffset` loads them); anti-aliased lines and polygons
  (`PP_CNTL` `ANTI_ALIAS` <25:24>: the alpha times the 4x4-sample
  coverage) [inference]; the primitive types `TRI_TYPE_2` (as a triangle
  list) and the 3-vertex point and line lists (each triple's vertices as
  points, its edges as lines) [inference: Mesa only checks their vertex
  count, a multiple of 3]; `3D_CLEAR_ZMASK`, the HyperZ fast clear, as
  its visible effect (the Z blocks read as `RB3D_DEPTHCLEARVALUE`), from
  the legacy DRM's reverse-engineered clear [inference: 8x8 blocks];
- textures (three units): I8, AI88, RGB332, ARGB1555, RGB565, ARGB4444,
  ARGB8888, RGBA8888, Y8, both YUV 4:2:2 orders (with YUV-to-RGB),
  DXT1, DXT2/3, DXT4/5; power-of-two mip chains and non-power-of-two
  images (`PP_TEX_SIZE`/`PP_TEX_PITCH`); nearest, linear, mip-nearest
  and mip-linear filters, LOD bias, the maximum level; the eight wrap
  modes and the border colour; cube maps (`CUBIC_MAP_ENABLE`: faces +X
  .. +Z at `PP_CUBIC_OFFSET_Tn_0..4`, -Z at `PP_TXOFFSET_n`, one level,
  the third coordinate in the Q slot -- Mesa's `cube_emit_cs`,
  `radeon_swtcl.c`; OpenGL's face selection); micro-tiled textures and
  the texture endian swaps (`PP_TXOFFSET` <1:0>, `PP_TXFORMAT` <27:26>);
- the combiners (`PP_TXCBLEND`/`PP_TXABLEND`, three stages): add,
  subtract, add-signed, blend, dot3, complemented arguments, the texture
  factor, x2/x4 scale and clamping; the specular colour sum; fog;
- table fog (`PP_FOG_COLOR` `FOG_TABLE`: 256 8-bit entries loaded
  through `FOG_TABLE_INDEX`/`DATA` four a dword, indexed by the depth or
  an alpha) [inference];
- the alpha test, stencil (eight functions, six operations, masks), Z
  (16-, 24- with stencil and 32-bit integer; 24- and 32-bit floating-point
  Z; 16-, 24- and 32-bit floating-point W -- stored as unsigned floats
  whose integer order is the value's [inference: the layouts are not
  documented]), blending (the GL factors, add and subtract), the logic
  op, the plane mask; colour buffers in ARGB8888, RGB565, ARGB1555,
  ARGB4444, RGB332, Y8 and RGB8, linear or micro-tiled (32-byte tiles of
  4x2, 8x2 or 8x4 pixels, Mesa's `radeon_tile.c`; the depth buffer's
  HyperZ tiling is taken to be the same [inference]), with the colour and
  depth endian swaps (`COLOR_ENDIAN`, `DEPTH_ENDIAN`); the colour's
  quantisation as `RB3D_CNTL` says -- truncated, rounded (`ROUND_ENABLE`),
  dithered by horizontal error diffusion (`DITHER_ENABLE`, with
  `DITHER_INIT` restarting each line) or by an ordered pattern
  (`SCALE_DITHER_ENABLE`); Mesa's driconf options name the modes, the
  pattern (4x4 Bayer) and the error's arithmetic are inferences.

**Rasterisation rules.** No source documents the R100's setup
arithmetic -- its sub-pixel precision beyond `SE_CNTL`'s `ROUND_PREC`
and `ROUND_MODE` fields, its fill convention, its Z interpolation
precision. The model snaps vertices as those fields say and then
rasterises in double precision with the top-left rule and attribute
planes evaluated at pixel centres; that stays as it is until a source
says otherwise.

## How it is checked

`ALPHABOX_RADEON_SELFTEST=1` (or `=exit` to end the emulator with the
result) runs at the end of the card's `init()`, before the machine
starts, and puts the card back as it found it. It drives the engines as a
driver does -- register writes through the MMIO BAR, packets through the
CP ring in VRAM -- and compares each result with a reference written
independently in `RadeonSelfTest.cpp` and, for the 3D scenes, the
generation's `r100/RadeonR100SelfTest.cpp` (its own ROP3 truth table,
line properties, bit expansion, a double-precision barycentric
rasteriser, texture decoders, combiner equations, transform and
lighting):

```
cd lab/radeon-selftest      # an ES40 config with pci0.2 = radeon and gui = sdl
SDL_VIDEO_DRIVER=dummy ALPHABOX_RADEON_SELFTEST=exit alphabox run
```

It prints `%RADEON-I-SELFTEST: <check> ok|FAILED` per check and `PASS`
or `FAIL`. 68 checks: 11 on the command FIFO, the engine's busy time,
the CP's streams and micro-engine, the GART and the clocks
(`RadeonSelfTestQueue.cpp`), 4 on the microcode lookups (the known-image
table, synthetic images of each word width, the R100 and R200 packet
sets, the self-test's own unknown image; `RadeonMicrocode.cpp`), 26 on
the 2D engine, the CP and the cursor,
26 on the 3D engine (the generation's scenes, run between the 2D checks
and the last two 2D/CP ones), and one that every wait for idle ended. The self-test
waits for the engine as a driver does (64 free FIFO entries, then
`GUI_ACTIVE` clear) before it touches memory the engine draws in, and
for the CP before it goes back to MMIO. With `ALPHABOX_RADEON_SYNC=1`
the queue checks are left out (58 checks). The scenes are written as PNGs to `ALPHABOX_RADEON_SELFTEST_DIR`
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
| 11-primitives | quads, quad strips, polygons, lines, points, wide lines; the IMMD, VBUF (two arrays), INDX, RNDR_GEN_INDX_PRIM and register-port walks; an IMMD_2 (an R200 packet) that draws nothing |
| 12-stipple-pattern | the polygon stipple with offsets, the line pattern |
| 13-tcl-lit-cube | TCL: matrices, two lights (directional with specular, local with attenuation), linear fog, Z, culling |
| 14-tcl-texmatrix-ucp | TCL: the texture matrix, a user clip plane |
| 15-tcl-near-clip | TCL: clipping at the near plane (a vertex behind the eye) |
| 16-render-composite | X.org's Render composite as `R100PrepareComposite` programs it: OVER, an A8 mask, non-power-of-two textures |
| 17-misc-state | LOD bias, mirror-once and GL clamp, the logic op and plane mask, float colours, 32-bit indices, an indexed `RNDR_GEN_INDX_PRIM` |
| 18-tiling-endian | a micro-tiled, dword-swapped colour buffer read back by the reference's own de-tiling; a micro-tiled, byte-swapped RGB565 texture drawn 1:1 |
| 19-zbias-dither | polygon offset by the constant (coplanar quads) and by the slope factor (a sloped quad pushed behind its twin); RGB565 truncation, rounding and ordered dither and error diffusion are checked pixel by pixel beside it |
| 20-cube-fogtable-aa | the six cube faces; table fog over a depth ramp; an anti-aliased triangle blended by its coverage |
| 21-floatz-wbuffer-fastclear | crossing triangles in 24- and 32-bit float Z and the 24-bit W buffer (where the crossing moves); a `3D_CLEAR_ZMASK` fast clear deciding a later depth test |
| 22-tcl-twoside-blend | TCL lighting of front and back faces without and with `LIGHT_TWOSIDE`; TCL back-face culling; vertex blending of two matrices |
| 23-prims-r200-packets | `TRI_TYPE_2`, the 3-vertex point and line lists; `INDX_BUFFER` after an index-less `3D_DRAW_INDX` with the R200 packets switched on, and nothing without |

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
mono cursor) -- its server loads the microcode, enables the CP and paces
itself on RBBM_STATUS and the scratch registers, and its login screen's
last frame is the same with the asynchronous engine as before it
(`lab/radeon-gaps/`); nada's Windows 2000 driver (MMIO only, waiting on
the FIFO and the cache flush) runs its test unchanged.

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

## Not emulated, and what a real card would have to settle

Said once at run time when a guest asks for them:

- macro tiling of colour buffers and textures: no source gives the
  R100's macro tile layout (Linux and X.org only set the bits); drawn
  linear;
- the texture chroma key: no source names the register the key colour is
  in; volume (3D) textures: Mesa's r100 driver refuses them and nothing
  gives their layout; the sprite primitive (`SPIRIT_LIST`);
- `CNTL_SMALLTEXT` and `LOAD_MICROCODE`: no source gives their formats.

Silently not modelled: dual-cone spots and the specular threshold (no
source says what they compute); the hierarchical Z RAM (it only lets
the chip skip work); the destination caches' contents (the engine writes
memory directly); the R5xx-only packets (PRED_EXEC, COND_EXEC,
WAIT_SEMAPHORE, WAIT_MEM, MPEG_INDEX); the micro-engine's own program.

Inferences a real card would have to confirm, besides those marked
above: the engine's busy time, the CP's PIO queue depth, the PLL's
update and the test counter's rate, the source scissor's effect, the
float and W depth layouts, the dither pattern, table fog's table, the
anti-aliasing coverage, vertex blending, the 3-vertex lists, the fast
clear's block geometry. The self-test's references are written from the
same sources as the model: a pass shows consistency, not the silicon's
behaviour.
