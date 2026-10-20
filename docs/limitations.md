# Known limitations

## Hosts and guests

- Big-endian hosts are not supported.
- More than two CPUs under Windows: Windows 2000 Professional is licensed
  for two, and the Windows 2000 Server beta HAL only sends inter-processor
  interrupts to CPUs 0–1. Tru64 is untested with more than one CPU.
- A time the guest sets is not kept across a restart: every start is at
  the host's time.

## The processor

- The guest's cycle counter runs ahead of real time. A driver busy-waiting
  on `RPCC` is handed the cycles it is waiting for instead of spinning
  through them.
- A guest that compares `RPCC` against the interval timer therefore sees a
  faster processor than the one configured; time of day is unaffected.
  `ALPHABOX_STALL_SKIP=0` restores the real wait.
- Every knowing divergence from a real 21264 is listed in
  [cpu-fidelity.md](cpu-fidelity.md).

## Storage and network

- Some SCSI and IDE commands are missing. INQUIRY, MODE SENSE, READ TOC
  and REQUEST SENSE pad their reply with zeros up to the allocation
  length, where SPC-3 stops at the data, and INQUIRY sends at least 36
  bytes. MODE SELECT acts on a block descriptor's block length only and
  ignores mode pages.
- The 53C8xx arbitrates as SCSI id 7 whatever SCID holds. It never
  raises Bus Fault, parity, SCSI gross error or handshake time-out
  interrupts (a DMA to an address nothing claims carries on), has no
  target mode, reselection or disconnect, and aborts a Memory Move of
  zero bytes, which the manual leaves undefined.
- The ES47's on-board Adaptec AIC-7892 is stood in for by a Symbios
  53C895.
- Not modelled: the DS25's AIC-7899 (configuration space and ROM only, no
  SCSI), its Broadcom BCM5703 network chip, and the ES45 and DS25
  hot-plug controllers.

## The ISA bridge

- The 8254 models modes 0, 2 and 3; modes 1, 4 and 5 have no output, and
  counter 0 raises IRQ0 in mode 3 only. A thread steps the counters once a
  millisecond, so counter 0 programmed above about 1 kHz loses edges.
  Every guest known uses mode 3 for counter 0, or the TOY clock.
- The TOY clock raises no alarm and no update-ended flag, and register C's
  IRQF is never set by the periodic flag; writing register B with PIE set
  makes register C read F0h once. The divider bits do not stop the clock.
- The interval timer runs at the rate in register A whatever PIE says:
  Windows 2000 never sets PIE and keeps its time from that timer.
- A second request from a slave 8259 in automatic EOI or special mask
  mode, while its output stays high, reaches the master only when that
  output falls. No guest known runs the slave that way.
- The Super I/O's logical device registers for the serial ports, floppy
  and keyboard controller are kept but not acted on: those devices stay
  where the configuration file puts them.

## Graphics

- **Mach64**: the front-end scaler and bus-master DMA are not modelled
  (Windows 2000's drivers use neither: stretched blits go through the 3D
  engine). The 3D Rage II+ has DirectDraw but no Direct3D, as Windows' own
  driver gives it none.
- **S3 ViRGE**: the Windows driver exposes no mipmaps, and the chips have
  no alpha test, no texture clamping, no specular colour and only square
  textures. Direct3D scenes using those differ from Microsoft's software
  rasteriser.
- **S3 ViRGE**: the driver draws lines as thin triangles whose colour does
  not follow the line. The ViRGE, ViRGE/VX, ViRGE/DX and ViRGE/GX2 are
  verified; the GX, which shares the DX's device ID, has not been run on
  its own. The VX's Windows driver offers no video overlay.
- **TGA (ZLXp-E1)**: no SRM console. SRM V7.3-1 lists it but has no driver
  for it. Windows 2000's driver draws only in simple mode, so the 21030's
  other drawing modes are unverified by any guest. The 24-plane E2/E3 are
  not modelled.
- **TGA2 (PowerStorm 3D30/4D20)**: runs only beside a VGA card, which
  stands in for the board's own Cirrus VGA. Neither AlphaBIOS nor SRM
  drives a TGA2 itself.
- **Permedia 2**: no SRM console. SRM V7.3-1 starts a card's BIOS without
  telling it where the card is, and every ELSA and 3Dlabs BIOS checks that
  before doing anything. AlphaBIOS starts it properly, so Windows is
  unaffected. Its video streams unit is not modelled.
- **Radeon 7500**: no Windows for Alpha shipped a driver for it; without
  nada's driver Windows runs it as a standard VGA. Its 3D engine is
  checked by a self-test and by that driver, not against a real card.
  Where a pixel centre falls exactly on a texel boundary, and how the chip
  does its triangle-setup arithmetic, are not documented anywhere
  ([radeon.md](radeon.md)).

## USB

- OpenVMS uses only the first three functions of a USB controller, so
  devices on the ES47's on-board USB go on ports 1–3.
- On macOS, host devices a system driver holds cannot be passed through
  unless alphabox runs as root ([usb.md](usb.md)).
