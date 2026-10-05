# Source layout and architecture

The detailed map of `src/`: what each directory holds, the major devices,
and how the CPU, the JIT, configuration and threading fit together.
`CLAUDE.md` carries the short version and points here.

Source layout under `src/`:

| Directory | Contents |
| --- | --- |
| `cpu/` | `AlphaCPU*`, the `cpu_*.hpp` opcode headers, vmspal, IEEE/VAX FP, `CpuModel`/`CpuModels` (the processor's identity and core family, a row per part: EV68CB, EV7, EV7z); `ev7/` (what the EV7 family changes: its 44-bit address map with the PID encoding, `Ev7.hpp`, and the registers the XSROM hands the console, `Ev7Reset.cpp`) |
| `jit/` | asmjit translator: `jitengine.cpp` (x86-64), `jitemit_a64.hpp` |
| `system/` | generic machine plumbing: `System` (memory, device ranges, CPU threads, LL/SC, console loading, state files), `SystemComponent`, `Configurator`, `Port80`, `TraceEngine` |
| `chipsets/` | the system logic behind `Chipset.hpp` (`CChipset`: non-memory decode, interrupts, interval timer, PCI hose spaces, DMA translation, reset/state), built by `Chipsets.cpp`; `tsunami/` (`CTsunami`, the 21272 of the ES40/DS20E/DS10/DS20L, split by chip: `Tsunami.cpp` decode + interrupts, `TsunamiCchip`, `TsunamiPchip`, `TsunamiTig`, `TsunamiMemory` (AARn arrays, SPD on the MPD pins)); `titan/` (`CTitan`,
the 21274 of the ES45: `Titan.cpp` decode + interrupts, `TitanCchip`,
`TitanPachip` -- two PA-chips, a G-port and an A-port each, four hoses --,
`TitanTig`); `PciWindows.hpp` and `DimmModel.*`, what the Tsunami and the
Titan share (DMA windows; the DIMM model, reached as `CChipset::dimms()`);
`marvel/` (`CMarvel`, the EV7 machines: memory per PID, `CEv7Csr` -- each processor's 4 MB on-chip register block, Rbox interrupts/timer/scratch start protocol --, `Gio` -- the GIO port to the management processor, with a pluggable far side: on the ES47 the CMM model in `platforms/es47/Cmm.cpp` (byte window, SMLAN mailboxes, console terminal on `serial0`, NVRAM in `rom.nvram`); `ALPHABOX_TRACE_CSR=1`, `ALPHABOX_GIO_LOG=<file>`, `ALPHABOX_TRACE_RBOX=<ms>` -- interrupts an EV7 leaves pending) |
| `platforms/` | which machine is emulated: `Platform.hpp` + the board rows in `Platforms.cpp` (chipset, processors, memory, slots, interrupt wiring, firmware form, secondary start, board hardware), chosen with `platform = "<name>";`; one directory per board for what only it has (`Boards.hpp` declares them): `es40/` (`DPR`, `Flash`, interrupt map, the console speed patches), `ds20e/`, `ds10/` (`PCF8584`, the I2C controller the DS20E shares), `ds20l/`, `es45/` (Titan: the console's interrupt table, `Es45Dpr` -- the ES40's DPR with the ES45's backplane FRU and RMC --, a two-part `CFlash`), `es47/` (Marvel: the CMM model `Cmm.cpp` on GIO -- one memory per module, one port per PID, the MBM answered per drawer; the IO7 with its four PCI hoses is in `chipsets/marvel/Io7.cpp`, the torus routes in `chipsets/marvel/Topology.cpp`), `gs1280/` (the ES80/GS1280 layouts on the same model: the board row's `marvel_layout`); OpenVMS 8.4 boots on the ES47, ES80 (8 EV7s) and GS1280 (16) |
| `devices/common/` | device parts more than one family uses: `Eeprom93cx6` (the Microwire serial EEPROM the Intel NICs and the QLogic adapters keep their settings in), `i2c_spd` (an I2C bus and the 24C02 serial EEPROM: the Tsunami's SPD parts, the boards' I2C ROMs) |
| `devices/isa/` | the legacy devices behind the bridge: `DMA`, `FloppyController`, `Keyboard`, `Serial`, `MPU401` |
| `devices/pci/` | `PCIDevice`, `AliM1543C` + its `_ide`/`_usb`/`_pmu` functions (`_usb` also holds `COhci`, the OHCI engine the EHCI card's companions share), `IdeController` (the ATA/ATAPI core: `AliM1543C_ide` and `Cmd649`, the CMD 649 PCI-IDE, are parts on it), `SCSIBus`, `SCSIDevice`; `sym53c8xx/` (the Symbios 53C8xx family, split by concern, parts in `Sym53C8xxChips.cpp`); `isp1040/` (the QLogic ISP SCSI adapters: mailboxes and request/response queues rather than SCRIPTS); `i8255x/` (the Intel 8255x NIC family, same layout, parts in `I8255xChips.cpp`); `bridge/` (`PCIBridge`: PCI-PCI bridges and the multi-port boards built on them, parts in `PCIBridgeChips.cpp`); `tulip/` (`CTulip`: the DECchip 21040/21041/21140/21143 NICs, whose parts differ in how they name themselves and pick a medium -- `TulipMedia.cpp`); `ehci/` (`CEhci`: a USB 2.0 card, a NEC uPD720101 -- the EHCI and two OHCI companions (`COhci`, the ALi's engine), four ports routed between them by CONFIGFLAG/PORT_OWNER; high-speed isochronous iTDs; `companions = false` for the EHCI alone; the `uss344` class is the same card without the EHCI, the ES47's Agere USS-344: four single-port OHCI functions; `ALPHABOX_EHCI_SELFTEST=1` checks it without a guest driver); `virtio/` (paravirtual devices, legacy virtio-pci: `CVirtioPci` -- registers, split virtqueues, device thread, INTA -- and `CVirtioBlk` (`virtio_blk`, a `disk0.0`), `CVirtioNet` (`virtio_net`, on the NIC backends); `ALPHABOX_VIRTIO_SELFTEST=1` checks them without a guest driver; the driver writer's reference is `docs/virtio.md`); `es137x/` (`CES137x`: the Ensoniq AudioPCI sound cards, sharing one DMA engine; the ES1371's AC'97 codec and sample rate converter are in `ES137xCodec.cpp`) |
| `devices/storage/` | `Disk`, `DiskController`, `DiskDevice`, `DiskFile`, `DiskRam` |
| `devices/video/` | `VGA` (MAME-derived core), `VGACard` (shared card plumbing + standard VGA registers), `ibm8514a`, MAME-derived shims, the dead pre-MAME `Cirrus`; one subdirectory per card family: `s3/` (`S3Trio64`), `cirrus/` (`CirrusGD54xx` split by concern, the device-independent `CirrusBlitter`, `CirrusGD5430`/`CirrusGD5434`), `mach64/` (the ATI Mach64 family through the Rage Pro), `permedia2/` (the 3Dlabs Permedia 2: SVGA, RAMDAC, graphics processor, delta unit), `virge/` (`CS3Virge`, the S3 ViRGE family, `s3virge` config class with a `chip` key: S3 SVGA extensions, MMIO window, streams processor, the S3d engine's 2D and 3D commands split by concern; the ViRGE, VX, DX and GX2 are verified, the GX not run on its own), `tga/` (`CTga`, the DEC TGA / DECchip 21030 on the ZLXp-E1, and the TGA2 on the PowerStorm 3D30/4D20 (`TgaRgb561` for the 4D20's RAMDAC), `tga` config class with a `model` key (`e1`, `3d30`, `4d20`): not a VGA -- its own render thread, core-space decode, the graphics modes, the Bt485; it leaves the window to a VGA card beside it), `radeon/` (the ATI Radeon family, one directory per generation: the root is what the generations share -- `CRadeon`, `radeon` config class with a `chip` key (a row of `RadeonChips.cpp`, which names the part's generation; `rv200`, the Radeon 7500, is the only part) and a `model` key (`agp`/`pci`): a register file with the BIOS's PLL/memory-controller/CRTC registers, the PLL-timed CRTC (`RadeonTiming.cpp`), the extended modes and cursor, the overlay scaler (`RadeonOverlay.cpp`: the video window over the CRTC's picture -- its register lock, YUV and RGB surfaces, scaling and filtering, the colour keys), the 2D engine behind a 64-entry command FIFO drained by an engine thread (`RadeonQueue.cpp`; `ALPHABOX_RADEON_SYNC=1` for the old synchronous engine), the command processor -- ring buffer, indirect buffers, PIO queue, packets; no packets until a driver loads microcode -- that OpenVMS's DECwindows drives it through, the card's PCI GART (`RadeonGart.cpp`); a generation's 3D engine is behind `CRadeonEngine3D` and reaches the card only through `CRadeonEngineBus` (`RadeonEngine3D.hpp`, with the rule that engines run under the card's execution lock); `r100/` the R100 generation's 3D engine `radeon::r100::CRadeonR100_3D` (`RadeonR100_3D.cpp` vertex fetch and the 3D packets, `RadeonR100Tcl.cpp`, `RadeonR100Raster.cpp` rasteriser and pixel pipeline), checked by `ALPHABOX_RADEON_SELFTEST=1` (common checks in `RadeonSelfTest.cpp`, the generation's scenes in `r100/RadeonR100SelfTest.cpp`) since no guest drives it; `ALPHABOX_TRACE_RADEON`; adding a generation: the checklist in [docs/radeon.md](docs/radeon.md)) |
| `devices/usb/` | devices on the emulated USB, behind the OHCI controllers (`COhci` in `AliM1543C_usb`) and the EHCI card: `UsbDevice` (endpoint 0, chapter 9 requests; `CUsbPort`, a root hub port that owns its device), `UsbTablet` (absolute HID pointer), `UsbKeyboard` (a HID boot keyboard, `port<n> = "keyboard"`: the GUI's keys through `gui_guest_key`, the only keyboard of a machine without an 8042), `UsbStorage` (Bulk-Only mass storage around a `CDisk`, declared as `disk<port>.0`), `UsbAudio` (a USB Audio Class 1.0 speaker, `port<n> = "audio"`: isochronous OUT, SDL output, `ALPHABOX_USBAUDIO_WAV` capture), `UsbHostDevice` (a host device passed through with libusb, `port<n> = "host:vvvv:pppp"`; optional, `HAVE_LIBUSB`), `UsbAsyncShim` (test harness: an emulated device with libusb-like timing, isochronous included); user guide in `docs/usb.md` |
| `devices/net/` | `Ethernet`, `NicAddress` (shared station-address default), `NetworkBackend` and its backends `NetworkPcap`, `NetworkTap`, `NetworkUdp`, `NetworkNull` |
| `gui/` | `bx_gui` backends; SDL3 (`sdl.cpp`) is the maintained one |
| `common/` | `StdAfx`, `datatypes`, `es40_debug` (+ `Exception`, what its `FAILURE()` throws), `es40_endian`, `config_debug`, `banner`, `telnet`, `lockstep`, `WakeSemaphore` (a device thread's wake-up) |
| `src/*` | entry points `Main.cpp`, `AlphaSim.cpp`, `es40-cfg.cpp` (+ its `*Question.hpp`), and `config.hpp.in` |

Headers are included unqualified (`#include "System.hpp"`) — every source
directory is on the include path, so moving a file needs no include changes.
Devices are grouped by bus, not by chip: all four `AliM1543C*` components
are PCI functions (each calls `add_function()`, and the Configurator marks
every one `IS_PCI`), so they live in `devices/pci/` even though the M1543C
is the ISA bridge; `devices/isa/` holds only the devices behind it.

Everything hangs off `CSystem` (`system/System.cpp`), which owns physical
memory, the registered device ranges, the CPUs and the console loading, and
forwards the rest to the board's chipset (`cSystem->chipset()`, a
`CChipset` from `chipsets/`): an address that is neither memory nor a
device range goes to `read_io`/`write_io` (the Tsunami's Cchip/Dchip/Pchip/
TIG registers and unclaimed PCI space), `cSystem->interrupt()` and
`PCI_Phys()` to the chipset's interrupt controller and DMA windows. RAM
accesses never reach the chipset. The board row (`platforms/`) names the
chipset and builds the board's own hardware. Devices derive from
`CSystemComponent` (base class in `system/SystemComponent.cpp`), register
memory ranges with the system, and implement `ReadMem`/`WriteMem`, optional
`init()`/`start_threads()`/`stop_threads()`/`check_state()`, and
`SaveState`/`RestoreState`. PCI devices derive from `CPCIDevice`
(config space, BARs; `myPCIBus` is the hose, and a device declared inside a
bridge's block as `pci.<dev>` sits on its secondary bus: config space at the
bus number the firmware assigns, INTx rotated onto the bridge's slot); disk controllers from `CDiskController` with `CDisk`
children (`CDiskFile`/`CDiskDevice`/`CDiskRam`, BIN/CUE support in
`devices/storage/DiskFileBinCue.hpp`).

Major devices: `CAliM1543C` (ISA bridge: PIT/RTC-TOY/PIC/DMA + SuperIO) with
separate `_ide`/`_usb`/`_pmu` PCI functions (the IDE one a part on the
`CIdeController` core, as `CCmd649` is: `cmd649`, both channels native), `CSerial` (telnet or
null_attach UARTs), `CKeyboard` (KBC + PS/2 aux mouse, Bochs-derived),
`CTulip` and `CI8255x` (NICs: `dec2104x`, `de600`/`i8255[789]`; pcap, TAP, UDP or null backend), `CSym53C8xx` (SCSI: 53C810/825/875/895, and the two-channel 896 as two PCI functions) and `CIsp1040` (SCSI: QLogic ISP1020/1040/1080/1240, a mailbox and queue interface rather than SCRIPTS; the 1240 has two SCSI buses on one PCI function), `CS3Trio64` + `CVGA` +
MAME-derived rendering into the `bx_gui` plugin layer (`src/gui/`, SDL3 is
the maintained backend), `CCirrusGD5430`/`CCirrusGD5434` (`cirrus` config
class, `chip` key; same `CVGACard` base as the S3), `CS3Virge` (`s3virge`: the ViRGE family -- ViRGE, VX, DX/GX, GX2 -- with its 2D/3D S3d engine; Windows 2000's `s3m` desktop and Direct3D HAL run on it), `CTga` (`tga`: the DEC ZLXp-E1, 8-plane TGA, AlphaBIOS and Windows 2000's `tga` desktop, SRM lists it but cannot use it; and the TGA2 PowerStorm 3D30/4D20, Windows 2000's `tga2` desktop beside a VGA card), `CRadeon` (`radeon`: the ATI Radeon 7500, the ES47's AGP card; SRM console, and OpenVMS DECwindows through its command processor), `CES137x` (sound:
`es1370`, `es1371`, SDL audio), `CFlash`+`CDPR`
(firmware NVRAM).

Where the emulated processor knowingly differs from a real 21264 --
unaligned accesses that do not trap, `FPCR[UNDZ]`, machine checks that are
never raised, the oversized icache -- is recorded in `docs/cpu-fidelity.md`,
along with what was audited and found correct. Read it before "fixing" CPU
behaviour, and add to it when a divergence is found or closed.

CPU: `CAlphaCPU` (`cpu/AlphaCPU.cpp`) is a per-instruction interpreter
(`execute()`, opcode implementations in `cpu/cpu_*.hpp` headers included
into it); `cpu/AlphaCPU_vmspal.cpp` is a native fast-path reimplementation
of OpenVMS PALcode entry points; `cpu/AlphaCPU_ieeefloat/vaxfloat` implement
FP. `state` struct = the whole architectural state (savefile format). Guest
timing is wall-clock based: `state.cc` (RPCC) advances by real elapsed time,
CPU 0 fires the chipset's interval timer (`interval_tick()`, the Cchip's on
the Tsunami) at dispatch-batch boundaries, and the
8254 PIT/TOY in AliM1543C are wall-clock paced (see `cpu/AlphaCPU.hpp`
comments).

JIT (`src/jit/jitengine.cpp`, `ES40_JIT` builds only): translates Alpha
basic blocks to host code via asmjit -- x86-64 emitter in jitengine.cpp,
AArch64 emitter in `src/jit/jitemit_a64.hpp` (same bail/chain/frame protocol;
`ALPHABOX_JIT_FPTEST=1` on a `JIT_VERIFY` build self-tests the inline IEEE FP
ops against the interpreter), direct-mapped block cache keyed by
physical PC, poly-link direct chaining between blocks, register pinning;
bails to the interpreter for anything hairy. The trace tier (`JIT_TRACES`)
is deliberately dormant. `jit_run()` in `cpu/AlphaCPU.cpp` is the dispatch
loop; memory access goes through `jit_read/jit_write` helpers that mirror
`cpu/cpu_memory.hpp` semantics.

Configuration: `CConfigurator` (`system/Configurator.cpp`) parses `es40.cfg` into a
tree and instantiates the device graph; each device class has an allow-list
(`kv_*[]`) of the config values it reads — unknown values warn at startup
(`%SYS-W-UNKNOWNCFG`), so add new config keys to the matching list. The
sample `es40.cfg` at the repo root documents every value.

Threading: each active device runs a `std::thread` (`myThread`, lambda
calling `run()`, `std::atomic_bool myThreadDead` checked by
`check_state()`). The Poco-style wrappers inherited from es40 (`src/base/`:
CMutex, CSemaphore, CThread, ...) were removed in October 2026 -- do not
bring them back with ported code; use `std::mutex`/`std::thread`/
`std::chrono`/`std::condition_variable` (hard project rule). A lock or wait
never times out into a failure: a host that sleeps, or a stopped process,
must make the emulator late, not dead.
