# Source layout and architecture

The detailed map of `src/`: what each directory holds, the major devices,
and how the CPU, the JIT, configuration and threading fit together.
`CLAUDE.md` carries the short version and points here.

## The directories

| Directory | What it holds |
| --- | --- |
| [`cpu/`](#cpu) | the processor: interpreter, PALcode fast paths, floating point, the processor rows |
| [`jit/`](#jit) | the translator from Alpha code to host code |
| [`debug/`](#debug) | the gdb stub: run control, breakpoints, guest state for a debugger |
| [`hv/`](#hv) | the experimental Hypervisor.framework runtime (macOS) |
| [`system/`](#system) | generic machine plumbing: memory, devices, CPU threads, configuration |
| [`chipsets/`](#chipsets) | the system logic: Tsunami, Titan, Marvel |
| [`platforms/`](#platforms) | one board row per machine, and what only that board has |
| [`devices/`](#devices) | devices grouped by bus: `common/`, `isa/`, `pci/`, `storage/`, `video/`, `usb/`, `net/` |
| [`gui/`](#gui-common-and-the-entry-points) | the `bx_gui` back ends; SDL3 is the maintained one |
| [`common/`, `emu/`](#gui-common-and-the-entry-points) | shared headers, and the MAME shim |
| [`src/` itself](#gui-common-and-the-entry-points) | the entry points |

Headers are included unqualified (`#include "System.hpp"`). Every source
directory is on the include path, so moving a file needs no include
changes. A new directory must be added to the CMake glob list and to
`target_include_directories`, or its files are silently left out.

## cpu/

- `AlphaCPU.cpp`: `CAlphaCPU`, a per-instruction interpreter (`execute()`).
  The opcodes are in the `cpu_*.hpp` headers it includes.
- `AlphaCPU_jit.cpp`: `jit_run()`, the dispatch loop between the
  interpreter and compiled blocks.
- `AlphaCPU_vmspal.cpp`: native fast paths for OpenVMS PALcode entry points.
- `AlphaCPU_ieeefloat.cpp`, `AlphaCPU_vaxfloat.cpp`: IEEE and VAX floating
  point.
- `CpuModel.hpp`, `CpuModels.cpp`: the processor's identity and core
  family, one row per part (EV68CB, EV7, EV7z).
- `diag_rpcc.hpp`: cycle counter diagnostics.
- `ev7/`: what the EV7 family changes. `Ev7.hpp` is its 44-bit address map
  with the PID encoding; `Ev7Reset.cpp` sets the registers the XSROM hands
  the console.

## jit/

- `jitengine.cpp`, `jitengine.hpp`: the block cache, chaining, register
  pinning and the x86-64 emitter. Ported from ES40-Emu.
- `jitemit_a64.hpp`: the AArch64 emitter, written here.

## debug/

The guest debugger ([debugging.md](debugging.md)):

- `GdbStub`: `CGdbStub`, the gdb remote serial protocol server on its own
  thread (`gdb` and `gdb.wait` in `sys0`, or `ALPHABOX_GDB`).
- `DebugTarget`: `CDebugTarget`, all-stop run control. Each processor parks
  itself at a dispatch boundary (`CAlphaCPU::jit_run`, or `run_loop` in the
  interpreter-only build), where it also learns how far it may run before a
  breakpoint. Registers, side-effect-free address translation and memory.
- `DebugMonitor.cpp`: the `monitor` commands.

## hv/

`HvRuntime` runs the emulator's own code at EL1 inside a virtual machine
(`alphabox hvtest`); `HvProbe` measures the host's costs (`alphabox
hvprobe`). Built with `-DALPHABOX_HVF=ON`. See [hypervisor.md](hypervisor.md).

## system/

- `System`: `CSystem`, which owns memory, the device ranges, the CPU
  threads, LL/SC, console loading and the state files.
- `SystemComponent`: the base class of every device.
- `InterruptLines.hpp`: the chipset's device interrupt inputs as wires,
  each the OR of every source that drives it (`CSystem::interrupt`).
- `Configurator`: parses `es40.cfg` and builds the device graph.
- `Port80`.

## chipsets/

`Chipset.hpp` declares `CChipset`, the system logic every board has:
non-memory decode, interrupts, the interval timer, the PCI hose spaces,
DMA translation, reset and state. `Chipsets.cpp` builds the right one.
What the Tsunami and the Titan share is beside it: `PciWindows.hpp` (the
DMA windows) and `DimmModel` (the DIMMs, reached as
`CChipset::dimms()`).

### tsunami/

`CTsunami`, the 21272 of the ES40, DS20E, DS20, DS10 and DS20L, split by chip:

- `Tsunami.cpp`: decode and interrupts.
- `TsunamiCchip`, `TsunamiPchip`, `TsunamiTig`.
- `TsunamiMemory`: the AARn arrays, and the SPD on the MPD pins.

Trace: `ALPHABOX_TRACE_CSR=1`, every chipset register access with its value.

### titan/

`CTitan`, the 21274 of the ES45, DS25 and DS15:

- `Titan.cpp`: decode and interrupts.
- `TitanCchip`, `TitanTig`.
- `TitanPachip`: two PA-chips with a G-port and an A-port each, so four
  hoses.

### marvel/

`CMarvel`, the EV7 machines, with memory per processor (PID):

- `Ev7Csr`: `CEv7Csr`, each processor's 4 MB on-chip register block. It
  holds the Rbox interrupts, the timer and the scratch start protocol.
- `Gio`: the GIO port to the management processor. Its far side is
  pluggable; on the ES47 it is the CMM model in `platforms/es47/Cmm.cpp`.
- `Io7`: the IO7 I/O bridge with its four PCI hoses.
- `Topology`: the processors' torus and its routes.

Traces: `ALPHABOX_TRACE_CSR=1`, `ALPHABOX_GIO_LOG=<file>`, and
`ALPHABOX_TRACE_RBOX=<ms>` for interrupts an EV7 leaves pending.

## platforms/

`Platform.hpp` and the board rows in `Platforms.cpp` say which machine is
emulated: its chipset, processors, memory, slots, interrupt wiring,
firmware form, how secondary processors start, and the board's own
hardware. A configuration picks one with `platform = "<name>";`.

Each board has a directory for what only it has (`Boards.hpp` declares
them):

| Directory | What only that board has |
| --- | --- |
| `es40/` | `DPR`, `Flash`, the interrupt map, the console speed patches |
| `ds20e/` | its interrupt map and slots, and its I2C parts; the DS20 shares all of it but one part |
| `ds10/` | `PCF8584`, the I2C controller the DS20E shares |
| `ds20l/` | its interrupt map and slots |
| `es45/` | the console's interrupt table; `Es45Dpr`, the ES40's DPR with the ES45's backplane FRU and RMC; a two-part `CFlash`; the Model 1 backplane with its AGP slot (`es45m1`) |
| `ds25/` | `Ds25Dpr`; `Ds25Aic7899`, a stand-in for the on-board SCSI; the on-board 82559ER |
| `ds15/` | `Ds15Dpr` (its RMC mailbox), `Ds15Tig` |
| `es47/` | the CMM model `Cmm.cpp` on GIO: one memory per module, one port per PID, the MBM answered per drawer, the console terminal on `serial0`, NVRAM in `rom.nvram`, the FRU EEPROMs, VRMs, power supplies and fans |
| `es80/`, `gs1280/` | their processor and I/O layouts on the same model (the board row's `marvel_layout`) |

OpenVMS 8.4 boots on every board, from the ES40 to the GS1280 with 16 EV7s.

## devices/

Devices are grouped by bus, not by chip. All four `AliM1543C*` components
are PCI functions (each calls `add_function()`, and the Configurator marks
every one `IS_PCI`), so they live in `devices/pci/` even though the M1543C
is the ISA bridge. `devices/isa/` holds only the devices behind it.

### devices/common/

Parts more than one family uses:

- `Eeprom93cx6`: the Microwire serial EEPROM the Intel NICs and the QLogic
  adapters keep their settings in.
- `i2c_spd`: an I2C bus and the 24C02 serial EEPROM, for the Tsunami's SPD
  parts and the boards' I2C ROMs.

### devices/isa/

The legacy devices behind the bridge: `DMA`, `FloppyController`,
`Keyboard` (KBC and PS/2 mouse, Bochs-derived), `Serial` (a COM port and
its host side: the telnet connection, the BREAK menu, `null_attach`) and
`Uart16550` (the 16550's registers, receive FIFO and interrupt, with no
host side; `test/unit/uart16550_test.cpp` tests it).

The ALi bridge's own ISA core is here too, as models over the bridge's
state with no `CSystem` behind them, each tested in `test/unit/`: `I8254`
(the timer, on a clock the owner passes in), `I8259` (the cascaded pair;
the master's INT output is a callback), `Mc146818` (the TOY clock and CMOS
RAM, on the host's wall clock; `ToySeed` turns the `timezone` and `time`
keys into its initial offset, for the Marvel CMM's clock too) and `SuperIo` (`CSuperIo`, the
configuration ports and logical devices, and `CIsaPnp`, the ISA Plug and
Play ports; the ranges they place go back to the bridge through a
callback).

### devices/pci/

- `PCIDevice`: `CPCIDevice`, configuration space and BARs.
- `AliM1543C` and its `_ide`, `_usb` and `_pmu` functions. `_usb` also
  holds `COhci`, the OHCI engine the EHCI card's companions share.
- `IdeController`: the ATA/ATAPI core. `AliM1543C_ide` and `Cmd649` (the
  CMD 649 PCI IDE, both channels native) are parts on it.
- `SCSIBus`, `SCSIDevice`.

One directory per family:

| Directory | Family |
| --- | --- |
| `sym53c8xx/` | the Symbios 53C810, 825, 875, 895, and the two-channel 896 as two PCI functions; split by concern, parts in `Sym53C8xxChips.cpp` |
| `isp1040/` | the QLogic ISP1020, 1040, 1080 and 1240 (two SCSI buses on one function): mailboxes and request/response queues rather than SCRIPTS |
| `i8255x/` | the Intel 8255x NICs (DE600, DE602, 82559ER), parts in `I8255xChips.cpp` |
| `tulip/` | `CTulip`, the DECchip 21040, 21041, 21140 and 21143 NICs; the parts differ in how they name themselves and pick a medium (`TulipMedia.cpp`) |
| `bridge/` | `PCIBridge`: PCI-PCI bridges and the multi-port boards built on them, parts in `PCIBridgeChips.cpp` |
| `ehci/` | `CEhci`, a NEC uPD720101 USB 2.0 card (details below) |
| `virtio/` | paravirtual devices on legacy virtio-pci (details below) |
| `es137x/` | `CES137x`, the Ensoniq AudioPCI sound cards sharing one DMA engine; the ES1371's AC'97 codec and sample rate converter are in `ES137xCodec.cpp`; the WAV tap and the guest-less self-test are in `ES137xDebug.cpp` |

The EHCI card has the EHCI and two OHCI companions (`COhci`, the ALi's
engine), with four ports routed between them by CONFIGFLAG and
PORT_OWNER, and high-speed isochronous iTDs. `companions = false` gives
the EHCI alone. The `uss344` class is the same card without the EHCI: the
ES47's Agere USS-344, four single-port OHCI functions.
`ALPHABOX_EHCI_SELFTEST=1` checks it without a guest driver.

The virtio devices are `CVirtioPci` (registers, split virtqueues, the
device thread, INTA), `CVirtioBlk` (`virtio_blk`, with a `disk0.0`) and
`CVirtioNet` (`virtio_net`, on the NIC back ends).
`ALPHABOX_VIRTIO_SELFTEST=1` checks them without a guest driver; the
driver writer's reference is [virtio.md](virtio.md).

A device declared inside a bridge's block as `pci.<dev>` sits on the
bridge's secondary bus: its configuration space is at the bus number the
firmware assigns, and its INTx lines are rotated onto the bridge's slot.

### devices/storage/

`Disk` (`CDisk`, including the SCSI and ATAPI command sets),
`DiskController`, and the disk kinds `DiskFile`, `DiskDevice` and
`DiskRam`. `DiskFileBinCue.hpp` reads BIN/CUE images; `DiskOverlay` is
the copy-on-write overlay a `file` disk can keep its writes in;
`DiskLatency` is the optional model of a drive's seek and transfer time.
`DiskCdbTest` is `alphabox cdbtest`, the conformance test of the SCSI
disks and the 53C8xx SCRIPTS processor.

### devices/video/

The shared parts:

- `VGA`: the MAME-derived VGA core, with `ibm8514a` and the MAME shims
  (`mame_shims.hpp`, `address_map.hpp`, `attotime.hpp` and the rest).
- `VGACard`: what every VGA card shares, the plumbing and the standard
  VGA registers.
- `MonitorEdid.hpp`: the monitor a card finds on its DDC lines.

One directory per card family:

| Directory | Cards |
| --- | --- |
| `s3/` | `S3Trio64` |
| `cirrus/` | `CirrusGD54xx` split by concern, the device-independent `CirrusBlitter`, the `CirrusGD5430` and `CirrusGD5434` |
| `mach64/` | the ATI Mach64 family through the 3D Rage Pro |
| `permedia2/` | the 3Dlabs Permedia 2: SVGA, RAMDAC, graphics processor, delta unit |
| `virge/` | `CS3Virge`, the S3 ViRGE family (config class `s3virge`, key `chip`) |
| `tga/` | `CTga`, the DEC TGA and TGA2 (config class `tga`, key `model`) |
| `radeon/` | the ATI Radeon family, one subdirectory per generation (details below) |

**The S3 ViRGE** has the S3 SVGA extensions, the MMIO window, the streams
processor, and the S3d engine's 2D and 3D commands, split by concern. The
ViRGE, VX, DX and GX2 are verified; the GX has not been run on its own.

**The TGA** is the DECchip 21030 on the ZLXp-E1 (`model = "e1"`), and the
TGA2 is on the PowerStorm 3D30 and 4D20 (`TgaRgb561` is the 4D20's
RAMDAC). It is not a VGA: it has its own render thread, core-space
decode, the graphics modes and the Bt485, and it leaves the window to a
VGA card beside it.

**The Radeon** directory root holds what the generations share:

- `CRadeon`, the config class `radeon` with a `chip` key (a row of
  `RadeonChips.cpp` naming the part's generation; `rv200`, the Radeon
  7500, is the only part so far) and a `model` key (`agp` or `pci`).
- The register file with the BIOS's PLL, memory controller and CRTC
  registers; the PLL-timed CRTC (`RadeonTiming.cpp`); the extended modes
  and the cursor.
- The overlay scaler (`RadeonOverlay.cpp`): the video window over the
  CRTC's picture, its register lock, YUV and RGB surfaces, scaling and
  filtering, and the colour keys.
- The 2D engine behind a 64-entry command FIFO drained by an engine
  thread (`RadeonQueue.cpp`; `ALPHABOX_RADEON_SYNC=1` gives the old
  synchronous engine).
- The command processor: ring buffer, indirect buffers, PIO queue and
  packets. It takes no packets until a driver loads microcode. OpenVMS's
  DECwindows drives the card through it.
- The card's PCI GART (`RadeonGart.cpp`).

A generation's 3D engine sits behind `CRadeonEngine3D` and reaches the
card only through `CRadeonEngineBus` (`RadeonEngine3D.hpp`); engines run
under the card's execution lock. `r100/` holds the R100 generation's
engine, `radeon::r100::CRadeonR100_3D`: vertex fetch and the 3D packets
(`RadeonR100_3D.cpp`), transform and lighting (`RadeonR100Tcl.cpp`), and
the rasteriser and pixel pipeline (`RadeonR100Raster.cpp`).

No guest drives the 3D engine except nada's driver, so
`ALPHABOX_RADEON_SELFTEST=1` checks it: common checks in
`RadeonSelfTest.cpp`, the generation's scenes in
`r100/RadeonR100SelfTest.cpp`. `ALPHABOX_TRACE_RADEON` traces the card.
Adding a generation follows the checklist in [radeon.md](radeon.md).

### devices/usb/

Devices on the emulated USB, behind the OHCI controllers and the EHCI
card. The user guide is [usb.md](usb.md).

| File | Device |
| --- | --- |
| `UsbDevice` | endpoint 0 and the chapter 9 requests; `CUsbPort`, a root hub port that owns its device |
| `UsbTablet` | an absolute HID pointer |
| `UsbKeyboard` | a HID boot keyboard (`port<n> = "keyboard"`): the GUI's keys through `gui_guest_key`, and the only keyboard of a machine without an 8042 |
| `UsbStorage` | Bulk-Only mass storage around a `CDisk`, declared as `disk<port>.0` |
| `UsbAudio` | a USB Audio Class 1.0 speaker (`port<n> = "audio"`): isochronous OUT, SDL output, `ALPHABOX_USBAUDIO_WAV` capture |
| `UsbHostDevice` | a host device passed through with libusb (`port<n> = "host:vvvv:pppp"`); optional, `HAVE_LIBUSB` |
| `UsbAsyncShim` | a test harness: an emulated device with libusb-like timing, isochronous included |

### devices/net/

`Ethernet`, `NicAddress` (the shared default station address), and
`NetworkBackend` with its back ends: `NetworkPcap`, `NetworkTap`,
`NetworkUdp`, `NetworkNat` (a user-mode NAT router on libslirp, optional,
`HAVE_SLIRP`; a worker thread and one mutex around every library call),
`NetworkLoopback` (a plug on the port) and `NetworkNull`.

## gui/, common/ and the entry points

- `gui/`: the `bx_gui` back ends. SDL3 (`sdl.cpp`) is the only one; `gui = win32`
  and `gui = X11` in a configuration name the SDL window.
- `common/`: `StdAfx`, `datatypes`, `es40_debug` (with `Exception`, what
  its `FAILURE()` throws), `es40_endian`, `config_debug`, `banner`,
  `telnet`, and `WakeSemaphore` (a device thread's wake-up).
- `emu/`: `emu.hpp`, the shim that stands in for MAME's header.
- `src/` itself: the entry points `Main.cpp`, `AlphaSim.cpp`,
  `es40-cfg.cpp` (with its `*Question.hpp`), and `config.hpp.in`.

## How it fits together

Everything hangs off `CSystem` (`system/System.cpp`). It owns physical
memory, the registered device ranges, the CPUs and the console loading.
The rest it forwards to the board's chipset (`cSystem->chipset()`):

- An address that is neither memory nor a device range goes to
  `read_io`/`write_io`: on the Tsunami, the Cchip, Dchip, Pchip and TIG
  registers and unclaimed PCI space.
- `cSystem->interrupt()` goes to the chipset's interrupt controller, and
  `PCI_Phys()` to its DMA windows. Each interrupt input is a wire that
  every device on it can assert (devices behind a bridge whose pins rotate
  onto one line, say): the chipset is told the OR of all of them.
- RAM accesses never reach the chipset.

The board row names the chipset and builds the board's own hardware.

Devices derive from `CSystemComponent` (`system/SystemComponent.cpp`).
They register memory ranges with the system and implement:

- `ReadMem` and `WriteMem`;
- optionally `init()`, `start_threads()`, `stop_threads()` and
  `check_state()`;
- `SaveState` and `RestoreState`.

PCI devices derive from `CPCIDevice`, whose `myPCIBus` is the hose. Disk
controllers derive from `CDiskController` and have `CDisk` children.

## The processor

`state` is the whole architectural state, and the save file's format.

Guest time follows the wall clock:

- `state.cc` (RPCC) advances by real elapsed time.
- CPU 0 fires the chipset's interval timer (`interval_tick()`, the
  Cchip's on the Tsunami) at dispatch-batch boundaries.
- The ALi's 8254 PIT and TOY clock are paced by the wall clock too.

The comments in `cpu/AlphaCPU.hpp` give the details.

Where the emulated processor knowingly differs from a real 21264 is
recorded in [cpu-fidelity.md](cpu-fidelity.md): unaligned accesses that do
not trap, machine checks that are never raised, the oversized
instruction cache. It also lists what was audited and found correct. Read
it before "fixing" CPU behaviour, and add to it when a divergence is found
or closed.

## The JIT

The JIT exists in `ES40_JIT` builds only. It translates Alpha basic
blocks to host code with asmjit:

- The x86-64 emitter is in `jitengine.cpp` (ported from ES40-Emu, where it
  is BSD-1-Clause).
- The AArch64 emitter is in `jitemit_a64.hpp`, with the same protocol for
  bailing out, chaining and frames. `ALPHABOX_JIT_FPTEST=1` on a
  `JIT_VERIFY` build checks its inline IEEE operations against the
  interpreter.

Blocks live in a direct-mapped cache keyed by physical PC, are chained
directly to each other, and keep guest registers pinned in host
registers. Anything hairy bails out to the interpreter. The trace tier
(`JIT_TRACES`) is deliberately dormant.

`jit_run()` in `cpu/AlphaCPU_jit.cpp` is the dispatch loop. Memory accesses
go through the `jit_read` and `jit_write` helpers, which mirror the
semantics of `cpu/cpu_memory.hpp`.

## Configuration

`CConfigurator` (`system/Configurator.cpp`) parses `es40.cfg` into a tree
and builds the device graph from it.

Each device class has an allow-list (`kv_*[]`) of the values it reads.
Unknown values warn at startup (`%SYS-W-UNKNOWNCFG`), so a new key goes
into the matching list. The sample `es40.cfg` at the repo root documents
every value.

## Threading

Each active device runs a `std::thread` (`myThread`, a lambda calling
`run()`), and `check_state()` checks its `std::atomic_bool myThreadDead`.

Use `std::mutex`, `std::thread`, `std::chrono` and
`std::condition_variable`; this is a hard project rule. The Poco-style
wrappers inherited from es40 (`src/base/`: CMutex, CSemaphore, CThread
and the rest) were removed in October 2026. Do not bring them back with
ported code.

A lock or wait never times out into a failure. A host that sleeps, or a
stopped process, must make the emulator late, not dead.
