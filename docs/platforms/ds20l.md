# AlphaServer DS20L work packet

The third machine, and the first brought up the way a real one is: by
running its own firmware update utility, which installs the console into
the flash, and then booting what it installed.

**Branch**: `platform/ds20l` · **Config**: `platform = "ds20l";`
· **Status**: L2 (console prompt reached)

## The machine

| | |
| --- | --- |
| Family, code name | Tsunami family; Linux calls this board "Shark" and drives it with the ES40's interrupt table ("Sharks strongly resemble Clipper") |
| CPU | up to two (assumed); the EV68CB row is used, and the console reports 800 MHz |
| Chipset | Tsunami/Typhoon 21272, two PCI buses |
| Memory | up to 4 GB (assumed) |
| PCI | slots not yet established; the ES40's wiring is used, marked assumed |
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

## Plan

| # | Item | Level | Status |
| --- | --- | --- | --- |
| 1 | Raw (headerless) image form, and finding a console installed in flash | L1 | done |
| 2 | Board row | L1 | done, slots and interrupts assumed |
| 3 | The machine's real slots and interrupt wiring, from its own assignments | L3 | open |
| 4 | Its device set: what belongs on the board rather than the ES40's | L3 | open |
| 5 | Console listings against a reference | L3 | blocked: no reference |
| 6 | Console tests, guest boot | L4-L5 | open |

## Rules

As in [TEMPLATE.md](TEMPLATE.md).
