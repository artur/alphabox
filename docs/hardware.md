# Emulated hardware

## The machines

| Area | Devices |
|---|---|
| Machines | AlphaServer ES40, DS20E, DS20, DS20L and DS10 (Tsunami); ES45 (Model 2, and Model 1 with its AGP slot), DS25 and DS15 (Titan); ES47, ES80 and GS1280 (EV7, "Marvel"). All boot OpenVMS 8.4 ([platforms.md](platforms.md)) |
| CPU | Alpha EV68CB (21264): 1–4 on the Tsunami and Titan machines; EV7 and EV7z (21364), or the EV8 (21464) in an EV7-compatible mode: 2 on the ES47, up to 8 on the ES80 and 16 on the GS1280 |
| Chipset | Tsunami/Typhoon (Cchip, Dchip, 2 × Pchip, TIG, DPR/RMC); Titan (Cchip, Dchip, PA-chips with G and A ports, TIG); Marvel (each EV7's on-chip router and memory controllers, IO7 I/O bridges, the CMM management processors over GIO) |
| Memory | 64 MB – 32 GB; on the Tsunami and Titan boards in up to four memory arrays (`memory.arrays`); on the EV7 machines, per processor at its own physical base |
| ISA bridge | ALi M1543C: 8259 PIC, 8254 PIT, MC146818 RTC, 8237 DMA, SuperIO, PMU |
| Other | 2 × 16550 serial ports (telnet or unconnected), keyboard and PS/2 mouse, flash and NVRAM persistence |

## Devices

| Area | Devices |
|---|---|
| SCSI | Symbios 53C810, 53C825, 53C875, 53C895 and 53C896 (two channels); QLogic ISP1020, ISP1040 (KZPBA), ISP1080 and ISP1240 (two buses on one function) |
| IDE and other storage | ALi M1543C and CMD 649 IDE (disks and ATAPI CD-ROM); 82077AA floppy; RAM disk; virtio-blk |
| Network | DEC 21040, 21041, 21140 and 21143 (Tulip); Intel 82557, 82558 and 82559 (DE600-AA), the 82559ER on the DS25's board, and the two-port DE602-AA and DE602-B boards behind a bridge; virtio-net |
| Network back ends | pcap, TUN/TAP (Linux), a UDP link, a loopback plug, or nothing (`null`) |
| USB | the ALi M1543C's OHCI (USB 1.1, 3 ports), an EHCI card (USB 2.0, 4 ports, with OHCI companions) and the ES47's on-board Agere USS-344 (four OHCI functions) ([usb.md](usb.md)) |
| USB devices | a HID tablet and keyboard, Bulk-Only mass storage, a USB Audio speaker, and host devices passed through with libusb |
| Sound | Ensoniq AudioPCI ES1370 and ES1371 (AC'97 codec and sample-rate converter) |
| Expansion | DECchip 21050, 21052, 21152, 21153 and 21154 PCI-PCI bridges (nested buses, multi-port boards) |

## Graphics

| Card | Chips | What is emulated | Guests and consoles |
|---|---|---|---|
| S3 Trio64 | 86C764 | VGA, IBM 8514/A acceleration | SRM console, AlphaBIOS, Windows NT and 2000 |
| Cirrus Logic | CL-GD5430, CL-GD5434 | VGA, the BitBLT engine | SRM console, Windows 2000 |
| ATI Mach64 | CT, 264VT2, 264VT3, 3D Rage II+, 3D Rage Pro | the drawing engine, the hardware cursor, a monitor on the DDC lines, modes to 32 bpp, the video overlay; on the Rage Pro the triangle setup engine | SRM console; Windows 2000 (Direct3D on the Rage Pro) |
| 3Dlabs Permedia 2 | TVP4020 | the graphics processor and delta unit: 2D, and 3D with depth, texturing, fog and blending | AlphaBIOS; Windows 2000 with Direct3D; OpenVMS DECwindows |
| S3 ViRGE | ViRGE, ViRGE/VX, ViRGE/DX, ViRGE/GX2 | the S3d engine: 2D, and 3D with depth, texturing, fog and blending; the streams processor's 24-bit modes and video overlay | SRM console, AlphaBIOS, Windows 2000 with Direct3D |
| DEC ZLXp-E1 | DECchip 21030 (TGA), Bt485 | 8 planes, the 21030's graphics modes | AlphaBIOS; Windows 2000 desktop to 1280x1024 |
| PowerStorm 3D30, 4D20 | TGA2, Bt485 or IBM RGB561 | 8 planes (3D30), 32-bit true colour (4D20) | Windows 2000 desktop to 1280x1024 and 1600x1200, beside a VGA card |
| ATI Radeon 7500 | RV200 | the BIOS, the extended modes, the hardware cursor, the overlay scaler, the 2D engine, the command processor with its FIFO, microcode and GART, the 3D engine with transform and lighting ([radeon.md](radeon.md)) | SRM console on the ES40 and in the AGP slots of the ES47 and the ES45 Model 1; OpenVMS DECwindows; nada's Windows 2000 driver with Direct3D |

[peripherals.md](peripherals.md) lists every device the ES40 firmware
knows, what each emulated one does, and the candidates for what comes
next.
