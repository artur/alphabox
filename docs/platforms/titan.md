# The Titan chipset (21274)

The system logic of the AlphaServer ES45, DS25 and DS15, in
`src/chipsets/titan/` behind `CChipset`. Board packets:
[es45.md](es45.md), [ds25.md](ds25.md) and [ds15.md](ds15.md) (all L5).

The boards differ around the chipset, not in it: the DS15 has one PA-chip
(hoses 0 and 2), reaches its RMC through a different mailbox, and its
console's TIG halt register must not halt the processor; the DS25 marks an
absent processor in the RMC's FRU byte with a different bit. These are
board parts (`platforms/ds25/`, `platforms/ds15/`), registered over the TIG
bus addresses they answer, so the chipset model is unchanged.

## What it is, next to the Typhoon

Sources: Linux `core_titan.h` (from the "Titan Chipset Engineering
Specification" rev 0.12), `core_titan.c`, `sys_titan.c`; the ES45 console.

| | Typhoon (`tsunami/`) | Titan (`titan/`) |
| --- | --- | --- |
| Cchip CSRs | 801.A000.0000 | the same place; MISC, DIM0-3/DIR0-3, DRIR, AARn, MPD laid out as on the Typhoon, plus PRBEN, IIC0-3, MPR0-3, TTR/TDR, PWR, CMON*, CPEN |
| Processors | 4 (DIM/DIR per CPU) | 4, the same |
| Dchip CSRs | 801.B000.0800 | the same |
| PCI | two Pchips, one hose each | two PA-chips, each a G-port (PCI/PCI-X) and an A-port (PCI/AGP): four hoses. Hose 0 = PA-chip 0 G, 1 = PA-chip 1 G, 2 = PA-chip 0 A, 3 = PA-chip 1 A |
| Port CSRs | Pchip n at 801/803.8000.0000 | the G-port there, the A-port 0x1000 above it; error registers and TLB invalidates per port (SERROR/GPERROR, AGPERROR/APERROR, GTLBIV/ATLBIV ...), SCTL on the G-port |
| Hose spaces | hose h: memory 800.0000.0000 + h*2.0000.0000, I/O +1.FC00.0000, configuration +1.FE00.0000 | the same formula for h = 0-3, so hoses 0 and 1 are where the Tsunami has them |
| DMA windows | WSBA/WSM/TBA x4, direct or scatter-gather | the same scheme (window 3 scatter-gather only); shared code in `chipsets/PciWindows.hpp` |
| Interrupts | DRIR<55:0> devices, <63:58> errors | the same; the board's table says which bit a slot's pin is |
| TIG bus | 801.0000.0000: flash, DPR, TIG registers at 801.3000.0000 | the same arrangement on the ES45 |

`phys_mask` is 0x807'ffff'ffff, as on the Tsunami: PA<34:33> selects the
hose within the I/O space.

## The design

- One class, `CTitan`, split by chip: `Titan.cpp` (decode, interrupts,
  hose spaces, DMA, state), `TitanCchip.cpp` (Cchip and Dchip CSRs),
  `TitanPachip.cpp` (the four ports' CSRs, a 0x24-quadword register file
  per port), `TitanTig.cpp` (TIG registers).
- Shared with the Tsunami where the hardware is the same, moved without
  behaviour change: `PciWindows.hpp` (window translation) and
  `DimmModel.*` (the DIMM population, its SPD EEPROMs on the MPD pins, and
  what the management processor reports, reached through
  `CChipset::dimms()`). MISC, DIM and the interval timer are written out
  again rather than shared: the Tsunami's live inside its byte-exact state
  layout (the state file), and the Titan's are a few lines.
- What the consoles print for the chips (show config) comes from
  MISC<39:32>, DREV, SCTL<7:0> and TIG offset 0; they read as the real
  listings: Dchip and PA-chips 17 (constants in `Titan.hpp`), the Cchip
  17 on the ES45 and 18 on the DS25 and DS15 (the board row's
  `titan_layout::cchip_rev`; the consoles only print it).
- The memory arrays (AARn) and CSC<51>, which the consoles print as the
  interleave mode ("1-Way" when set): CSC<51> is set unless arrays 0 and 2
  are populated alike, the rule the ES45's, DS25's and DS15's real
  listings all follow (ds15.md, Findings). The DS15 row fills arrays 0 and
  2 (`paired_arrays`); the others one array up to 8 GB.
- `memory.arrays = "2048,512,2048,512";` gives the arrays' sizes instead
  (MB, array 0 first, 0 for an empty one; the DS15 takes arrays 0 and 2
  only). The encoding is the consoles' own: the size is
  `2^(AARn<15:12> + 23)` bytes, 0 for an empty array, and the base is
  `AARn<34:24>` (DS15 V7.3-2 console, 0x90540 and 0x905c0; the ES45's
  0x957a0 and 0x95820 are the same code). Checked 2026-10-05
  (`lab/mem-arrays/`) on the ES45, DS25 and DS15 consoles: three and four
  equal arrays, descending and mixed sizes, an empty array between two
  populated ones; `show memory`, the power-up listing and `show config`
  give every array its size and base, with no error. Two real listings
  come out line for line: the DS25's 512 + 1024 MB (`"512,0,1024"`: array
  0 at 0x40000000, array 2 at 0, 1-Way) and the shape of the ES45's 10 GB
  (`"2048,512,2048,512"`: arrays 0 and 2 first, then 1 and 3, 2-Way).
  OpenVMS 8.4 on the ES45 with `"2048,2048,1024,1024"` boots to the SYSTEM
  login and reports 6.00 GB (`lab/mem-arrays/vms-four/`).
- What the consoles say and do not say about population: none of the
  three refuses or warns about any layout, a smaller array 0 included;
  they list the arrays in array order with the bases they find. The mode
  column is one value for all arrays: "1-Way" when CSC<51> is set,
  otherwise on the ES45 and DS25 "4-Way" when arrays 0 and 1 have one
  size and "2-Way" when not (ES45 console, 0x96290-0x962c4), on the DS15
  "2-Way" when arrays 0 and 2 have one size. So three equal arrays read
  "4-Way" here; what a real machine's serial ROM leaves in CSC<51> for
  three arrays is not known.
- Board facts stay in `platforms/<board>/`: the interrupt table, the slots,
  the DPR contents, the flash parts, the `titan_layout`.

## AGP

The A-port of PA-chip 0 (hose 2) is an AGP bus on the board that wires it
as one: `titan_layout::agp`, true for the ES45's Model 1 backplane
(`platform = "es45m1";`, [es45.md](es45.md)) and for no other board. The
chipset then reads APCTL<57> (AGP_PRESENT) as 1, and that bit is all the
ES45 console asks (V7.3-2, `is_titan_agp` at 0x8abe0: hose 2 is APCTL<57>
of PA-chip 0, hose 3 that of PA-chip 1).

What the console does with the port, from its code:

- `setup_io` (0x900f0) sets PCTL bits 0x4.c000.00c2 on all four ports,
  then clears APCTL<63:58> and <55:52> on both A-ports -- the queue depths,
  AGP_EN, sideband addressing and the rate (the mask is
  0x030f.ffff.ffff.ffff). It never enables AGP transactions: the card in
  the AGP slot is configured and run as a 66 MHz PCI device (configuration
  space, BARs, its BIOS under the x86 emulator). It sets <57> only when it
  runs in its own simulator (`platform()` = 1), never on hardware, and its
  mask keeps the bit: a strap **[inferred]**.
- Everything else is naming: the model (`build_dsrdb`), "PAchip 0" for
  "PPchip 0" and "AGP" for "PCI" in the hose's heading (`show_core_system`
  0x96e00, `show_pci_config` 0x97310), "Hose 2 - AGP bus" at probe time,
  an AGP node in the FRU tree (`build_agp_fru` 0x99ac0), and the hot-plug
  code leaving that hose alone (`cpqphpc_configure` 0x8dc50).

So the model is the bit. The AGP fields of APCTL and AGPLASTWR hold what
is written and nothing more: AGP transactions are not modelled, and no
guest so far turns them on (OpenVMS 8.4 boots with the card there). Linux's
`titan_agp_*` would, building the aperture from the port's scatter-gather
window: untried.

The DS25 and DS15 have no AGP. The DS25's console carries the same code
(`is_titan_agp` at 0x952e0, called from its show config) and would print
"PAchip 0" and "AGP" if the bit were set; the real DS25's listing says
"PPchip 0" and "Hose 2, Bus 0, PCI - 66 MHz", and its hose 2 holds on-board
devices. The DS15's console has the routine (0x89d50) and nothing calls
it. Both rows say `agp = false`: the value they always had.

## Assumed, not established

- CSC (but bit 51) and the Dchips' DSC/STR/DSC2: the Typhoon's values.
  (The AAR encoding is no longer assumed: it is read off the consoles'
  code and checked for one to four arrays, above.)
- Where the arrays' memory starts: the largest array at 0 and the others
  above it in order of size, arrays of one size in array order. The
  serial ROM, which is not emulated, decides this on a real machine; the
  real listings agree as far as they go (two of them, above). The order
  among four equal arrays (0, 1, 2, 3 here) is inferred.
- CSC<51> for layouts no real listing shows (three arrays; four with
  arrays 0 and 2 alike but 1 and 3 not): the rule above, extended.
- With `memory.arrays` and a total that is not a power of two, the
  addresses between the last array and the next power of two are backed
  like memory; a real machine has nothing there. The consoles and OpenVMS
  size memory from the arrays and do not go there.
- PCTL<17> (66 MHz) and APCTL<57> (AGP present) read-only; SCTL<7:0>
  read-only. The ES45 console writes neither on hardware.
- APCTL<57> follows the backplane, not whether a card is in the slot: the
  console names the machine "Model 1" from it alone (AGP, above).
- Errors are never raised: the error registers read 0.
- The scatter-gather TLB is not modelled (invalidates are ignored).
