# AlphaServer DS25 reference output

`show-config.txt` is Example 2-6 (repeated as 5-2) of the *AlphaServer DS25
Owner's Guide* (hp, EK-DS250-UG.D01), a real DS25 with two 1 GHz EV68CBs,
SRM V6.3-1. It was extracted from the PDF's text layer (the guide is in
`lab/docs-titan/`), so the column layout is lost and the guide's callout
markers were removed; the content is what the console printed. The same
guide's table 1-1 maps the six physical slots to logical ones: slots 1-6
are hose 1 devices 1 and 2, hose 3 devices 2 and 1, hose 0 devices 9 and 10.

What to compare against Alphabox's V7.3-2 console (docs/platforms/ds25.md):
the machine name, the core logic (Dchip, PPchip 0 and 1 at 17, TIG 2.6),
the on-board devices -- hose 0 device 7 (ISA bridge), 8 (Intel 82559ER),
12 (hot-plug controller), 16 (IDE); hose 2 device 1 (the AIC-7899, two
functions, `pka`/`pkb`), 5 (Broadcom 5703c); hose 3 device 6 (hot-plug
controller) -- and that cards appear at the logical slots of table 1-1.
Differences that come from the configuration, not the machine: processors,
memory, the cards and the console version.
