# What is missing, and the plan

The machines' processors and chipsets, as of 2026-10-05 (and unchanged
on 2026-10-19): what is not modelled, what is modelled on an assumption,
and the order in which to close it. All eleven machines boot OpenVMS 8.4;
nothing here stops a guest that runs today.

**No real Alpha is available to this project.** So an item is closed by
what can be had: the consoles' own code, the guests' drivers, published
listings and manuals, and the Linux sources. Where only a real machine
could settle a question, the model keeps its inference, marked as one in
the machine's packet, and the question is *not* carried as open work.

## Is the EV7 a real EV7?

The 21364's core **is** the 21264's (EV68 generation): the same
instruction set, PALcode instructions and internal registers
([marvel.md](platforms/marvel.md), "The EV7 core against the EV68"). Both
of its PALcodes use only 21264 registers, and the core here implements
every one (E1). A second core would be a copy of the first. What makes an
EV7 an EV7 is around the core, and that is where the model is thin:

| Part | State | To close it |
| --- | --- | --- |
| Internal registers the EV7 PALcodes use | done (E1): 45 registers in the OpenVMS PALcode, 43 in the UNIX one, all 21264 registers, none EV7-only; the core implements each with the fields the code uses (marvel.md, "The EV7 PALcodes' internal registers") | nothing left. Index 0x2d (M_FIX), M_CTL<5:4> and VA_CTL<3> stay inferences |
| `AMASK`, `IMPLVER` | IMPLVER is 3, the value the Alpha Architecture Reference Manual gives the 21364 (4th ed., Table D-4); until 2026-10-24 it was the EV68's 2. AMASK is the EV68's: no document lists the 21364's, and neither PALcode nor the console executes either (E1) | AMASK left: an inference |
| UNIX PALcode | runs Linux on the ES47 (E2, 2026-10-24, [linux.md](linux.md)): Gentoo's live CD and the ES40's install, one to four EV7s, the IO7's LSIs with EOI_DAT, the clock, IPIs, the TOY through cserve. It has no `CALL_PAL WTINT`, which Linux patches out itself | done |
| Native PALcode fast paths | none: the ES40's, which mirrored its VMS PALcode V1.98-104 and were never on for EV7, were removed in October 2026 ([cpu-fidelity.md](cpu-fidelity.md)) | **E3**: measure OpenVMS MIPS on the ES47 against the ES40 first; write EV7 paths only if the gap is worth it |
| Cbox, Bbox, Zbox, pads, OCLA registers | read back a configured state; writes kept, nothing acts on them. The PALcodes reach 55 registers, all modelled: at reset, in the whole-cache CFLUSH and in the error logout (E1) | nothing more without a guest that uses them |
| Performance counters, ProfileMe | not modelled (as on the 21264 here) | left |
| Machine checks, correctable errors, logout frames | never raised; error registers read clean | left: recorded in [cpu-fidelity.md](cpu-fidelity.md) |
| Rbox router | route table as OpenVMS reads it; the I/O entries (0x100-0x113) empty; no packets, no port errors | with M9 |
| More than 16 processors | GS1280 row stops at 16 (one host thread each) | M9 |

## Marvel (ES47, ES80, GS1280)

| Gap | To close it |
| --- | --- |
| IO7: MSIs (`MSI_CTL`), `INT_PND`/`INT_CLR`/`MISC_PND` | **M8**, done by Linux (2026-10-24): it writes MSI_CTL with the target PID and reads it back, and enables an MSI only for a device whose MSI the console turned on, which none is (`arch/alpha` has no MSI support, and the CD's kernel has `CONFIG_PCI_MSI` off); the pending registers, MSI_PND/MSI_CLR and POx_MSI_WBASE it never touches, nor does the UNIX PALcode ([linux.md](linux.md)). Nothing to model until a guest reads them |
| IO7: data mover, error reporting | left (no guest uses them; errors are never raised) |
| CMM: FRU EEPROMs, fans, power, VRMs, the console data log | done by **M10** (2026-10-20): answers built from the console's own decoders; `show fru`, `show power` and `boot` are clean on the ES47. The EEPROMs' contents are invented ([marvel.md](platforms/marvel.md), M10) |
| CMM: a GS1280's power supply tray (0x090e), cabinet control panel and PBM EEPROMs | left: only the GS1280 asks; its FRUs print "-" |
| One partition only | M9 |
| One I/O layout per machine: no expansion or high-performance drawers, one IO7 on the GS1280 | M9 |
| ES80 drawer numbers and backplane revision, the GS1280's hose speeds | inference; a published listing would settle it |
| ES47 on-board Adaptec AIC-7892: a Symbios 53C895 stands in | **A1** (with the DS25's AIC-7899) |
| DKA400 shows 2 errors under OpenVMS booted from the CD (one per mount otherwise) | none: the guest's own probe ([below](#the-cd-errors)) |
| A time set by the guest is not kept across a restart (every board: the clock is the host's plus an offset held in memory) | a choice: every start is at the host's time ([below](#the-guests-clock)) |
| The console's ALi M1543C driver: which Marvel I/O has that chip | unknown; nothing depends on it |

### The CD errors

DKA400's 2 errors are the guest's probe: READ DISC INFORMATION (0x51),
which the CD (a DEC RRD42, a SCSI-2 CD-ROM) rejects as a real one would.
The other 2 of the former 4 were the missing control mode page, fixed on
2026-10-05. The ES40 shows the same: the device, not the board
([marvel.md](platforms/marvel.md), M5).

### The guest's clock

The ES47's console network boot and OpenVMS's SET TIME through the TOY
calls work (checked 2026-10-05).

## Titan (ES45, DS25, DS15)

| Gap | To close it |
| --- | --- |
| DS25 Broadcom BCM5703c | **N1**: a new NIC family |
| DS25 AIC-7899: configuration space and ROM only, no SCSI | A1 |
| Hot-plug controllers (ES45, DS25) | **H1**: with the IO7's hot-plug registers as the model |
| Scatter-gather TLB: invalidates ignored | fidelity only: translations are read from the tables every time |
| AGP transactions on the A-port: AGP_EN, the rate and the request queues hold what is written and do nothing | left until a guest turns AGP on: Linux's `titan_agp_*`, with **E2** ([below](#agp-on-the-titan)) |
| CSC (but bit 51) and the Dchips' DSC/STR/DSC2 assumed the Typhoon's | none: nothing reads them that the consoles or OpenVMS show ([below](#the-titans-memory-arrays)) |
| `show fru`, the ES45 model "B" | its own item: the ES45's FRU images are the RMC's, in the DPR; M10 was Marvel's CMM only |

### AGP on the Titan

The ES45 Model 1's AGP slot is there (`es45m1`): the Radeon 7500 in it is
the SRM console, and OpenVMS boots. The console turns AGP off and runs the
card as a PCI device, so nothing has used the A-port's AGP transactions
yet.

### The Titan's memory arrays

The AAR encoding is done. It was read from the consoles' code and checked
with `memory.arrays` for one to four arrays, equal and unequal, on the
ES45, DS25 and DS15, and with OpenVMS 8.4 on four arrays
([titan.md](platforms/titan.md)). Still inferred, with no listing to
settle them:

- the order of the bases among four equal arrays;
- CSC<51> for three arrays.

## Tsunami (ES40, DS20E, DS20, DS10, DS20L)

| Gap | To close it |
| --- | --- |
| UP2000, XP1000 boards | none without their consoles: neither image is on the CD or anywhere in `roms/` (T1, [ds20.md](platforms/ds20.md)) |
| No reference listings for the DS boards (no L3) | published listings if they turn up; otherwise stays |
| The ES40 console prints "4-Way" for every `memory.arrays` layout, unequal arrays included | its own rule, not read; nothing else depends on it |

## The plan

In order. Each step is one agent with one question, merged through the
gate.

1. **Q1, the cheap ones**. Done, nothing left:
   - the 82559ER (`i82559er`, on the DS25's board);
   - DKA400's errors (the control mode page; the two left are the guest's
     probe);
   - network boot on the ES47 (`boot ewa0`: BOOTP and TFTP);
   - the loopback plug (`type = "loopback"` on any NIC: the console's
     `nettest` passes in every mode on the DS10 and the ES40);
   - AGP on the ES45 (the Model 1 backplane, `es45m1`);
   - the memory arrays (`memory.arrays`, Tsunami and Titan). Three and
     four arrays were checked on five consoles, which found the DS20E,
     DS10 and DS20L consoles reading arrays to 1 GB only: their 2 and 4 GB
     are now two and four arrays, and the DS20L row stops at 2 GB.
2. **E1, the PALcode audit**. Done:
   - `ev7_pal_iprs.py` lists every internal register and field both EV7
     PALcodes touch. Each is a 21264 register the core implements, so the
     core did not change; the table is in marvel.md.
   - OpenVMS 8.4 boots to DCL on the ES47, and the UNIX PALcode runs from a
     boot image (`unix_pal_probe.py`), neither with an unknown register.
3. **E2 + M8, Linux on the ES47**: with no hardware, a second operating
   system is the best test there is. It runs the UNIX PALcode, a second
   interrupt path and the MSIs, and on the Radeon the real Mesa r100
   driver. Linux on the ES40 first (the easy half), then Marvel. The
   ES40 half runs ([linux.md](linux.md), 2026-10-20): Gentoo's CD to a
   shell, an install that boots from disk on one and on two processors,
   and four emulator bugs fixed on the way. The ES47 half runs too
   (2026-10-24): the CD's live shell on one and two EV7s and the ES40's
   install on two and on four (the GS1280 row; a two-drawer ES47 is past
   what the CD's kernel can map), with the CD's Marvel kernel and one byte
   changed for a Linux bug. Two emulator bugs fixed: the TOY's register B,
   and instruction-cache lines that outlived another processor's IMB. M8
   needed no change. Left: the Radeon with Mesa's r100 driver (phase 2).
4. **E3**: OpenVMS MIPS on the EV7 against the ES40, and native PALcode
   paths if the gap is large.
5. **M10, the CMM's FRU and environment answers**. Done (2026-10-20):
   - `show fru` lists every ES47 FRU that has an EEPROM with part and
     serial numbers (the RIMMs' part numbers are the console's own, from
     their presence detect); VRMs, power supplies and fans are reported;
   - `boot` prints no management-interface error, and the system serial
     number is set (it cannot be set from the prompt);
   - `show power` prints nothing, as on a real ES47: it is a null routine
     in this console.
6. **M9**: partitions, I/O drawers, several IO7s on the GS1280, 32
   processors, the route table's I/O entries.
7. **T1**, DS20, UP2000, XP1000. Done as far as the media goes:
   - the DS20 (`ds20`) runs the DS20E's console, which calls it a DS20
     when the operator panel's PCF8574 is absent from the I2C bus; it is
     at L5, OpenVMS 8.4 from its CD and an installed disk on one and two
     processors ([ds20.md](platforms/ds20.md));
   - the UP2000 and XP1000 are not started: neither console image is on
     the CD (it has no directory for either) or anywhere in `roms/`. Each
     can start when its image is copied in from media the owner has.
8. **A1, N1, H1**, each its own decision: the Adaptec AIC-78xx sequencer
   (about 5.8k lines by the last estimate), the BCM5703c, the hot-plug
   controllers. No guest needs them: OpenVMS runs on the stand-ins.

Not planned: machine checks and error injection, performance counters,
the IO7's data mover, and anything only a real machine could confirm.
