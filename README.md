# AXPbox Alpha emulator

AXPbox is a fork of the discontinued es40 emulator. It could theoretically used for running any operating system that runs on the OpenVMS or Tru64 PALcode (e.g. OpenVMS, Windows 2000, Tru64 UNIX, Linux, NetBSD),).

The emulator supports SCSI, sound, IDE, serial ports, Ethernet (using pcap, or TUN/TAP on Linux) and [VGA graphics](https://github.com/lenticularis39/axpbox/wiki/VGA) (using SDL).

**Note: active development happens over at the ES40-Emu project**. Please go there for the latest improvements and speedups. We try to backport features from [ES40-Emu/es40](https://github.com/ES40-Emu/es40) using LLM's. You should check out that fork if you do not want to use AI code.

![OpenVMS 8.4 desktop](screenshots/openvms.png)


OpenVMS 8.4 desktop in AXPbox. [Here is a wiki page showing you how to get this CDE desktop running](https://github.com/lenticularis39/axpbox/wiki/GUI-Desktop-Environment-(CDE))

![Windows 2000](screenshots/win2000.png)

Windows 2000 build 2128 running on AXPbox. [Full guide to install Windows 2000 here](https://web.archive.org/web/20260705122517/https://www.zx.net.nz/computers/dec/axpemu-es40.shtml)


## Getting AXPbox

Pre-built binaries for generic Linux amd64, Windows 11 amd64 and macOS amd64 are available for each release, and also as artifacts produced for each commit in CI. T2 SDE has an [official package](http://t2sde.org/packages/axpbox) for AXPbox, and openSUSE's Emulators project has an [AXPbox package](https://build.opensuse.org/package/show/Emulators/axpbox), too. The former gets updated the same day when a release happens, while requests are submitted now the latter that undergo approval of Emulators maintainers.

You can also build from source using CMake, see the next section.

## Building from source

You need CMake (3.24+) and a C++17 compiler. Optional dependencies are
pcap for networking and SDL3 or X11 for graphics. SDL3 is bundled as a
git submodule (`third_party/SDL`) and is built in statically whenever no
system SDL3 is found, so clone with submodules:

```
git clone --recurse-submodules https://github.com/lenticularis39/axpbox
cd axpbox
# or, for an existing clone:
git submodule update --init
```

### Linux

```
sudo apt install build-essential cmake libpcap-dev
# GUI (only needed when building the bundled SDL3; skip for headless):
sudo apt install libx11-dev libxext-dev libxrandr-dev libxcursor-dev \
                 libxi-dev libxtst-dev libxfixes-dev libxss-dev libxkbcommon-dev libwayland-dev libegl-dev
# a distro-packaged SDL3 (libsdl3-dev) is used instead when available

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

The binary is `build/axpbox`. For a headless build (no GUI) add
`-DDISABLE_SDL=yes -DDISABLE_X11=yes` to the configure step; to build
without networking add `-DDISABLE_PCAP=yes`.

### Windows

Requirements:

- Visual Studio 2022 with the "Desktop development with C++" workload
  (MSVC x64), plus CMake
- the [npcap SDK](https://npcap.com/dist/npcap-sdk-1.13.zip) for
  networking support at build time — unzip it e.g. to `C:\pcap`
- [Npcap](https://npcap.com/#download) installed for networking at *run*
  time (any install mode works; `wpcap.dll` is located at run time, the
  exe also starts fine without Npcap, just without networking)

SDL3 needs no separate install — the submodule is compiled in
statically. From a developer command prompt (or any shell with CMake on
the PATH):

```
git clone --recurse-submodules https://github.com/lenticularis39/axpbox
cd axpbox
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 ^
      -DPCAP_INCLUDE_DIR=C:/pcap/Include ^
      -DPCAP_LIBRARY=C:/pcap/Lib/x64/wpcap.lib
cmake --build build --config Release
```

The binary is `build\Release\axpbox.exe`, self-contained (SDL3 is linked
statically and pcap is loaded dynamically).

### JIT

See the JIT section below for the optional asmjit-based JIT lane (x86-64 and AArch64 hosts).

## Usage

First invoke the interactive configuration file generator:
```
axpbox configure
```
This creates a file named es40.cfg, which you can now modify (the generator UI doesn't allow to set all options). The sample [es40.cfg](es40.cfg) in the repository root documents every available configuration value. After the configuration file and the required ROM image are ready, you can start the emulation:
```
axpbox run
```

`axpbox --version` prints the version, the commit and the optional features compiled in (JIT, SDL3, PCap, TAP). Ctrl-C or SIGTERM ends a run gracefully, saving the flash and DPR (NVRAM) images; with `exit_on_pal_halt = true` in the `sys0` section, a guest halt (an OS shutting down to the console) ends it the same way, which is handy for scripted runs.

Please read the [Installation Guide](https://github.com/lenticularis39/axpbox/wiki/OpenVMS-installation-guide) for information to get OpenVMS installed in the emulator. A guide for NetBSD is [also available on the Wiki](https://github.com/lenticularis39/axpbox/wiki/NetBSD-9.2-install-guide)



### JIT

An optional JIT (ported from [ES40-Emu/es40](https://github.com/ES40-Emu/es40), based on asmjit) can be enabled at build time on x86-64 and AArch64 (Apple Silicon, arm64 Linux) hosts:

```
git clone https://github.com/lenticularis39/axpbox
cd axpbox # repo folder
git clone https://github.com/asmjit/asmjit third_party/asmjit
git -C third_party/asmjit checkout 0bd5787b54b575ed94bf32ac452153b34385c514
cmake -B build -DCMAKE_BUILD_TYPE=Release -DES40_DISABLE_ASMJIT=OFF
cmake --build build
```

### S3 Trio64 graphics / ARC / Windows NT

The S3 Trio64 emulation (ported from [ES40-Emu/es40](https://github.com/ES40-Emu/es40) uses the real S3 VGA BIOS; obtain `86c764x1.bin` from the [86Box ROM set](https://github.com/86Box/roms/tree/master/video/s3) and point the s3 config section at it (see the sample es40.cfg). This is required for the ARC/AlphaBIOS console and Windows NT-family guests: flash AlphaBIOS from the Alpha Systems Firmware v7.3 CD, then enter `arc` at the SRM prompt.


### Serial consoles, networking, and sound

- **Serial ports**: the sample config attaches both UARTs as `null_attach`
  (present but unconnected), so the emulator starts without waiting for
  anything and the console lives on the VGA window (`vga_console = true`).
  To use a telnet console instead, set `port = 21264;` in a serial section —
  note the emulator then **waits at startup** until a client connects
  (`nc localhost 21264`). Ports left out of the config entirely are
  synthesized as `null_attach` automatically.
- **Networking**: the DEC 21143 NIC (`pci0.4 = dec21143`) connects to the
  host through one of two backends, selected with `type` in the nic
  config section:
  - `type = "pcap"` (default): captures on an existing host interface.
    Set `adapter = "eth0";` (Linux) or the `\Device\NPF_{...}` name
    (Windows/Npcap). On Linux, grant the binary capture permission once:
    ```
    sudo setcap cap_net_raw,cap_net_admin+eip ./axpbox
    ```
    otherwise startup fails with "Error opening adapter". On Windows,
    install [Npcap](https://npcap.com/#download); `wpcap.dll` is located
    automatically at run time. Don't leave `adapter` unset on unattended
    runs — the emulator interactively asks which adapter to use.
  - `type = "tap"` (Linux only): uses a TUN/TAP device instead of
    capturing — the guest becomes reachable from the host (and can be
    bridged onto the LAN). Options: `adapter = "tap0";` (device name,
    created if needed — requires CAP_NET_ADMIN or root),
    `host_ip = "10.0.0.1/24";` (optional host-side IP for the tap),
    `bridge = "br0";` (optional: create a bridge and add the tap to it),
    `uplink = "eno1";` (optional: also add this physical NIC to the
    bridge), `tap_create = true;`.

  Optional values for both backends: `mac` (default
  `08-00-2B-E5-40-<nic#>`), `queue` (rx queue depth, default 1024),
  `crc`, `trace_packets`. The sample [es40.cfg](es40.cfg) documents
  every option.
- **Sound**: `pci1.1 = es1370 {}` adds an Ensoniq AudioPCI ES1370 (SDL
  builds). Guest drivers exist for Windows NT 4; other guests ignore it.
- **Mouse**: click the window to grab, Ctrl+F10 to release; `mouse.speed`,
  `mouse.invert_x`, `mouse.invert_y` in the `sdl` section tune it (see the
  WSLg note below). `video.scale_ratio` / `video.scale_change_enable`
  control window scaling (Ctrl+PageUp / Ctrl+PageDown at runtime).
- **Hotkeys**: every GUI shortcut can be rebound with `hotkey.*` in the
  `sdl` section (e.g. `hotkey.ctrl_alt_delete = "GUI+Shift+D";` on a Mac
  keyboard without an End key); the active bindings are printed at startup
  (`%SDL-I-HOTKEYS`) and shown in the window title. Defaults: Ctrl+F10
  mouse, Ctrl+F11 / Ctrl+Shift+F11 media, Ctrl+Alt+End sends
  Ctrl+Alt+Delete, Ctrl+Alt+Home resets the window size.
- **CD images**: a cdrom `file` ending in `.cue` is parsed as a BIN/CUE
  image (multi-file, MODE1/MODE2/audio tracks); anything else is treated
  as a flat ISO. CD drives are read-only unless `read_only = false`, and
  a CD or floppy drive with no `file` (or a file that cannot be opened)
  starts empty. **Ctrl+F11** opens a file picker and inserts the chosen
  image into the first CD drive: the image is opened and checked right
  away (a bad file is reported on the console and the current disc stays),
  then swapped in between guest commands, and the guest sees a normal
  "medium changed" notification. If the guest has locked the drive
  (PREVENT MEDIUM REMOVAL) the change is refused; **Ctrl+Shift+F11** forces
  it. Guests can open and close the tray themselves unless
  `allow_guest_eject = false`.

### Headless testing and input-injection hooks

For automated or headless testing (CI, AI, scripted firmware navigation, driving
the emulator over SSH), the SDL GUI honors a set of debug environment
variables. They are read at startup; unset means disabled. Combined with
`SDL_VIDEO_DRIVER=offscreen` the whole GUI stack runs without any visible
window or display server.

| Variable | Effect |
|---|---|
| `AXPBOX_DUMP_FB=<prefix>` | Write the emulated screen as a PPM image (`<prefix>-NNN-WxH.ppm`) every ~2 seconds. This is how you "see" the VGA output on a headless run. |
| `AXPBOX_KEYSCRIPT="<sec>:<key>,..."` | Press named keys at fixed second offsets from GUI start, e.g. `AXPBOX_KEYSCRIPT="40:a,41:r,42:c,43:enter"` types `arc` + Enter at the SRM prompt 40 s in. Good for boot flows whose timing you already know. |
| `AXPBOX_KEYPIPE=<file>` | Interactive variant: keys appended to `<file>` while the emulator runs are typed into the guest (one token per ~120 ms). Example: `echo "f2 down down enter" >> keys.txt` navigates an AlphaBIOS menu. Start with an empty file; the emulator remembers how far it has read. |
| `AXPBOX_AUTOKEY_ENTER=<sec>` | Press Enter every `<sec>` seconds (blunt tool for firmware "press any key" prompts). |
| `AXPBOX_AUTOMOUSE=<sec>` | Starting `<sec>` seconds in, inject synthetic PS/2 mouse motion (a repeating square pattern plus a periodic left click) directly into the guest, bypassing host input entirely. Like a real PS/2 mouse, nothing is sent until the guest driver enables data reporting, so no motion before the OS mouse driver loads is expected. If the guest cursor moves with this but not with your real mouse, the problem is host-side SDL event delivery, not the emulated hardware. |
| `AXPBOX_MOUSE_DEBUG=1` | Trace every host mouse-motion event, grab/focus transition, and relative-mouse-mode failure, plus the guest's mouse (aux) commands and any mouse bytes dropped by the emulated controller, to stderr (`MOUSEDBG ...` lines). |
| `AXPBOX_PC_SAMPLE=1` | Print the guest program counter on every CPU state poll (~100 ms) — identifies where a guest is stuck on headless runs. |
| `AXPBOX_IRQSTATS=1` | Every 5 s, print interrupt rates: CPU interrupt entries by source (external lines, software interrupts, ASTs), Cchip interval-timer ticks, 8259 edges/acknowledges per ISA IRQ and Cchip DRIR rises. Spots interrupt storms. |
| `AXPBOX_IRQTRACE=<n>` | Log interrupt entries `n`..`n+39` together with the IER/SIRR/CM writes and ISUM reads between them (`IRQT` lines). |
| `AXPBOX_IDETRACE=1` | Timestamped IDE timeline: each command (opcode, drive, LBA/count), ATAPI packet opcode, bus-master start and interrupt (`IDET` lines). Separates guest-paced from emulator-paced I/O. |
| `AXPBOX_MEDIA_SWAP=<image1>:<image2>:<ms>` | Media-change stress test (all builds, including headless): every `<ms>` milliseconds insert `<image1>` / `<image2>` alternately into the first CD drive, forced past a guest lock. Each swap is applied only between guest commands; failures and every 50th swap are logged. |
| `AXPBOX_USBTRACE=1` | Log each OHCI register write with the per-register read counts since the previous write (`USBT` lines). |
| `AXPBOX_JIT_COMPILE_AFTER=<n>` | JIT builds: interpret a block `n` times before compiling it (default 1). |
| `AXPBOX_JIT_NO_DLINK=1` | AArch64 JIT: use the tag-checked link scan for static block exits instead of epoch-keyed data links (A/B switch for chaining issues). |
| `AXPBOX_NO_IDLE=1` | JIT builds: disable idle pacing. By default a CPU spinning in the Windows NT idle loop, or in the Windows 2000 HAL's loop for a processor it has not started (both recognized by their instructions), sleeps until an interrupt is raised for it or 1 ms passes, so an idle guest uses a few percent of a host core instead of 100% per emulated CPU. |
| `AXPBOX_IDLESTATS=1` | JIT builds: print idle-pacing counters (`%CPU-I-IDLESTATS`: idle-loop head visits, sleeps, time slept, blocked sleeps) every 2000 visits. |

Key names for `AXPBOX_KEYSCRIPT`/`AXPBOX_KEYPIPE`: `a`–`z`, `0`–`9`,
`enter`, `esc`, `tab`, `space`, `up`, `down`, `left`, `right`, `del`, `ins`,
`home`, `end`, `bksp`, `bslash`, `dot`, `minus`, `equals`, `f1`–`f12`,
`pgup`, `pgdn`.

A typical fully headless firmware run:

```
SDL_VIDEO_DRIVER=offscreen AXPBOX_DUMP_FB=fb \
AXPBOX_KEYPIPE=keys.txt AXPBOX_KEYSCRIPT="40:a,41:r,42:c,43:enter" \
axpbox run &
# ... watch fb-*.ppm to see the screen, echo keys >> keys.txt to react
```

On macOS the `offscreen` driver is unavailable (it needs EGL); use
`SDL_VIDEO_DRIVER=dummy` for headless runs there.

Note: a `serial` section configured with a `port` waits for a telnet
connection at startup before the GUI comes up — connect a client (e.g.
`nc localhost <port>`) or configure `null_attach = true` for unattended runs.

### Mouse on WSLg / Wayland

Click inside the emulator window to grab the mouse; Ctrl+F10 releases it.
On WSLg (Windows Subsystem for Linux GUI) the default Wayland backend
delivers **no relative mouse motion** while the grab is active — the guest
pointer never moves even though the grab succeeds. Run the emulator through
XWayland instead:

```
SDL_VIDEO_DRIVER=x11 DISPLAY=:0 SDL_RENDER_DRIVER=software axpbox run
```

(`SDL_RENDER_DRIVER=software` avoids a fatal GLX error under WSLg's
XWayland.) Diagnose mouse problems with `AXPBOX_MOUSE_DEBUG=1`: motion
lines with `grab=1` mean host input reaches the guest; none after a
`grab -> 1` line means the host backend isn't delivering relative motion.

## Changes in comparison with the original es40

- Renamed from es40 to AXPbox to avoid confusion with the physical machine (AlphaServer ES40)
- CMake is used for compilation instead of autotools
- OpenVMS host support was dropped
- es40 and es40_cfg were merged into one executable (axpbox)
- The code was cleaned to compile without warnings on most compilers
- Code modernizing, replacing POCO framework parts by native C++11 counterparts not available in 2008 (std::threads, etc)
- Incorporate various patches from other es40 forks, for example, added MC146818 periodic interrupt to allow netbsd to boot and install, skip_memtest for faster booting.
- The [ES40-Emu/es40](https://github.com/ES40-Emu/es40) revival work is ported (at its 2026 state): MAME-based S3 Trio64/IBM 8514A graphics, ARC/AlphaBIOS + Windows NT/2000 device support, wall-clock guest timing, interpreter correctness fixes, firmware flash updates with system reset, and the optional asmjit x86-64 JIT.
- Bug fixes, less segfaults, overall less crashes. 
- [More](https://github.com/lenticularis39/axpbox/wiki/) documentation and usage information on the various features and operating systems

## What doesn't work (also see issues)

- Some guest operating systems (see [Guest support](https://github.com/lenticularis39/axpbox/wiki/Guest-support))
- Multiple CPU system emulation is experimental: SRM boots to `P00>>>` with up to four CPUs (`cpu1`..`cpu3 = ev68cb` in `es40.cfg`), and Windows 2000 RC2 (multiprocessor HAL) runs on two CPUs on the AArch64 JIT build; other guests are untested with more than one CPU
- Running on big endian platforms
- Some SCSI and IDE commands
- Copying large files between IDE CD-ROM to IDE hard drive (this usually doesn't affect OpenVMS installation)
