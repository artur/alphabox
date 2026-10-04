# Guest drivers written for Alphabox

Some of the hardware Alphabox emulates never had a driver on an Alpha
operating system: no Windows for Alpha shipped a USB 2.0 stack, a Radeon
driver or a virtio driver. Those drivers have since been written, for
Windows 2000 RC2 (build 2128) on the Alpha, with **nada** -- a separate
project: a C compiler that targets Alpha Windows, 32- and 64-bit, user mode
and kernel mode (`-t alpha-windows -mdriver`). They are original code,
written from the hardware specifications, Microsoft's public DDK
documentation and the open register documents; no Microsoft or ReactOS code
is in them.

The drivers live in nada's tree, not here, because nada builds them. Each
has a `DESIGN.md` (what it does, how it was tested, what it leaves out), a
`build.sh` and a `run.sh` that installs it on a clone of an installed
Windows 2000 guest and tests it headless.

| Driver | Device it drives | Path in nada | Tested with nada | What it gives Windows 2000 |
|---|---|---|---|---|
| `nadaehci.sys` | the `ehci` card (NEC uPD720101: EHCI with two OHCI companions) | `examples/alpha-nt/usb/` | ab2f74f (2026-09-30) | USB 2.0 disks at high speed; full- and low-speed devices handed to the companions, which Windows' own `openhci.sys` drives; recovery from injected USB faults ([usb.md](usb.md)) |
| `nadavblk.sys` | `virtio_blk` (1AF4:1001) | `examples/alpha-nt/virtio/` | a49f021 (2026-09-30) | a paravirtual disk, as a SCSI miniport ([virtio.md](virtio.md)) |
| `nadavnet.sys` | `virtio_net` (1AF4:1000) | `examples/alpha-nt/virtio/` | a49f021 (2026-09-30) | a paravirtual NIC, as an NDIS 5 miniport ([virtio.md](virtio.md)) |
| `nadarad.sys` + `nadarad.dll` | the `radeon` card (ATI Radeon 7500, RV200) | `examples/alpha-nt/radeon/` | 0d5c9d9 (2026-10-03) | 640x480 to 1280x1024 at 8, 16 and 32 bpp with the 2D engine and hardware cursor, all through the command processor's ring; DirectDraw and a Direct3D 7 HAL on the card's 3D engine (pre-transformed vertices, two texture stages) ([radeon.md](radeon.md)) |
| `nadadrv.sys` | none | `examples/alpha-nt/driver/` | e535a89 (2026-09-29) | a minimal test driver and its installer, the toolkit's first step; also built for Whistler AXP64 |

`VINST.EXE` (in `examples/alpha-nt/usb/install.c`) is nada's generic
installer: `VINST.EXE /wizard "<device instance>"` walks Windows' Found New
Hardware wizard for a device, which is how the drivers are installed
unattended.

## What the drivers are for here

A real driver asks questions a test never thinks of. Each of these drivers
was the first guest code to exercise part of an emulated device: the
Radeon's blits, colour brushes and ARGB cursor were first used by
`nadarad`, and its Direct3D runs exposed two model bugs no self-test had
caught (the provoking vertex in triangle strips, and line rasterisation
through sub-pixel endpoints). When a
driver and the model disagree, the register documentation decides, not
the driver: see the `d3d_check` notes in [radeon.md](radeon.md) for how a
disagreement is classified.

## Tests in this repository that use them

- `test/tools/d3d_check.sh` with the Direct3D program in
  `test/tools/d3dcheck/`: renders the same scenes on a card's HAL and on
  Direct3D's software rasteriser and compares them (the Rage Pro, Permedia
  2, ViRGE and, with `nadarad`, the Radeon 7500).
- `test/tools/usb_bench.sh`: USB storage throughput by the guest's clock,
  including a disk on the EHCI card through `nadaehci`.

The guest images these run on are the owner's and live in `lab/`, which is
not part of the repository.
