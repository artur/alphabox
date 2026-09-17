# AXPbox — AlphaServer ES40 emulator

AXPbox emulates an HP/DEC AlphaServer ES40: one to four Alpha EV68CB CPUs on
the Tsunami/Typhoon chipset, with SCSI, IDE/ATAPI, floppy, serial, USB,
Ethernet, sound and S3 Trio64 graphics. It boots OpenVMS, Tru64 UNIX, NetBSD
and the Windows NT/2000 family, on x86-64 and AArch64 hosts (Linux, macOS,
Windows).

![OpenVMS 8.4 desktop](screenshots/openvms.png)

OpenVMS 8.4 in AXPbox. [How to get this CDE desktop running](https://github.com/lenticularis39/axpbox/wiki/GUI-Desktop-Environment-(CDE))

![Windows 2000](screenshots/win2000.png)

Windows 2000 build 2128 on AXPbox. [Installation guide](https://web.archive.org/web/20260705122517/https://www.zx.net.nz/computers/dec/axpemu-es40.shtml)

## Speed

On an Apple M-series host with the JIT build, Windows 2000 runs at roughly
**2250 MIPS per emulated CPU**. That figure comes from the guest's idle loop
(`AXPBOX_NO_IDLE=1`, JIT_STATS build, two CPUs) and is therefore an upper
bound, not a mixed-workload number: a CPU-bound job inside the guest — a
25-million-iteration `cmd` loop — takes about 110 s, and the run-to-run
spread there is a few percent. Both depend on host and workload.

An **idle** guest costs almost nothing: idle pacing recognizes the NT idle
loop and the HAL's parked-processor loop and sleeps until an interrupt
arrives, so an idle two-CPU Windows desktop sits at ~3–6% of one host core
instead of 100% per emulated CPU (`AXPBOX_NO_IDLE=1` disables this).

## Emulated hardware

| Area | Device |
|---|---|
| CPU | 1–4 × Alpha EV68CB (21264), SMP, wall-clock-paced RPCC and interval timer |
| Chipset | Tsunami/Typhoon: Cchip, Dchip, 2 × Pchip, TIG, DPR/RMC |
| Memory | 64 MB – 32 GB (`memory.bits` 26–35), reported as up to four memory arrays with matching DIMM/SPD data |
| Storage | Sym53C810 / Sym53C895 SCSI, ALi M1543C IDE (disks + ATAPI CD-ROM), 82077AA floppy, RAM disk, raw and BIN/CUE images |
| ISA bridge | ALi M1543C: 8259 PIC, 8254 PIT, MC146818 RTC/TOY, 8237 DMA, SuperIO, PMU |
| Input | i8042 keyboard controller with PS/2 keyboard and mouse |
| Graphics | S3 Trio64 (+ IBM 8514/A) or Cirrus Logic CL-GD5434, with the real VGA BIOS, rendered through SDL3 |
| Network | DEC 21143 (Tulip) over pcap, or TUN/TAP on Linux |
| Sound | Ensoniq AudioPCI ES1370 |
| Other | OHCI USB, 2 × 16550 serial (telnet or bit-bucket), flash and DPR NVRAM persistence |

## Getting AXPbox

Build from source — see below. (The upstream project is packaged in
[T2 SDE](http://t2sde.org/packages/axpbox) and
[openSUSE](https://build.opensuse.org/package/show/Emulators/axpbox); those
packages track [lenticularis39/axpbox](https://github.com/lenticularis39/axpbox),
not this fork.)

You need CMake and a C++17 compiler. Optional: pcap for networking, SDL3 for
graphics (bundled as a submodule and linked statically when no system SDL3
is found), asmjit for the JIT.

```
git clone --recurse-submodules https://github.com/artur/axpbox
cd axpbox
# existing clone: git submodule update --init
```

Without `-DCMAKE_BUILD_TYPE` the build defaults to Release.

### Linux

```
sudo apt install build-essential cmake libpcap-dev
# only needed when building the bundled SDL3 (skip for headless):
sudo apt install libx11-dev libxext-dev libxrandr-dev libxcursor-dev \
                 libxi-dev libxtst-dev libxfixes-dev libxss-dev libxkbcommon-dev libwayland-dev libegl-dev

cmake -S . -B build
cmake --build build -j$(nproc)
```

The binary is `build/axpbox`. Headless: add `-DDISABLE_SDL=yes
-DDISABLE_X11=yes`; without networking: `-DDISABLE_PCAP=yes`.

### macOS

```
brew install cmake sdl3
cmake -S . -B build
cmake --build build -j$(sysctl -n hw.ncpu)
```

### Windows

Visual Studio 2022 with "Desktop development with C++" and CMake. For
networking at build time you need the
[npcap SDK](https://npcap.com/dist/npcap-sdk-1.13.zip) (unzip to e.g.
`C:\pcap`), and [Npcap](https://npcap.com/#download) installed at run time;
the emulator also starts without it, just without networking. SDL3 needs no
separate install.

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 ^
      -DPCAP_INCLUDE_DIR=C:/pcap/Include ^
      -DPCAP_LIBRARY=C:/pcap/Lib/x64/wpcap.lib
cmake --build build --config Release
```

The binary is `build\Release\axpbox.exe`.

### JIT

The JIT translates Alpha basic blocks to host code (asmjit), with emitters
for **x86-64 and AArch64** (Apple Silicon, arm64 Linux). Clone asmjit at the
pinned revision and enable it:

```
git clone https://github.com/asmjit/asmjit third_party/asmjit
git -C third_party/asmjit checkout 0bd5787b54b575ed94bf32ac452153b34385c514
cmake -S . -B build-jit -DES40_DISABLE_ASMJIT=OFF
cmake --build build-jit -j$(nproc)
```

Debug lanes: `-DCMAKE_CXX_FLAGS="-DJIT_VERIFY"` re-runs every compiled block
against the interpreter and reports any divergence (expect `0 mismatches`);
`-DJIT_STATS` adds throughput, cold-path and bail counters. On a JIT_VERIFY
build, `AXPBOX_JIT_FPTEST=1` self-tests the inline IEEE floating-point ops
against the interpreter over ~8.5 M cases.

## Usage

```
axpbox configure     # interactive generator, writes es40.cfg
axpbox run           # start the machine
axpbox --version     # version, commit and compiled-in features
```

The sample [es40.cfg](es40.cfg) documents every configuration value; the
generator covers the common ones. You also need an SRM ROM image (and, for
the ARC/AlphaBIOS path, a flashed AlphaBIOS image and the S3 VGA BIOS).

Ctrl-C or SIGTERM ends a run gracefully, saving the flash and DPR (NVRAM)
images. With `exit_on_pal_halt = true` in the `sys0` section, a guest halt
(an OS shutting down to the console) ends the run the same way, which is
handy for scripted use.

Guest installation guides live in the
[lenticularis39/axpbox wiki](https://github.com/lenticularis39/axpbox/wiki):
[OpenVMS](https://github.com/lenticularis39/axpbox/wiki/OpenVMS-installation-guide),
[NetBSD](https://github.com/lenticularis39/axpbox/wiki/NetBSD-9.2-install-guide).

### S3 Trio64 graphics / ARC / Windows NT

The S3 Trio64 emulation uses the real S3 VGA BIOS: obtain `86c764x1.bin`
from the [86Box ROM set](https://github.com/86Box/roms/tree/master/video/s3)
and point the `s3` config section at it. This is required for the
ARC/AlphaBIOS console and Windows NT-family guests: flash AlphaBIOS from the
Alpha Systems Firmware v7.3 CD, then enter `arc` at the SRM prompt.

### Serial consoles, networking, and sound

- **Serial ports**: the sample config attaches both UARTs as `null_attach`
  (present but unconnected), so the emulator starts without waiting for
  anything and the console lives on the VGA window (`vga_console = true`).
  To use a telnet console instead, set `port = 21264;` in a serial section —
  the emulator then **waits at startup** until a client connects
  (`nc localhost 21264`). Ports left out of the config are synthesized as
  `null_attach` automatically.
- **Networking**: the DEC 21143 NIC (`pci0.4 = dec21143`) connects to the
  host through one of two backends, selected with `type`:
  - `type = "pcap"` (default): captures on an existing host interface.
    Set `adapter = "eth0";` (Linux) or the `\Device\NPF_{...}` name
    (Windows/Npcap). On Linux, grant capture permission once:
    ```
    sudo setcap cap_net_raw,cap_net_admin+eip ./axpbox
    ```
    otherwise startup fails with "Error opening adapter". Don't leave
    `adapter` unset on unattended runs — the emulator asks interactively.
  - `type = "tap"` (Linux only): uses a TUN/TAP device, so the guest becomes
    reachable from the host and can be bridged onto the LAN. Options:
    `adapter = "tap0";` (created if needed — needs CAP_NET_ADMIN or root),
    `host_ip = "10.0.0.1/24";`, `bridge = "br0";`, `uplink = "eno1";`,
    `tap_create = true;`.

  Both backends also take `mac` (default `08-00-2B-E5-40-<nic#>`), `queue`
  (rx queue depth, default 1024), `crc` and `trace_packets`.
- **Sound**: `pci1.1 = es1370 {}` adds an Ensoniq AudioPCI ES1370 (SDL
  builds). Guest drivers exist for Windows NT 4; other guests ignore it.
- **Mouse**: click the window to grab, Ctrl+F10 to release; `mouse.speed`,
  `mouse.invert_x`, `mouse.invert_y` tune it (see the WSLg note below).
  `video.scale_ratio` / `video.scale_change_enable` control window scaling.
- **Hotkeys**: every GUI shortcut can be rebound with `hotkey.*` in the
  `sdl` section (e.g. `hotkey.ctrl_alt_delete = "GUI+Shift+D";` on a Mac
  keyboard without an End key). The active bindings are printed at startup
  (`%SDL-I-HOTKEYS`) and shown in the window title. Defaults: Ctrl+F10
  mouse capture, Ctrl+F11 / Ctrl+Shift+F11 media, Ctrl+Alt+End sends
  Ctrl+Alt+Delete, Ctrl+Alt+Home resets the window size.
- **CD and floppy images**: a cdrom `file` ending in `.cue` is read as a
  BIN/CUE image (multi-file, MODE1/MODE2/audio tracks); anything else is a
  flat ISO. CD drives are read-only unless `read_only = false`, and a CD or
  floppy drive with no `file` (or an unreadable one) starts empty.
  **Ctrl+F11** opens a file picker and inserts the chosen image into the
  first CD drive: the image is opened and validated immediately (a bad file
  is reported and the current disc stays), then swapped in between guest
  commands, and the guest sees a normal "medium changed" notification. A
  drive locked by the guest (PREVENT MEDIUM REMOVAL) refuses the change;
  **Ctrl+Shift+F11** forces it. Guests may open and close the tray
  themselves unless `allow_guest_eject = false`.

### Headless testing and input-injection hooks

For automated or headless use (CI, scripted firmware navigation, driving the
emulator over SSH), the SDL GUI honors debug environment variables, read at
startup; unset means disabled. With `SDL_VIDEO_DRIVER=offscreen` (Linux) or
`dummy` (macOS) the whole GUI stack runs without a window or display server.

| Variable | Effect |
|---|---|
| `AXPBOX_DUMP_FB=<prefix>` | Write the emulated screen as a PPM image (`<prefix>-NNN-WxH.ppm`) every ~2 seconds. This is how you "see" the VGA output on a headless run. |
| `AXPBOX_KEYSCRIPT="<sec>:<key>,..."` | Press named keys at fixed second offsets from GUI start, e.g. `AXPBOX_KEYSCRIPT="40:a,41:r,42:c,43:enter"` types `arc` + Enter at the SRM prompt 40 s in. |
| `AXPBOX_KEYPIPE=<file>` | Interactive variant: keys appended to `<file>` while the emulator runs are typed into the guest (one token per ~120 ms). Example: `echo "f2 down down enter" >> keys.txt`. Start with an empty file; the emulator remembers how far it has read. |
| `AXPBOX_AUTOKEY_ENTER=<sec>` | Press Enter every `<sec>` seconds (blunt tool for firmware "press any key" prompts). |
| `AXPBOX_AUTOMOUSE=<sec>` | Starting `<sec>` seconds in, inject synthetic PS/2 mouse motion (a square pattern plus a periodic left click) directly into the guest, bypassing host input. Nothing is sent until the guest driver enables data reporting, as with a real PS/2 mouse. |
| `AXPBOX_MOUSE_DEBUG=1` | Trace host mouse motion, grab/focus transitions, relative-mode failures, the guest's aux commands and any dropped mouse bytes (`MOUSEDBG` lines). |
| `AXPBOX_PC_SAMPLE=1` | Print each CPU's program counter every state poll (~100 ms) — finds where a guest is stuck. |
| `AXPBOX_IRQSTATS=1` | Every 5 s, print interrupt rates: CPU interrupt entries by source, Cchip interval-timer ticks, 8259 edges/acknowledges per ISA IRQ, Cchip DRIR rises. Spots interrupt storms. |
| `AXPBOX_IRQTRACE=<n>` | Log interrupt entries `n`..`n+39` with the IER/SIRR/CM writes and ISUM reads between them. |
| `AXPBOX_IDETRACE=1` | Timestamped IDE timeline: commands, ATAPI packet opcodes, bus-master starts and interrupts, and every ATAPI check condition with its sense key. |
| `AXPBOX_MEDIA_SWAP=<image1>:<image2>:<ms>` | Media-change stress test: alternate two images in the first CD drive every `<ms>` ms, forced past a guest lock, applied between guest commands. |
| `AXPBOX_USBTRACE=1` | Log each OHCI register write with the per-register read counts since the previous write. |
| `AXPBOX_JIT_COMPILE_AFTER=<n>` | JIT builds: interpret a block `n` times before compiling it (default 1). |
| `AXPBOX_JIT_NO_DLINK=1` | AArch64 JIT: use the tag-checked link scan for static block exits instead of epoch-keyed data links (A/B switch for chaining issues). |
| `AXPBOX_JIT_FPTEST=1` | JIT_VERIFY builds: self-test the inline IEEE FP ops against the interpreter at startup, then exit with the verdict. |
| `AXPBOX_NO_IDLE=1` | JIT builds: disable idle pacing (see [Speed](#speed)). |
| `AXPBOX_IDLESTATS=1` | JIT builds: print idle-pacing counters every 2000 idle-loop visits. |

Key names for `AXPBOX_KEYSCRIPT`/`AXPBOX_KEYPIPE`: `a`–`z`, `0`–`9`,
`enter`, `esc`, `tab`, `space`, `up`, `down`, `left`, `right`, `del`, `ins`,
`home`, `end`, `bksp`, `bslash`, `dot`, `minus`, `equals`, `comma`, `slash`,
`semicolon`, `quote`, `lbracket`, `rbracket`, `grave`, `f1`–`f12`, `pgup`,
`pgdn`, `win`, `menu`, `ctrl`, `shift`, `alt`. Prefix a key with modifiers
joined by `-` for a chord: `win-r`, `shift-5` (`%` on a US layout),
`ctrl-alt-del`. `test/tools/keys_for.py "<text>"` turns a line of text into
tokens for a US keyboard layout.

A fully headless firmware run:

```
SDL_VIDEO_DRIVER=offscreen AXPBOX_DUMP_FB=fb \
AXPBOX_KEYPIPE=keys.txt AXPBOX_KEYSCRIPT="40:a,41:r,42:c,43:enter" \
axpbox run &
# watch fb-*.ppm to see the screen, echo keys >> keys.txt to react
```

Note: a `serial` section with a `port` waits for a telnet connection before
the GUI comes up — connect a client or use `null_attach = true` for
unattended runs.

### Test tools

`test/tools/` holds the scripts used to verify changes; they find the repo
from their own location and write to `$AXPBOX_WORK` (default `lab/`).

| Tool | Purpose |
|---|---|
| `srm_run.sh` | SRM firmware boot to `P00>>>` for one build, with a console-log diff against the reference and the JIT_VERIFY mismatch count |
| `srm_probe.sh` | SRM probes: CPU count, `memory.bits`, SCSI/IDE/floppy drives, console commands, SIGTERM / disconnect / halt exit paths |
| `win_bench.sh` | Headless guest boot on a throwaway clone: final screenshot, host CPU, per-CPU MIPS |
| `win_workload.sh` | Times a CPU-bound command inside a booted Windows guest (real-workload benchmark) |
| `build_lanes.sh`, `build_revs.sh` | Build every configured build directory, or every commit of a series in a worktree |

### Mouse on WSLg / Wayland

On WSLg the default Wayland backend delivers **no relative mouse motion**
while the grab is active — the guest pointer never moves even though the
grab succeeds. Run through XWayland instead:

```
SDL_VIDEO_DRIVER=x11 DISPLAY=:0 SDL_RENDER_DRIVER=software axpbox run
```

(`SDL_RENDER_DRIVER=software` avoids a fatal GLX error under WSLg's
XWayland.) Diagnose with `AXPBOX_MOUSE_DEBUG=1`: motion lines with `grab=1`
mean host input reaches the guest; none after a `grab -> 1` line means the
host backend isn't delivering relative motion.

## What doesn't work

- Some guest operating systems — see
  [Guest support](https://github.com/lenticularis39/axpbox/wiki/Guest-support).
- **More than two CPUs in a guest.** SRM itself boots and re-initializes
  with four CPUs, but the guests tested here cannot use them: Windows 2000
  Professional is licensed for two, and the Windows 2000 Server beta for
  Alpha has a HAL that only sends IPIs to CPUs 0–1. Other guests
  (OpenVMS, Tru64) are untested with more than one CPU.
- Big-endian hosts.
- Some SCSI and IDE commands.
- Copying large files from an IDE CD-ROM to an IDE hard disk (rarely
  affects OpenVMS installation).

## History and credits

AXPbox descends from the **es40** emulator by Camiel Vanderhoeven
(2007–2008), by way of
[lenticularis39/axpbox](https://github.com/lenticularis39/axpbox), which
renamed the project, moved it to CMake, merged the two binaries into one,
replaced the POCO-era threading with C++11 equivalents and fixed many
crashes.

This fork continues from there as its own project. The
[ES40-Emu/es40](https://github.com/ES40-Emu/es40) revival is treated as a
source of candidate changes rather than a source of truth: each one is
reviewed on its merits and adopted, adapted, improved or rejected — the goal
is the best code, not parity. Work done here includes:

- an **AArch64 JIT** emitter beside the x86-64 one, with block chaining,
  register pinning and a differential verify mode;
- **idle pacing**, so an idle guest costs a few percent of a host core;
- **SMP fixes** — Cchip interrupt-mask decode, IPI delivery, multiprocessor
  start — that let SRM run with four CPUs and Windows 2000 with two;
- **memory up to 32 GB**, presented as up to four memory arrays with DIMM
  and SPD data the console accepts;
- **removable media**: images validated before insertion and swapped at safe
  points, a tray model with guest lock handling, and a file picker;
- **configurable hotkeys**, `--version`, graceful SIGTERM and
  `exit_on_pal_halt`, NVRAM written as it changes;
- correctness work on LL/SC (DMA interaction and an ABA guard), IEEE
  floating point on the x86-64 JIT, the 8237 DMA and 82077 floppy
  controllers, the 8254 timer, the keyboard controller and the S3;
- the headless test tooling in `test/tools/`.

Guest-visible identifiers (disk serials, the `es40.cfg` file name, the
emulated machine type) deliberately keep their ES40 names.

## License

GNU General Public License, version 2 or later — see [LICENSE](LICENSE) and
the headers of the source files.
