# Known limitations

- More than two CPUs under Windows: Windows 2000 Professional is licensed
  for two, and the Windows 2000 Server beta HAL only sends inter-processor
  interrupts to CPUs 0–1. Tru64 is untested with more than one CPU.
- Big-endian hosts.
- Some SCSI and IDE commands. The ES47's on-board Adaptec AIC-7892 is
  stood in for by a Symbios 53C895, and the DS25's AIC-7899 and Broadcom
  network chip and the ES45/DS25 hot-plug controllers are not modelled.
- Cirrus screen-to-system BitBLT transfers (Windows 2000 does not use them),
  and the Mach64's front-end scaler and bus-master DMA (Windows 2000's
  drivers use neither: stretched blits go through the 3D engine). The 3D
  Rage II+ has DirectDraw but no Direct3D, as Windows' own driver gives it
  none.
- The S3 ViRGE family: the Windows driver exposes no mipmaps, and the chips
  have no alpha test, no texture clamping, no specular colour and only square
  textures, so Direct3D scenes using those differ from Microsoft's software
  rasteriser; the driver draws lines as thin triangles whose colour does not
  follow the line. The ViRGE, ViRGE/VX, ViRGE/DX and ViRGE/GX2 are verified;
  the GX, which shares the DX's device ID, has not been run on its own. The
  VX's Windows driver offers no video overlay.
- The TGA (ZLXp-E1) is no SRM console: SRM V7.3-1 lists it but has no
  driver for it. Windows 2000's driver draws only in simple mode, so the
  21030's other drawing modes are unverified by any guest; the 24-plane
  E2/E3 are not modelled. The TGA2 (PowerStorm 3D30/4D20) runs only beside
  a VGA card, which stands in for the board's own Cirrus VGA: neither
  AlphaBIOS nor SRM drives a TGA2 itself.
- The Permedia 2 is no SRM console: SRM V7.3-1 starts a card's BIOS without
  telling it where the card is, and every ELSA and 3Dlabs BIOS checks that
  before doing anything. AlphaBIOS starts it properly, so Windows is
  unaffected. Its video streams unit is not modelled.
- The Radeon 7500: no Windows for Alpha shipped a driver for it; without
  nada's driver Windows runs it as a standard VGA. Its 3D engine is checked
  by a self-test and by that driver, not against a real card: where a pixel
  centre falls exactly on a texel boundary, and how the chip does its
  triangle-setup arithmetic, are not documented anywhere
  ([radeon.md](radeon.md)).
- USB: OpenVMS uses only the first three functions of a USB controller, so
  devices on the ES47's on-board USB go on ports 1–3; on macOS, host
  devices a system driver holds cannot be passed through.
- The guest's cycle counter runs ahead of real time: a driver busy-waiting on
  `RPCC` is handed the cycles it is waiting for instead of spinning through
  them. A guest that compares `RPCC` against the interval timer therefore
  sees a faster processor than the one configured; time of day is
  unaffected. `ALPHABOX_STALL_SKIP=0` restores the real wait. Every knowing
  divergence from a real 21264 is listed in
  [cpu-fidelity.md](cpu-fidelity.md).
