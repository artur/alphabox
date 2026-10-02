# AlphaServer ES45 reference output

`show-config.txt` is Example 2-6 of the *AlphaServer ES45 Owner's Guide*
(Compaq, EK-ES450-UG), a real ES45 Model 2B with four 1 GHz EV68CBs, SRM
V5.9-9. It was extracted from the PDF's text layer, so the column layout
is lost; the content is what the console printed. The same guide's tables
2-1 to 2-3 map the physical slots of the Model 1B, 2B and 3B backplanes to
these logical slots.

What to compare against Alphabox's V7.3-2 console (docs/platforms/es45.md):
the machine name, the core logic revisions (17, 17, 17, 17, TIG 2.6), the
on-board devices at hose 0 slots 7 (ISA bridge) and 16 (IDE), the hose
speeds (33 MHz on hose 0, 66 MHz on 1 to 3), and that cards appear at the
logical slots the guide lists (hose 0: 8 to 11; hoses 1 to 3: 1 and 2).
Differences that come from the configuration, not the machine: processors,
memory, the cards and the console version.
