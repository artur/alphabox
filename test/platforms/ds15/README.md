# AlphaServer DS15 reference output

`show-config.txt` is example 2-5 of the *hp AlphaServer/AlphaStation DS15
Owner's Guide* (EK-DS150-OG.A01; the PDF and its text are in
`lab/docs-titan/`), a real DS15 with one 1 GHz EV68CB, console X6.6-2092.
It was extracted from the PDF's text layer (callout markers removed). The
guide's table 2-1 maps the four physical slots to hose 2 devices 7 to 10;
example 2-7 shows one 1024 MB array as "1-Way".

What to compare against Alphabox's V7.3-2 console (docs/platforms/ds15.md):
the machine name, the core logic (Dchip and PPchip 0 at 17, TIG 1.9, one
PA-chip), the on-board devices -- hose 0 device 7 (ISA bridge), 8/0 and 8/1
(the AIC-7899), 9 and 10 (Intel 82559ER, ports A and B), 13 (IDE) -- the
hose speeds (33 MHz on hose 0, 66 MHz on hose 2) and that cards appear at
hose 2 devices 7 to 10. Differences that come from the configuration, not
the machine: processor speed, memory, the cards and the console version.
