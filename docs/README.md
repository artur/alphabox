# Alphabox documentation

- [Building](building.md): Linux, macOS, Windows, the JIT, and diagnostic
  builds.
- [Running and configuring](configuration.md): firmware and VGA BIOS
  images, ARC/AlphaBIOS, serial consoles, networking, sound, mouse and
  hotkeys, disk and CD images, and guest installation guides.
- [Headless operation and debug hooks](headless.md): window-less runs,
  screen dumps, scripted keyboard and mouse input, and trace switches.
- [Development and testing](development.md): source layout, test tools, and
  what verifying a change involves.
- [Machines](platforms.md): how a CPU, a chipset and a board fit together,
  and the work packet a new machine is added with (pilot:
  [DS20E](platforms/ds20e.md)).
- [Peripherals](peripherals.md): the devices the ES40 firmware names, what
  is emulated, and what comes next.
- [Performance](performance.md): what has been measured, what bounds each
  workload, and which optimizations did not pay off.
- [Processor fidelity](cpu-fidelity.md): where the emulated 21264 does not
  match the real one, what a guest can therefore not be used to test, and
  how to check the processor yourself.

The sample [`es40.cfg`](../es40.cfg) documents every configuration value.
