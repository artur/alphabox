# AlphaServer ES47 reference listings

For the L3 step of the Marvel packet ([docs/platforms/marvel.md](../../../docs/platforms/marvel.md)).

Every file here comes from one real machine: Matt Turner's (GitHub
`mattst88`) ES47 7/1300, with two EV7z at 1300 MHz (`EV7 rev 3.0`, 1.75 MB),
2 x 4 GB, one IO7 pass 3 with the embedded I/O, in one 2P drawer, running
SRM V7.3-11. Note it is V7.3-**11**; the image on our firmware CD is
V7.3-1, so banner lines will differ.

`show-config.txt` was first copied from https://mattst88.com/computers/es47/
on 2026-10-01. On 2026-10-25 its owner sent the captures below in
[issue #3](https://github.com/artur/alphabox/issues/3) ("Reference output
from a real ES47 7/1300 (SRM V7.3-11)"), and they are kept here byte for byte
from the issue, under its file names. The issue's `show-config.txt` replaced
the site's copy: the same listing with its prompt line and trailing blanks.

| File | What it holds |
| --- | --- |
| `power-up.log` | the whole start: the MBM's failsafe loader and firmware (VxWorks), the partition's power-on, the topology map, the MBM diagnostics (tests 10-50), then the console's own start to the banner |
| `show-version.txt` | `show version` |
| `show-config.txt` | `show config` |
| `show-cpu.txt` | `show cpu`: `Type Major 15, Minor 5` on both processors |
| `show-memory.txt` | `show memory` |
| `show-device.txt` | `show device` |
| `show-pal.txt` | `show pal` |
| `show-fru.txt` | `show fru`: part and serial numbers of the drawer's FRUs |
| `show-fru-e.txt` | `show fru -e`: the header only, no FRU with an error |
| `show-hwrpb.txt` | `show hwrpb`: the HWRPB, both per-processor slots, the CTB, CRB, memory descriptors and DSRDB |
| `show-power.txt`, `show-error.txt` | `show power` and `show error`: nothing printed |
| `linux-implver-amask.txt` | IMPLVER and AMASK read under Linux on each processor, and a `/proc/cpuinfo` excerpt (timer 991.82 Hz) |

What they settle and what is still open for the emulated ES47 is in
marvel.md, "Captures from a real ES47 7/1300".

Not here, and where they can be found:

- `show memory` and `show cpu` examples from a 4-processor GS1280 7/800:
  *AlphaServer ES47/ES80/GS1280 SRM Console Reference*, pp. 55 and 62
  (`show cpu` prints `Type Major 15, Minor 2`; `show mem` lists PID 1's
  memory at 0x400000000).
- Full power-on logs of 8- and 32-processor machines: *User Information*
  v3.0 pp. 65-71 and *Installation Information* pp. 121-122.
