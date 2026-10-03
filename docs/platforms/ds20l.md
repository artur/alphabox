# AlphaServer DS20L work packet

The third machine, and the first brought up the way a real one is: by
running its own firmware update utility, which installs the console into
the flash, and then booting what it installed.

**Branch**: `platform/ds20l` · **Config**: `platform = "ds20l";`
· **Status**: L5 (OpenVMS 8.4 boots an installed disk to login, on one and
two processors); L3 not claimable without a reference listing

## The machine

| | |
| --- | --- |
| Family, code name | Tsunami family; Linux calls this board "Shark" and drives it with the ES40's interrupt table ("Sharks strongly resemble Clipper") |
| CPU | up to two; the EV68CB row is used, and the console names it "Alpha 21264C-6 833 MHz (EV68CB pass 4.0)". Both processors are found and run OpenVMS |
| Chipset | Tsunami/Typhoon 21272, two PCI buses |
| Memory | up to 4 GB (assumed) |
| PCI | the console gives interrupt lines to hose 0 devices 3 to 6 and hose 1 devices 3 to 5 (see Findings); it scans from device 3 and finds the ALi functions at the ES40's 7, 15 and 19 |
| Board hardware | not investigated: the console runs without it |

## Firmware

`roms/alpha-firmware-v7.3/DS20L/DS20L_V6_6.EXE` is not a console image but
this machine's **firmware update utility**, with the console inside it. It
has no header at all: it starts directly with the self-decompressor, which
is the `FW_RAW_IMAGE` form. The other machines on the CD (DS15, DS25, ES45
and the rest) ship the same way.

Running it once installs the console, exactly as on a real machine:

```
# 1. the update utility, from the CD image
PLATFORM=ds20l ROM=roms/alpha-firmware-v7.3/DS20L/DS20L_V6_6.EXE ...
#    answer its questions: <return>, then "update", then Y
# 2. stop the emulator; the flash is saved
# 3. boot again in the same directory, with no ROM= :
#    the console is found in the flash and started
```

The utility reports `srm Updating to 6.6-10... Verifying 6.6-10... PASSED`,
and writes a standard ROM-header image at flash offset 0x90000 (681,984
bytes, loading at 0x900000). Alphabox now searches the flash for such an
image rather than expecting the ES40's partition layout, so the next boot
starts the installed console.

## Findings

**It works.** The console reaches `P00>>>` and names itself correctly:

```
hp AlphaServer DS20L 800 MHz Console V6.6-10, Nov 11 2003 09:10:13
```

That is worth noting against the DS20E, which calls itself "AlphaPC 264DP"
because it cannot read the machine code it picks its name by: this console
either does not need it or reads something we do provide.

**On the way it says:**

- `unable to assign PCI base address ... bus 0, slot 19 ... size ffff1000
  (sparse)` -- the USB function of the ALi bridge in the test configuration
  asks for a window this machine's console will not place. Harmless here;
  the machine's own device set is not established yet.
- `ERROR: ISA table corrupt! Initializing table to defaults` on the first
  boot only, which is what a machine with an empty flash says.
- `system serial number not set`, as on the DS20E.

### Interrupt lines and OpenVMS 8.4 (2026-10-02, L5)

Transcripts in `lab/platforms/ds20l/`. The console comes from the flash
the update utility wrote (`lab/ds20l-lfu/flash.rom`, V6.6-10); runs copy
it (`FLASH=` in `lab/platforms/tsu/tsu_srm.sh`).

**This console has no interrupt table** like the DS20E's and the DS10's
(none was found in its memory image). What it does is visible in the line
it writes into each card's configuration space: DE500s at every device
number of both hoses got

| | 3 | 4 | 5 | 6 | others found |
| --- | --- | --- | --- | --- | --- |
| hose 0 | 0x10 | 0x14 | 0x18 | **0x1f** | 8-12: none |
| hose 1 | 0x20 | 0x24 | **0x2b** | none | 7-10: none |

Devices 0 to 2 were not found on either hose. Most of this is the ES40's
formula, (device + 1) * 4 + 16 * hose, as Linux says; the two marked
entries are the fourth input of the device's group rather than the first,
so those two places are wired differently on this board. The row follows
the console for pin A. **Pins B to D of those two devices are a guess**
(the group's remaining inputs in turn), and the slots are those the console
gives lines to; which of them are on-board devices and which are slots is
not known.

**OpenVMS with NICs at the four kinds of position** (hose 0 devices 3 and
6, hose 1 devices 3 and 5), the same receive check as the DS20E's: every
NIC counted 98-100 frames in 50 s (`tsu-ds20l-nic-1cpu`). With the ES40's
wiring the two odd positions would have been raised on an input OpenVMS
was not told about.

**OpenVMS 8.4** boots the disk installed on the ES40 (`boot dqa0`; the
ALi IDE at device 15 as on the ES40) to the SYSTEM login: startup done at
17 s, `F$GETSYI("HW_NAME")` = `hp AlphaServer DS20L 833 MHz`. With two
processors `CPU #1 has joined the active set` and `SHOW CPU` lists 0 and 1
active (`tsu-ds20l-2cpu`).

OpenVMS warns: `%SYSBOOT-W-FIRMREV, Firmware rev. 6.6 is below the
recommended minimum of 6.8`. V6.6 is the newest DS20L console on the
firmware CD V7.3, so this is a limit of the media we have, not something to
work around; nothing failed because of it so far.

The second processor is released when processor 0 clears the Cchip
arbitration (this console does it at PALcode 0x136f5), as on the DS20E.
Four repeated two-processor boots on the JIT lane all logged in
(`tsu-rep-ds20l.txt`).

**The interpreter must run this console's real PALcode.** On the
interpreter lane, two-processor boots bugchecked (`INVEXCEPTN` on CPU 1 in
SYSINIT, 3 of 3, with and without NICs) until the vmspal routines were
switched off: with `palcode.vms.nohle = true` the same boot logged in. The
native routines replace the ES40 console's PALcode and were applied to any
PALcode at 0x8000; this console's is a different build (V1.98-74). The
board row now says so (`native_vmspal = false`; since the chipset split
`vmspal_pal_base = 0`), and the boot logs in on
the interpreter with no options (`tsu-ds20l-2cpu-int-fix`). The DS20E had
the same problem (its packet).

**Network boot (L4)** works with a DE600 at hose 0 device 3: the power-up
`Testing ei* devices.` passes, and `boot eia0 -protocols bootp` gets its
address from `net_peer.py`, transfers the image over TFTP and runs it to
its HALT (`tsu-ds20l-net-ei`). A DE500 does not work with this console:
`show config` lists `ewa0.0.0.3.0` with no station address (all eight
DE500s of the slot probe too) and `boot ewa0` answers `device ewa0 is
invalid` (`tsu-ds20l-net`), although OpenVMS drives DE500s on this board.

**Why a DE500 does not work (2026-10-03): this console has no DE500
driver.** Its driver table (at 0x13b400 in memory: procedure descriptor,
name, start phase) lists `ei` and `eg` among the phase-5 drivers but no
`ew`, where the DS10's V7.3-1 console has `ew` between `isp1020` and `ei`;
and the tulip driver's own strings (`TULIP CSRS:`, `Edit 21143 EEROM
parameters.`) are absent from the image. The DE500 names come from the
console's shared PCI name table, which is why the card is named and given
a device name without anything to start it: the console never touches its
registers (a `DEBUG_NIC` build saw no CSR access at all). Nothing in the
emulation is missing: a real DS20L with this firmware would do the same.
OpenVMS has its own driver, so DE500s work under OpenVMS. For network boot on this board use a DE600
(`de600`, or another 8255x).

The ES40's console speed patches were applied to this console too, until
2026-10-03: in this image `0x68320` is a routine's `ret`, `0x8bc94` a
`ldq gp` restoring the caller's frame pointer and `0x8bb78`/`0x8bc0c`
arithmetic. The row no longer patches it (see the DS10 packet, where one of
them broke the power-up network test). The console still reaches `P00>>>`,
passes `Testing ei* devices.` and network-boots from a DE600 without them.

Not done here: a CD boot (the CD is the same media the DS20E and DS10 boot
from).


| # | Item | Level | Status |
| --- | --- | --- | --- |
| 1 | Raw (headerless) image form, and finding a console installed in flash | L1 | done |
| 2 | Board row | L1 | done, slots and interrupts assumed |
| 3 | The machine's real slots and interrupt wiring, from its own assignments | L3 | done for pin A, checked with OpenVMS; pins B-D of two devices guessed |
| 4 | Its device set: what belongs on the board rather than the ES40's | L3 | open |
| 5 | Console listings against a reference | L3 | blocked: no reference |
| 6 | Console tests, guest boot | L4-L5 | L4: network boot with a DE600 (the V6.6-10 console has no DE500 driver); L5: OpenVMS 8.4 to login, 1 and 2 CPUs |

## Rules

As in [TEMPLATE.md](TEMPLATE.md).
