# Alphabox documentation

## Using it

- [Building](building.md): Linux, macOS, Windows, the JIT, and diagnostic
  builds.
- [Running and configuring](configuration.md): the machines, firmware and
  VGA BIOS images, ARC/AlphaBIOS, serial consoles, networking, sound,
  mouse and hotkeys, disk and CD images, and guest installation guides.
- [Headless operation and debug hooks](headless.md): window-less runs,
  screen dumps, scripted keyboard and mouse input, and trace switches.
- [Debugging a guest with gdb](debugging.md): the gdb stub, breakpoints
  and stepping in compiled code, SMP, the `monitor` commands, and the
  guest's clock while it is stopped.

## Machines and devices

- [Emulated hardware](hardware.md): every machine, chip and card.
- [Known limitations](limitations.md): what is missing or known to differ.
- [Machines](platforms.md): how a CPU, a chipset and a board fit together,
  which machines run, and the work packet a new machine is added with.
  The packets are in [platforms/](platforms/): the DS20E, DS10, DS20L,
  the Titan boards (ES45, DS25, DS15) and the EV7 machines (ES47, ES80,
  GS1280).
- [What is missing, and the plan](backlog.md): the processors' and
  chipsets' gaps, and the order they are closed in.
- [Peripherals](peripherals.md): the devices the ES40 firmware names, what
  is emulated, and the candidates for what comes next.
- [USB](usb.md): the built-in OHCI, the EHCI card and the ES47's USS-344,
  the tablet, keyboard, USB disks and speaker, host passthrough, and the
  hooks for testing a guest's USB driver.
- [Paravirtual devices](virtio.md): virtio-blk and virtio-net (legacy
  virtio-pci): PCI identity, registers, queues, interrupts and the
  self-test, as a reference for driver writers.
- [The ATI Radeon 7500](radeon.md): what the model covers, how it is
  checked, and how a Radeon generation is added.
- [The Radeon's CP microcode](radeon-microcode.md): what the public
  sources say about the micro-engine, every published image, and what the
  images show.

## Guests

- [OpenVMS](openvms.md): installing and running OpenVMS 8.4 on the ES40,
  the SCSI and network drivers, DECwindows.
- [Linux](linux.md): Gentoo's install CD on the ES40, an install to disk
  without a network, and the emulator bugs Linux found.
- [Guest drivers](guest-drivers.md): the Windows drivers written with nada
  for hardware no Alpha Windows had a driver for (USB 2.0, virtio, the
  Radeon 7500 with Direct3D).

## Inside

- [Source layout and architecture](source-layout.md): every source
  directory, device family and chip, and how the CPU, JIT, configuration
  and threading fit together.
- [Development and testing](development.md): the test tools, and what
  verifying a change involves.
- [Processor fidelity](cpu-fidelity.md): where the emulated 21264 does not
  match the real one, what a guest can therefore not be used to test, and
  how to check the processor yourself.
- [Performance](performance.md): what has been measured, what bounds each
  workload, and which optimizations did not pay off.
- [Recompiler techniques](jit-techniques.md): what Dolphin, QEMU, PCSX2,
  PPSSPP, RPCS3, Rosetta 2 and the literature do about block linking,
  indirect branches and memory translation, and what they abandoned.
- [Hypervisor.framework](hypervisor.md): the analysis of running the CPU
  core inside a macOS virtual machine, its measured costs, and the
  experimental runtime.

## History

- [History and acknowledgements](acknowledgements.md): where the code came
  from, who wrote it, and the third-party code and its licences.

The sample [`es40.cfg`](../es40.cfg) documents every configuration value.
