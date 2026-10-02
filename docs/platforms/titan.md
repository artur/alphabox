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
- What the ES45 console prints for the chips (show config) comes from
  MISC<39:32>, DREV, SCTL<7:0> and TIG offset 0; they read as the real
  ES45's (17, 17, 17, TIG 2.6). Constants in `Titan.hpp`.
- Board facts stay in `platforms/es45/`: the interrupt table, the slots,
  the DPR contents, the flash parts.

## Assumed, not established

- CSC, the Dchips' DSC/STR/DSC2 and the AAR encoding: the Typhoon's values.
  The console's memory listing agrees for one array.
- PCTL<17> (66 MHz) and APCTL<57> (AGP present) read-only; SCTL<7:0>
  read-only.
- No AGP: hose 2 reports no AGP device (APCTL<57> = 0), so the console
  chooses the PCI models.
- Errors are never raised: the error registers read 0.
- The scatter-gather TLB is not modelled (invalidates are ignored).
