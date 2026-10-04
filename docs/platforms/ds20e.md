# AlphaServer DS20E work packet

Pilot packet: the first machine after the ES40, chosen because it shares the
chipset and the CPU family, so almost everything it exercises is the
machine-independent structure rather than new hardware. See
[platforms.md](../platforms.md) for the layers and the acceptance ladder.

**Branch**: `platform/ds20e` (own worktree) · **Config**: `platform = "ds20e";`
· **Status**: L5 (OpenVMS 8.4 boots from its CD and from an installed
disk, on one and two processors); L3 not claimable without a reference
listing

## The machine

| | |
| --- | --- |
| Family, code name | Tsunami family. "Goldrack" per a hardware listing (assumed); Linux's Tsunami variation table has DP264, Monet, Clipper (= ES40), Goldrush, Webbrick (= DS10), Shark (= DS20L) and no Goldrack. **Which variation this firmware reports, and therefore which board wiring it expects, is open question 1 below.** |
| CPU | 1-2, EV6 500 MHz, EV67 667 MHz or EV68AL 833 MHz (assumed, from a hardware listing). Alphabox has EV68CB only, so this packet adds at least one CPU model row |
| Chipset | Tsunami/Typhoon 21272, as the ES40 (known: same family; whether this board has one or two Pchips is open question 2) |
| Memory | up to 4 GB (assumed) |
| PCI | 6 slots (assumed); how they map to hoses and which interrupt bits they use is open question 2 |
| South bridge | assumed an ALi M1543C as on the ES40; the original DS20 used a different part, so this must be confirmed from the firmware |
| Console devices | serial console; VGA where fitted |
| Board hardware | flash, and whatever the firmware expects in place of the ES40's management processor and FPGA registers: unknown, and the main discovery work |

Everything above marked assumed is a starting hypothesis, not a fact. The
firmware decides.

## Firmware

- Image: `roms/alpha-firmware-v7.3/DS20/PC264SRM.ROM` (Alpha firmware CD
  V7.3, directory `DS20`, which serves DS20 and DS20E). 735 KB, standard
  Alpha ROM header (`c3c3 5a5a 3c3c a5a5`, 0x38 bytes, destination
  0x900000), SRM console V7.3-1, OpenVMS PALcode V1.98-79, Tru64 PALcode
  V1.92-74 (from `DS20/FWREADME.TXT`).
- Also present: `PC264NT.ROM` (AlphaBIOS 5.71), `PC264FSB.ROM` (fail-safe
  booter), `DS20_V7_3.EXE` (the update bundle). Not needed for L2.
- Alphabox loads the ES40's bundle format today; this raw form needs a
  loader that honours the ROM header. The destination address matches the
  one the ES40 path already uses, so the self-decompression step should be
  reusable.

## Sources

- Linux `arch/alpha/kernel/sys_dp264.c`: the per-board interrupt tables
  (`dp264_map_irq`, `monet_map_irq`, `webbrick_map_irq`, `clipper_map_irq`)
  and machine vectors; `arch/alpha/kernel/setup.c` for the Tsunami variation
  names; `core_tsunami.c` for the chipset.
- NetBSD `sys/arch/alpha/pci/pci_6600.c`: takes the interrupt line the
  console assigned rather than a board table -- a guest that behaves this
  way will work as soon as the console's own numbering is right.
- The ES40 code in Alphabox, which is the nearest machine.
- DS20E owner's and service documentation for the slot and connector layout.

## Reference output

`test/platforms/ds20e/` : `show config`, `show memory`, `show device` from a
real DS20E. **Not yet obtained** -- without it L3 cannot be claimed
honestly. Sources to try, in order: documentation that quotes a console
session, a recorded session from a real machine, or (weakest, and marked as
such) the structure of the ES40 listing with DS20E slot names.

## Plan

| # | Item | Level | Status |
| --- | --- | --- | --- |
| 1 | Raw ROM-header image loader, selected by the platform descriptor | L1 | done |
| 2 | `platform = "ds20e"` descriptor: CPUs, memory limits, slots, firmware | L1 | done (slots and interrupts still the ES40's, assumed) |
| 3 | CPU model row(s): EV67 and/or EV68AL identity values | L1 | not needed yet: the firmware accepts the EV68CB row |
| 4 | Run with the unknown-access trace; catalogue what the firmware touches that the ES40 model does not provide | L1 | done, see Findings |
| 5 | Second processor: this firmware does not find one | L2 | done: it finds both |
| 6 | Processor SROM data: the revision, the cache size, and probably the machine's own name | L2 | open, investigated |
| 7 | The bytes PALcode writes at PCI memory 0xfff80000 | L2 | answered: a diagnostic display, nothing reads it back |
| 8 | Interrupt wiring: the slot-to-interrupt-bit map this firmware expects | L3 | done: the console's own table, confirmed with OpenVMS (see Findings, 2026-10-01) |
| 9 | Why the IDE and USB functions are not listed | L3 | answered: the console looks no further than device 10 |
| 10 | Console listings compared with the reference | L3 | blocked: no reference yet |
| 11 | Console tests: network boot with `net_peer.py`, disk boot, `test` | L4 | done: network boot, SCSI listing, disk boot of OpenVMS |
| 12 | Guest boot | L5 | done: OpenVMS 8.4 CD to its menu, installed disk to login, 1 and 2 CPUs |
| 13 | ES40 regression sweep and JIT cross-check | L6 | done (2026-10-02, at the DS10 commit): srm_run.sh diff clean on the interpreter and JIT lanes; a JIT_VERIFY lane diff clean with 0 mismatches in 116.5 M compiled-block executions, and 0 in 214.5 M on a two-processor DS20E console; the ES40 Windows 2000 guest (`win2k-installed`, `es40-window.cfg`, win_bench.sh 300 s) reaches its desktop, idle at 1.7 % host CPU, no warnings |

## Open questions

1. **Which Tsunami variation is this firmware?** Decide from evidence, not
   from a name: read the machine type and variation the firmware records for
   the operating system at boot, and compare the interrupt bits it programs
   per slot against the Linux tables. Record the answer in **Findings**.
2. **One hose or two, and what is in each slot?** The ES40 has two; the
   listing the firmware prints at L3 will say. Until then, do not copy the
   ES40's slot table silently -- copy it and mark it assumed.
3. **What replaces the ES40's management processor and FPGA registers?**
   Likely different, possibly absent. The trace answers this.

## Findings

**The firmware runs, first try (2026-09-17).** With the ROM-header loader,
the `ds20e` descriptor and the existing ES40 hardware model,
`PC264SRM.ROM` reaches `P00>>>`. That settles the shape of the work: this
board is close enough to the ES40 that the remaining items are differences,
not a bring-up.

**It calls itself "AlphaPC 264DP 800 MHz", console V7.3-1**, with OpenVMS
PALcode V1.98-79 and Tru64 PALcode V1.92-74 -- the revisions the CD's
`FWREADME.TXT` lists for DS20/DS20E. So this image is the DP264 firmware,
the design the DS20 and DS20E are built on, which is evidence for open
question 1 but not an answer: the machine type and variation it records for
the operating system still has to be read out.

**The processor row was accepted**: the console prints "Alpha EV68CB pass
4.0 800 MHz" from our EV68CB identity, so an EV67 or EV68AL row is a
refinement, not a prerequisite.

**Differences from the ES40 already visible:**

- "Bcache is disabled", where the ES40 reports 8 MB.
- "SROM Revision:" prints rubbish, and the console says `file open failed
  for iic_cpu0`: it wants per-processor SROM data this machine does not have.
- The floppy is `dva0.0.0.0.0`, not the ES40's `dva0.0.0.1000.0`.
- `show config` lists the M1543C bridge at hose 0 slot 7 but neither the IDE
  nor the USB function that the configuration provides at slots 15 and 19.
- A second configured processor is not found: no "CPU 1" line, and the
  console never starts on it. Processor presence is discovered differently
  here than on the ES40.
- The TIG reports revision 7.30 and an arbiter revision appears, neither of
  which the ES40 listing has.

**Accesses nothing claims** (`ALPHABOX_TRACE_UNKNOWN=1`): the empty-slot
configuration reads any bus scan makes, and byte writes at PCI memory
`0xfff80000` and `+1`.

Those writes are **this machine's I2C bus controller** -- a PCF8584, the
same part the DS10 has at `0xffff0000`. This packet guessed twice before
getting there: first "flash", then "a diagnostic display", the second
because the writes come from PALcode and nothing read them back. What
settled it was the DS10 packet, where the console's own code named the
part: the byte values are its initialisation sequence (0x80 and 0x00 to
the control register, 0x5b its own address, 0x20, the clock 0x15, then
0xc0), and PALcode writes them because PALcode initialises the bus.

With the controller modelled, the console stops writing into nothing and
starts a bus: it addresses parts at 0x27, 0x4f and 0x60 to 0x67, the same
block of serial ROMs the DS10 reads.

**The second processor: found, and fixed (2026-09-17).** Tracing the
registers a console uses to bring processors up (`ALPHABOX_TRACE_MP=1`)
showed this console *does* try: it writes the halt register for processor 1
(TIG `0x300005c0`, bit 1) and its handshake register, then gives up. Our
second processor was parked waiting to be started, so nothing answered.

The ES40's console starts its processors itself through the management
processor, and they wait until it does. This board has none: every
processor must already be running PALcode when the console asks. Releasing
the secondary at the PALcode reset entry -- where the ES40's management
processor puts one too -- makes it answer, and `show config` lists both
processors. That is now a board property (`console_starts_secondaries`;
since the chipset split `secondaries = SECONDARIES_AFTER_ARBITRATION`),
not a guess: the console's own attempt is the evidence.

Three things were ruled out along the way:

- *The I2C bus is untouched.* With `ALPHABOX_TRACE_I2C=1` this console
  never addresses the chipset's I2C bus at all, so neither the processor
  data it wants (`iic_cpu0`) nor processor presence arrives that way.
- *The processor does run.* Letting the second processor run from reset
  instead of waiting to be started (which is how the ES40's management
  processor brings it up, and this board has none) gets it executing --
  `*** CPU1 *** STARTING ***` -- but the console still lists one processor.
  That experiment was reverted: it changed nothing and had no evidence
  behind it.
- *It is not the unmodelled TIG registers.* The console reads TIG
  `0x38000140` four times and `0x380001c0` twice and writes `0x38000100`;
  feeding those reads 0xff instead of 0 changes nothing.

Processor discovery turned out to be the halt-line handshake above.

**How to read this firmware** (the technique, for the other open items).
The trace names the instruction and the return address of an access nothing
claims:

```
%SYS-T-UNKNOWN: read 8 bits at 00038000140 (TIG register) from cpu0 pc=...1b5018 ra=...82da0
```

Those land in access helpers: `0x1b5014` reads a quadword, `0x1b50ac`
writes one, and `0x82d60`/`0x82db8` are the TIG wrappers, which build the
address as the TIG base plus the register index times 0x40 (so index
0xe00004 is register `0x38000100`). Searching the decompressed image for
the instruction that loads a given index finds the callers: `0x38000100` is
written with 1 at `0x7f728` and with 0 at `0x81cfc`. The emulator writes
the decompressed console out (`rom.decompressed`), the image starts 16
bytes into that file at address 0, and `lab/alphadis.py` disassembles it.

**Where this machine's devices live, and how they interrupt
(2026-09-17).** The console scans PCI device numbers 0 to 10 and no
further, which is why the IDE and USB functions at 15 and 19 never
appeared: they are outside the machine's own numbering, not broken. A SCSI
controller at device 8 is found and listed.

Its interrupt wiring is not the ES40's. The board's devices sit at device
numbers 5 to 10 (5 the ISA bridge, 6 the board's own SCSI, 7 to 10 the
slots) and their pins reach chipset interrupt inputs counting down from 15
as the device number rises. The console confirms it: it assigns a
controller at device 8 pin A interrupt 0x1b (input 16+11), exactly what the
table gives, and Linux carries the same table as `dp264_map_irq`. With the
ES40's wiring in place the controller was found but its disk never
appeared; with this one, `show device` lists the disk (`dka0`, an RZ58).

**Network boot works (L4).** With a DE600 at device 9 on the UDP backend,
`boot eia0 -protocols bootp` gets its address, transfers the image over
TFTP and runs it to the halt, the same as on the ES40.

**The machine's own name is data in the firmware (2026-09-17).** The
console's string table holds a table of machines, each a name and a code:
"AlphaPC 264DP" (0x72e), "AlphaServer DS20" (0x730, 0x780), "COMPAQ
AlphaServer DS20E" (0x781, 0x793, 0x794, 0x795, 0x7be, 0x7bf) and "COMPAQ
AlphaStation DS20E" (0x796, 0x797, 0x798, 0x7ac). So one firmware serves
all of them and picks by a code it reads somewhere; ours falls to the
first entry, which is why it says "AlphaPC 264DP".

**Where that code comes from: the I2C bus** (2026-09-18). The earlier
search ruled out the Dchip, the Cchip, the TIG registers and the console's
environment, and concluded "the I2C bus is never used" -- which was true
only because the bus controller was not modelled, so the console's writes
went nowhere. With the PCF8584 in place and parts answering, the console
reads a machine code and names itself a DS20E variant ("COMPAQ
AlphaStation DS20E") instead of falling back to "AlphaPC 264DP".

**That name is not yet evidence.** The serial ROMs are erased parts (0xff
throughout), as on the DS10, because what a real machine records in them
is unknown; and the two parts at 0x27 and 0x4f are not known to be serial
ROMs at all -- erased ROMs stand in so the bus answers. Which DS20E
variant the console names therefore follows from the stand-in data, not
from anything established. A real machine's I2C contents would settle it.

Still unfixed, and probably from the same source: the garbled "SROM
Revision", the "file open failed for iic_cpu0", and the cache reported
disabled.

**What the garbled SROM revision actually is (2026-09-17).** The console
prints three bytes, `a8 ca 1c`. Dumping guest memory
(`ALPHABOX_DUMP_MEMORY=1`) finds them at address `0x1ccaa8`: the quadword
there is `0x001ccaa8`, a pointer to itself, with the same value again at
`+4`. That is an empty list -- head and tail pointing at themselves -- and
the console is printing the bytes of the head as if they were the
revision string.

So this is not a register returning rubbish: it is a list of per-processor
records that nothing fills. On a real machine the processor's serial ROM
leaves those records behind, which is the same source as the cache size the
console reports and very likely the machine code that picks its name. To go
further, the record's format has to come out of the console's own code --
a longer chase than the earlier items, and one that buys three cosmetic
lines. It is left here, with the address and the mechanism recorded, for
whoever wants it.

**Console settings do persist**: `set`, `init`, `show` keeps the value, so
this machine's non-volatile storage already works.

**Not yet proven:** everything above is the console's own account of itself.
Without the reference listing from a real DS20E, L3 is not claimed.

### OpenVMS 8.4 (2026-10-01, L5)

Transcripts are kept in `lab/platforms/ds20e/` (git-excluded): each run's
`es40.cfg`, `console.log` and `result.txt`. The driver is
`lab/platforms/tsu/tsu_srm.sh` with `tsuboot.py` (boot, date prompt,
login, DCL lines).

**The layout that works.** The console scans devices 0 to 10, so the ALi
functions go where it looks: the ISA bridge at `pci0.5` and the IDE at
`pci0.6`, the board's own device (the USB function has no place and is left
out). The console then lists `dqa0.0.0.6.0` and `dqa1.1.0.6.0`, and both
OpenVMS media boot from them. This is our placement, not the real board's:
the real DS20E's south bridge is believed to be a Cypress 82C693 (the
console carries its name and its IDE's), which Alphabox does not model.
The console complains `Vector allocation failed for hose 0, bus 0, slot 6,
pin 1, irq 19` because the IDE's two channels share one device and line;
it is harmless (OpenVMS drives the IDE on ISA IRQ 14 and 15).

**What boots** (JIT lane, 1 GB):

- the distribution CD, `boot dqa1`: banner at 20 s, the date prompt at
  52 s, the installation menu at 81 s (`tsu-ds20e-cd`);
- the installed system disk from `lab/vms84` (installed on the ES40),
  `boot dqa0`: startup done at 16 s, SYSTEM logged in at 32 s. OpenVMS calls
  the machine `COMPAQ AlphaStation DS20E 833 MHz` (`F$GETSYI("HW_NAME")`,
  SYSTYPE 34) and accepts the disk as it is (`tsu-ds20e-disk`);
- two processors: `%SMP-I-CPUTRN, CPU #1 has joined the active set`,
  `SHOW CPU` lists 0 and 1 active (`tsu-ds20e-2cpu`).

**The interrupt map, from the console's own table.** `PC264SRM.ROM`
V7.3-1 carries one table of interrupt lines, a byte per pin for devices 5
to 10 of each hose, at 0x156be0 of the decompressed image:

```
hose 0: dev 5 ff ff ff ff, 6 13 12 ff ff, 7 1f 1e 1d 1c, 8 1b 1a 19 18,
        9 17 16 15 14, 10 ff ff ff ff
hose 1: dev 5 ff ff ff ff, 6 ff ff ff ff, 7 2f 2e 2d 2c, 8 2b 2a 29 28,
        9 27 26 25 24, 10 23 22 21 20
```

The board row had Linux's `dp264_map_irq`, which differs in two places:
it wires hose 0 device 10 (to the inputs of device 6) and hose 1 device 6.
The console wires neither, and a card at hose 0 device 10 gets no
interrupt line. The row now follows the console, and the configuration
refuses an add-in device where the console gives none.

**Confirmed with a guest driver that waits for interrupts.** OpenVMS's LAN
driver started on DE500s (dec21143, UDP backend) with
`MC LANCP SET DEVICE EWx0/MOPDLL=ENABLE`, while
`lab/platforms/tsu/frame_tx.py` sent a broadcast frame to each NIC every
0.5 s. `Packets received` after 50-60 s:

| Position | Line | Received | Run |
| --- | --- | --- | --- |
| hose 0 device 7 | 0x1f | 90-100 | `tsu-ds20e-nic3-2cpu`, `-nic3b-1cpu` |
| hose 0 device 8 | 0x1b | 90 | `tsu-ds20e-nic3b-1cpu` |
| hose 0 device 9 | 0x17 | 124 | `tsu-ds20e-nic` |
| hose 0 device 10 | none | 1 | `tsu-ds20e-nic3-2cpu`, `-nic3b-1cpu` |
| hose 1 device 7 | 0x2f | 91 | `tsu-ds20e-nic3b-1cpu` |
| hose 1 device 8 | 0x2b | 123 | `tsu-ds20e-nic-swap` |
| hose 1 device 9 | 0x27 | 98 | `tsu-ds20e-nic3-2cpu` |

Hose 1 device 10 and the second pins were not exercised.

**Which processor is primary.** With every processor released at the
PALcode reset at once, the first to run clears the Cchip arbitration and
becomes the console's primary, so the host's thread start order decided it:
one of the first seven two-processor boots came up as `P01>>>`. The
secondaries are now released when processor 0 clears the arbitration
(`CSystem::release_secondaries`), a modelling choice recorded in the code.
Since then all 8 repeated two-processor boots and every SRM probe came up
on `P00>>>`; 7 of the 8 reached the login (`tsu-rep2-ds20e.txt`; the eighth
is the halt below).

**Network boot and SCSI under the new layout** (`probe-l4`): a 53C875 at
device 7 lists `dka0` and `dka500`; a DE500 at device 8 boots over BOOTP
and TFTP from `net_peer.py` to the image's HALT. `srm_probe.sh` places the
devices for `PLATFORM=ds20e` itself now (`SLOTS` in `srm_cfg.py`).

**The interpreter must run this console's real PALcode** (2026-10-02). The
runs above are on the JIT lane, which always runs the real PALcode. On the
interpreter, whose native vmspal routines replace the OpenVMS PALcode
wherever PAL_BASE is 0x8000, OpenVMS hung after its banner on one
processor (`tsu-ds20e-1cpu-int`) and bugchecked on CPU 1 with two
(`tsu-ds20e-2cpu-int`); with `palcode.vms.nohle = true` it logged in
(`tsu-ds20e-1cpu-int-nohle`). Those routines were written against the ES40
console's PALcode (V1.98-104); this one is V1.98-79. The board row now
carries `native_vmspal` (since the chipset split `vmspal_pal_base`, 0
for every board but the ES40), and the
interpreter boots to login on two processors with no options
(`tsu-ds20e-2cpu-int-fix`). The ES40 interpreter still uses the routines.

**Still open:**

- Resolved 2026-10-03 in the models: *two processors, intermittent* (in 2
  of 11 two-processor OpenVMS boots processor 0 executed a HALT at PC 0
  during startup). The JIT's data-page cache outlived the TB entry that
  filled it (86b2469), and a UART race lost console interrupts (0abdd52);
  0 of 81 two-processor boots failed after them (`lab/smp-flaky/`). The
  ES40's one hang after the password in 6 two-processor boots has not
  come back: 0 of 8 on the SMP work's base binary, 0 of 12 after the
  fixes, and 0 of 20 on main 29e213e (2026-10-04, JIT, each boot logging
  in and copying and comparing a 1049-block file:
  `lab/smp-flaky/results/t-es40-jit2.txt`).
- Not a fault: *the console "stalls" at `Testing the Memory`* (seen in 1
  of 6 interpreter boots with two processors, and once on JIT_VERIFY, by
  harnesses that gave the console 300 s to reach its prompt). That step is
  a real memory test: the power-up script runs `testmem`, and with 1 GB
  `memtest` walks the memory (the pattern loops at 0xa6400-0xa6800 hold the
  processor's samples). It takes about 110 s on the interpreter and 10 s
  on the JIT. 2026-10-04 on main 29e213e, 20 boots each of the interpreter
  and the JIT on one and two processors, timed line by line with the
  emulator's own CPU time and the host's load logged each second
  (`lab/tsu-bugs/srmloop2.sh`, results `lab/tsu-bugs/results/m-*`): 80 of
  80 reached `P00>>>`. Prompt times: interpreter 114-206 s, two outliers at
  388 s and 899 s, both two-processor runs; JIT 23-32 s. In the two
  outliers the emulator received 0.25 to 0.6 CPU seconds per second (2.0
  when its two processor threads run) while the host's load average was 30
  to 210 on its 16 cores (other sessions' guests and benchmarks, and once
  this work's own seven-lane build); the threads were runnable, not
  waiting, and the guest's instruction rate fell in step with the CPU time
  it got. No guest-side wait was involved. The ES40's console speed
  patches, which had hit an `lda` at 0x8bb78 in this console, are not
  applied to the DS20E since 213aae5. The harnesses now wait 900 s for the
  prompt (`SRMTO`, `lab/smp-flaky/tsuboot.py`, `lab/platforms/tsu/`).
- Resolved 2026-10-02 in the models, not the board: *OpenVMS with a SCSI
  controller bugchecked on every machine* (`INVEXCEPTN` in `SYSMAN`). The
  routine at `SYS$CPU_ROUTINES_2208+050A0` is `IOC$READ_IO`'s worker, and
  the null pointer an I/O handle PKWDRIVER maps only when the 53C875 has an
  expansion ROM; the ISP1040 crashed in PKQDRIVER on firmware memory that
  read as zeros. The DS20E now initialises, mounts and writes a disk on
  each (`docs/openvms.md`, SCSI controllers).
- Not a fault: *the DE600 received nothing under OpenVMS* because the test
  sent only broadcasts, and OpenVMS sets the 8255x's Broadcast Disable
  while no protocol wants them; unicast and multicast frames are received
  (`docs/openvms.md`, Network).
- The SROM records, the I2C parts and the reference listing, as above.

## Rules

- Firmware images are copied from media the user owns, never downloaded.
- The ES40 must not change behaviour: its console-log check stays clean and
  the JIT cross-check stays at 0 mismatches.
- An unknown register access is traced and reported, never given a
  convenient value to make the firmware proceed. A value chosen to satisfy
  the firmware is marked as such, in the code and in **Findings**.
- Threading uses `std::` facilities, never Poco-style wrappers.
- Format changed lines with the repository's `.clang-format`.
- Report the highest acceptance level actually reached, with the command
  output that shows it.
