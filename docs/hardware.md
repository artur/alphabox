# Emulated hardware

| Area | Devices |
|---|---|
| Machines | AlphaServer ES40, DS20E, DS20L and DS10 (Tsunami); ES45 (Model 2, and Model 1 with its AGP slot), DS25 and DS15 (Titan); ES47, ES80 and GS1280 (EV7, "Marvel") -- all boot OpenVMS 8.4 ([platforms.md](platforms.md)) |
| CPU | Alpha EV68CB (21264): 1–4 on the Tsunami and Titan machines; EV7 and EV7z (21364): 2 on the ES47, up to 8 on the ES80 and 16 on the GS1280 |
| Chipset | Tsunami/Typhoon (Cchip, Dchip, 2 × Pchip, TIG, DPR/RMC); Titan (Cchip, Dchip, PA-chips with G and A ports, TIG); Marvel (each EV7's on-chip router and memory controllers, IO7 I/O bridges, the CMM management processors over GIO) |
| Memory | 64 MB – 32 GB; on the EV7 machines, per processor at its own physical base |
| Storage | Symbios 53C810 / 53C825 / 53C875 / 53C895 / 53C896 (two channels) and QLogic ISP1020 / ISP1040 (KZPBA) / ISP1080 / ISP1240 (two buses on one function) SCSI; ALi M1543C and CMD 649 IDE (disks and ATAPI CD-ROM); 82077AA floppy; RAM disk; virtio-blk |
| ISA bridge | ALi M1543C: 8259 PIC, 8254 PIT, MC146818 RTC, 8237 DMA, SuperIO, PMU |
| Graphics | S3 Trio64 (with IBM 8514/A acceleration); Cirrus Logic CL-GD5430 / CL-GD5434 (with BitBLT); ATI Mach64 CT / 264VT2 / 264VT3 / 3D Rage II+ / 3D Rage Pro (drawing engine, hardware cursor, a monitor on the DDC lines, modes to 32 bpp; on the Rage Pro the triangle setup engine, for Direct3D); 3Dlabs Permedia 2 (its graphics processor and delta unit: 2D, and Direct3D with depth, texturing, fog and blending); S3 ViRGE / ViRGE/VX / ViRGE/DX / ViRGE/GX2 (the S3d engine: 2D, and Direct3D with depth, texturing, fog and blending; the streams processor's 24-bit modes and video overlay); DEC ZLXp-E1 (DECchip 21030 "TGA", 8 planes, Bt485: AlphaBIOS and the Windows 2000 desktop to 1280x1024); PowerStorm 3D30 and 4D20 (TGA2: 8 planes with a Bt485, and 32-bit true colour with an IBM RGB561; the Windows 2000 desktop to 1280x1024 and 1600x1200, beside a VGA card); ATI Radeon 7500 (RV200: its BIOS, the extended modes, the hardware cursor, the 2D engine, the command processor with its FIFO, microcode and GART, and the 3D engine with transform and lighting; the SRM console on the ES40 and in the AGP slots of the ES47 and the ES45 Model 1, DECwindows on OpenVMS, and nada's Windows 2000 driver with Direct3D -- [radeon.md](radeon.md)) |
| USB | the ALi M1543C's OHCI (USB 1.1, 3 ports), an EHCI card (USB 2.0, 4 ports, with OHCI companions) and the ES47's on-board Agere USS-344 (four OHCI functions); a HID tablet and keyboard, Bulk-Only mass storage, a USB Audio speaker, and host passthrough through libusb ([usb.md](usb.md)) |
| Network | DEC 21040 / 21041 / 21140 / 21143 (Tulip); Intel 82557/82558/82559 (DE600-AA), the 82559ER on the DS25's board, and the two-port DE602-AA / DE602-B boards behind a bridge; virtio-net -- host access through pcap, TUN/TAP (Linux), a UDP link or a null back end |
| Sound | Ensoniq AudioPCI ES1370 and ES1371 (AC'97 codec and sample-rate converter) |
| Expansion | DECchip 21050/21052/21152/21153/21154 PCI-PCI bridges (nested buses, multi-port boards) |
| Other | 2 × 16550 serial ports (telnet or unconnected), keyboard and PS/2 mouse, flash and NVRAM persistence |

[peripherals.md](peripherals.md) lists every device the ES40
firmware knows and which ones are coming next.
