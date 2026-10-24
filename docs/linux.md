# Linux

Linux/Alpha on the emulated ES40 (backlog E2, its first half) and on the
ES47 ([below](#linux-on-the-es47), its second half). With no real Alpha
to compare against, a second operating system is the best test the
emulator has, and Linux found four bugs in it on its first boot on the
ES40 and one more on the ES47.

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
| [Linux on the ES47](#linux-on-the-es47) | the same CD and install on Marvel's EV7s: the configuration, what runs, four processors, the quirks, the fixes, the MSIs |
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

**The VGA console**, with an S3 card on the PCI bus: the kernel's
`vgacon` takes it ("colour VGA+ 80x25") when the kernel line has
`console=tty0`. The installed disk's `aboot.conf` has only
`console=ttyS0`, so the line is typed at aboot's prompt (`boot dqa0
-flags i`). The README's Linux screenshot is that console.

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

These are Linux's own (or the configuration's), and a real ES40 would
show them too:

- **The date is 2006.** Linux guesses the RTC's epoch from its year
  register (`init_rtc_epoch` in `arch/alpha/kernel/rtc.c`): below 20 is
  2000, 20 to 47 the NT epoch, 1980, 48 to 69 the Digital UNIX epoch,
  1952, and 70 up 1900. It reads register B and the year only; there is
  no century byte to read. The year register holds 26, as on any PC-style
  clock and as an ES40's SRM leaves it, so 2026 reads as 2006 ("Using
  epoch 1980 for rtc year 26"), a guess that predates 2020. The kernel
  parameter `epoch=2000` overrides it. Without it, `tar` warns about every
  file of a stage3 ("time stamp ... in the future"). With
  `arc_year_compat = true` in `sys0` the register holds years since 1980
  (46), which Linux reads right ("rtc year 46", 2026) until 2027; from
  2028 it would take it for the Digital UNIX epoch.
- **The clock is local time.** The TOY starts from the host's local time
  unless the `ali` block says `timezone = "utc"`, and Linux reads it as
  UTC, so `date -u` shows local time. With `timezone = "utc"`, `date -u`
  and `hwclock` agree with the host's UTC.
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
  driver built in. With a `dec21143` added on the UDP back end and
  `test/tools/net_peer.py` at the other end, the driver comes up as
  `enp0s4`, OpenRC's dhcpcd takes the peer's BOOTP answer (10.0.2.15),
  and the guest pings the peer. The driver loads perfect filter tables,
  and a hash table once more than 14 multicast groups are joined.

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

## Linux on the ES47

The same CD and the same install run on the emulated ES47 (`es47`, two
EV7s, Marvel; 2026-10-24). This is the first operating system on the
EV7's UNIX PALcode, and Marvel's interrupt path as Linux programs it.

| | |
| --- | --- |
| What it reaches | the live CD's root shell on the console terminal with one and with two EV7s; the ES40's install, with the CD's Marvel kernel, to a login on two, where gcc builds and runs a program and `poweroff` halts to SRM; the same install on four EV7s of the `gs1280` row. A four-processor ES47 (two drawers) and the live CD on four: Linux's limits, [below](#four-processors) |
| Kernel | the CD's `gentoo_nolsa`, with one byte changed (`test/tools/es47_linux.sh`, [below](#guest-side-quirks-on-marvel)) |
| Console | the SRM console terminal (`console=srm0`): Marvel has no 16550, and the terminal is the CMM's, as for OpenVMS |
| JIT_VERIFY | two EV7s, the installed disk from `boot` to a login and `poweroff`: 1.4 and 1.6 billion compiled blocks on the two processors checked against the interpreter, no mismatch |
| Fixed on the way | the CMM's TOY register B said binary, and Linux's Marvel RTC code needs it to say BCD; a processor kept old code in its 2 KB instruction-cache lines past another one's `IMB` ([below](#what-was-fixed-on-the-es47)) |

### The configuration

The ES47's OpenVMS machine ([marvel.md](platforms/marvel.md), M7a) with
the CD on the embedded CMD 649, where the console boots it (`dqa0`), and
a boot disk beside it (`dqa1`):

```
sys0 = tsunami
{
  platform = "es47";
  memory.bits = 30;                # per processor: 29 with two
  rom.srm = ".../roms/alpha-firmware-v7.3/GS1280/SRM_V7_3.EXE";
  rom.nvram = "cmm_nvram.bin";
  cpu0 = ev7 { speed = 1000M; }    # and cpu1
  pci2.2 = cmd649
  {
    disk0.0 = file { file = "install-alpha-minimal-20260426T153103Z.iso";
                     read_only = true; cdrom = true; model_number = "CD-W216E"; }
    disk0.1 = file { file = "es47-boot.img"; }
  }
  serial0 = serial { address = "127.0.0.1"; port = 23200; }
}
```

The boot disk comes from the CD: `test/tools/es47_linux.sh bootdisk <iso>
es47-boot.img` (bsdtar, gzip, python3 and e2fsprogs on the host). Its
`etc/aboot.conf` line 0 boots the CD's `gentoo_nolsa` kernel and initrd
with the CD's own arguments, `cdroot` (the live root is still the CD),
`console=srm0` and `epoch=2000`. At `P00>>>`:

```
P00>>> set os_type unix
P00>>> init
P00>>> boot dqa1 -flags 0
```

aboot switches to the console's UNIX PALcode itself ("switching to OSF/1
PALcode version 2.8"), and the boot works the same without `set os_type
unix` and `init` (the four-processor runs, the `linux-es47` check). The
environment is not kept here: no run wrote `cmm_nvram.bin`, so a `set` is
gone at the next start (open, below). The live CD logs in as root on
`srm0` by itself.

**The installed disk.** `test/tools/es47_linux.sh retarget <iso> <clone>`
changes a clone of the ES40's install (`lab/linux/disk`) for Marvel: the
same kernel into `/boot/vmlinuz`, aboot line 0 `root=/dev/sda2
console=srm0 epoch=2000`, and a getty on `srm0` for the one on `ttyS0`.
On the CMD 649 as `dqa0` (`disk0.0`, no CD), `boot dqa0 -flags 0` comes up
to "es40 login:"; nothing else in the install changed. That is the
`linux-es47` check of `test/tools/guests_check.sh` (log in, `uname -a`,
the processors and the platform from `/proc/cpuinfo`, `date`, `poweroff`).

### What runs

From the kernel's log and `/proc`:

- "Booting GENERIC on Marvel variation Marvel/EV7 using machine vector
  MARVEL/EV7 from SRM"; `cpu model EV7`, `platform string hp AlphaServer
  ES47 7/1000`, cycle frequency 1000000000, timer frequency 953.67 Hz.
  The tick is each EV7's RBOX_IT at the rate the console set (cpu_hz /
  (8 * 2^17)); 1024 Hz is only the kernel's HZ.
- "Found an IO7 at PID 0" from the GCT, three hoses (hose 3, the AGP
  port, is the one the console turns off with no card in it), "Initializing
  interrupts for IO7 at PE 0", "Interrupts reported to CPU at PE 0".
- `pata_cmd64x` on LSI 0x45, IRQ 85 (16 + 0x45), as the console set it:
  the CD (`sr0`), the boot disk or the install (`sda`).
  The UNIX PALcode takes the IO7's interrupt and ends it with EOI_DAT, as
  the OpenVMS one does (M5).
- The clock from the TOY through `cserve` 0x49 ("setting system clock to
  2026-10-24T09:24:06 UTC"), `hwclock -r`; local time read as UTC, as on
  the ES40 ([above](#guest-side-quirks)).
- Two EV7s: "SMP: 2 CPUs probed", "Brought up 1 node, 2 CPUs", the tick
  and IPIs counted on both in `/proc/interrupts`; two `xz -1 | xz -d`
  round trips of an 8 MB random file, run at once, each gave the same
  MD5. Linux sends the device interrupts to PID 0; PID 1 has no IO7, so
  `marvel_smp_callin` redirects nothing.
- `uname -m` alpha, "EV7 Marvel"; the live CD's autoconfig names the
  machine: "Detected 2 EV7 CPU(s) in a hp AlphaServer ES47 7/1000 system".

### Four processors

An ES47 holds up to two 2P drawers (User Information, "An ES47 system
supports up to two 2P drawers"), so four EV7s are the `es80` row's first
two drawers, PIDs 0, 1, 8 and 9, with memory at 0, 16 GB, 256 GB and
272 GB. Linux runs there from the installed disk with **2 GB or more per
processor**, and not with 1 GB or less; the live CD cannot boot on any
Marvel with memory above 32 GB:

- **The memory map needs 2 GB per processor.** The CD's kernel has no
  `SPARSEMEM`, so one `struct page` array covers 0 to 272 GB, about
  1.9 GB that must fit in one processor's memory. `setup_memory` limits
  Marvel to its first 32 GB for this very reason, but only through
  `max_low_pfn`: the node still spans every memory cluster the HWRPB
  lists, and so does the array; `mem=` does not help either. With 512 MB
  or 1 GB per processor: "Kernel panic - not syncing: Failed to allocate
  2003828736 bytes for node 0 memory map" (2000158720 at 512 MB), after
  which the boot processor waits for secondaries that never started, and
  spins. With 2 GB and with 4 GB per processor (`memory.bits = 31` and
  `32`, the installed disk, 2026-10-24): "SMP: 4 CPUs probed", "Brought
  up 1 node, 4 CPUs", `cpus active: 4`, four parallel `xz` round trips
  with the file's MD5, and `poweroff` back to `P00>>>`
  (`lab/runs/linux-es80-mem-*`). Real ES47s had 512 MB to 8 GB per
  processor, so this is a configuration a real machine could have had
  either way. On Alpha `grep -c processor /proc/cpuinfo` prints 0; the
  "cpus active" line counts them.
- **The initrd.** aboot loads the initrd at the top of memory, above
  32 GB on any Marvel with more than two processors' memory; Linux moves
  it below its limit with a top-down allocation that lands above it again
  (`move_initrd`, called before `memblock_set_bottom_up`), and drops it:
  "initrd extends beyond end of memory ... disabling initrd". The live CD
  then has no root ("VFS: Unable to mount root fs").

Four processors were first run on the `gs1280` row's 8P drawer, PIDs 0 to 3
(memory at 0, 16, 32 and 48 GB, a 390 MB memory map), from the installed
disk, which needs no initrd: "SMP: 4 CPUs probed", "Brought up 1 node, 4
CPUs", `cpus active: 4`, IPIs on all four, and four `xz -1 | xz -d`
round trips of an 8 MB random file, run at once, each gave the file's
MD5. The first try stopped with "Instruction fault 4" in
`arch_cpu_idle`: the emulator's, fixed ([below](#what-was-fixed-on-the-es47)).
One run that typed `init` first came up with three: the console printed
"CPU 3 failed to take jump address part 0" as it restarted its
secondaries (open, below).

### Guest-side quirks on Marvel

These are Linux's (or the CD's), and a real ES47 would show them too:

- **`/boot/gentoo` cannot load**: it is built with
  `ALPHA_LEGACY_START_ADDRESS` (0x310000), inside the memory the Marvel
  console keeps, so aboot refuses it ("Busy (Reserved)"). The kernel's
  Kconfig help says such a kernel cannot run on Marvel; `gentoo_nolsa` is
  the CD's kernel without it. The ES40's install has the first one.
- **Every kernel on the CD oopses in `vgacon_startup`** when the console
  is not a graphics one ("Unable to handle kernel paging request at
  virtual address 0"). `setup_arch` registers the VGA console
  unconditionally; on Marvel, `marvel_ioremap` returns NULL for the legacy
  VGA memory at 0xb8000 while no console VGA hose is known
  (`marvel_find_console_vga_hose` sets one only for a CTB of TERM_TYPE 3,
  a graphics console), and `vgacon_startup` reads through that NULL (its
  memory probe, `scr_readw`). On the ES40 the same `ioremap` gives an
  address on hose 0, the probe finds no memory there, and Linux takes the
  dummy console ("Console: colour dummy device 80x25", as on the ES47 with
  the byte changed). Linux has no parameter that keeps vgacon off, so
  `es47_linux.sh` sets `vgacon_screen_info.orig_video_lines` to 0 in a
  copy of the kernel: `vgacon_startup` takes its "no VGA" path, and the
  console is the dummy one ("Console: colour dummy device 80x25"). A
  graphics console on the Radeon would avoid it (phase 2).
- **`console=srm`, not `srm`**: the SRM console's tty is `srm0`, and the
  live CD's `fixinittab` runs `agetty` on the name the kernel line gives,
  so with `console=srm` its getty respawns until init disables it.
  `console=srm0` works for both (`setup.c` matches the `console=srm`
  prefix).
- **"i8042: i8042 controller selftest timeout"**: Marvel has no keyboard
  controller; Linux's Marvel I/O routines answer port 0x60 and 0x64 with 0
  (`marvel_ioread8`).
- **"FIXME: disabling master aborts"**, once per hose, is Linux's own
  `printk` in `io7_init_hose`.
- **The epoch**: the TOY's year is 26, which `init_rtc_epoch` takes for
  the NT epoch (2006), as on the ES40; `epoch=2000` on the kernel line.
- **The live CD's `/usr/portage` mount fails** ("mount point does not
  exist"), on the ES40 as well.

### What was fixed on the ES47

**A processor kept old code past another one's `IMB`.** Four EV7s
stopped with "Instruction fault 4" at `arch_cpu_idle+0x8`. The EV7 UNIX
PALcode has no `CALL_PAL WTINT` (its slot at 0x42f80 goes to the OPCDEC
path), so the first processor to idle traps on it; Linux overwrites the
call with `mov 0,$0` and runs `IMB` on that processor only (`traps.c`,
`do_entIF`). The boot processor had run `copy_thread`, in the same 2 KB
instruction-cache line, before the patch, executed the old call from that
line, trapped, found the patched word and died. A real 21264 keeps old
code too (its Icache is not coherent, EV68CB/DC HRM 4.5.1), but only in
the 64-byte blocks it fetched. Every processor's `IMB` now drops every
other processor's lines ([cpu-fidelity.md](cpu-fidelity.md)), which is
more coherent than the hardware and allowed. With one and two processors
it had not shown.

**The TOY's register B said binary.** The first boot printed "Using epoch
1980 for rtc year 38", "hctosys: unable to read the hardware clock", and
OpenRC's `hwclock` failed. Linux reads the Marvel TOY through `cserve`
GET_TOY, and `__marvel_rtc_io` (`core_marvel.c`) passes every byte it
returns through `bin2bcd`, register B included; the MC146818 code then
honours register B's DM bit (`RTC_ALWAYS_BCD` is 0, `asm/io.h`). So Linux
decodes the CMM's binary TOY right only if register B says BCD. The CMM
model kept 0x06 there (24-hour, binary): Linux read the BCD 0x26 of 2026
as 38 and the month as 16. Register B is now 0x02, with the values still
binary; OpenVMS reads them as binary and ignores DM (`vms-es47`: SHOW
TIME the host's date and time, as before). The UNIX PALcode's GET_TOY
(0x4acc8 in the decompressed console) returns the CMM's byte as it is.

### What Linux uses of the IO7's interrupt registers (M8)

An `ALPHABOX_TRACE_IO7=1` run (two EV7s, the CD to its shell) lists every
IO7 register Linux and the UNIX PALcode touched, which agrees with
`sys_marvel.c` and `core_marvel.c`:

- **LSI_CTL** (0x310000): at `init_io7_irqs` every implemented one (0-0x5f,
  0x74, 0x75) is written with the boot processor's PID in <22:14> and the
  enable clear, and read back; afterwards `io7_enable_irq` and
  `io7_disable_irq` set and clear <24> with a read back, twice for every
  interrupt (`handle_level_irq` masks and unmasks). The model already did
  this.
- **MSI_CTL[16]** (0x314000): written at init with the PID in <22:14> and
  read back, nothing else. The enable path (`io7_enable_irq` for an IRQ at
  0x80 + 16 or above) runs only for a device whose MSI the console had
  turned on (`marvel_map_irq` reads the device's MSI capability). Alpha
  has no MSI support of its own in Linux (no `arch/alpha` MSI hooks, and
  the CD's kernel has `CONFIG_PCI_MSI` off), so no driver turns one on,
  and the console turns on none. The model holds what is written, which
  is all Linux asks.
- **HLT_CTL, HPI_CTL, CRD_CTL, STV_CTL, HEI_CTL**: read, the target PID put
  in <32:24> (`io7_redirect_irq`), written and read back once.
- **EOI_DAT** of the device's port: written by the UNIX PALcode at the end
  of each interrupt, as by OpenVMS's.
- **Never touched**, by Linux or by the UNIX PALcode: INT_PND, INT_CLR
  and INT_EOI (0x318000-0x319880), MISC_PND and MISC_CLR, MSI_PND and
  MSI_CLR, the MSI window (POx_MSI_WBASE), and PO7_SCRATCH, which the
  OpenVMS PALcode reads as its vector block (M5). Linux names them in
  `core_marvel.h` and reads none.

So M8 needs no change to the model: it closes with this list. The
pending registers stay unmodelled until a guest reads them.

## Open

- Four processors, and why two parallel jobs were slower than they
  should be (above).
- A full Gentoo install from the network. The Tulip works on the UDP
  back end (above); what is missing is a way out to the Internet without
  host privileges, a user-mode NAT back end.
- A kernel built for the machine (Gentoo's `sys-kernel/gentoo-kernel`
  or a hand-built one); the installed system runs the CD's.
- A graphics card's framebuffer driver (the kernel has `tgafb`,
  `radeonfb`, `pm2fb`); the text console works (above).
- SCSI: the kernel has the Symbios driver as a module.
- On the ES47: the console's environment is not kept (no `cmm_nvram.bin`
  is ever written, `set os_type unix` is gone at the next start), and one
  run with four processors lost the fourth when `init` restarted the
  secondaries ("CPU 3 failed to take jump address part 0", the host busy
  at the time). Neither was looked into.
- On the ES47: a graphical console on the Radeon 7500 with the Mesa r100
  driver (phase 2); it would also keep `vgacon` from faulting without the
  patched byte.
