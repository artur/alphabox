# AlphaServer ES47 reference listings

For the L3 step of the Marvel packet ([docs/platforms/marvel.md](../../../docs/platforms/marvel.md)).

- `show-config.txt`: `show config` of a real ES47 7/1300 (two EV7 rev 3.0 at
  1300 MHz, 2 x 4 GB, one IO7 pass 3 with the embedded I/O, SRM V7.3-11).
  Published by Matt Turner at https://mattst88.com/computers/es47/, copied
  2026-10-01. Note it is V7.3-**11**; the image on our firmware CD is
  V7.3-1, so banner lines will differ.

Not yet here, and where they can be found:

- `show memory` and `show cpu` examples from a 4-processor GS1280 7/800:
  *AlphaServer ES47/ES80/GS1280 SRM Console Reference*, pp. 55 and 62
  (`show cpu` prints `Type Major 15, Minor 2`; `show mem` lists PID 1's
  memory at 0x400000000).
- Full power-on logs of 8- and 32-processor machines: *User Information*
  v3.0 pp. 65-71 and *Installation Information* pp. 121-122.

None of these is from an ES47 with the configuration we will emulate, so
L3 compares structure (PIDs, memory placement, hoses, processor
identification) rather than whole listings.
