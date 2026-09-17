# Headless operation and debug hooks

For automated or headless use (CI, scripted firmware navigation, driving the
emulator over SSH), the SDL GUI reads a set of environment variables at
startup; each one is off unless set.

To run the whole GUI stack without a window or a display server, set
`SDL_VIDEO_DRIVER`:

- `dummy` works on every host tried, macOS included.
- `offscreen` works on Linux, but on macOS it fails to create the window
  ("Could not initialize OpenGL / GLES library") and nothing renders.

## Environment variables

**Screen and input**

| Variable | Effect |
|---|---|
| `ALPHABOX_DUMP_FB=<prefix>` | Write the emulated screen as a PPM image (`<prefix>-NNN-WxH.ppm`) every ~2 seconds. This is how you "see" the VGA output on a headless run; `test/tools/ppm2png.py` converts a dump to PNG. |
| `ALPHABOX_KEYSCRIPT="<sec>:<key>,..."` | Press named keys at fixed second offsets from GUI start, e.g. `ALPHABOX_KEYSCRIPT="40:a,41:r,42:c,43:enter"` types `arc` + Enter at the SRM prompt 40 s in. |
| `ALPHABOX_KEYPIPE=<file>` | Interactive variant: keys appended to `<file>` while the emulator runs are typed into the guest, one token per ~120 ms. Example: `echo "f2 down down enter" >> keys.txt`. Start with an empty file; the emulator remembers how far it has read. |
| `ALPHABOX_AUTOKEY_ENTER=<sec>` | Press Enter every `<sec>` seconds (a blunt tool for firmware "press any key" prompts). |
| `ALPHABOX_AUTOMOUSE=<sec>` | From `<sec>` seconds in, inject synthetic PS/2 mouse motion (a square pattern plus a periodic left click) straight into the guest, bypassing host input. As with a real PS/2 mouse, nothing is sent until the guest driver enables data reporting. |
| `ALPHABOX_MOUSE_DEBUG=1` | Trace host mouse motion, grab/focus transitions, relative-mode failures, the guest's aux commands and any dropped mouse bytes (`MOUSEDBG` lines). |

**Guest diagnostics**

| Variable | Effect |
|---|---|
| `ALPHABOX_PC_SAMPLE=1` | Print each CPU's program counter every state poll (~100 ms); finds where a guest is stuck. |
| `ALPHABOX_IRQSTATS=1` | Every 5 s, print interrupt rates: CPU interrupt entries by source, Cchip interval-timer ticks, 8259 edges and acknowledges per ISA IRQ, and Cchip DRIR rises. Spots interrupt storms. |
| `ALPHABOX_IRQTRACE=<n>` | Log interrupt entries `n`..`n+39`, with the IER/SIRR/CM writes and ISUM reads between them. |
| `ALPHABOX_IDETRACE=1` | Timestamped IDE timeline: commands, ATAPI packet opcodes, bus-master starts and interrupts, and every ATAPI check condition with its sense key. |
| `ALPHABOX_MEDIA_SWAP=<image1>:<image2>:<ms>` | Media-change stress test: alternate two images in the first CD drive every `<ms>` ms, forced past a guest lock, applied between guest commands. |
| `ALPHABOX_USBTRACE=1` | Log each OHCI register write with the per-register read counts since the previous write. |

**JIT builds**

| Variable | Effect |
|---|---|
| `ALPHABOX_JIT_COMPILE_AFTER=<n>` | Interpret a block `n` times before compiling it. The default is 16 (1 on JIT_VERIFY builds). |
| `ALPHABOX_JIT_NO_DLINK=1` | AArch64 only: use the tag-checked link scan for static block exits instead of epoch-keyed data links (an A/B switch for chaining issues). |
| `ALPHABOX_JIT_FPTEST=1` | JIT_VERIFY builds only: self-test the inline IEEE FP ops against the interpreter at startup, then exit with the verdict. |
| `ALPHABOX_NO_IDLE=1` | Disable idle pacing. |
| `ALPHABOX_IDLESTATS=1` | Print idle-pacing counters every 2000 idle-loop visits. |

## Key names

These names work for `ALPHABOX_KEYSCRIPT` and `ALPHABOX_KEYPIPE`:

- letters and digits: `a`–`z`, `0`–`9`;
- editing and navigation: `enter`, `esc`, `tab`, `space`, `bksp`, `del`,
  `ins`, `home`, `end`, `pgup`, `pgdn`, `up`, `down`, `left`, `right`;
- punctuation: `bslash`, `dot`, `minus`, `equals`, `comma`, `slash`,
  `semicolon`, `quote`, `lbracket`, `rbracket`, `grave`;
- function keys: `f1`–`f12`;
- modifiers and system keys: `ctrl`, `shift`, `alt`, `win`, `menu`.

For a chord, prefix a key with modifiers joined by `-`: `win-r`, `shift-5`
(`%` on a US layout), `ctrl-alt-del`. `test/tools/keys_for.py "<text>"` turns
a line of text into tokens for a US keyboard layout.

## Example: a fully headless firmware run

```
SDL_VIDEO_DRIVER=dummy ALPHABOX_DUMP_FB=fb \
ALPHABOX_KEYPIPE=keys.txt ALPHABOX_KEYSCRIPT="40:a,41:r,42:c,43:enter" \
alphabox run &
# watch fb-*.ppm to see the screen; echo keys >> keys.txt to react
```

A `serial` section with a `port` waits for a telnet connection before the GUI
comes up. For unattended runs, connect a client or use
`null_attach = true`.

## Tracing accesses no device claims

`ALPHABOX_TRACE_UNKNOWN=1` reports every read or write that no device
answered -- the address, its width, and the instruction or device that made
it:

```
%SYS-T-UNKNOWN: read  32 bits at 803fe002800 (PCI configuration) from cpu0 pc=00000000001a1358
```

The instruction is rarely the interesting one -- firmware reaches hardware
through access helpers -- so the return address is reported with it.

`ALPHABOX_TRACE_I2C=1` does the same for the chipset's I2C bus, reporting
every address the firmware puts on it and whether anything answered.

`ALPHABOX_TRACE_MP=1` reports the registers a console uses to start other
processors and hand work to them -- how a machine brings its processors up
differs from board to board.

`ALPHABOX_DUMP_MEMORY=1` writes guest memory to `memory_000000000000.dmp`
when the emulator is asked to stop, which is how to find what a firmware
left in memory (and what it did not).

A firmware scanning empty configuration space produces these normally; the
traces matter when a machine's firmware wants hardware that is not emulated
yet (see [platforms.md](platforms.md)).
