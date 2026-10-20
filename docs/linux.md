# Linux

Linux/Alpha on the emulated ES40 (backlog E2, its first half; Linux on the
ES47 comes next). With no real Alpha to compare against, a second
operating system is the best test the emulator has, and Linux found four
bugs in it on its first boot.

| | |
| --- | --- |
| Media | Gentoo `install-alpha-minimal-20260426T153103Z.iso` (265 MB, `distfiles.gentoo.org/releases/alpha/autobuilds/`), SHA-256 `1ba9f219...642f74e` |
| Kernel | Linux 7.0.1-gentoo-alpha, `CONFIG_ALPHA_GENERIC`, SMP, gcc 15.2; boot loader aboot 1.0_pre20040408 (`/boot/bootlx` on the CD) |
| What it reaches | the live CD's root shell on the serial console, about 2.5 minutes after `boot` on the JIT lane (single runs: an estimate); a Gentoo stage3 installed from it boots from the disk to a login prompt, where gcc works; with two processors as well |
| JIT_VERIFY | the CD's boot, through the kernel, udev and the squashfs decompression, ran 4.5 billion compiled blocks against the interpreter with no mismatch |
| Fixed on the way | `FNOP` faulted with the FPU off; the CD drive had no READ TOC session format; an IDE command kept the last one's error bit; the UART's interrupt enable register kept bits a 16550 does not have ([below](#what-was-fixed)) |

| Section | What is in it |
| --- | --- |
| [Booting the install CD](#booting-the-install-cd) | the configuration, the SRM commands |
| [What works](#what-works) | what the kernel finds and uses |
| [Guest-side quirks](#guest-side-quirks) | messages that are Linux's, not the emulator's |
| [What was fixed](#what-was-fixed) | the four bugs, and how the first was found |
| [Installing to a disk](#installing-to-a-disk) | a disk SRM boots, with no network |
| [Open](#open) | what has not been tried |

## Booting the install CD

The machine is the regression test's no-VGA ES40 with 1 GB and the CD on
the ALi IDE's secondary master (`dqb0` to SRM):

```
sys0 = tsunami
{
  memory.bits = 30;
  rom.srm = "cl67srmrom.exe";
  ...
  cpu0 = ev68cb { speed = 800M; }
  serial0 = serial { address = "127.0.0.1"; port = 21200; }
  pci0.15 = ali_ide
  {
    disk1.0 = file
    {
      file = "install-alpha-minimal-20260426T153103Z.iso";
      read_only = true;
      cdrom = true;
    }
  }
  pci0.7 = ali { }
  pci0.19 = ali_usb { }
}
```

At the `P00>>>` prompt, on the serial console:

```
P00>>> set os_type unix
P00>>> init
P00>>> boot dqb0 -flags 2
```

- `-flags` picks the line of the CD's `/etc/aboot.conf`. Lines 2 and 3
  add `console=ttyS0`; 0 and 1 are for a graphics console.
- aboot switches to the OSF/1 PALcode itself ("switching to OSF/1
  PALcode version 1.92"); `os_type unix` was set for every run here.
- `boot dqb0 -flags i` gives aboot's own prompt, where a kernel line can
  be typed whole, for instance with `epoch=2000` added
  ([below](#guest-side-quirks)).

The live system logs in as root on `ttyS0` by itself.

## What works

The kernel's view, from its log:

- "Booting GENERIC on Tsunami variation Clipper using machine vector
  Clipper from SRM": the ES40's own interrupt table; one EV68CB, 1 GB.
- The ALi M5229 (`pata_ali`): the CD at PIO4 on `ata2`. The driver turns
  ATAPI DMA off for this chip revision by default ("ATAPI DMA disabled for
  reliability issues"), so the CD reads by PIO: about 39 MB/s in one
  `dd` run, an estimate.
  `sr0`, ISO 9660 with Rock Ridge and Joliet, squashfs over loop.
- Hard disks on the same controller (`sda`, `sdb`), and a BSD disklabel
  read as partitions by Linux's OSF partition code.
- Both serial ports as 16550A, the i8042 keyboard and mouse ports, the
  floppy controller, the RTC (`rtc-alpha`).
- udev, OpenRC 0.63 and the live CD's autoconfig, which names the
  machine: "Detected 1 EV68CB CPU(s) in a AlphaServer ES40 system".
- `cdrom_id` on the CD: ID_CDROM_MEDIA_CD, one data track.

`/proc/cpuinfo` says "system type Tsunami, system variation Clipper,
platform string AlphaServer ES40", cycle frequency 800000000, timer 1024
Hz.

**Two processors** (a `cpu1` block added, the installed disk booted):
"SMP: 2 CPUs probed", "Brought up 1 node, 2 CPUs", both in
`/proc/cpuinfo`'s active mask, and IPIs counted on both in
`/proc/interrupts`. `xz -T0` keeps both busy. Three `xz -T2 | xz -d`
round trips of a 30 MB random file, run at once, each gave back the
file's SHA-1.

An observation, not measured with `perf_ab.py`: two `xz -1` jobs at once
took about 4.5 times the wall time of one, where twice would be
expected, with the host busy with other work. Whether that is the guest
switching six threads on two processors or the emulator is not known.

## Guest-side quirks

These are Linux's own, and a real ES40 would show them too:

- **The date is 2006.** Linux guesses the RTC's epoch from its year
  register (`arch/alpha/kernel/rtc.c`): 20 to 47 is taken for the NT
  epoch, 1980, so 2026 reads as 2006 ("Using epoch 1980 for rtc year
  26"), a guess that predates 2020. The kernel parameter
  `epoch=2000` overrides it. Without it, `tar` warns about every file of a
  stage3 ("time stamp ... in the future").
- **"HWRPB cycle frequency bogus, and unable to estimate a proper
  value!"**: Linux's table of plausible clocks (`validate_cc_value` in
  `arch/alpha/kernel/time.c`) puts the EV68CB at 1000 to 1333 MHz, the
  ES45's parts. The ES40's 833 MHz EV68CB is outside it as
  well as the 800 MHz configured here. Linux then uses the HWRPB's value,
  which is the rate the emulated cycle counter runs at.
- **No USB**: this kernel has no OHCI PCI glue (`CONFIG_USB_OHCI_HCD_PCI`
  is off), so the ALi USB controller has no driver.
- **No network** in this configuration: no NIC is configured, and the
  emulator has no user-mode network back end. The kernel has the Tulip
  driver built in.

## What was fixed

### `FNOP` faulted with the FPU off

The first boot printed nothing after aboot's "starting kernel". The CPU
sat in the PALcode at PAL_BASE + 0x2000, the `CALL_PAL HALT` entry, in a
loop. That memory held no PALcode but six-quadword frames: PS 7, PC 0, a0
0, a1 1, a2 -1. That is an OSF/1 PALcode exception frame for an
instruction fetch from address 0 (`entMM`, access violation, I-stream).

The frame at the top of the kernel stack was the first one: a FEN fault
(`entIF` type 3) at `memmove+0x184`, called from `printk`. The
instruction there is `FNOP`, part of the `nop; fnop; unop` padding GNU as
puts in for alignment. This early, Linux has not called `trap_init`, so
every trap vector is 0. The fault went to 0, the fetch from 0 faulted,
and each fault pushed a frame lower, over the kernel and then the
PALcode.

`FNOP` "generates no exceptions" (Alpha Architecture Handbook,
appendix A), and the 21264 drops it before it issues. The emulator
checked the FPU enable for every floating-point operate instruction
first. Now the exact `FNOP` encoding does nothing, and the JIT treats it
as a NOP ([cpu-fidelity.md](cpu-fidelity.md)). The FPU is still off at
that point; once `trap_init` has run, Linux answers a FEN fault by
turning it on (`do_entIF`).

How it was found, as a recipe for the next silent hang:

1. `ALPHABOX_PC_SAMPLE=1`: the PC, with `ra` and `pv`, every 100 ms.
2. `kill -USR2 <pid>` (JIT builds): the `dbg-dump`, now with PAL_BASE,
   `EXC_ADDR`, `EXC_SUM`, `VA`, `MM_STAT` and the code around a PALcode PC
   read physically ([headless.md](headless.md)).
3. `ALPHABOX_DUMP_MEMORY=1` and a SIGTERM: the low guest memory as a
   file, to read the stack from.
4. The kernel's `System.map` (on the CD) and `lab/alphadis.py --words`
   on words taken from the decompressed `boot/gentoo` ELF.

### The CD drive had no READ TOC session format

udev's `cdrom_id` asks every CD for its session information: READ
TOC/PMA/ATIP with format 1. The drive stopped the emulator for any format
but 0 ("I don't understand READ TOC/PMA/ATIP with format 1"), killing the
IDE thread and with it the machine, as soon as udev started. It now
answers format 1 (one session, track 1 at the start), and refuses the
formats a CD-ROM drive does not have (2 to 5) and a track the medium does
not have with ILLEGAL REQUEST, INVALID FIELD IN CDB.

### An IDE command kept the last one's error bit

Every time udev probed the CD, libata reported "exception Emask 0x3 ...
(HSM violation)" on READ DISC INFORMATION (0x51) and reset the port. The
drive refuses that command, correctly, with ILLEGAL REQUEST. libata then
sends REQUEST SENSE, a new PACKET command, and the status it read while
the drive waited for the command packet (BSY clear, DRQ set) still had
ERR set from the refused command. libata calls that "DRQ=1 with device
error" and gives up on the port. A new command clears ERR; the
controller now does that when the command register is written.

### The UART kept interrupt enable bits a 16550 does not have

The kernel listed both serial ports as "XScale". Linux's 8250 probe
writes bit 6 of the interrupt enable register (UUE on Intel's XScale
UARTs) and reads it back. A 16550's IER has bits 3 to 0 only, and the
others read 0; the emulated one kept all eight. They are now masked, and
the ports are 16550A.

## Installing to a disk

Done once, to a 4 GB image: Gentoo's stage3
(`stage3-alpha-openrc-20261018T124555Z.tar.xz`, 265 MB) with the CD's
kernel boots from the disk, `boot dqa0 -flags 0`, to a login prompt, and
gcc 15 builds and runs a C program there.

The minimal CD has no network here and no aboot tools (`swriteboot`,
`abootconf`), so:

1. **The disk is laid out on the host** by `test/tools/aboot_disk.py
   gentoo.img 4096 bootlx`: a sparse image with, in sector
   0, an SRM boot block and a BSD disklabel; aboot's `bootlx` (from the
   CD) in sectors 1 to 158; partition a (Linux `sda1`, aboot's partition
   1) from sector 2048, 128 MB, for `/boot`; partition b (`sda2`) the
   rest, for `/`.
   - The boot block is what `make_halt_floppy.py` writes: the block
     count at byte 0x1E0, the starting block at 0x1E8, flags 0 at 0x1F0,
     and at 0x1F8 the sum of the first 63 quadwords.
   - The disklabel sits at byte 64 of sector 0 (the Alpha's
     LABELOFFSET), inside what the checksum covers, so it is written
     first. Linux reads it with its OSF partition code (`sda1 sda2`), and
     aboot says "valid disklabel found: 2 partitions".
2. **The stage3 comes in as a disk.** The tarball, padded with zeros to
   a whole sector, is the read-only primary slave (`disk0.1`, `sdb`).
   An xz file is a whole number of 4-byte units, and xz takes zero
   padding after a stream, so `xz -dc /dev/sdb | tar xpf - ...` reads it
   from the raw device. No second ISO is needed.
3. **From the live CD's shell**:
   ```
   mkfs.ext2 -b 1024 -I 128 -O none -L boot /dev/sda1
   mkfs.ext4 -q -L root /dev/sda2
   mount /dev/sda2 /mnt/gentoo; mkdir /mnt/gentoo/boot
   mount /dev/sda1 /mnt/gentoo/boot
   xz -dc /dev/sdb | tar xpf - --xattrs-include='*.*' --numeric-owner -C /mnt/gentoo
   cp /mnt/cdrom/boot/gentoo /mnt/gentoo/boot/vmlinuz
   cp -a /lib/modules/7.0.1-gentoo-alpha /mnt/gentoo/lib/modules/
   mkdir /mnt/gentoo/boot/etc
   echo '0:1/vmlinuz root=/dev/sda2 console=ttyS0 epoch=2000' > /mnt/gentoo/boot/etc/aboot.conf
   ```
   then an `/etc/fstab`, the `s0` agetty line in `/etc/inittab` enabled
   for `ttyS0`, a hostname and `chpasswd` in a chroot. The unpack took
   under three minutes of guest time on the JIT lane, in one run.
   - `/boot` is plain ext2 (128-byte inodes, no optional features):
     aboot's file system code dates from 2004, before ext4's extents.
     Nothing newer was tried.
   - No initramfs: the CD's kernel has the ALi IDE driver, the disk
     driver and ext4 built in, so `root=/dev/sda2` mounts directly.
4. **Boot**: `boot dqa0 -flags 0`. aboot finds `etc/aboot.conf` on
   partition 1 with no `abootconf`, loads `1/vmlinuz` and passes line 0's
   arguments. OpenRC comes up to "es40 login:", with the right date
   thanks to `epoch=2000`.

Linux's `reboot` brings SRM back to boot the device it booted last, with
flags 0. After the install that was the CD, which then booted without a
serial console. Stop the emulator instead, or set `bootdef_dev` and
`boot_osflags` in SRM.

The image after the install holds 1.5 GB of the host's disk; the guest
sees 1.3 GB used of 3.8 GB.

## Open

- Four processors, and why two parallel jobs were slower than they
  should be (above).
- A NIC (Tulip or Intel) on a back end, and a full Gentoo install from
  the network.
- A kernel built for the machine (Gentoo's `sys-kernel/gentoo-kernel`
  or a hand-built one); the installed system runs the CD's.
- The VGA console and a graphics card's framebuffer driver (the kernel
  has `tgafb`, `radeonfb`, `pm2fb`).
- SCSI: the kernel has the Symbios driver as a module.
- Linux on the ES47 (E2's second half), with the MSIs (M8).
