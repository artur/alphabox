# Per-machine reference output

What a real machine's console prints, for the L3 step of a platform work
packet (see [docs/platforms.md](../../docs/platforms.md)): one directory per
machine, holding its `show config`, `show memory` and `show device`
listings, with a note on where each came from.

The ES40's own regression logs live in `test/rom/` instead, because they
predate this layout.
