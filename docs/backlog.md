# What is missing, and the plan

The machines' processors and chipsets, as of 2026-10-05: what is not
modelled, what is modelled on an assumption, and the order in which to
close it. All ten machines boot OpenVMS 8.4; nothing here stops a guest
that runs today.

**No real Alpha is available to this project.** So an item is closed by
what can be had: the consoles' own code, the guests' drivers, published
listings and manuals, and the Linux sources. Where only a real machine
could settle a question, the model keeps its inference, marked as one in
the machine's packet, and the question is *not* carried as open work.

## Is the EV7 a real EV7?

The 21364's core **is** the 21264's (EV68 generation): the same
instruction set, PALcode instructions and Ibox/Mbox register numbers
([marvel.md](platforms/marvel.md), "The EV7 core against the EV68"). Its
PALcode's reset path runs on the core here without an unknown register.
A second core would be a copy of the first. What makes an EV7 an EV7 is
around the core, and that is where the model is thin:

| Part | State | To close it |
| --- | --- | --- |
| Internal registers the EV7 PALcodes use | only what the VMS PALcode's paths touched has run | **E1**: scan both PALcode images (VMS V2.11-25, UNIX V2.08-19) for every `HW_MFPR`/`HW_MTPR` index and field, and check each against the core |
| `AMASK`, `IMPLVER` | assumed the EV68's; no document lists them | E1: what the PALcode and the console test; otherwise stays an inference |
| UNIX PALcode | never run: no UNIX guest on an EV7 | **E2**: Linux on the ES47 (below) |
| Native PALcode fast paths | off on EV7 (they mirror the ES40's VMS PALcode V1.98-104) | **E3**: measure OpenVMS MIPS on the ES47 against the ES40 first; write EV7 paths only if the gap is worth it |
| Cbox, Bbox, Zbox, pads, OCLA registers | read back a configured state; writes kept, nothing acts on them | E1 shows which the console reads; nothing more without a guest that uses them |
| Performance counters, ProfileMe | not modelled (as on the 21264 here) | left |
| Machine checks, correctable errors, logout frames | never raised; error registers read clean | left: recorded in [cpu-fidelity.md](cpu-fidelity.md) |
| Rbox router | route table as OpenVMS reads it; the I/O entries (0x100-0x113) empty; no packets, no port errors | with M9 |
| More than 16 processors | GS1280 row stops at 16 (one host thread each) | M9 |

## Marvel (ES47, ES80, GS1280)

| Gap | To close it |
| --- | --- |
| IO7: MSIs (`MSI_CTL`), `INT_PND`/`INT_CLR`/`MISC_PND` | **M8**, driven by Linux, whose `sys_marvel.c` uses them |
| IO7: data mover, error reporting | left (no guest uses them; errors are never raised) |
| CMM: FRU EEPROMs, fans, power, VRM are stubs; `get_cdl_error` answers status 1, which the console prints at `boot` | **M10**: answers built from the console's own decoders, until `show fru`, `show power` and `boot` are clean |
| One partition only | M9 |
| One I/O layout per machine: no expansion or high-performance drawers, one IO7 on the GS1280 | M9 |
| ES80 drawer numbers and backplane revision, the GS1280's hose speeds | inference; a published listing would settle it |
| ES47 on-board Adaptec AIC-7892: a Symbios 53C895 stands in | **A1** (with the DS25's AIC-7899) |
| DKA400 shows 2 errors under OpenVMS booted from the CD (one per mount otherwise) | the guest's probe: READ DISC INFORMATION (0x51), which the CD (a DEC RRD42, a SCSI-2 CD-ROM) rejects as a real one would. The other 2 of the former 4 were the missing control mode page, fixed. Same on the ES40: the device, not the board. See marvel.md, M5 |
| A time set by the guest is not kept across a restart (every board: the clock is the host's plus an offset held in memory) | a choice: every start is at the host's time. The ES47's console network boot and OpenVMS's SET TIME through the TOY calls work (checked 2026-10-05) |
| The console's ALi M1543C driver: which Marvel I/O has that chip | unknown; nothing depends on it |

## Titan (ES45, DS25, DS15)

| Gap | To close it |
| --- | --- |
| DS25 Broadcom BCM5703c | **N1**: a new NIC family |
| DS25 AIC-7899: configuration space and ROM only, no SCSI | A1 |
| Hot-plug controllers (ES45, DS25) | **H1**: with the IO7's hot-plug registers as the model |
| Scatter-gather TLB: invalidates ignored | fidelity only: translations are read from the tables every time |
| AGP transactions on the A-port: AGP_EN, the rate and the request queues hold what is written and do nothing | the ES45 Model 1's AGP slot is there (`es45m1`, the Radeon 7500 as SRM console; OpenVMS boots), and the console turns AGP off and runs the card as a PCI device. Left until a guest turns it on: Linux's `titan_agp_*`, with **E2** |
| CSC, Dchip and AAR encodings assumed the Typhoon's | what `memory.bits` can describe matches (ES45, DS25 to 8 GB: one array; DS15: one and two). Three and four arrays, and unequal ones, cannot be configured: **Q1**, a key for the arrays' sizes (Tsunami and Titan), then the check |
| `show fru`, the ES45 model "B" | with M10's FRU work |

## Tsunami (ES40, DS20E, DS10, DS20L)

| Gap | To close it |
| --- | --- |
| DS20, UP2000, XP1000 boards | **T1**: board rows; their firmware is on the CD |
| No reference listings for the DS boards (no L3) | published listings if they turn up; otherwise stays |

## The plan

In order. Each step is one agent with one question, merged through the
gate.

1. **Q1, the cheap ones**. Done: the 82559ER (`i82559er`, on the DS25's
   board); DKA400's errors (the control mode page; the two left are the
   guest's probe); network boot on the ES47 (`boot ewa0`: BOOTP and TFTP);
   the loopback plug (`type = "loopback"` on any NIC: the console's
   `nettest` passes in every mode on the DS10 and the ES40); AGP on the
   ES45 (the Model 1 backplane, `es45m1`).
   Left: a key for a board's memory arrays, then three and four arrays on
   the Titan.
2. **E1, the PALcode audit**: a script lists every internal register and
   field both EV7 PALcodes touch; each is checked against the core. The
   result is a table in marvel.md and fixes for what differs.
3. **E2 + M8, Linux on the ES47**: with no hardware, a second operating
   system is the best test there is. It runs the UNIX PALcode, a second
   interrupt path and the MSIs, and on the Radeon the real Mesa r100
   driver. Linux on the ES40 first (the easy half), then Marvel.
4. **E3**: OpenVMS MIPS on the EV7 against the ES40, and native PALcode
   paths if the gap is large.
5. **M10**: the CMM's FRU and environment answers.
6. **M9**: partitions, I/O drawers, several IO7s on the GS1280, 32
   processors, the route table's I/O entries.
7. **T1**: DS20, UP2000, XP1000.
8. **A1, N1, H1**, each its own decision: the Adaptec AIC-78xx sequencer
   (about 5.8k lines by the last estimate), the BCM5703c, the hot-plug
   controllers. No guest needs them: OpenVMS runs on the stand-ins.

Not planned: machine checks and error injection, performance counters,
the IO7's data mover, and anything only a real machine could confirm.
