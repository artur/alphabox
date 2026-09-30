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
| `tablet:X:Y[:B]` (a `KEYSCRIPT`/`KEYPIPE` token) | Put the USB tablet's pointer (`port1 = "tablet";` on `ali_usb`) at X,Y -- fractions of the screen when at most 1, else guest pixels -- with buttons B (bit 0 left, 1 right, 2 middle). |
| `usb:<op>:<port>[:<ep>[:<n>]]` (a `KEYSCRIPT`/`KEYPIPE` token) | Inject a fault on a USB root hub port -- the `ehci` card's if there is one, else `ali_usb`'s -- for testing a guest's USB driver. `usb:detach:<p>` / `usb:attach:<p>` unplug and replug the device (connect change and interrupt as on hardware). `usb:stall:<p>:<ep>[:<n>]` halts endpoint `<ep>` (an address: `0x81` = IN 1, `0x02` = OUT 2): it answers STALL until the host sends CLEAR_FEATURE(ENDPOINT_HALT), and halts again on its next transfer, `n` times in all (default 1). `usb:phase:<p>`: the USB disk's next command ends with a phase-error CSW (status 2). `usb:nak:<p>:<ep>:<ms>`: the endpoint NAKs for `<ms>` milliseconds. `test/tools/usb_bench.sh` fires them with `FAULTS=`/`PRE_FAULTS=`. |
| `ALPHABOX_EHCI_SELFTEST=1` | At start-up, drive the `ehci` card from inside the emulator with a minimal EHCI driver (structures in the top 16 MB of guest RAM, before any firmware runs): port reset, enumeration, SET_ADDRESS/SET_CONFIGURATION, and a Bulk-Only READ(10) of sector 0 from the USB disk on port 1, which must end in 55 AA. Prints `%EHCI-I-SELFTEST` lines and PASS or FAIL. |
| `ALPHABOX_VIRTIO_SELFTEST=1` | At start-up, drive every `virtio_blk` and `virtio_net` device through its legacy registers with a minimal driver (rings in 1 MB per device, 16-32 MB below the top of guest RAM, addressed physically): reset, features, queues, blk GET_ID, sector 0 (must end in 55 AA), a write of the last sector read back and restored, FLUSH, error statuses; net transmit and receive (needs a backend that returns frames: UDP with `udp_local` = `udp_remote`). Prints `%VIRTIO-I-SELFTEST` lines and PASS or FAIL per device ([virtio.md](virtio.md)). `ALPHABOX_VIRTIO_SELFTEST_DELAY=<s>` runs it `<s>` seconds later, through the DMA window the console set up. |
| `ALPHABOX_VIRTIO_TRACE=1` | Trace virtio register accesses, queue addresses, notifies and interrupt changes (`VIRTIO` lines). |
| `ALPHABOX_USB_ASYNC_US=<us>` | USB storage answers like a device behind libusb: data transfers finish on another thread after `<us>` microseconds, NAKed until then. Tests and times the controller's passthrough path without a host device (`test/tools/usb_bench.sh`). |
| `ALPHABOX_USBAUDIO_WAV=<file>` | The USB speaker (`port<n> = "audio";`) writes the PCM stream the guest sends it into `<file>` as a WAV (16-bit stereo, at the rate the guest chose; a rate change part way starts `<file>` with `-1`, `-2`... before the extension). The header is kept current every quarter second, so the file is whole even after a kill. Use it with `SDL_AUDIO_DRIVER=dummy` to keep a headless run off the host's speakers. |
| `ALPHABOX_USBTRACE=1` | Trace OHCI register writes (with read counts in between) and every control request a USB device serves, and each USB storage command and status wrapper (`USBT` lines). |
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
| `ALPHABOX_BLIT_STATS=1` | Every 5 s, print what the 8514/A drawing engine has drawn: pixels, drawing commands and host-data transfers, with the host time the commands took. Says whether a sluggish-feeling guest is drawing-bound, and how the driver is drawing (a listing that scrolls a console draws ~900 million pixels, one transfer each). The timing is per command, not per pixel, so it does not swamp what it measures -- but it does slow a drawing-heavy guest noticeably, so leave it off when timing anything. |

**JIT builds**

| Variable | Effect |
|---|---|
| `ALPHABOX_JIT_COMPILE_AFTER=<n>` | Interpret a block `n` times before compiling it. The default is 16 (1 on JIT_VERIFY builds). |
| `ALPHABOX_JIT_FPTEST=1` | JIT_VERIFY builds only: self-test the inline IEEE FP ops against the interpreter at startup, then exit with the verdict. |
| `ALPHABOX_NO_IDLE=1` | Disable idle pacing. |
| `ALPHABOX_IDLESTATS=1` | Print idle-pacing counters every 2000 idle-loop visits. |
| `ALPHABOX_STALL_SKIP=0` | Make a guest's `RPCC` delay loop wait in real time, as the hardware would, instead of being handed the cycles it is waiting for. Costs a Windows 2000 boot 35 seconds; see docs/cpu-fidelity.md for what the default buys and what it diverges on. |
| `ALPHABOX_PORT61_PACE=0` | Spin on the ISA refresh-toggle bit of port 61h instead of sleeping to the next edge. The guest's timing is the same either way; this one only decides whether a host core burns for it. |
| `ALPHABOX_JIT_NOPFLUSH=0\|2` | `0`: flush the instruction cache on every `IMB`, without asking whether anything was written to memory code was compiled from. `2`: flush anyway, but report any block whose source changed while the code-page map said nothing had (the audit; see test/tools/smc_test.sh). |
| `ALPHABOX_JIT_DPC2=0` | No second level behind the data page cache: a level-1 miss goes straight to the helper. AArch64 compiled code and the helpers; the x86-64 emitter never probes it. |
| `ALPHABOX_DPC_KEEP=0` | A data-TB fill empties the page-cache slots of the page it evicts and of the page it inserts, whatever they hold, as before. See docs/cpu-fidelity.md, "Data translations outlive their TB entry". |
| `ALPHABOX_JIT_UNALIGNED=0` | Every unaligned load from compiled code bails to the interpreter, instead of the read helper doing it. |
| `ALPHABOX_JIT_DPC_SLOTS=64\|128` | Use only that many of level 1's 256 page-cache slots a row (the index is masked), for a same-binary A/B of the slot count. |
| `ALPHABOX_JIT_REUSE=0` | Every memory op probes the data page cache itself, instead of reusing the block's previous probe when it is on the same page (AArch64). |
| `ALPHABOX_JIT_PEEP=0` | The AArch64 operate emitter without its in-place forms (longword arithmetic, logical immediates, byte manipulation): operands shuttled through x0/x1 as before. For a same-binary A/B. |
| `ALPHABOX_JIT_ADAPTPIN=0` | Keep the starting pin set instead of following the registers the running code uses (AArch64 emitter). See docs/performance.md, "Sixteen pins, chosen by what runs". |
| `ALPHABOX_JIT_PINLOG=1` | Print each change of pin set, with the share of register accesses the old and the new set cover. |
| `ALPHABOX_JIT_PIN16=0` | Start with the 14 pins there were before `x20` and `x28` were freed. |
| `ALPHABOX_JIT_PINSET=1\|2` | Experiment: start from a different pin set of 14 (AArch64 emitter). `1` gives R17, R18 and R27's slots to R13-R15, the nada benchmark's hot registers; `2` gives R9-R11, R26, R27, R29 and R30's to R8, R4-R6 and R21-R23, those of Microsoft's `makecab`. Combine with `ALPHABOX_JIT_ADAPTPIN=0` to keep it. See docs/performance.md, "What a pinned register is worth". |
| `ALPHABOX_JIT_RPCCTEST=1` | Check the generated `RPCC` stub against the helper it replaces, from six fixed starting states, and print the verdict. |
| `ALPHABOX_INTERP=1` | Interpret everything; never compile. The control arm for what compiled code is worth. |
| `ALPHABOX_RATE=<sec>` | Every `<sec>` seconds (fractions allowed), print each processor's instruction rate, and what its `IMB`s, its cycle-counter reads and its delay loops are costing. Accurate enough to measure with -- it looks at the clock once every 256 batches -- and `cpu_bench.sh` reads it rather than timing runs from outside. |
| `ALPHABOX_JIT_OFFSETS=1` | Print the field offsets compiled code addresses `this` by. For when a member has been added in the wrong place and the emitter's displacements no longer reach. |

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

`ALPHABOX_TRACE_CALLS=1` reports the firmware's own calls, once per call
site and routine, which is how a silent failure inside a console is read
(it found the DS10's). Interpreter builds only.

`ALPHABOX_TRACE_FLASH=1` reports the commands firmware sends the flash,
which tells "it never found the part" from "it read what it wanted".

`ALPHABOX_TRACE_SERIAL=1`, `ALPHABOX_TRACE_KBC=1` and
`ALPHABOX_TRACE_PORT61=1` report the UARTs, the keyboard controller and the
ISA refresh-toggle port; `ALPHABOX_TRACE_LFB=1` reports the S3's linear
framebuffer window as it is offered and withdrawn, and
`ALPHABOX_TRACE_CODEWRITE=1` reports the guest writing to memory some block
was compiled from, which is what decides whether an `IMB` has work.

`ALPHABOX_DUMP_MEMORY=1` writes guest memory to `memory_000000000000.dmp`
when the emulator is asked to stop, which is how to find what a firmware
left in memory (and what it did not).

A firmware scanning empty configuration space produces these normally; the
traces matter when a machine's firmware wants hardware that is not emulated
yet (see [platforms.md](platforms.md)).
