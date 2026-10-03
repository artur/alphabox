# Marvel (AlphaServer ES47 / ES80 / GS1280) work packet

The EV7 machines. This is phase 0, which covers research, the plan and the
first contact with the firmware. It is a separate project in the sense of
[platforms.md](../platforms.md): the processor is not an EV6-family part,
and there is no chipset in the Tsunami sense. Each EV7 carries its own
memory controllers, interrupt logic and a router, and reaches the I/O
through an IO7 bridge.

**Config**: `platform = "es47";` (two processors), `"es80"` (up to eight)
or `"gs1280"` (up to sixteen) with `ev7` (or `ev7z`) processors and a
`serial0` for the console terminal, PCI devices as `pci<hose>.<slot>`,
hose = PID * 4 + IO7 port · **Status**: packets M0-M6 done (2026-10-02):
L5 on all three (M6c: the ES80 with eight processors and four IO7s, the
GS1280 with sixteen, OpenVMS 8.4 to DCL on every processor). M7
(2026-10-03): OpenVMS 8.4 installed to a SCSI disk on the ES47 boots to
login (and on the ES80), and DECwindows draws its login box on the Radeon
7500 with a USB keyboard and pointer on the ES47's own USB; a JIT-only
crash in OpenVMS's USB driver at boot (about one boot in ten) is open. On emulated
EV7s, with the management processor (CMM) on the other side of each
processor's GIO port emulated and the IO7 on PID 0, the console reaches
`P00>>>`, `show config` lists IO7 0 with its four buses and the devices
placed behind it, and `boot dka400` boots OpenVMS 8.4 from its CD on both
processors to the date prompt, the installation menu and DCL (see "M5").

The ES47 is the target: the smallest Marvel, with two EV7s on one board
and one IO7. The ES80 and GS1280 are the same platform with more
processors, more IO7s and more cabinets. EV7z is a CPU row.

Throughout, **[guess]** marks reasoning without a source, and
**[inference]** marks something derived from sources but not stated in
them. The working files (Linux sources, manuals, the console's strings and
register tables) are in `lab/docs-ev7/`, which git does not track. Section
**Sources** lists them.

## The machine

| | |
| --- | --- |
| Family, code name | "Marvel": Linux `ST_DEC_MARVEL` = 39, machvec "MARVEL/EV7". Linux knows a single variation, "Marvel/EV7". **known** (`hwrpb.h`, `setup.c`) |
| CPU | Alpha 21364 "EV7", 800 MHz to 1.15 GHz; the console names revisions 1.0 to 2.2. The "EV7z" (Linux: EV79; listings print "EV7 rev 3.0") runs at 1.15-1.3 GHz. The ES47 has 2 per drawer, the ES80 up to 8 and the GS1280 up to 64 (the clock ranges and counts are **assumed**, from general knowledge). HWRPB CPU type 15 (EV7) / 16 (EV79). **known** (`hwrpb.h`; SRM reference p.55 `show cpu`: "Type Major 15, Minor 2") |
| Chipset | none. Each EV7 has two Zbox memory controllers (RDRAM RIMMs), a Cbox with a 1.75 MB L2 cache, and an Rbox router with N/S/E/W inter-processor ports and one I/O port. **known** (Technical Summary; the console's register tables) |
| Memory | per EV7, up to 32 GB (Technical Summary). Each processor's memory is placed by its PID: PID 1 at 0x4_0000_0000, PID 2 at 0x8_0000_0000, PID 4 at 0x20_0000_0000. **known** (console listings) |
| PCI | one IO7 per EV7 I/O port. An IO7 has 4 ports: S0 to S2 are PCI/PCI-X, S3 is AGP. A hose is numbered PID*4 + port. **known** (`core_marvel.h`, `marvel_find_console_vga_hose`, console logs) |
| South bridge | none: the ES47's "Embedded I/O" is an I/O expander module on hose 2 with an AIC-7892 SCSI controller (slot 1), a CMD 649 IDE controller for the CD (slot 2) and a four-function USB controller (slot 3), an Agere USS-344 (four single-port OHCI functions; the console's table names 11C1:5803 with subsystem 11C1:5803 "USB"); a real ES47 adds option cards (DEGX2-TA gigabit Ethernet in hose 0 slot 1, a Radeon 7500 in the AGP slot). Sources: the real `show config`, User Information (2P backplane: "a 1-slot high performance PCI-X bus, two 2-slot PCI-X buses, and an AGP bus"; 2P I/O expander module). Linux's Marvel code says there is no legacy (ISA) hose. The console also carries an "Acer Labs M1543C" driver, which no Marvel drawer is known to use |
| Console devices | the console terminal is reached through the management hardware, not a UART on the I/O drawer **[inference]**: the console has a `giott` (a terminal over the GIO port), and the manuals reach the console through the MBM's serial port or the management LAN |
| Board hardware | per CPU module a **CMM** (CPU Module Manager). Per drawer an **MBM** (Marvel Backplane Manager) and **PBM** (PCI Backplane Manager). The SROM and XSROM run on the EV7 itself and are loaded through the CMM. **known** (User Information v3.0 pp. 68-71) |

### The EV7 core against the EV68

**What is the same.** The 21364 core is the 21264 core (EV68 generation)
with the system logic moved on chip. **known** (Technical Summary; the
PALcode reset path we ran uses EV6 IPR numbers throughout). In detail:

- the ISA, including BWX/FIX/CIX/MVI;
- the PALcode instructions (HW_MFPR/HW_MTPR/HW_LD/HW_ST/HW_REI);
- the IPR numbering for the Ibox and Mbox: ITB/DTB, I_CTL at 0x11,
  PAL_BASE at 0x10, IER_CM, SIRR, ISUM, EXC_ADDR, CC, VA, M_CTL, DC_CTL.

The console's PALcode V2.11-25 reset code ran on our EV68 core with **no
unimplemented-IPR warning** up to the console's C entry. Linux has no
EV7-specific AMASK or IMPLVER code **[guess: IMPLVER 2, AMASK as the
EV68]**.

**What differs.** Each item below is something an EV7 model must change
or add.

- **Physical address**: 44 bits, PA<43> = I/O. The PID ("PE") is stored
  **inverted** in PA<43:35>, as `EV7_IPE(pe) = (~pe & 0x1ff) << 35`
  (`core_marvel.h`).
  - Memory carries the PID in the middle bits instead. v4.19
    `marvel_node_mem_start`: PID<1:0> to PA<35:34>, PID<6:2> to PA<41:37>.
  - The console's PALcode forms CSR addresses as sign-extended 64-bit
    values (it touched 0xffff_ffff_ffc0_0110), while its C code uses
    0xfff_ffc8_0000. **The hardware ignores PA<63:44>, and the model must
    mask to 44 bits.** **[inference]** This is observed in the probe.
  - Our CPU passes the unmasked value to `CSystem::ReadMem`, which masks
    it to the Tsunami's `0x807_ffff_ffff`. That mask is wrong for Marvel.
- **The CSR space**: `EV7_CSR_PHYS(pe, off) = EV7_IPE(pe) | 0x7FFC << 20 |
  off`, a 4 MB window per processor. For PE 0 it is 0xFFF_FFC0_0000. The
  console carries a full table of names and offsets
  (`lab/docs-ev7/srm73-csr-tables.txt`, extracted from the running image):
  - Rbox:
    - RBOX_CFG 0x0, NSVC 0x10, EWVC 0x20, WHOAMI 0x30, TCTL 0x40;
    - INT 0x50, IMASK 0x60, IREQ 0x70, INTQ 0x80, INTA 0x90;
    - IT 0xa0, SCRATCH1 0xb0, L_ERR 0xd0;
    - per-port N/S/E/W/IO CFG/ERR/PERF/T1CFG at 0x4000/0x6000/0x8000/0xa000/0xc000.
  - GIO: **GIO_CFG 0x100, GIO_DAT 0x110, GIO_CTL 0x120**, and a word at
    **0x80000** used as a lock (not in the table; seen in the probe).
  - Cbox: CTL 0xe000 to PRF_CNT 0xe0b0. Bbox: 0x1c000 to 0x1c040.
  - Zbox0: 0x10000 to 0x14110. Zbox1: 0x16000 to 0x1a110. These cover the
    DRAM timing, mapper, refresh and error registers.
  - Pads: 0x20000 to 0x22900. OCLA (on-chip logic analyser): 0x24000 to
    0x26xxx.

  Linux knows only the Rbox block up to L_ERR.
- **Memory controllers on chip (Zbox)**: there is no Dchip or Cchip
  array configuration. Memory is configured through the Zbox CSRs (DRAM
  timing, mapper). The SROM, XSROM and CMM do that before the console
  runs, using RIMM SPD data that the CMM reads over I2C (User Information
  p.68, step 2). **[inference]** The console reads the result back, so
  the model needs a plausible configured state, not the configuration
  sequence.
- **The router (Rbox)**: the N/S/E/W ports and the routing tables. The
  ES47 is two EV7s with one link between them. The XSROM configures the
  routes; the console reports `NS,EW (0,0)` and `(1,0)` coordinates.
- **Caches**: L1 64 KB 2-way I and D, as the EV68. The L2 is on chip:
  1.75 MB, 7-way, `CSHAPE(7*1024*1024/4, 6, 7)` in `setup.c`.
  - The console prints 1.50 MB for rev 2.0 parts and 1.75 MB for rev 3.0
    (console listings).
  - There is no board-level Bcache, so the EV6 C_DATA/C_SHIFT serial
    Bcache configuration does not apply. **[inference]** Our PAL run did
    not touch them.
- **Identification**:
  - The HWRPB CPU type is 15 for EV7 and 16 for EV79. These are set by
    the console, not read from the chip.
  - The PALcode reads **I_CTL<29:24>** (`and #63`) and compares it with 2,
    at 0x3eaa4. The console has strings "EV7 chip ID %d", "EV79 chip ID
    %d" and "EV7 rev 1.0/2.0/2.1/2.2/3.0".
  - The chip ID per revision (M6b): the console's type table gives minor
    = index + 1, the index being the chip ID (plus CSR 0x28020<19> when it
    is 2), and names minors 1-5 "EV7 rev 1.0", "2.0", "2.1", "2.2", "3.0".
    So chip ID 2 is rev 2.1 (2.2 with bit 19), and "EV7 rev 3.0" -- the
    EV7z of a real 7/1300 -- needs chip ID 4 **[inference]**.
- **Interrupt delivery**: the EV6 receives six external IRQ pins into
  ISUM<38:33>. The EV7 receives interrupts as packets into its Rbox:
  - I/O interrupts arrive from IO7s as "IIDs";
  - IPIs go through RBOX_IREQ **[guess, from the register name and Linux's
    use of `wripir`]**;
  - the interval timer is RBOX_IT, which the console clears early;
  - errors are signalled the same way.

  The PALcode reads RBOX_INT/INTQ and hands the OS a vector:
  `PE << 16 | 0x800 + (LSI << 4)` for LSIs and `0x1000 + (MSI << 4)` for
  MSIs (`io7_device_interrupt` in `sys_marvel.c`). How the Rbox signals
  the core (which ISUM bit) is **not known**; the PALcode's interrupt
  entry will show it.
- **Machine checks and errors**: logout frames are subpacket lists
  (`err_ev7.h`). The subpackets are:
  - EV7 processor: I_STAT, DC_STAT, C_ADDR/C_SYNDROME, C_STAT/C_STS,
    Cbox/Bbox registers;
  - Zbox, Rbox, IO7;
  - environmental: temperature, fan, voltage, power.

  SCB vectors: 0x620 system correctable, 0x630 processor correctable,
  0x660 system machine check, 0x670 processor machine check, 0x680
  system event. The console can store a logout frame "in CMM RAM". An
  emulator that never raises machine checks (as ours does not:
  `docs/cpu-fidelity.md`) only needs the error registers to read as
  clean.
- **The PALcode interface** is the EV7 PALcode's, not the EV6's. The
  console's VMS PALcode V2.11-25 and UNIX PALcode V2.08-19 are EV7 builds
  inside the console image. The OS-visible calls are the standard ones;
  Linux uses whami, wripir, cserve 0x48/0x49 (TOY), wrmces and draina.
  The **native vmspal fast paths** (`cpu/AlphaCPU_vmspal.cpp`) mirror the
  EV6 VMS PALcode's internal state, so they must be **off for EV7**
  until checked against this PALcode.

**What EV7z changes**: clock (1.15-1.3 GHz) and revision. Linux calls it
EV79 and gives it CPU type 16. Real ES47 7/1300 listings print "EV7 rev
3.0, 1300 MHz" with 1.75 MB cache, against the 7/800's "EV7 rev 2.0, 800
MHz" with 1.50 MB. EV7z is a CPU row: chip ID 4, which the console calls
"EV7 rev 3.0", type 15 minor 5 (M6b). This console never reports type 16:
its type table holds 15 throughout, and its "EV79 rev" strings are not
referenced by the revision table it uses.

### Marvel's address map

From `core_marvel.h`/`core_marvel.c`, with the arithmetic for PE 0 checked
by hand:

| Space | Address | Notes |
| --- | --- | --- |
| Memory of PID n | `((n & 3) \| (n & 0x7c) << 1) << 34`, 16 GB slots | PID 0 at 0; confirmed by real `show mem` listings |
| EV7 CSRs of PID n | `(~n & 0x1ff) << 35 \| 0x7FFC << 20 \| off` | PE 0: 0xFFF_FFC0_0000, 4 MB |
| IO7 hose (PID n, port p) | `(~n & 0x1ff) << 35 \| (~p & 7) << 32` | PE 0 port 0: 0xFFF_0000_0000 |
| + PCI memory | + 0x0000_0000 (Linux assumes 2 GB) | dense |
| + PCI config | + 0xFE00_0000; `bus << 16 \| devfn << 8 \| reg` | type 0 vs type 1 by bus number; root bus addressed as 0 |
| + PCI I/O | + 0xFF00_0000 (8 MB) | dense; there is no sparse space anywhere on Marvel |
| + port CSRs | + 0xFF80_0000 + off | `io7_ioport_csrs`, registers 64 bytes apart |
| IO7's own CSRs | port 7: `IO7_CSR_PHYS(pe, 7, 0x300000)` | PE 0: 0xFF8_FFB0_0000 |

**Per IO7 port** (console table "POx_*", the same as Linux):

- control:
  - POx_CTRL 0x0;
  - POx_CACHE_CTL 0x40 (Linux treats a port as present when this reads 8);
  - TIMER, IO/MEM_ADR_EXT, XCAL_CTRL;
- data mover: DM_* at 0x200;
- AGP: AGP_CAP_ID/STAT/CMD at 0x400, on port 3 only;
- monitors and scratch: 0x500-0x780;
- DMA windows: **WBASE0-3 0x1000, WMASK0-3 0x1100, TBASE0-3 0x1200**,
  SG_TBIA 0x1300, MSI_WBASE 0x1340;
- errors: ERR_SUM to MULT_ERR at 0x2000-0x21c0;
- hot plug: HP_* at 0x4000-0x4200, on the PCI-X ports.

**DMA translation**: the same scatter-gather design as Tsunami's Pchip.

- Windows: WBASE has enable bit 0, sg bit 1, dac bit 2 (window 3 only)
  and an address in bits 31:20, with a matching WMASK and TBASE.
- PTEs: `(paddr >> 12) << 1 | 1`.
- Flushing: write SG_TBIA, then read it back.
- Inbound DAC: offset 1 << 49.
- The console sets its own windows; Linux saves and replaces them.

**The IO7's own block** (port 7, offsets from 0x300000):

- identification and reset: IO_ASIC_REV, IO_SYS_REV, PO7_RST1/2, POx_RST0-3
  (POx_RST3 carries the AGP PLL range);
- IO7_UPH and IO7_MAF;
- errors: PO7_ERROR_SUM 0x302000 and the symptom registers;
- interrupt control (below) at 0x310000-0x31c800.

The console's table names IO7_IREQ_OFF and IO7_INTA_OFF at 0x300440/480,
where Linux has RBX_IREQ_OFF/RBX_INTA_OFF.

### Interrupts

- **IO7 to EV7**: every interrupt source in an IO7 has a control
  register:
  - LSI_CTL[128] at 0x310000 for the PCI INTx pins: port<7:5> |
    slot<4:2> | INTx<1:0>, with LSIs 0x74/0x75 for AGP slot 5;
  - MSI_CTL[16] at 0x314000, 32 vectors each;
  - HLT/HPI/CRD/STV/HEI_CTL at 0x313ec0-0x313fc0.

  Each register holds a **target PID** (bits 22:14 for LSI/MSI, 32:24 for
  the others) and an **enable** (bit 24). The IO7 sends an "IID" packet
  to that EV7 (`int_num` <8:0>, `msi` bit 13, `ipe` <23:14>). INT_PND/
  CLR/EOI[4] (0x318000-0x319880) and MISC_PND are the pending state.
- **Who programs it**: the console. Linux keeps the console's PCI setup
  (PCI_PROBE_ONLY) and takes each device's IRQ from INTERRUPT_LINE, which
  the console sets to the LSI number **[inference]**. The answer key for
  the interrupt map is therefore what the console writes there, as on the
  DS20E.
- **IPIs**: Linux uses `wripir`, and the PALcode does the rest. Matt
  Turner's 2026 SMP fixes (tested on a real ES47) say IPIs travel "as
  edge-triggered hardware signals through the IO7". The mechanism below
  the PALcode is **not known**: RBOX_IREQ in the target's CSR space is the
  candidate.
- **Interval timer**: there is no Cchip. Linux's `marvel_init_rtc` does
  not program an 8254 or the CMOS periodic interrupt: the tick arrives as
  entInt type 1 from the PALcode. The source is the EV7's own
  **RBOX_IT**, cleared by the console's PALcode early in our probe. The
  console's own test "41, Local Interval Timer Interrupts" confirms that
  each EV7 has one. **[inference]**
- **TOY/RTC**: not a chip the OS reaches. Linux emulates port 0x70 as an
  index latch and reads or writes through **`cserve` 0x49 (GET_TOY) / 0x48
  (PUT_TOY)**, on the boot CPU only (`__marvel_access_rtc`). The console
  has `toy_read`/`toy_write`/`rtc_read_pb`. **[guess]** The time comes
  from the CMM/MBM over GIO; the strings show no M1543C RTC use.

### The console

**Packaging.** `SRM_V7_3.EXE` is the same "LFU APU" wrapper as the ES40's
`cl67srmrom.exe`: the header at 0x200 reads `V7.3-1 CPQ MARVEL ALPH SRM`,
and the payload is 0x0cb400 bytes from file offset 0x240. So it is
`FW_LFU_BUNDLE`, loaded at 0x900000 and entered at 0x900001 in PALmode;
Alphabox's existing loader handles it unchanged.

- The `.SYS` file is the same with a 512-byte MOP header in front.
- `GS1280_V7_3.EXE` is the full update utility (LFU), starting directly
  with the decompressor (`FW_RAW_IMAGE`).
- The self-decompressor is the usual Compaq one, which checks its load
  address (0x30000, 0x30240 or 0x900000).
- It leaves:
  - the **EV7 PALcode at 0x30000-0x4c000**, entry 0x30001 with
    PAL_BASE 0x30000;
  - a small stub at 0x70000;
  - the **console proper at 0x280000-0x440000**: about 1.8 MB, C code
    with strings, *above* the 2 MB that Alphabox's `decompressed.rom`
    cache saved. Fixed by M3: the board row's `console_bytes` (0x480000
    on the ES47) sets how much the cache holds.
- The real secondaries start "at address 400030000" (PID 1's memory +
  0x30000): each processor runs its PALcode from its own memory.

**On a real machine** (User Information pp. 68-71, the power-up flow
table):

1. The CMM powers the CPU, holds it in reset and loads the **SROM** into
   it through the SROM port from its FPGA.
2. The EV7 runs BIST and the SROM. The SROM configures the IPRs (except
   RAMbus and router) and the cache, and loads its **PID into
   CBOX_WHAMI**. Then it "inits communication to CMM".
3. The **XSROM** is loaded "via GIO PORT". The XSROM tests and
   configures memory (Zbox), the I/O port and the IO7, loads the router
   configuration, and runs route, memory and interrupt tests across the
   mesh. The CMM passes the MBM's commands to it.
4. The MBM partition coordinator elects a primary per partition. The
   secondaries run an XSROM loop "on RBOX_SCRATCH waiting for jump
   address". The **primary CMM loads the console/PAL into memory** (the
   console has `cmm_dma`) and transfers control.
5. The console reads its configuration from the MBM (`get_mbm_configuration`,
   `build_mbm_hw`, `build_mbm_fru`, `get_pbm_configuration`) and the
   partition database. It builds the **GCT** (Galaxy Configuration Tree,
   `GALAXY_ID_MARV01`, node magic 'GLXY'), which the OS reads through the
   HWRPB, and starts the secondaries by address.

**What the console expects at entry.**

- The decompressor saves r1, r2 and r28 at its first instructions (`bis
  t0, zero, t3; bis t1, zero, t4; bis at, zero, s5`).
- The PALcode stores r28 as the processor's PID at PAL scratch +0x90
  (0x3e780), and tests r18/r19 to choose a path at 0x3e790.
- **[inference]** The SROM/XSROM hand off the PID in r28 and flags in
  r18/r19. Our CPUs start with zero registers, so the probe ran as PID 0
  on the default path.
- The CSR base for every later access is computed from that PID.

**The management interface: GIO.** This is the console's first
conversation with the outside world, and the probe watched it in order:

- A **lock** at CSR+0x80000: the console reads it until it reads zero,
  and writes zero to release it **[inference: a read-to-acquire
  semaphore]**.
- A transaction:
  1. write a command to **GIO_CTL** (0x120): 4 = send, 0 then 1 = receive
     **[inference: bits 2:1 an opcode, bit 0 a go bit]**;
  2. write a word to **GIO_DAT** (0x110) for a send;
  3. poll GIO_DAT until **bit 63** is set (done), with a timeout of 2^28
     reads;
  4. take the result from GIO_DAT.
- The first words sent are 0x40004, 0x412a0 and 0x412a6, which look like
  progress or status codes. After them come receives, a send of 2/3 and
  a 0x10/4 exchange.
- The console has `wait_for_gio`, "*** GIO Timeout", `giott` (a terminal
  over GIO), `cmm_open/read/write/dma`, `cmm_watchdog` and "*** CMM
  Timeout".
- So **the console terminal, the TOY, the configuration, the FRU data and
  the console's own NVRAM all reach the CMM/MBM through GIO**. The
  framing above the GIO register level was the main reverse-engineering
  task of this project; M4 worked it out (far-side registers, a byte
  window into the CMM's memory, mailboxes carrying SMLAN messages, the
  terminal as two registers): see "M4" under Findings.

**What an emulator must provide instead of SROM/XSROM/CMM/MBM:**

1. **The SROM/XSROM's result, not its code.** The SROM and XSROM images
   are on the CD (`MVSROM_V1_0_9.BIN`, 6 KB; `MVXSROM_V1_0_31.BIN`,
   64 KB), but running them would mean modelling the RAMbus, the router
   training and the CMM's FPGA. Instead, the loader must leave each EV7
   as the XSROM leaves it:
   - CBOX_WHAMI/RBOX_WHOAMI holding the PID;
   - Zbox CSRs describing the memory that exists;
   - Rbox routes "configured";
   - the IO7 out of reset with its ports present (CACHE_CTL = 8);
   - the handoff registers (r28 = PID, and whatever r1/r2/r18/r19 mean);
   - the secondaries polling RBOX_SCRATCH for a jump address, or simply
     waiting in the emulator for a write there.
2. **A CMM endpoint on each EV7's GIO port.** It must answer the
   console's commands (configuration, partition database, FRU and
   environmental data, TOY, NVRAM) and carry the console terminal to a
   telnet port, the way `CSerial` does today.
3. **The partition database and MBM configuration** that the console
   asks for, as data: one hard partition, one soft partition, two EV7s,
   one IO7.

The CMM firmware (`CMM3_V2_7_5.BIN`) and the MBM firmware
(`MBM_V2_7_6.BIN`) are on the CD. They are the other side of these
protocols, but they are not Alpha code (the MBM runs its own OS, with an
`MBM>` prompt), so they are references to read, not programs to run.

### Which operating systems run on Marvel

| OS | Marvel support | Media here |
| --- | --- | --- |
| OpenVMS | 7.3-1 (ES47/ES80/GS1280 support added), 7.3-2, 8.2-8.4 | `lab/ALPHA084.ISO` (8.4) |
| Tru64 UNIX | 5.1B (5.1A with a patch kit, **[guess]**) | none here |
| Linux | `arch/alpha` MARVEL machvec, still in mainline; Matt Turner's 2026 SMP fixes were tested on an ES47 | none here |

These support claims are general knowledge, not from the documents in
`lab/docs-ev7`, except Linux. Check them against the OS SPDs before
relying on them.

## Firmware

- Console: `roms/alpha-firmware-v7.3/GS1280/SRM_V7_3.EXE` (Alpha
  firmware CD V7.3, ES47/ES80/GS1280 directory). The ES47, ES80 and
  GS1280 share one image. It is SRM V7.3-1, VMS PALcode V2.11-25, UNIX
  PALcode V2.08-19, in the `FW_LFU_BUNDLE` form, loaded at 0x900000.
- Update utility: `GS1280_V7_3.EXE` (`FW_RAW_IMAGE`). On a real machine
  it writes the console into the MBM's flash, which this packet does not
  need.
- SROM `MVSROM_V1_0_9.BIN`, XSROM `MVXSROM_V1_0_31.BIN` (and the `_NO_Z1`
  variant): references for what the handoff state is. Both carry the same
  0x40-byte "CPQ MARVEL ALPH" header as the console, with the code after
  it: "SROMFW", 0x1640 bytes, and "XSROMFW", 0xfd60 bytes. Disassembling the
  XSROM's end is the best source for the handoff registers.
- MBM, CMM, PF, FPGA and CPLD images: firmware of the management
  processors, not Alpha code. The CMM's is Intel 386EX code (M4).
- What the firmware requires that the emulator does not have yet: nothing
  for the ES47's console and OpenVMS 8.4's CD boot (the 44-bit decode, the
  EV7 CSR block, the GIO transport and the larger cache came with M1-M3,
  the CMM's answers with M4, the IO7 with M5). Open items are listed under
  "M5".

## Sources

Saved in `lab/docs-ev7/` (git-ignored):

- **Linux** (`linux-arch_alpha_*`; master, plus v4.19 and v2.6.39 copies
  of the files that changed):
  - `core_marvel.h/.c`, `sys_marvel.c`: the address map, IO7, interrupts,
    TOY;
  - `err_marvel.c`, `err_ev7.c`, `err_ev7.h`, `err_impl.h`: the error
    frames;
  - `gct.h/.c`: the Galaxy Configuration Tree;
  - `hwrpb.h`, `setup.c`, `smp.c`, `time.c`, `rtc.c`, `pci_iommu.c`.
- **HP manuals**:
  - *ES47/ES80/GS1280 User Information* v3.0 (power-up flow pp. 68-71,
    power-on logs pp. 65-67);
  - *Installation Information* (8-CPU log pp. 121-122);
  - *Technical Summary*;
  - *SRM Console Reference* (`show config` pp. 53-54, `show cpu` p.55,
    `show mem` p.62);
  - *Firmware release notes V7.2*.

  Text extractions are alongside, as `*.txt`.
- **A real ES47's `show config`**, from mattst88.com, kept in
  `test/platforms/es47/show-config.txt`.
- **The console itself**:
  - `srm73-dump-strings.txt`: the strings of the running image;
  - `srm73-csr-tables.txt`: four register tables (EV7 141 entries, IO7
    PCI-X port 50, IO7 AGP port 45, IO7 port 7 62), extracted from the
    image by `csrtab.py` (in the session scratchpad; it scans for
    20-byte records {name, offset, 1, 0, 0}).
- **Not found**: the *Alpha 21364 Hardware Reference Manual* and the
  *Marvel/EV7 System Programmer's Manual* (Rev 1.00, May 2001, cited by
  `core_marvel.h`). No public copy turned up. The register tables inside
  the console are the best substitute so far.

## Reference output

`test/platforms/es47/`:

- `show-config.txt`, from a real ES47 7/1300 running SRM V7.3-11;
- a README pointing at the GS1280 `show mem`/`show cpu` examples in the
  SRM reference.

There is no ES47 `show memory` or `show device` of the machine we will
configure. L3 can compare structure (PIDs, memory placement, hoses, CPU
type and revision), not whole listings.

## The acceptance ladder for Marvel

| Level | For Marvel | Notes |
| --- | --- | --- |
| L0 | every lane builds with the EV7 core and the Marvel module | |
| L1 | the console image loads, its PALcode runs the reset path, the console proper starts | **reached on the `es47` row** (EV7 rows, Marvel registers; first on the retired probe row) |
| L2 | `P00>>>` on the telnet port, through the emulated CMM's GIO terminal | **reached** with M4 (2026-10-02) |
| L3 | `show config` matches the real ES47 listing in structure: "PID 0 ... EV7 rev x, NNN MHz", "Memory 0 ...", "IO7 0 ...", "PCI Bus 0 Hose 0 ...", PID 1 "No Local I/O", the RIMM table; `show cpu` "Type Major 15"; `show mem` with PID 1 at 400000000 | **reached** with M5 (2026-10-02). Real listings exist only for other configurations and console versions (V7.3-11 against our V7.3-1), so L3 is a structural match. Device lines depend on which I/O we emulate behind the IO7 |
| L4 | `test` and console network boot through a NIC on an IO7 hose; disk boot of a CD | **reached** for the CD: `boot dka400` loads OpenVMS's APB from a SCSI CD on hose 2 (M5); network boot not tried |
| L5 | OpenVMS 8.4 boots from `lab/ALPHA084.ISO` | **reached** with M5: date prompt, installation menu and DCL on both EV7s (2026-10-02); with M7 installed to disk, booted to login on the ES47 and the ES80, DECwindows on the Radeon (2026-10-03) |
| L6 | the ES40 console-log check is clean, the JIT cross-check is 0 | after every packet that touches shared code (M0, M1) |

## Plan

The staged implementation, in packets. Sizes are rough estimates of new or
moved lines of code, **[guess]**.

| # | Packet | Contents | Size | Level | Status |
| --- | --- | --- | --- | --- | --- |
| P | L1 probe | experimental `marvel-probe` board row (EV68, ES40 devices); the trace reports unclaimed accesses with the full address and folds repeated accesses | <50 | L1 | **done**; the row was retired by M3 (the `es47` row replaces it) |
| M0 | **Separate the chipset from CSystem** | Today `CSystem` *is* the Tsunami: its `ReadMem`/`WriteMem` mask to 0x807_ffff_ffff and decode Cchip/Dchip/Pchip/TIG, `interrupt()` is the Cchip's DIR/DIM, and the PCI hoses are the Pchips. Split a `Chipset` interface out (non-memory decode, interrupt delivery, timer, PCI hose access, DMA translation), keep Tsunami as the first implementation with no behaviour change, and let the board row pick it. Titan (ES45) needs the same split, so it is shared work with the Tsunami-family effort | 1.5-3 k moved, ~500 new | L6 | **done** 2026-10-02 (see "M0: the chipset split") |
| M1 | **EV7 CPU model** | `cpu_model` grows a family field (EV6 / EV7). An EV7 row: chip ID, type 15, minor per revision, EV7z row (type 16 if the console agrees). Physical addresses masked to 44 bits. Per-CPU PID (WHAMI). Interrupt delivery from the Rbox instead of the EIR pins. vmspal fast paths and JIT PAL shortcuts off for EV7 until verified. Kept apart from the EV6 code wherever the PALcode interface differs | 500-1000 | L1 | **done** 2026-10-02 (see "M1-M3") |
| M2 | **EV7 on-chip CSR block** | per EV7 a device answering its 4 MB CSR window: Rbox (WHOAMI, INT/IMASK/IREQ/INTQ/INTA, the IT interval timer, SCRATCH, routing CFG reading as configured), Cbox, Zbox (memory configuration consistent with the board row, errors clean), GIO registers and the 0x80000 lock | 1-2 k | L1+ | **done** 2026-10-02: the transport and a recording management side; the answers are M4 |
| M3 | **SROM/XSROM replacement** | a loader that leaves each EV7 as the XSROM leaves it: registers, CSR state, memory per PID, secondaries waiting on RBOX_SCRATCH for a jump address, and the console placed by the existing LFU loader (plus the >2 MB cache) | 300-600 | L1+ | **done** 2026-10-02, with the `es47` board row |
| M4 | **CMM/MBM replacement (GIO protocol)** | the console's GIO protocol, reverse-engineered from its `cmm_*`/`giott`/`get_mbm_configuration` code and the CMM/MBM firmware as references; a terminal on telnet; the configuration, partition database, FRU, TOY and NVRAM answers | 1.5-3 k, **most uncertain** | L2 | **done** 2026-10-02 (see "M4"): ~800 lines |
| M5 | **IO7 module** | port 7 CSRs, four ports (3 PCI/PCI-X + AGP), config/mem/IO windows, SG DMA (reusing the Pchip window logic once M0 has separated it), LSI/MSI control routing IIDs to an EV7's Rbox, the error registers clean; existing PCI devices on IO7 hoses | 2-3 k | L3-L4 | **done** 2026-10-02 (see "M5"): ~900 lines, and L5 with two CMM fixes |
| M6 | **Board rows** | `es47` (2 EV7, 1 IO7), then `es80` (up to 8, router mesh) and `gs1280` (up to 64, multiple IO7s, partitions); the PID-to-memory placement, the hose numbering, which I/O sits behind the ES47's embedded IO7 | 200-400 each | L3 | **done** 2026-10-02 for one partition: routes (M6a), EV7z (M6b), ES80 and GS1280 rows (M6c) |
| M7 | **Guests** | OpenVMS 8.4 from the ISO: GCT, HWRPB checks, interrupts end to end, TOY through cserve | ? | L5 | OpenVMS: **done** 2026-10-03: the CD boot to DCL came with M5; from the real CMD 649 IDE (`dqa0`) with M7a, beside the real USB; installed to disk, login on the ES47 and the ES80, DECwindows on the Radeon with M7b. Linux is open |

The order is M0, then M1+M2+M3 together (the console reaches the GIO
conversation with real answers to its CSR reads), then M4 (the prompt),
M5 (devices) and M6/M7. M4 can start as research in parallel at any time:
it is disassembly of the console and of the CMM firmware, no emulator
code.

## Findings

### M7a: the ES47's on-board I/O (2026-10-03)

**Result**: the I/O expander module's USB and IDE controllers are the real
parts now, and `show config` matches the real ES47's listing
(`test/platforms/es47/show-config.txt`) everywhere the emulated machine
has the same hardware: the IO7 and drawer lines, "Backplane rev 2", and
hose 2's slots 2 and 3 line for line -- "CMD 649 PCI-IDE ... dqa",
`dqa.0.0.2.2`, `dqa0.0.0.2.2 CD-W216E` (with `model_number = "CD-W216E"`),
"USB" usba..usbd at functions 0-3, `hub` under usba-usbc and none under
usbd. What still differs is what is configured differently: the option
cards (DEGX2-TA, the SIIG serial card, the Radeon), the console version
(V7.3-1 against V7.3-11), and slot 1, where a 53C895 still stands in for
the AIC-7892 (below). The machine block (`lab/es47-onboard/final.cfg`;
the listing and its diff against the real one are
`lab/es47-onboard/final-show-config.{txt,diff}`):

```
  pci2.1 = sym53c895 { disk0.0 = ramdisk { size = 64M; } disk0.1 = ... }
  pci2.2 = cmd649
  {
    disk0.0 = file { file = "ALPHA084.ISO"; read_only = true; cdrom = true;
                     model_number = "CD-W216E"; }
  }
  pci2.3 = uss344 { port1 = "tablet"; }
```

**The USB part is an Agere (Lucent) USS-344 QuadraBus**, not a NEC
uPD720101: the console's PCI table (0x3a9a08) names 11C1:5803 "USB" only
with subsystem 11C1:5803 -- the entry the real listing's four "USB"
functions come from --, and the USS-344 data sheet (Advance Data Sheet
rev. 9, June 2001) has exactly four PCI functions, each a single-port
OHCI 1.0a host controller, all on INTA, revision 0x10, a power-management
capability at 0x50, the OHCI legacy support registers. That is the `ehci`
card's companions without its EHCI, so it is the same model (`CEhci`,
class `uss344`): a chip row says whether there is an EHCI, how many OHCI
functions and with what identity; without an EHCI the ports are the OHCIs'
for good and the EHCI's schedule thread never starts. The console lists
the four functions as the real one does, root hubs included; OpenVMS 8.4
configures OHA0, OHB0 and OHC0 -- three of the four; why not the fourth
is not known (the console, real and emulated, gives usbd no root hub
either) -- and shows them Offline in the installation environment, where
the USB configuration manager is not started. Windows 2000 RC2 on an ES40 (the card at `pci1.2`) binds
"Standard OpenHCD USB Host Controller" to all four functions and mounts a
USB disk on port 2 (`lab/es47-onboard/w2k-uss344-evidence.txt`).

**The CMD 649** (`cmd649`, `devices/pci/Cmd649.*`) is a new part on the
IDE core that the ALi's IDE function was: `CIdeController`
(`IdeController.*`, the former `AliM1543C_ide.*`, moved with no change in
behaviour) holds the task file, PIO, bus-master DMA, ATAPI and the
controller threads; a part declares its PCI function and where a
channel's interrupt goes. The ALi's are its legacy ports and the ISA
IRQ 14/15 or, in native mode, INTA; the CMD 649's are 1095:0649, class
0101 programming interface 8F (both channels native), BARs 0-4 in I/O
space, one INTA, and the CMD's per-channel interrupt latches (CFR<2>,
ARTTIM23<4>, both in MRDMODE<3:2> and bus-master byte 1; write one to
clear; MRDMODE<5:4> keep a channel off INTA), from the register layout
Linux's cmd64x driver programs. Neither the console nor OpenVMS touched
those latches in any run: both drive it as a plain SFF-8038i controller.
The console prints "do not use secondary IDE channel on CMD controller"
while probing hose 2, as real power-up logs in the User Information do.

**The expander's interrupts share slot 1's lines.** The console gives the
three controllers interrupt lines 0x44, 0x45 and 0x46 -- the IO7 LSIs it
expects them on --, where their slots' own INTA would be 0x44, 0x48 and
0x4c, and OpenVMS enables LSI_CTL 0x45 for the CMD 649. So slot s's INTA
is slot 1's INTx s - 1, a board fact in `es47_pci_interrupt`
**[inference from the console and OpenVMS: no schematic]**. With the
slots' own lines, OpenVMS's DQDRIVER waited ten seconds for each command
(IDENTIFY, IDENTIFY PACKET, ...) and never reached its date prompt; the
USB functions interrupted on an LSI nobody enabled.

**OpenVMS 8.4 boots from dqa0** (the 53C895 and the USS-344 beside it,
`lab/es47-onboard/onboard.cfg`; `lab/es47-onboard/vms-dqa0-dcl-console.log`):
`boot dqa0` to the date prompt, the installation menu and DCL in about
four minutes from power-on on the JIT lane, both EV7s active. `SHOW
DEVICE`: DQA0 mounted (ALPHA084, 0 errors, 1525 operations), DQA1 (the
empty slave position: the driver's SYS$CONFIG entry gives the CMD 649 two
units) offline with one error, PKA0, EWA0, OHA0-OHC0; `DIRECTORY
DQA0:[000000]` lists the CD. The console's part of the boot -- reading
APB, then everything OpenVMS loads through the console's I/O callbacks
until its own DQDRIVER takes over -- moves one 2 KB block about every
20 ms (`dq_poll`: the console's IDE driver never enables its interrupt and
polls), some 6 MB in 95 s.

**Windows 2000** has no CMD 649 driver -- RC2's `mshdc.inf` names only the
CMD 0640, 0643 and 0646 -- and needs none: the card (on an ES40,
`pci1.1`) matches `PCI\CC_0101` and runs on the generic "Standard Dual
Channel PCI IDE Controller" (`pciide`), with both channels native. A FAT
disk on its primary master was read and written (`dir`, `copy`), and its
CD-ROM got a drive letter (the OpenVMS CD's ODS-2 file system is not one
Windows reads: "Incorrect function")
(`lab/es47-onboard/w2k-cmd649-evidence.txt`). Two IDE controllers in one
Windows 2000 guest need distinct `serial_number` values on their drives:
with the default (every disk "ES40EM00000") the second controller's drives
duplicate the first's device IDs and Windows stops with 0xCA
(PNP_DETECTED_FATAL_ERROR, duplicate PDO).

**Where the "I/O Drawer" line comes from** (`show_core_system`,
0x2dcd3c-0x2dcdd4): the drawer, cabinet and riser are bytes 0-2 of the
IO7's entry in the CMM's partition database (`find_io7_data`, 0x2f2d70,
finds it by the EV7's coordinates in bytes 3-4), and the backplane
revision is the IO7's IO_SYS_REV<3:0> (`read_p7_csr` of 0x300040). The
board row gives the revision (`marvel_layout::io_backplane_rev`: 2 on the
ES47 as listed, 2 on the ES80 **[inference: the same 2P drawer]**, 0 on
the GS1280); the CMM fills the drawer and cabinet from the PID as the
console's own `pid2drawer`/`pid2rack` read it (<4:3>, <7:5>), riser 0. On
the ES47 nothing in the listing changes but the revision; an ES80's IO7s
now print drawers 0-3 **[inference: no real ES80 listing]**.

**The AIC-7892, assessed** (it stays a 53C895 stand-in): the console's
`aic78xx` driver is Adaptec's CHIM (Common Hardware Interface Module):
421 routines, 138 KB of console code (0x34c4c0-0x36dff0), including
`scsihloadsequencer` -- it downloads Adaptec's own sequencer program into
the chip. OpenVMS drives 9005:008F with `SYS$PKADRIVER` (its
SYS$CONFIG entry "Adaptec AIC-7892"), which carries its own sequencer
code too. So the chip cannot be modelled at the level of a command
interface, as the ISP1040's mailboxes are: an emulation has to execute
the AIC-7xxx sequencer -- its instruction set, the SCB RAM and the
queue-in/queue-out FIFOs, the data FIFOs and DMA, and the SCSI bus phases
at the REQ/ACK level the sequencer works at -- against two different,
undocumented programs (the free aic7xxx sequencer source documents the
hardware, not Adaptec's code). That is a model the size of the
53C8xx's SCRIPTS processor (some 5000 lines here) or larger, with no
reference trace to check it against; OpenVMS and the console already
have a working SCSI path in the 53C895, so it was left.
### M7b: OpenVMS 8.4 installed, DECwindows on the Radeon (2026-10-03)

**Result**: OpenVMS 8.4 installs from its CD onto a SCSI disk of the
emulated ES47 and boots from it to the login prompt on both EV7s; the same
disk boots on the ES80 row with eight. With a USB keyboard (new) and the
tablet on the ES47's own USB -- the USS-344 in hose 2 slot 3 (M7a) --
DECwindows starts on the Radeon 7500 in the AGP slot and draws the CDE
login box, "Welcome to ES47", at 1024x768: typed keys reach it, the pointer
moves and clicks, its Help dialog opens. Without licences it goes no
further than on the ES40 (docs/openvms.md). On the final build it started
cleanly in 8 of 8 two-processor runs. A crash in OpenVMS's USB driver at
boot, first taken for a JIT fault, was the OHCI answering control transfers
faster than any controller can (below, fixed). The disk image, its README,
the transcripts and frames are in `lab/platforms/marvel/m7/`.

**The installation** (`install-console.log`): `boot dka400` with the CD at
hose 2 slot 1 (53C895, ID 4) and an empty 4 GB file at ID 0 (DKA0), then
the usual answers (INITIALIZE, DKA0, ODS-5, SCSNODE ES47, no DECnet,
DECwindows and TCP/IP). The PCSI execution phase took 12 minutes on the JIT
lane, as on the ES40; the first boot of DKA0 asked for the date (every
first boot does), ran AUTOGEN and rebooted through the console by itself,
and came up to `Username:`. Later boots reach a DCL prompt in about a
minute. `SHOW CPU`: "hp AlphaServer ES47 7/1000", 0 and 1 active.

**The ES80** (`es80-8cpu-login-console.log`, `platform = "es80"`, eight
`ev7` at 512 MB each, the same devices on PID 0's IO7): the installed disk
boots to login, seven `%SMP-I-CPUTRN`, `SHOW CPU` "hp AlphaServer ES80
7/1000" with 0, 1, 8, 9, 16, 17, 24, 25 active, `SHOW MEMORY` 4.00 GB.

**The Radeon's driver.** The CD's own system has no `SYS$GHDRIVER.EXE`;
the DECwindows Motif kit on the same CD installs it, with
`DECW$SERVER_DDX_RADEON.EXE` (and DRM, Mesa and GLX images) in SYS$LIBRARY.
`GHA0` configures on the first boot with the card, and the startup sets
WINDOW_SYSTEM to 1. `@SYS$MANAGER:DECW$STARTUP` by hand then starts the
server -- once there is a keyboard and a mouse.

**Input devices.** The EV7 machines have no 8042: DECW$DEVICE.COM's
platform path waits 15 s for `MOU` and then `KBD` USB devices
(`%DECW$DEVICE-I-NOINPUTDEVICES` otherwise), which is where the server
stopped at first. OpenVMS 8.4 Alpha has only the OHCI driver
(`SYS$OHCIDRIVER`, bound to the Lucent/Agere USS-344, the NEC 1033:0035 and
the Philips 1131:1561 in SYS$CONFIG.DAT) and the HID class
(`SYS$HIDDRIVER`, `SYS$KBDDRIVER` for usage page 1 usage 6,
`SYS$MOUDRIVER` for usage 2). So, on the on-board USB:

```
  pci2.3 = uss344
  {
    port1 = "tablet";
    port2 = "keyboard";
  }
```

The tablet's top collection is a mouse, and `SYS$MOUDRIVER` follows its
absolute coordinates (the pointer lands where it is put). The keyboard is
the new `CUsbKeyboard` (docs/usb.md). OpenVMS enumerates them as `HID0`,
`MOU0`, `KBD0` under `UCM0`, and sets the keyboard's report protocol, its
LEDs and idle 0. It configures OHA0-OHC0, three of the USS-344's four
functions (M7a): `SYSMAN IO SHOW BUS` lists nodes 24-26 (slot 3,
functions 0-2) and no node 27, although the console gives function 3 its
BAR and line 0x46 like the others -- why is still not known.

All four functions interrupt on one LSI (0x46), and that only works
because the USS-344 is not a NEC: `SYS$OHCIDRIVER`'s interrupt routine
(SYS$OHCIDRIVER+0A10) services, for a NEC controller, only its own; for
anything else it polls up to four controllers that share the vector
(`IO_INTERRUPT` keeps one VEC per SCB slot). Before M7a, with the `ehci`
card (a NEC) in that slot, OHA's start-of-frame interrupt was never
acknowledged and the line stormed until startup stopped; a NEC card works
in an option slot (`pci1.1`), where its functions have lines of their own.

**Fixed:**

1. **The embedded slots' interrupt lines** (`marvel_pci_interrupt`,
   platforms/es47/Es47.cpp; one implementation, which M7a's rotation of
   slot s's INTA onto slot 1's INTx s - 1 is now part of). On port 2 of an
   IO7 whose I/O type is "Embedded I/O", the console (the routine at
   0x2eab70 that writes configuration register 0x3c) gives slot 2 slot 1's
   INTB -- function 1 INTD -- and slot 3 slot 1's INTC for every function,
   whatever their pins say; OpenVMS takes the line from that register and
   enables that LSI. Probed: a card in 2/2 gets 0x45, three functions in
   2/3 all 0x46, cards in 2/4 and 2/5 0x50 and 0x54. That agrees with M7a
   for every device there (the 53C895 0x44, the CMD 649 0x45, the USS-344's
   functions, all on INTA, 0x46) and differs only where M7a's rotation was
   an inference: other pins, and slot 2's function 1. The board's
   `pci_interrupt` hook takes the function (-1 behind a bridge; every other
   board ignores it). It is the rule of any embedded-I/O IO7, so the ES80's
   and GS1280's too.
2. **Shared lines** (`CPCIDevice::do_pci_interrupt`). Functions of one
   device on the same input are now ORed: one function dropping its request
   no longer drops another's (the USS-344's four on 0x46).
3. **The clock rendezvous** (Ev7Csr.cpp). Every 128th interval-timer
   interrupt, the PALcode's handler (0x39344; the console's and OpenVMS's)
   makes the primary write RBOX_INT<23> to every other processor's
   RBOX_IREQ (0x396b8), while a secondary clears its own <23> and spins, in
   PALmode, until <23> or <22> is set (0x3972c). On the hardware the
   secondary is waiting before the broadcast leaves; here CPU 0's thread
   ticks both and runs its own handler first, so the broadcast often came
   before CPU 1 had taken the same tick, CPU 1's clear erased it, and CPU 1
   spun a whole window (128 ticks, ~130 ms) with its interrupts off -- every
   window. `ALPHABOX_TRACE_RBOX` showed PID 1 holding enabled interrupts
   150-280 ms in PALmode at 0x39734 and merging a quarter of its ticks
   (59,000 of 252,000); OpenVMS bugchecked CPUSPINWAIT when the DECwindows
   server started, in 11 of 14 two-processor runs (one processor: 3 of 3
   clean). A broadcast that arrives before the processor has begun waiting
   is now held and set at its clear. After: 861 merged in 279,000, and no
   150 ms waits.
4. **A USB keyboard** (`port<n> = "keyboard"`, devices/usb/UsbKeyboard.*):
   a boot keyboard whose state changes are queued as reports, so a scripted
   press and release between two polls still types.
5. **Keys and pointer without an 8042** (gui/sdl.cpp): the window's keys
   and `ALPHABOX_KEYPIPE`/`ALPHABOX_KEYSCRIPT` now reach the USB keyboard as
   well as the PS/2 one (`gui_guest_key`); before, a key pressed in the
   window of a machine with no 8042 dereferenced a null keyboard.
6. **JIT_VERIFY counted only GPR differences.** A differing store, STx_C,
   PC, IPR, FP register or store count was printed and left out of the
   "N mismatches" summary, so `srm_run.sh` and a verify run reported 0 with
   real differences on the screen. Each now counts its block as a mismatch
   (once), with the block's words printed. Counting showed 86 in one
   OpenVMS boot of the ES47, all one block (a kernel-process context switch
   at ffffffff80a63948: `MF_FPCR f0; STT f0`, ... `LDT f0; MT_FPCR f0`):
   **the verifier**, not the JIT -- it restored the GPRs, FP registers and
   the IPRs a compiled pass reads before that pass, but not FPCR, so the
   compiled `MF_FPCR` read what the interpreter's later `MT_FPCR` had left.
   FPCR and EXC_SUM are now restored too. That exposed one real
   divergence, in the JIT: `LDS`/`LDT` to f31 (a prefetch) was dropped,
   where the interpreter's FPSTART takes the FEN trap with FP disabled and
   otherwise clears EXC_SUM; both emitters now do the same. After: an ES47
   OpenVMS boot on the verify lane, 546 and 554 million compiled blocks on
   one and two processors, 0 mismatches; `srm_run.sh` on the verify lane 0.

Also: the OHCI register trace (`ALPHABOX_USBTRACE`) names its controller
(`ohci0`..`ohci3` on the card) and counts reads per controller.

**Fixed (2026-10-03): the USB crash at boot was not the JIT's.** In about
one boot in ten with USB devices, OpenVMS bugchecked INVEXCEPTN during
startup, at the same moment every time (just after `%EWA0, Half Duplex
10BaseT connection selected`): an access violation in
`SYS$USBDRIVER+04F48`, a structure pointer read as 2, on CPU 0 at IPL 8
with IOLOCK8 held. The counts M7b first gathered (4 of 19 on the JIT,
0 of 10 on the interpreter, 0 of 6 on the verify lane) made it look
JIT-only; they were too small to say so, and the interpreter crashed the
same way in 1 of 30 boots once it was given that many.

What the dump says (`runs/boot-crash1`, ANALYZE/CRASH; transcripts in
`lab/jitverify-sweep/sda-crash1*.txt`). `+04F48` is in a completion routine
(the procedure at `+04EF0`) that the USB stack's kernel process calls for
each request on its work queue (the loop at `+01970`). The routine asks a
helper (`+03790`) for a pointer out of the request, and the helper refuses
-- the request's state word is 0, not 5 -- returning an error status the
caller does not check; the 2 is whatever was in the stack slot. The
request had been completed already. Each request keeps a ring of its own
events with timestamps and caller addresses (`+0x88`, `+0x90`, `+0x890`
from its base), and this one shows the queue-to-kernel-process and
dequeue entries twice: once with the rest of its life, and again 5.0 s
later -- a request timeout firing for a request that had completed
[inference: that its timeout was started after the completion came back,
so there was nothing to cancel].

What the emulator did to bring that about (`ALPHABOX_USBTRACE=2`, which
also made the crash far more likely: 2 of 5 boots). Both devices -- the
tablet on the USS-344's function 0, the keyboard on function 1 --
enumerate at once, on two controllers. In each crash one of them sent a
control request (SET_ADDRESS twice, GET_DESCRIPTOR once), the controller
ran it and wrote the done queue back within the same 0.1 ms -- the OHCI
model's early done queue, there for usbstor's sake, applied to control
transfers too -- the OHCI interrupt was serviced, and that device's
enumeration never took another step. Five seconds later the driver
disabled and reset its port, and the system bugchecked (or, once, hung).
No real controller answers that fast: a control transfer takes its
stages' bus time inside a frame and is written back at the frame's end.

**The fix** (`COhci::write_back_early`, devices/pci/AliM1543C_usb.cpp): a
done queue holding a control TD now waits at least a millisecond of real
time after the TD was retired, and goes back at the next frame end or pass
after that; bulk and interrupt TDs keep the early done queue (usb.md).
`ALPHABOX_OHCI_EARLY_CTL=1` restores the old behaviour for A/B runs.

| build | trace | crashed |
| --- | --- | --- |
| main 0104207, JIT | -- | 1 of 13 |
| main 0104207, interpreter | -- | 1 of 30 |
| main + trace, JIT | `ALPHABOX_USBTRACE=2` | 2 of 5 |
| control TDs at the frame's end only (first try) | `ALPHABOX_USBTRACE=2` | 0 of 17 |
| control TDs at the frame's end only (first try) | -- | 1 of 30 |
| first try + 10 ms root-port reset | `ALPHABOX_USBTRACE=2` | 1 of 10 (a hang: the same lost completion) |
| **the fix: a control TD's completion at least 1 ms after it ran** | `ALPHABOX_USBTRACE=2` | **0 of 15** |
| **the fix**, JIT | -- | **0 of 60** |
| **the fix**, interpreter | -- | **0 of 20** |

The first try (write a control TD back only at a frame's end) was not
enough: a request made just before the frame thread's tick still came back
within microseconds (the hang's trace has a GET_DESCRIPTOR answered within
0.1 ms of the request). A 10 ms root-port reset, tried with it, did not help
-- OpenVMS polls a reset for about 9 ms, then asks again -- and was
dropped. The verify lane's earlier 0 of 6 said nothing either way: it runs
the guest several times slower, which changes the timing the race needs.
The production JIT with chaining, inline memory and the RPCC stub all
switched off (`ALPHABOX_JIT_CHAIN=0`, `INLMEM=0`, `RPCC=0`) ran clean in
the 7 boots it had before the interpreter's crash made the question moot.

Counts and traces: `lab/jitverify-sweep/usbcrash/` (`bootcount.txt`, the
crashes' console logs and `ALPHABOX_USBTRACE=2` output).

**No longer seen** on the final build (8 two-processor DECwindows runs):
the CPUSPINWAIT that remained after the rendezvous fix (1 of 3 starts
before the rebase, CPU 1 holding INVALIDATE and XFC with no interrupt
pending -- plausibly the DTB page-cache fault 86b2469 fixed), and the
keyboard's failed enumeration ("hub_configure_device Set configuration
failed", 3 times in about thirty boots before).

**Diagnostic.** `ALPHABOX_TRACE_RBOX=<ms>` (default 200) starts a thread
that reports any EV7 leaving an enabled interrupt (RBOX_INT & RBOX_IMASK)
pending that long -- with its EI lines, the core's pc, `eir`, `eien`,
`check_int` and instruction count -- and when it is finally taken, and
every 10 s each processor's interval ticks and how many found the previous
one still pending (`%MVL-T-RBOX`). It tells an interrupt not raised from
one not taken, and a processor stuck in PALmode from one not running.
`ALPHABOX_TRACE_IO7=1` shows which processor OpenVMS sends each LSI to
(LSI_CTL<22:14>): the OHCIs to PID 0, the DE500 to PID 1.

### M6c: the ES80 and the GS1280 (2026-10-02)

**Result**: two more board rows on the same console image, both at L5.

- `platform = "es80";` with eight `ev7` processors: the console reaches
  `P00>>>` with all eight in `show cpu` (PIDs 0, 1, 8, 9, 16, 17, 24, 25),
  `show memory` lists each processor's memory at its PID's base, `show
  config` lists four IO7s (PIDs 0, 8, 16, 24; hoses 0-3, 32-35, 64-67,
  96-99) and the devices placed behind them, and OpenVMS 8.4 boots from the
  CD to DCL with all eight active (NICs on three IO7s configure).
- `platform = "gs1280";` with sixteen: the same, a 4x4 torus, PIDs 0-15,
  one IO7 on PID 0, OpenVMS `SHOW CPU` "Active 0-15". With eight the console
  places the memory exactly as the real 8-processor GS1280 power-up in the
  Installation Information does (PID 4 at 0x20_0000_0000 and so on).

Transcripts in `lab/platforms/marvel/es80/`: `es80-console-show.log`,
`es80-8cpu-vms-dcl-console.log`, `gs1280-8cpu-console-show.log`,
`gs1280-16cpu-vms-dcl-console.log`. 512 MB per processor
(`memory.bits = 29`). From the ES80's:

```
hpcount = 1, spcount = 1, ev7_count = 8, io7_count = 4
IO7-100 (Pass 3) at PID 0 ... at PID 8 ... at PID 16 ... at PID 24
PID 8 memory: 4000000000, 512 MB
...
                           hp AlphaServer ES80 7/1000
PID 8		CPU 0		Cabinet 0  Drawer 1
 NS,EW (2,0)	Hard ID 8	1.50 MB Cache		EV7 rev 2.1, 1000 MHz
 Memory 8			512 MB
 IO7 8  			Embedded I/O		IO7 pass 3
   PCI Bus 0	Hose 32 	64 Bit, 66 MHz		PCI 2.2 mode
...
$$$ SHOW CPU
System: hp AlphaServer ES80 7/1000
   Active               0,1,8,9,16,17,24,25
$$$ SHOW MEMORY/PHYSICAL
  Main Memory (4.00GB)            524288      510903       13272         113
```

**The topology** (`chipsets/marvel/Topology.*`, the rows' `marvel_layout`
in `platforms/es47/`, `es80/`, `gs1280/`):

| | ES47 | ES80 | GS1280 |
| --- | --- | --- | --- |
| Source | User Information, a real `show config` | Technical Summary: up to four 2P drawers, N/S ports only, "a ring of processors" | Technical Summary: 8P drawers of four modules, all four ports, a torus |
| Processor n at | NS n, EW 0 | NS n, EW 0 (n < 8) | NS (n & 1) \| (n >> 3 & 1) << 1, EW (n >> 1) & 3 |
| PID (the console's coord2id) | n | (n & 1) \| (n >> 1) << 3 | n |
| CMM system type (0x40004) | 0x11 | 0x10011 ("ES80": <19:16> 1) | 0x1 |
| IO7s | PID 0 | each drawer's first processor: 0, 8, 16, 24 | PID 0 **[a choice]** |
| Max processors here | 2 | 8 | 16 (the console's layout goes to 64) |

What the console makes of a PID (its own `coord2*`/`pid2*` routines):
<0> the place on the module, <2:1> the module in its drawer, <4:3> the
drawer, <7:5> the cabinet. The ES47/ES80 rule ignores E/W and puts N/S
bits 1-2 in the drawer field, so an ES80's second drawer starts at PID 8.

**What changed for more than one module:**

- **The CMM** (`platforms/es47/Cmm.*`) keeps a memory per module (PID >>
  1) and a port per PID; each processor's area is its place on the module.
  Every processor but the partition's primary gets a non-zero start state.
  The **MBM configuration** (0x0321) goes to each drawer's MBM
  (10.<cabinet * 16 + drawer>.0.1; memconfig asks once per drawer) and
  lists that drawer's modules; module m holds PIDs base + 2m, + 2m + 1.
  The **partition database** lists every processor at its coordinates and
  every IO7. **Its I/O entries are <3> N/S, <4> E/W** -- the reverse of what
  M4 wrote down, invisible while the one IO7 sat at (0,0): `coord2pid`
  (0x2e1240) matches them with the processors' <1> N/S and <2> E/W, and
  with them swapped the console found no IO7 beyond PID 0's. The
  **memory assignment** has a chunk per processor at its PID's base. An
  8P drawer's MBM reports 5 sensors, a 2P drawer's 17 (build_mbm_hw,
  0x2f8db0).
- **PIDs** come from the board row: `CChipset::cpu_pid` gives each
  configured processor its PID (the index everywhere but Marvel). CMarvel
  has a register block for every PID the row's layout can hold, of which
  the configured ones answer.
- **Memory**: the host array spans to the last PID's memory (an ES80's PID
  25 ends past 0xC4_0000_0000, so 2^40 bytes); untouched pages cost
  nothing (macOS calloc of even 16 TB succeeds lazily). The holes read as
  zero, as before.
- **More than four processors**: `CSystem` holds up to `kMaxCPUs` (32);
  the load-lock state moved out of the state-file structure, which keeps
  the first four processors' locks as it always had (the file format is
  unchanged).
- **Hoses** are global: PID * 4 + port; `marvel_slot_refusal` accepts the
  hoses of the processors that have an IO7.

**Open:**

- `show config` prints "I/O Drawer 0 ... Riser 0" for every IO7: found
  and changed by M7a (the partition database's I/O entries);
- each ES80 drawer's second processor can be cabled to an I/O expansion
  drawer, a GS1280's processors to several I/O drawers: one layout choice
  each here;
- partitions (more than one hard or soft partition), and the GS1280's
  64-processor limit (16 here: host threads);
- the route table's IO entries (0x100-0x113), RBOX_ROUTE's own layout.

**The console terminal under OpenVMS stopped when typed at while it
printed** (found driving these boots: a menu choice sent while the menu
was still being printed left the output cut off mid-word, two runs out of
two, on every board). The PALcode's CMM interrupt handler (0x39bd0) reads
the reason register (GIO register 9) and dispatches only the first bit it
finds -- RX, TX, then the second processor's --, while the model cleared
every bit on the read: a transmit-ready read together with a received
character was lost, and OpenVMS waited for it for ever. Register 9 now
hands out one reason per read in the PALcode's order and the rest is
signalled again; the same race runs to DCL twice
(`es80/terminal-race-fixed-run*.log`). The real CMM's register is not
documented **[inference from the handler]**.

### M6b: the EV7z row (2026-10-02)

**Result**: with `ev7z` processors at `speed = 1300M` the console prints
what the real ES47 7/1300 prints (`test/platforms/es47/show-config.txt`),
and OpenVMS 8.4 boots from the CD to DCL on both
(`lab/platforms/marvel/es80/task2-ev7z-1300-dcl-console.log`):

```
                           hp AlphaServer ES47 7/1300
 NS,EW (0,0)	Hard ID 0	1.75 MB Cache		EV7 rev 3.0, 1300 MHz
 NS,EW (1,0)	Hard ID 1	1.75 MB Cache		EV7 rev 3.0, 1300 MHz
CPU 0   CurOwner 0  Owner 0   Type Major 15, Minor  5
...
$$$ SHOW CPU
System: hp AlphaServer ES47 7/1300
   Active               0,1
```

Before, the row (chip ID 2 with CSR 0x28020<19> set for "type 16")
printed "EV7 rev 2.2", type 15 minor 4. How the console names a part: the
PALcode (0x3ea94) indexes its table of (minor, 15) pairs at 0x3eb08 with
I_CTL<29:24> -- plus 0x28020<19> only when that is 2 --, minor = index +
1; the console's revision strings are a table at 0x3ac278 indexed by the
minor: "Unknown", "1.0", "2.0", "2.1", "2.2", "3.0". Minor 5 is reachable
only with chip ID 4. Other PALcode tests of the chip ID only single out 0
(rev 1.0: a quarter-rate interval timer). The `ev7` row's message minor
is now 3, what the console reports for it.

### M6a: the routes OpenVMS reads (2026-10-02)

**Result**: OpenVMS 8.4 no longer prints `mvcpu_get_numa_distances: bad
route IPR for self, cpu N, rt 0x0` on the ES47; both processors still join
and the CD boots to the date prompt (`lab/platforms/marvel/es80/task1-*.log`,
before and after).

**Which register.** Not a CSR read by OpenVMS: with `ALPHABOX_TRACE_CSR=1
ALPHABOX_TRACE_UNKNOWN=1` the only EV7 register OpenVMS itself read was
RBOX_WHOAMI. The routine, found in a memory dump of the booted CD (the
format string's address, then the procedure whose linkage section addresses
it: `lab/platforms/marvel/es80/tools/findref2.py`, `vmsva.py`), is
`mvcpu_get_numa_distances` at ffffffff800178f0 (disassembly in
`es80/numa-dis.txt`). For each PID 0-63 it calls **cserve 0x4f** (a1 = the
PID, a2 = the processor asked about) and keeps <27:4> of the answer; then
it requires the processor's route to itself to have bits 0x510 set, else
the message, with the entry as `rt`.

The PALcode's cserve 0x4f (0x3d2b0) reads a longword from a table in
memory: the console's route table (address at PAL area + 0x1b8, 0x94
longwords per PID; entries 0-0x7f for indices 0-0x7f, 0x80-0x93 for
0x100-0x113). Who fills it:

- the console for the primary (`start_secondaries`, 0x2dd0f0): from the
  CMM's memory, area + **0xe10** u16 a count, + **0xe12** u16 a second count,
  + **0xe14** that many longwords, each stored **shifted left by four** --
  what M4 had guessed was bad-memory data;
- the PALcode for each secondary from its own CMM area (0x3f108-0x3f368).

The table mirrors the EV7's **RBOX_ROUTE** registers: the console's
register table has an array entry the M1 extraction missed (`RBOX_ROUTE`,
offset 0x2000, 0x114 entries 0x10 apart). The XSROM sets the routes up
("Configure RBOX Routes", "Inverse Route Setup" in real power-up logs) and
leaves the copy in the CMM.

**The fields**, from how OpenVMS walks an entry (it counts hops from the
processor's own coordinates to the destination's, one direction at a time,
wrapping between the lowest and highest coordinate present and skipping
empty ones):

| Bits | Meaning |
| --- | --- |
| <4> | valid: a processor at that PID |
| <8> | set in the route to itself, with <4> and <10> **[guess: delivered to the local Cbox]** |
| <10> | the route is enabled (else unreachable) |
| <13>, <12> | E/W hops; <12> 1 = towards the higher coordinate |
| <15>, <14> | N/S hops; <14> 1 = towards the lower coordinate |
| <19:16>, <23:20> | the destination's E/W and N/S coordinates **[guess which is which; the walk is symmetric]** |
| <26>, <25:24> | a first hop in a fixed direction, not used |

**The model** (`chipsets/marvel/Topology.*`): `CMarvelTopology` knows each
processor's coordinates and PID (from the board row's `marvel_layout`) and
computes the route between any two: the shortest way round each ring,
N/S then E/W. The CMM writes each processor's 0x80 routes into its area;
`CEv7Csr` answers RBOX_ROUTE with the same values **[guess: the register's
own layout is not known, only the copy's]**. The IO routes (the second
count) are 0.

### M5: the IO7, devices behind it, and OpenVMS 8.4 (2026-10-02)

**Result: L5.** With the IO7 on PID 0 (`chipsets/marvel/Io7.*`), a
53C895 with a RAM disk and the OpenVMS 8.4 CD at hose 2 slot 1 and a
DE500-BA (`dec21143`, null backend) at hose 0 slot 1, the console reaches
`P00>>>` and `boot dka400` brings OpenVMS 8.4 up on both EV7s to its date
prompt, the installation menu and DCL, on the JIT and the interpreter
(headless) lanes. This is the first operating system on an emulated EV7.
The transcripts are in `lab/platforms/marvel/m5/` (`l5-*.log`,
`es47-l3-*.log`). The devices, in the machine block:

```
  pci2.1 = sym53c895
  {
    disk0.0 = ramdisk { size = 64M; }
    disk0.4 = file { file = "ALPHA084.ISO"; read_only = true; cdrom = true; }
  }
  pci0.1 = dec21143 { type = "null"; }
```

```
P00>>>show device
dka0.0.0.1.2               DKA0                           RZ58  2000
dka400.4.0.1.2             DKA400                        RRD42  4.5d
ewa0.0.0.1.0               EWA0              08-00-2B-E5-40-00
pka0.7.0.1.2               PKA0                  SCSI Bus ID 7
P00>>>boot dka400
(boot dka400.4.0.1.2 -flags 0)
...
    OpenVMS (TM) Alpha Operating System, Version V8.4
mvcpu_get_numa_distances: bad route IPR for self, cpu 0, rt 0x0
%PKA0, Copyright (c) 1998 IntraServer Technology Inc. PKW V2.1.22 ROM V1.0
%PKA0, SCSI Chip is SYM53C895, Operating mode is LVD Ultra2 SCSI
%SMP-I-SECMSG, CPU #1 message:   P01>>>START
%SMP-I-CPUTRN, CPU #1 has joined the active set.
Please enter date and time (DD-MMM-YYYY  HH:MM)  02-OCT-2026 12:00
    Installing required known files...
    Configuring devices...
%EWA0, Autosense mode set by console
...
Enter CHOICE or ? for help: (1/2/3/4/5/6/7/8/9/?) 8
(DCL) SHOW CPU
System: hp AlphaServer ES47 7/1000
   Active               0,1
(DCL) SHOW MEMORY/PHYSICAL
  Main Memory (2.00GB)            262144      254038        7946         160
```

`SHOW DEVICE` in DCL lists DKA0, DKA400 (mounted, ALPHA084), PKA0, EWA0
and OPA0.

**Against the real ES47's `show config`**, structurally (the rest of the
M4 table holds):

| | real | emulated | why |
| --- | --- | --- | --- |
| IO7 line | IO7 0, Embedded I/O, IO7 pass 3 | the same | IO_SYS_REV type 1, IO_ASIC_REV 0x12 |
| drawer line | I/O Drawer 0, Cabinet 0, Riser 0, Backplane rev 2 | Backplane rev 0 | IO_SYS_REV<3:0>: matches since M7a |
| hoses | Bus 0 66 MHz, 1-2 33 MHz, PCI 2.2 mode; AGP Bus 3, AGP rev 2.0, 1x/4x | the same | HP_DEV_CAP gives hose 0's slots 66 MHz (with no card there it reads 33 MHz) |
| devices | DEGX2-TA (0/1), a SIIG serial card (1/3), AIC-7892 (2/1), CMD 649 (2/2), USB (2/3), Radeon (3/5) | DE500-BA (0/1), 53C895 (2/1) | Alphabox has no AIC-7892, CMD 649 or BCM5703: the 53C895 stands in. The Radeon 7500 AGP exists since 2026-10-02 (`radeon` class; `pci3.5` gives the real listing's "Radeon 7500 AGP ... vga0.0.0.5.3", see docs/peripherals.md) for the AIC-7892 in its slot, the DE500-BA for the gigabit card; the console has drivers for both |

**The IO7 model.** Its space and registers are Linux's (`core_marvel.h`)
and the console's tables; what the registers *do* was read off the
console and its PALcode:

- **Identity.** `io7_init_np` (0x2e89e0) only builds its IO7 structure when
  IO_ASIC_REV<7:4> is 1 ("IO7-100"); <3:0> + 1 is the pass `show config`
  prints, so 0x12 is pass 3. IO_SYS_REV<16> says the type is valid and
  <7:4> is the I/O type, read by `get_io_type` (0x2ea0f0) and named from a
  table at 0x3ac378: 0 "3.3V PCI-X I/O", 1 "Embedded I/O", 2 "X-Shelf
  I/O", 3 "Std PCI-X I/O". The ES47's is 1.
- **Presence.** RBOX_IO_CFG <0> and <2> (the PALcode's test at 0x3941c):
  without them the console says "No Local I/O". The console writes
  POx_CACHE_CTL 0/8 to disable/enable a port and keeps the result in its
  own structure (`write_io_csr` 0x2e3fe0 sets bit 3 of a per-port flag on a
  non-zero write); configuration reads go nowhere on a port whose flag is
  clear. Linux tests CACHE_CTL == 8.
- **Hot plug.** The console drives a hot-plug controller on every port
  (`php_disconnect_all`, `php_pwr_on_all`, `php_connect_all` and the slot
  routines at 0x2e5490-0x2e65a0) and only probes slots it has powered and
  connected: HP_PWR and HP_CNTL take on/off masks in <13:8>/<5:0>, HP_MISC
  reports completions (<5:0> power, <21:16> connect; written with ones to
  clear), HP_INTR_IN gives the slot inputs (<s> interlock open, <8+s> power
  fault, <16+s> and <24+s> both set for an empty slot), HP_DEV_CAP the
  slots' speeds (<s> 66 MHz, <8+s> PCI-X, <16+s> 133). Hose 2's embedded
  slots are read but never powered by the console, so the model starts with
  every slot powered **[inference]**. Without the controller the console
  printed "is powered off" for every slot and found nothing.
- **Configuration space** at port + 0xFE000000, `bus << 16 | devfn << 8`,
  the layout `CPCIDevice` already uses; nothing there reads as all ones.
- **DMA**: four windows per port, the Pchip's scheme (`PciWindows.hpp`):
  the console opens window 1 (direct, 1 GB at 2 GB); OpenVMS adds window 2
  (scatter-gather, 1 GB at 3 GB).
- **Interrupts** (the PALcode's EI1 handler 0x398b4 and its end of
  interrupt at 0x38bc4): an LSI is port<7:5> slot<4:2> INTx<1:0>; when its
  line is asserted and LSI_CTL<24> is set, the IO7 sends the IID
  (PE <23:14>, LSI <8:0>) to the PID in LSI_CTL<22:14>. The EV7 queues it in
  RBOX_INTQ (<24> valid; the PALcode writes each one back to take it) and
  raises RBOX_INT<12>; the PALcode reads the IO7's PO7_SCRATCH<7:0> as the
  vector block and hands the OS 0x800 + LSI * 16. When the IPL drops it
  writes the IID to EOI_DAT of the port in IID<6:5>; the IO7 sends it again
  if the line is still asserted. The console enables LSI_CTL for the
  devices it uses and disables them when it stops its drivers; OpenVMS
  re-enables them itself (at ffffffff800159a4). MSIs are not modelled.

**What OpenVMS needed from the CMM** (`platforms/es47/Cmm.cpp`), found by
PC sampling and a memory dump of the stuck guest:

- **TOY byte 10 is an update flag, not register A.** OpenVMS's TOY read
  (ffffffff8001daf0) calls GET_TOY (cserve 0x49) for byte 10 and waits, in
  a delay loop, until the whole byte is zero; M4's model kept an
  MC146818's 0x26 there and OpenVMS waited for ever after its banner. The
  console's PALcode (GET_TOY of byte 0 answers -2 while <7> is set) tests
  only UIP. Byte 10 now reads 0, with <7> for the last 2 ms of each second.
- **The console terminal is interrupt-driven under OpenVMS.** The console's
  `cb_set_term_int` (`txon` 0x31aca0, `rxon`) sets the terminal's interrupt
  enables through cserve 0x46, which merges them into GIO register 8 (the
  PALcode's state word): <0>/<1> RX/TX for the module's first processor,
  <6>/<7> for its second. The CMM then raises RBOX_INT<9> (EI0); the
  PALcode's handler (0x39bd0) checks status <4>, reads register 9 for the
  reason and dispatches SCB vector 0x6c0 (RX) or 0x6d0 (TX) (0x700/0x710
  for the second processor). Without it OpenVMS printed nothing after its
  first messages and read no input. The model posts the reason and raises
  the interrupt from a per-tick hook (`GioManagement::tick`), outside the
  register-block and CMM locks, since the CMM is called with a register
  block locked.

**How it was found.** Each step was the next access the console made
(`ALPHABOX_TRACE_IO7=1` names every IO7 register access with pc and ra,
and shows every LSI_CTL/MSI_CTL write; `ALPHABOX_TRACE_UNKNOWN` the rest),
the call trace (`ALPHABOX_TRACE_CALLS`) for which routine stopped short,
and the console's routines read with `cdis.py`. Two console habits made
the static reading harder: format strings are addressed as 0x400f00 +
offset, and many routines are static (named after the symbol before
them). The tools are in `lab/platforms/marvel/m5/tools/` (`m5_run.sh`,
`m5_drive.sh` + `drive.py` for an OS session, `dis_dump.py` for code in a
memory dump).

**Checks** (no Tsunami or Titan behaviour change: the IO7 exists only on a
board that attaches one; the shared change is `CSystem::device_at`, a const
lookup): `srm_run.sh` diff clean on the JIT and interpreter (headless)
lanes and on the JIT_VERIFY lane with 0 mismatches in 110 million
compiled-block executions; the es47 row with the devices to `P00>>>`,
`show config` and `show device` on the JIT_VERIFY lane with 0 mismatches in
265 million; the es47 row without devices to `P00>>>` on the interpreter;
a two-processor DS20E probe at `P00>>>` (`show cpu`: 00 01); all three
lanes build.

**Open:**

- the IO7's MSIs, the data mover, error reporting (the registers read
  clean and nothing sets them), INT_PND/INT_CLR/MISC_PND;
- `get_pbm_configuration` (0x0322) is never asked on the embedded I/O;
  "Backplane rev" is IO_SYS_REV<3:0> (M7a);
- SMLAN 0x0b05 (`get_cdl_error`) at `boot` is answered with status 1;
- OpenVMS printed "mvcpu_get_numa_distances: bad route IPR for self" per
  CPU: fixed by M6a (the route table in the CMM);
- DKA400 shows 4 errors in OpenVMS's `SHOW DEVICE` **[not investigated]**;
- the AIC-7892, CMD 649 and USB of the real embedded I/O (the CMD 649 and
  the USB since M7a), network boot through the NIC (`net_peer.py`), an
  installation to disk, Linux.

### M4: the CMM, the console's management side (2026-10-02)

**Result: L2.** `platform = "es47";`, two `ev7` processors, a `serial0`
in the system block: the console prints its power-up log on the telnet
port, starts PID 1, builds its GCT and reaches `P00>>>`, within about a
minute on either lane (an estimate from the run times, not a measurement). `show config`,
`show memory`, `show cpu`, `show fru`, `show version`, `show pal` answer,
`set`/`show` of environment variables work and the variables survive a
restart (`rom.nvram`). The transcripts are in `lab/platforms/marvel/m4/`
(`es47-console-*.log`, `es47-cmds-*.log`, the SMLAN log
`es47-gio-int.log`). The model is `src/platforms/es47/Cmm.{hpp,cpp}`, a
`GioManagement` that `es47_board_devices` installs in place of M2's
recorder (`CMarvel::set_management`).

```
starting console on CPU 0
...
CPU 0 speed is 1000 MHz
...
entering idle loop
access NVRAM
Get Partition DB
hpcount = 1, spcount = 1, ev7_count = 2, io7_count = 1
hard_partition = 0
0 sub-partition 0:   start:00000000 00000000   size:00000000 40000000
PID 0 console memory base: 0, 1 GB
1 sub-partition 0:   start:00000004 00000000   size:00000000 40000000
PID 1 memory: 400000000, 1 GB
total memory, 2 GB
probe I/O subsystem
starting drivers
Starting secondary CPU 1 at address 400030000
initializing GCT/FRU*** system serial number not set. use set sys_serial_num command.
. at 556000
Initializing
AlphaServer Console V7.3-1, built on Feb 27 2007 at 12:48:34
P00>>>show cpu
CPU 0   CurOwner 0  Owner 0   Type Major 15, Minor  3
CPU 1   CurOwner 0  Owner 0   Type Major 15, Minor  3
```

**Against the real ES47's `show config`** (`test/platforms/es47/show-config.txt`,
a 7/1300 on V7.3-11), structurally:

| | real | emulated | why |
| --- | --- | --- | --- |
| banner | hp AlphaServer ES47 7/1300 | hp AlphaServer ES47 7/1000 | the clock is the configuration's (`speed`) |
| PID 0 / PID 1 lines | CPU 0 / CPU 1, NS,EW (0,0) / (1,0), Hard ID 0 / 1 | the same | partition database coordinates |
| cache, revision | 1.75 MB, EV7 rev 3.0 | 1.50 MB, EV7 rev 2.1 | the `ev7` row is a revision-2 part (the console derives the revision from the chip ID and CSR 0x28020); `ev7z` at 1300 MHz prints what the real 7/1300 prints (M6b) |
| memory | 4 GB each | 1 GB each (memory.bits 30) | configuration |
| RIMMs | PPPPP..... | PPPPP..... | MBM configuration |
| IO7 0 ... PCI Bus 0-3 | present | present since M5 (see the M5 section) | at M4 the console said "No Local I/O": RBOX_IO_CFG read 0 |
| device table, slots | DEGXA, AIC-7892, CMD 649, USB, Radeon | empty | M5 |

**The CMM firmware.** `CMM3_V2_7_5.BIN` is x86 code for an **Intel 386EX**:
a 0x40-byte APU header ("V2.7-5", "CPQ CMM3", "X86", "CMMFW"), then
32-bit protected-mode code whose first instructions program the 386EX's
chip-select unit (I/O ports 0xF400-0xF43E) and its 8259A (0xF020/0xF021);
the strings name "386EX Emulator (12.5Mhz)", "CMM Hardware (25Mhz)" and,
from the same code base, "AM186ES Eval Hardware (40Mhz)". The image is
linked at linear 0x03FA0000 (file offset = linear - 0x03FA0000: the data
initialisers hold pointers in that range, and the start-up far jump goes to
0x0100:0x03FC19DD). It runs a small multitasking executive ("Creating
process %s": env_poll, ppp_proc, smlan_rx/tx, smlan_ev7, srom_poll,
eu_uart, cmm_cli, sm_link), speaks PPP/IP/UDP/TFTP to the MBM over the
system management LAN ("SMLAN"), loads the SROM, XSROM and console images
into the EV7s (srom.c, "SROM Communication Area at %Fp"), and passes SMLAN
commands between the EV7s and the MBM (smlan_ev7_dispatch). Its terminal
side, eu_uart, is a pair of **virtual UARTs per processor** (COM1, COM2)
in an FPGA, with a status register (COM1/COM2 TX and RX full at
0x800-0x4000, an interrupt-pending bit), a mask register (RXF/TXE enables
0x1/0x2/0x40/0x80) and a reason register; "Console session is connected
via the CMM". Its dump routine (dumppkt.c) names the SMLAN header fields
and the command groups (DISCOVERY, PARTITION, ENVIRONMENTAL, ERRLOG,
DATETIME, WATCHDOG, VIRT_CNSL, EV7_SETUP, FW_UPGRADE, ...) and commands
("Get MBM Config", "Get PBM Config", "Get DB", "GetEnvVar"/"StoreEnvVar",
"GetBaseTime"/"SetBaseTime", "PutChar", "GetTemp", "GetFanRpm",
"GetEEROM", ...). The code-to-name table is built at run time and was not
extracted **[open]**, nor were the FPGA's I/O addresses on the CMM side:
the register map below is the EV7's side, read off the console.

**The XSROM** (`MVXSROM_V1_0_31.BIN`) is the third witness: it reads the
same status register through GIO and picks the processor block 0x40008 or
0x46008 by its bit 6 (0x498-0x4d8), as the console's PALcode does.

**How the console was read.** The console carries its own symbol table at
0x3bf728: pairs {procedure descriptor, name}, the entry point at
descriptor + 8. `lab/platforms/marvel/m4/tools/mksyms.py` turns it into
`syms.txt` (2826 routines: `cmm_*`, `smlan_*`, `read_gport_csr`,
`get_mbm_configuration`, `memconfig`, ...), and `cdis.py` disassembles a
routine with its calls, linkage and strings resolved (the routines address
their constants off the procedure value, VMS calling standard). The best
single source was the console's **simulator mode**: `platform()` (0x2df8a0)
is true when the longword at 0xfc holds 0xcafebeef, and then each SMLAN
request routine builds the answer itself instead of asking -- a reference
layout for every reply.

**GIO, the far side's registers** (`read_gport_csr`/`write_gport_csr`,
0x2e3c50/0x2e3cb0, on GIO_CTL/GIO_DAT as M2 found):

| Reg | Access | Meaning | Where |
| --- | --- | --- | --- |
| 0 | read | status: <0> window access busy, <3> attention pending, <5> the console's flag as written, <6> this processor is the module's second (block 0x46008), <7> terminal TX full, <8> terminal RX ready | `con$putchar`, `con$getchar`, `cpu_id`, `dma_start_wait` |
| 0 | write | control: <0> go (start a window access), <1> store the low byte, <2> store the high byte, <3> attention (a mailbox changed), <5> kept | `read_dma`/`write_dma`, `smlan_write` |
| 1 | r/w | the window's 16-bit data; a byte at an odd address in <15:8> | `read_dma`, `write_dma` |
| 2 | write | the window's CMM address | idem |
| 4 | write | the console terminal: one character out | `con$putchar` (waits on status <7>) |
| 5 | read | the console terminal: one character in | `con$getchar`, `con$checkchar` (status <8>) |
| 8 | write | the PALcode's state word (PAL scratch + 0x1b8), sent whenever it changes; 4 when it enters the console | PALcode 0x3e259, cserve 0x45-0x51 |
| 9 | read | read and discarded by `platform_init2` before it sets RBOX_IMASK **[guess: an interrupt reason, read to clear]** | 0x2dc6f0 |
| 0xa | read | the CMM's options: <0> ring attention after starting SMLAN, <5> ring attention after each mailbox change | `smlan_init_comm`, `smlan_write`, `smlan_read` |
| 0xb | r/w | <0>: the console sets it to start the SMLAN link and waits for the CMM to clear it | `smlan_init_comm` |

The model answers every transaction at once (no busy), so GIO_DAT<63> is
set by the time the processor polls it; reads of 0xa return 0 (no
attention needed: the model acts on the mailbox write itself).

**The byte window.** A read: write reg 2 = address; read reg 0; write reg 0
= (status & 0x20) | 1; poll reg 0 <0> until clear (the console gives up
after 1 s: "CMM DMA Timeout offset=%x"); read reg 1. A write: reg 2 =
address, reg 1 = data (shifted to <15:8> for an odd address), reg 0 =
(status & 0x20) | lanes | 1, with lanes 2 (even byte), 4 (odd byte) or 6
(an aligned word). `read_cmm_mem`/`write_cmm_mem` (0x307fe0/0x307f20) go
through the "cmm" device, byte by byte or word by word.

**The CMM memory the processors see**, per processor n (n = status <6>,
the processor's place on the module), from area(n) = 0x40000 + n * 0x6000
(the console's own arithmetic; the PALcode's "block" is area + 8):

| Address | Size | Contents | Read by |
| --- | --- | --- | --- |
| 0x40004 | 4 | the system type: byte 0x11 = ES47/ES80, <19:16> 0 = ES47 (1 = GS1280; anything else the development system "TS212c") | PALcode, `smlan_init`, `build_dsrdb` |
| area + 0xe10, + 0xe12, + 0xe14 | 2, 2, 4n | the route table: two counts and the routes (M6a: the RBOX_ROUTE copy the XSROM leaves) | `start_secondaries`, PALcode |
| area + 0x12a0 | 1 | the start state: 0 for the partition's primary, which builds its PAL area and HWRPB pointers from scratch (0x3ea0c); non-zero for a secondary, whose PALcode keeps what the console copied into its PAL area | PALcode 0x3e9fc |
| area + 0x12a6 | 2 | the processor's clock in MHz, returned by cserve 0x4a (`get_cpu_speed`; 800 if 0) | PALcode 0x3ef98 |
| area + 0x1aac | 12 | the TOY: MC146818 registers 0-11 (time, A, B) | `rtc_read`/`rtc_write` |
| area + 0x1ac8 | 3 x 0x818 | request mailboxes | `smlan_write` |
| area + 0x3310 | 3 x 0x818 | response mailboxes | `smlan_read` |
| area + 0x4b98 + i | | TOY NVRAM bytes, index 12 and up | `rtc_read`/`rtc_write` |

**Mailboxes.** A request slot: <0> u8 state (3 free and done, 0 posted),
<2> u16 length, <4> the message. `smlan_write` (0x308af0) takes a slot
whose state has <0> set, writes the message, the length and an id, then
the state 0, rings attention if reg 0xa <5> asks for it, and waits up to
3 s for the state to read 3. A response slot: <0> state (<0> full, set by
the CMM; the console writes (state | 2) & ~1 once it has the message and
rings attention if <2> was set), <4> the message. `smlan_read` (0x308860)
polls the three response slots for up to 10 s for one that is full and
carries its request's id.

**An SMLAN message** (the console's builders; field names from the CMM's
dumppkt.c): <0> u32 originator (0 from the console), <4> u32 destination
-- the micro asked, an IP address (`pid2ip`: the MBM is 10.0.0.1, the
module's CMM 10.0.<module + 1>.0, stored least significant byte first) --,
<8> u32 id, <0xc> u16 command, <0xe> u16 status (0 = success), <0x10>
data. Status 3 from `get_eerom_data` means "not there" and is taken
quietly; any other non-zero status prints "*** CPU n: Server Management
Interface responded to command %04x with status %04x".

**The commands the console sends** (every one, found by scanning for the
`zapnot r, 0xfc, r; lda r, cmd(r)` that builds them):

| Cmd | Routine | Request data | Answer | Model |
| --- | --- | --- | --- | --- |
| 0x0333 | `get_own_partition_number` | -- | hard, soft partition (2 bytes) | 0, 0 |
| 0x041c | `fetch_sm_nvram` | hard, soft partition | the console's NVRAM image, 0x800 | from `rom.nvram` |
| 0x041b | `save_sm_nvram` | hard, soft partition, 0x800 image | -- | to `rom.nvram` |
| 0x0323 | `get_partition_database` | 1 | the partition database, 0x800 (below) | one hard and one sub partition, both EV7s, the IO7 on PID 0 |
| 0x0321 | `get_mbm_configuration` | -- (to the MBM) | MBM configuration, 0xd8 (below) | one module, two EV7s, five RIMMs each |
| 0x0418 | `get_hard_partition_mem_assignm` | hard partition | memory groups and chunks, 0x800 (below) | each EV7's memory one chunk |
| 0x0100 | `get_hard_partition_mem_assignm` (another path) | | | not seen |
| 0x0322 | `get_pbm_configuration` | | PCI backplane: 6 bytes of answer (0x30a5b0 reads 0x16 = header + 6) | not seen, even with the IO7 (M5): no routine calls it on the ES47's embedded I/O |
| 0x0330 | `get_system_topology` | | | not seen |
| 0x0901 | `get_voltage_readings` | -- (to a micro) | count, 0x1c-byte records | CMM 6, MBM 13 |
| 0x0902 | `get_temperature_readings` | | idem | CMM 2, MBM 4 |
| 0x0903, 0x0908, 0x090e, 0x090f | fans, power supplies, PS tray, VRM status | | | success, nothing **[stub]** |
| 0x0a01 | `get_eerom_data` | offset, length (to a micro) | u16 length, the FRU EEPROM bytes | status 3, not there **[stub]** |
| 0x0801 | `get_fw_rev` | | | not seen |
| 0x0407, 0x0409, 0x0415 | reset/power off partition, power on/off | | | not seen |
| 0x0b01, 0x0b06 | IPMI SEL log, CDL error clear | | | not seen |
| 0x0b05 | `get_cdl_error` (0x30c000), at `boot` | 8 bytes, to 10.253.0.1 [guess: the MBM] | a reply in parts | not modelled: status 1, and the console prints "Server Management Interface responded to command 0b05 with status 0001"; harmless **[open]** |

An unknown command is logged and answered with status 1.

**The partition database** (0x800; `memconfig` 0x2f0e30 walks it, the
simulator's version is at 0x3095e0): four lists, each a count byte in a
longword followed by its entries.

| List | Entry | Fields |
| --- | --- | --- |
| hard partitions | 0x1c | <0> number, <4> u32 (0xff in the simulator's), <8> name |
| sub partitions | 0x20 | <0> hard partition, <2> 4 **[unknown, copied]**, <6> name |
| processors | 12 | <0> 0x80 = the sub partition's primary, 0 = another member (others refused); <1> N/S, <2> E/W; <3> PID; <4> hard partition; <6> sub partition |
| I/O | 8 | <3> N/S, <4> E/W of the EV7 the IO7 hangs on (corrected by M6c), <5> present |

`memconfig` makes the last primary of the sub partition the GCT builder
(0x282948), and only the primary's `powerup` builds the GCT: with both
processors marked 0x80 the primary waited for PID 1 to build it, which it
never does ("waiting for GCT/FRU to be built...", no CPU nodes, empty
`show cpu`). `coord2id` turns the coordinates into the Hard ID and CPU
number `show config` prints.

**The MBM configuration** (0xd8; simulator version 0x30a210, consumers
`memconfig`, `mem_config_get_rimm_size`, `build_mem_ctrl_hw`): four CPU
modules 0x34 bytes apart; <4> u16 0 for a module that is there, 0xffff for
one that is not; its processors at <8> and <0x20>, 0x18 bytes each (the
second's last words overlap the next module's first longword, which
nothing reads): <0> u16 1 for a processor that is there, <4> ten u16 RIMM
words, Zbox 0's RIMMs 0-4 then Zbox 1's 5-9. `show memory` prints P for a
non-zero word. The model writes each RIMM's size in MB **[guess at the
unit]**.

**The memory assignment** (0x0418, 0x800; `build_memory_chunks`
0x2f1d20): <0> u64 **[guess: the partition's total]**, <8> u32 the
number of groups; a group: <0> sub partition, <1> chunk count, two bytes,
then chunks {u64 base, u64 size}. The console's memory sizes come from
here.

**Sensor readings** (0x0901/0x0902): <0> u32 count, records of 0x1c
bytes: <0> u16 index, <2> s16 reading (<15> set: none), <4> the subpacket
data the console copies into the GCT (0x18 bytes for a voltage, 0xc for a
temperature). `build_cmm_hw` wants 8 sensors from the CMM and
`build_mbm_hw` 17 from an ES47's MBM (5 on a GS1280, 24 on a type-0x15
system), voltages and temperatures together, and prints "Sensor subpacket
count error" otherwise. Which sensors and in what units is **[guess]**.

**The console terminal** is the configuration's first serial port: the
CMM model drives `CSerial` through its 16550 registers (THR, LSR, RBR), so
telnet, `raw_mode` and the rest work unchanged. A CSerial on a board
without the ALi no longer touches the ALi's PIC.

**The TOY** is the MC146818 image in the CMM memory: the model refreshes
it from the host clock (binary, 24-hour, register B 0x06) on every read
of a time byte, and when the console clears SET after writing it keeps
the difference as an offset. The console's own TOY test and OpenVMS's
cserve GET_TOY/PUT_TOY were not exercised **[open]**.

**Things M4 had to fix outside the CMM:**

- **The interval timer.** The console writes RBOX_IT = 7 and tells the
  operating system (HWRPB intr_freq, `get_iclk_freq` 0x2e2270) that the
  tick runs at cpu_hz / ((n + 1) * 2^17), a quarter of that on a
  revision-1.0 part. M2's tick was the machine's 1 Hz fallback, and the
  console printed "*** no timer interrupts on CPU 0 ***" and ran its
  sleeps on a software timer. The schedule now runs at PID 0's RBOX_IT
  period (`CChipset::interval_period_ns`, used only on a board without the
  ALi), and such a board looks for a newly programmed period within a
  millisecond instead of a second.
- **The second processor.** The CMM's start-state byte (area + 0x12a0)
  must be non-zero for PID 1: with 0 its PALcode rebuilt its PAL-area
  pointers, mapped the console's addresses onto its own (empty) memory and
  looped through HALT. With 1 it runs the console from PID 0's memory and
  reports "EV7 rev 2.1" like the primary.
- **The L2 size.** `get_bcache_size_pid` counts the enabled ways in
  BBOX_CTL<6:0>, 256 KB each; the register now holds the row's ways, and
  the `ev7` row (which the console names rev 2.1) has 1.5 MB, as real
  revision-2 listings print.
- **The call trace** (`ALPHABOX_TRACE_CALLS`) dedupes per processor, so a
  secondary running the primary's code shows its own path.

**Checks** (no Tsunami behaviour change): `srm_run.sh` diff clean with 0
mismatches on the JIT and the interpreter (headless) lanes; a two-processor
DS20E probe at `P00>>>` (`show cpu`: 00 01, `show memory`: 64 MB); both
lanes build. Evidence in `lab/platforms/marvel/m4/` (`srm-*.txt`,
`ds20e-probe.txt`). The changes on shared paths are a branch on `theAli`
where it was already tested, so the ALi boards take the same paths as
before.

**What remains** (for M5 and beyond):

- the IO7 (M5): with RBOX_IO_CFG reading 0 the console skips the IO7 the
  partition database lists (done with M5; `get_pbm_configuration`, 0x0322,
  was not asked after all);
- FRU EEPROM contents (0x0a01): `show fru` lists the FRUs with no part or
  serial numbers; `set sys_serial_num` is asked for at every start;
- fans, power supplies and VRMs (0x0903/0x0908/0x090e/0x090f) report
  nothing, so `show power` is empty;
- the TOY's interplay with the operating system (M7), the CMM-side
  register addresses, and the meaning of the unknown fields marked above.

### M1-M3: the EV7, its registers and the XSROM's handoff (2026-10-02)

**What runs.** `platform = "es47";` with two `ev7` processors (the
configuration class names the row; `ev7z` is the other) and no devices.
The loader decompresses `SRM_V7_3.EXE` as before, saves the 4.5 MB the
console occupies to the decompressed-image cache, leaves processor 0 as the
XSROM would leave it, and parks processor 1 on its RBOX_SCRATCH1. The
console's PALcode then runs its reset path on PID 0, the console proper
starts at 0x2b65f0, and it reaches its first GIO receive in C code
(0x2e3a9c), where it waits for the CMM. With
`ALPHABOX_TRACE_UNKNOWN=1 ALPHABOX_TRACE_CSR=1` (run `lab/runs/ev7m-j1`,
JIT lane, about 3 minutes; the same on the interpreter, `ev7m-h2`, 4
minutes, and on the JIT_VERIFY lane, `ev7m-v2`) the unknown-access trace has **no line at all**:
every register the console touched on the way was a modelled one. They are,
in order:

| Register | Access | Where | What it is |
| --- | --- | --- | --- |
| GIO lock (CSR + 0x80000) | write 0, then read-to-take / write-0-to-free around every transaction | 0x3e829, then every GIO routine | the port's semaphore |
| GIO_CTL, GIO_DAT | 25 transactions (below) | PALcode 0x3e1f4-0x3f649, C 0x2e3a50 | the GIO transport |
| CSR + 0x28040, + 0x28020 | read, write all ones, read, write back | 0x3ead1-0x3eb05 | a revision probe on EV7 parts (bit 19 of 0x28020); not in the console's table |
| RBOX_IT | write 0 | 0x3f0a1 | interval timer off |
| RBOX_NSVC | read, write 0, read | 0x3e1ad-0x3e1c9 | router virtual-channel configuration |
| RBOX_IMASK | read, write 0, read | 0x3e1dd-0x3e1f1 | all interrupts masked |

Nothing else: no Cbox, Zbox, router port, IO7 or PCI access happens before
the console needs its CMM. With `ALPHABOX_EV7_START_SECONDARIES=1` (a test
hook doing what the console does once it can: copy its PALcode to PID 1's
memory and start PID 1 through its RBOX_SCRATCH1), PID 1 echoes `f2000400`,
starts at 0x4_0003_0000, runs the same reset path from its own memory with
its own register block and its own GIO port (`ev7m-j2-smp`), and the trace
again has no unknown access.

**M1, the processor** (`cpu/CpuModel.hpp`, `cpu/CpuModels.cpp`,
`cpu/ev7/`):

- `cpu_model` has a `family` (`CPU_FAMILY_EV6`, `CPU_FAMILY_EV7`) and the
  L1/L2 sizes. Rows `ev7` (21364: chip ID 2, HWRPB type 15) and `ev7z`
  (chip ID 4 since M6b), AMASK and IMPLVER as the EV68 **[guess]**,
  64 KB L1s, 1.75 MB L2. A board's row names its processor and a processor
  of the other family is refused.
- **The chip ID**: the PALcode takes I_CTL<29:24> (`srl 24; and 63`) and
  only enters its EV7 code when it is 2 (0x3eaa0). There it adds bit 19 of
  CSR 0x28020 (after writing all ones to it) and indexes a table of
  `(n, 15)` quadwords at 0x3eb08 with the sum: the console derives the
  processor type it reports, 15 throughout, itself. Bit 19 tells rev 2.2
  from 2.1 among chip-ID-2 parts; it reads 0 (M6b).
- **Addresses** (`cpu/ev7/Ev7.hpp`): 44 bits (`CChipset::phys_mask`), PA<43:35>
  the inverted PE, memory per PID at `memory_base(pid)`, the CSR block at
  `csr_base(pid)`, IO7 ports at `io7_base(pid, port)`. Nothing in the core
  changed: the CPU passes the raw address and the system masks it.
- **The PID** is per processor (`CAlphaCPU::get_pid()`, its number on the
  ES47) and decides which register block and which memory are its own.
- **vmspal and the JIT**: the `es47` row has `vmspal_pal_base` 0, so the
  native PALcode routines are off; the JIT has no PALcode shortcuts beyond
  them. The JIT runs the EV7 rows; the JIT_VERIFY lane's count is in
  "Checks" below.

**M2, the register block** (`chipsets/marvel/`):

- `CMarvel` (the `CChipset`) decodes each present processor's 4 MB window
  into a `CEv7Csr`; everything else -- IO7 space, absent PIDs, memory
  nobody owns -- is traced as unknown.
- `CEv7Csr` holds every register of the console's 141-entry table (plus
  RBOX_SCRATCH2 from Linux and the 0x28020/0x28040 pair) with what was
  written; WHOAMI reads the PID **[guess at the field]**; errors read clean.
  Modelled behaviour:
  - **interrupts**, from the PALcode's own decode (its interrupt entry,
    0x38ec0): the Rbox drives the core's EI<5:0> lines as a Tsunami does;
    RBOX_INT & RBOX_IMASK bits 0-10 and 24-63 are EI0, 12 and 14 EI1 (the
    IO7 queue in RBOX_INTQ), 15-17 EI2 (15 is the interval timer), 18-19
    EI3, 21-23 EI4 (interprocessor). A bit is cleared by writing it to
    RBOX_INT; writing bits to a processor's RBOX_IREQ sets them in its
    RBOX_INT (the PALcode sends 1 << 23 and 1 << 22 to other PIDs that way,
    0x396b8, 0x3f3f4). Bits 11, 13 and 20 drive nothing **[guess]**.
  - **the interval timer**: each tick of the machine's schedule sets
    RBOX_INT<15> on every processor whose RBOX_IT is not zero. How RBOX_IT
    sets the rate is **not known**; its <31:22> is read by the PALcode as a
    count of missed ticks (0x39344-0x393cc). Without the ES40's ALi there is
    no programmed period, so the schedule is CPU 0's default of one tick a
    second. Fine for the console's first steps; M4/M5 have to settle it.
  - **RBOX_SCRATCH1 start protocol** for a parked processor (below, M3).
  - **GIO** (`Gio.hpp`), from the console's code:
    - GIO_CTL<n:1> names a register on the CMM side, <0> is go;
    - a **write** is GIO_CTL = n << 1, then GIO_DAT = value;
    - a **read** is GIO_CTL = n << 1, then GIO_CTL = n << 1 | 1;
    - both poll GIO_DAT<63> for done; a read's answer is in GIO_DAT;
    - the PALcode polls 2^28 times and gives up (reading 0), the C code
      polls for ever (0x2e3aac);
    - the lock at + 0x80000 reads 0 when free and is then taken; writing 0
      frees it.
  - The far side is a `GioManagement`. The one installed, `GioRecorder`,
    takes every write, answers no read and logs each transaction
    (`%MVL-I-GIO`, and the file `ALPHABOX_GIO_LOG` names).

**The GIO conversation**, PID 0, in order (`lab/runs/ev7m-j1/gio.log`). What
the PALcode does with it is read off its code; the meaning of the CMM's
registers beyond that is M4's to find.

| # | Op | Reg | Data | PC | Part of |
| --- | --- | --- | --- | --- | --- |
| 0 | write | 2 | 0x40004 | 0x3f491 | read the CMM byte at 0x40004 (routine 0x3f42c) |
| 1 | read | 0 | -- | 0x3f4ed | status |
| 2 | write | 0 | 0x1 | 0x3f55d | request: (status & 0x20) \| 1 |
| 3 | read | 0 | -- | 0x3f5b9 | poll until bit 0 clears |
| 4 | read | 1 | -- | 0x3f621 | the 16-bit word; the byte is <7:0>, or <15:8> for an odd address |
| 5 | read | 0 | -- | 0x3e9b1 | status: bit 6 selects the processor's block, 0x40008 or 0x46008 |
| 6-10 | as 0-4 | | 0x412a0 | | CMM byte at block + 0x1298 (zero: cold start) |
| 11 | read | 0 | -- | 0x3ef51 | status |
| 12-16 | as 0-4 | | 0x412a6 | | CMM byte at block + 0x129e |
| 17 | read | 0 | -- | 0x3eff5 | status |
| 18-22 | as 0-4 | | 0x412a7 | | CMM byte at block + 0x129f |
| 23 | write | 8 | 0x4 | 0x3e259 | the PALcode's last word before it enters the console (its state flags at scratch + 0x1b8, bit 4 cleared) |
| 24 | read | 0 | -- | 0x2e3a9c | the console's own GIO routine (0x2e3a50): no timeout |

Every read went unanswered, so every PALcode poll timed out (2^28 reads,
about 6 s each on the JIT lane) and the bytes it read were 0. The CMM's
memory map behind registers 0-2 -- a byte-addressed window, 16-bit data,
processor blocks 0x6000 apart -- is the first thing M4 has to fill.

**M3, the XSROM's handoff** (`cpu/ev7/Ev7Reset.cpp`, from
`MVXSROM_V1_0_31.BIN` disassembled less its 0x40-byte header):

- the XSROM is a command loop driven by the CMM (dispatcher at 0xfa30-0xfc58,
  commands 0x10-0xdd); command **0x50** (0x6650) makes it wait on
  RBOX_SCRATCH1: `0xf1` in <31:24> with the address's upper 24 bits, which
  it echoes as `0xf2`, then `0xf3` with the lower 24; it cleans up its Cbox
  counters and OCLA (0x21e0) and enters the address in PALmode (`hw_ret`
  with bit 0). The console's C code starts secondaries with exactly this
  (0x2dd600: write f1, poll for f2 up to 50 times, write f3), and its
  PALcode parks a processor the same way (0x390d4);
- **r28 = the PID**: the XSROM never writes it, it only reads it
  (`sll at, 35, t8` is its CSR mask), so the SROM set it;
- **r19** = this processor's block in the CMM's memory, 0x40008 or 0x46008
  by bit 6 of a GIO status word (0x498-0x4d8), the same two the console's
  PALcode uses;
- **r1, r2** = whatever the XSROM's last CSR access left there;
- **r18** = the XSROM's own (written at 0x5718, 0x8b38 and elsewhere).

The console's side: its decompressor saves r1, r2, r16-r21, sp and r28 and
restores them before entering the inflated console, except that **r19
comes back as the decompressor's own address + 0x10** (it stores t1 in r19's
slot, 0x9007c4/0x9007f8). The PALcode's reset entry then stores r28 as the
PID (0x3e804) and takes the cold path when **r19 != 0 or r18 < 0**
(0x3e790); with r19 = 0 and r18 >= 0 it takes a restart path that indexes a
table by r18 and dereferences r21, which is what zeroed registers -- the
probe's -- sent into. r1 and r2 are dead: overwritten at 0x3e540 and
0x3e6b0 before being read. So the loader sets, on the primary, r28 = PID,
r19 = 0x900010 (the decompressor at 0x900000), r18 = r1 = r2 = 0; on a
secondary started through RBOX_SCRATCH1, r28 = PID and r19 = 0x40008 or
0x46008 (PID<0> **[inference]**).

The rest of what M3 asked for:

- **Memory per PID**: `memory.bits` is each processor's memory on the
  ES47 (29-33: 512 MB to 8 GB per EV7, GS1280 Technical Summary); PID n's is
  at `memory_base(n)` (PID 1 at 0x4_0000_0000). `CChipset::memory_span_bits`
  sizes the one host array to span them all (35 bits for two processors),
  and the host backs only the pages touched (`calloc` of untouched memory
  costs nothing on macOS or Linux). **The memory path is unchanged**: the
  CPU's and `CSystem::ReadMem`'s test is still `a < dram_size`; the cost is
  that the holes between processors' memory read as zero instead of as
  nonexistent memory **[a known divergence; nothing has been seen to touch
  them]**.
- **Secondaries** wait parked (`SECONDARIES_BY_CONSOLE`) and are released
  by the RBOX_SCRATCH1 protocol above, served by the Rbox while the
  processor is parked (`CEv7Csr::scratch_written`).
- **The cache**: the board row's `console_bytes` (0x480000 for the ES47)
  sets what `decompressed.rom` holds; a cache saved for a board with another
  size is not used. A second boot from the cache reaches the same GIO
  conversation (`ev7m-cache`).
- **Retired**: the `marvel-probe` row.

**Checks** (no Tsunami behaviour change): srm_run diff clean on the
interpreter, JIT and JIT_VERIFY lanes with 0 mismatches; a two-processor
DS20E probe at `P00>>>` (`show cpu`, `show memory`); the ES40 OpenVMS 8.4
CD boot (`lab/vms84/repro.sh`): PASS (asks for the date after 52 s); every lane builds. JIT_VERIFY on the
`es47` row to the C code's GIO conversation: 0 mismatches in 21.5 million compiled-block executions (`ev7m-v2`).

**What M4 needs**:

- the CMM's side of registers 0, 1, 2 and 8: the byte window (address in
  register 2, request/acknowledge through register 0 with the 0x20 toggle
  and the busy bit 0, data in register 1), the status bits (6: which
  processor block), and the contents of the processor blocks at 0x40008 and
  0x46008 -- at least the bytes the PALcode reads (0x40004, just below the
  first block, and block + 0x1298, + 0x129e, + 0x129f) and whatever the C
  code asks next;
- what the C code's receive at 0x2e3a50 expects in register 0, which
  decides everything after it;
- the interval timer's rate in RBOX_IT, once the console programs it;
- disassembly of `CMM3_V2_7_5.BIN` / `MBM_V2_7_6.BIN` for the meaning,
  their architecture first.


### M0: the chipset split (2026-10-02)

`src/chipsets/Chipset.hpp` is the interface, `src/chipsets/tsunami/` the
Tsunami behind it, `src/platforms/<board>/` the boards' own parts; no
behaviour changed (the ES40 console-log check on every lane, the JIT
cross-check with 0 mismatches, the DS20E/DS10/DS20L and 4-CPU ES40 probes,
OpenVMS 8.4 and Windows 2000 boots, the VGA frame hashes, and `perf_ab`
against the previous main; the rows are in `lab/results/ledger.md`).

What the interface gives a Marvel: `phys_mask()` (44 bits, so PA<63:44> is
dropped before decode instead of the Tsunami's 0x807'ffff'ffff),
`read_io`/`write_io` for the EV7 CSR windows and the IO7 ports,
`interrupt()`/`interval_tick()`/`ack_*` for the Rbox, `pci_space_base()`
and `pci_phys()` per IO7 hose.

What it does not give yet, and M1/M5/M6 must add:

- **Memory is one contiguous array from 0** in `CSystem` (`a >>
  memory.bits` decides memory vs I/O). Marvel memory is per PID at
  non-contiguous bases; that needs a memory map in `CSystem`, not the
  chipset, to keep RAM off the virtual-call path.
- **ISA devices and a few others hard-code Tsunami hose-0 addresses**
  (`0x801fc000xxx` in Serial, Keyboard, DMA, the floppy, MPU401, the ALi
  bridge; the S3's legacy window; the vmspal's PIC reads). `CPCIDevice`
  asks the chipset (`pci_space_base`); those do not yet.
- **The LL/SC I/O bit** (`CPU_LOCK_IO_MASK`, PA<43>) is the Tsunami's and
  the EV7's alike; the line mask now follows `phys_mask()`.
- **`interval_tick()` is one call from CPU 0** for the whole machine; a
  per-CPU RBOX_IT can be driven from it but CPU 0's schedule stays the
  clock.
- The JIT's device-access statistics (`AlphaCPU_jit.cpp`, JIT_STATS only)
  classify Tsunami CSR addresses by constant.

### The L1 probe (2026-10-01)

**Setup.** The board row `marvel-probe` (since retired) was the ES40 with the Marvel console image: one EV68CB
and 256 MB. The commands were:

```
ALPHABOX_TRACE_UNKNOWN=1 ALPHABOX_TRACE_CALLS=1, build-headless,
srm_cfg.py --platform marvel-probe --rom SRM_V7_3.EXE --membits 28,
run directory lab/runs/ev7-p4 (900 s) and ev7-p5 (2400 s)
```

The output was capped and deduplicated.

**What happened, in order:**

1. The LFU loader decompressed the image, the console entered at 0x30001
   (PALmode, PAL_BASE 0x30000), and the image was saved to the 2 MB
   cache, which cannot hold the console (see above).
2. The PALcode reset (0x3e540...) ran on EV6 IPR numbers without an
   unimplemented-IPR warning:
   - DC_CTL = 0xc3;
   - PAL_BASE from its own address;
   - ITB/DTB invalidate;
   - I_CTL, IER, HW_INT_CLR, PCTR_CTL, CC_CTL, M_CTL;
   - a CPU multiply loop.
3. It took the PID from r28 (0 here), formed the CSR base 0xffff_ffff_ffc0_0000
   for PE 0 (PE 0's 0xFFF_FFC0_0000, sign-extended), and stored it at PAL
   scratch.
4. **First access nothing claims**: a write of 0 to CSR+0x80000 (the GIO
   lock), at pc 0x3e828.
5. GIO send of 0x40004: lock, GIO_CTL = 4, GIO_DAT = 0x40004, then a poll
   of GIO_DAT<63> that times out after 2^28 reads. Then receives (GIO_CTL
   0 then 1; GIO_DAT = 1; GIO_CTL 2 then 3), each timing out. Without the
   trace a timeout costs about 1G instructions; with it, about two
   minutes.
6. Then:
   - GIO send 0x412a0 and the receive sequence;
   - I_CTL<29:24> == 2 is tested at 0x3eaa4 (not equal on the EV68, so
     the Rbox access that follows it is skipped);
   - GIO send 0x412a6 and more receives;
   - **RBOX_IT = 0** (interval timer off);
   - RBOX_NSVC read, write 0, read; RBOX_IMASK read, write 0, read;
   - GIO_CTL = 0x10 and GIO_DAT = 4.
7. **The PALcode jumps to the console**: 0x70000, then 0x423620, then
   C code at 0x2b65f0... Its first unclaimed access is the GIO lock
   again, written as the 44-bit 0xFFF_FFC8_0000 (pc 0x2e3e34). The
   PALcode used the sign-extended 64-bit form for the same register.

**So the firmware wants, in order:**

| # | Register | Access | What it is |
| --- | --- | --- | --- |
| 1 | EV7 CSR + 0x80000 (PE 0: 0xFFF_FFC8_0000) | write 0, read until 0 | GIO lock **[inference]** |
| 2 | GIO_CTL (CSR + 0x120) | write 4 / 0,1 / 2,3 / 0x10 | GIO command and go |
| 3 | GIO_DAT (CSR + 0x110) | write data; poll for bit 63 | GIO data and done |
| 4 | I_CTL<29:24> | IPR read | chip ID, compared with 2 |
| 5 | RBOX_IT (CSR + 0xa0) | write 0 | interval timer |
| 6 | RBOX_NSVC (CSR + 0x10) | read, write 0, read | router virtual channel config |
| 7 | RBOX_IMASK (CSR + 0x60) | read, write 0, read | interrupt mask |
| 8 | (console C code) GIO lock at 0xFFF_FFC8_0000 | write | as 1 |

**Where it stops** (run ev7-p5, 40 minutes):

1. After the PALcode, the console's C code runs its early initialisation
   (calls from 0x2b65f0 through 0x2b6a14).
2. It reaches its own GIO routine at 0x2e3a50, which:
   - takes the lock, with a timeout of 2^31 tries;
   - writes GIO_CTL = 0, then 1 (a receive);
   - then **polls GIO_DAT<63> with no timeout at all** (0x2e3aac-0x2e3ab8).
3. The trace counted 1.7 x 10^10 reads there before the run was stopped.

This is the end of what the console can do without a GIO responder: L1
ends here, and the next access the firmware wants is an answer on GIO.
Nothing outside the EV7's own CSR window was touched. There was no IO7
access, no PCI configuration access, nothing on the ES40's devices, and
no character written to any UART.

### Other findings

- **The console's own register tables** are the best register source
  there is for the EV7's CSRs and the IO7's: they include registers Linux
  never names (GIO, Cbox, Zbox, pads, OCLA, IO7 hot plug). See
  `lab/docs-ev7/srm73-csr-tables.txt`.
- **Hose numbering is global**: PID*4 + port (hose 33 = PID 8 port 1).
  An IO7 found on PID 8 makes hoses 32-35.
- **Every processor runs the console from its own memory**: secondaries
  start at their memory base + 0x30000, so the image is copied per
  processor.
- **The console has an Acer M1543C driver**. Which Marvel I/O carries one
  is open; the ES47's embedded I/O shows a CMD 649 and USB instead.
- **Tracing**: a GIO poll repeats one read 2^28 times. The unknown-access
  trace now prints the first three repeats and then each power of two
  with its count, which made the probe practical (ten minutes per poll
  before). Accesses outside every Tsunami space are now traced, with the
  processor's own address rather than the ES40-masked one.

## Risks and unknowns

1. **The GIO/CMM/MBM protocol** was the critical path to a prompt, and
   M4 has it (see "M4"): reverse-engineered from the console, whose
   simulator mode builds every answer itself, with the CMM firmware (Intel
   386EX) and the XSROM as witnesses. What is left unknown is marked
   there; FRU EEPROM contents and the fan/power answers are stubs.
2. **The XSROM handoff state**: which registers and CSRs the console
   relies on being set. Disassembling the XSROM's final jump is
   tractable: it is 64 KB of Alpha code.
3. **Chipset separation (M0)** touches the ES40's hot paths (memory
   decode, interrupts). It must be done with no behaviour change and
   measured. It is shared with Titan, so do it once, for both.
4. **The EV7 interrupt path** inside the core (which ISUM bit, how
   RBOX_INT/INTQ are acknowledged) is not documented anywhere we have.
   The console's PALcode interrupt entry is the source.
5. **The EV7 chip ID and revision encoding** is not known from a
   document; the console's tables of "EV7 rev x.y" strings have to be
   matched to the code that indexes them.
6. **No ES47 reference for our exact configuration**: L3 is structural.
7. **Scale**: the ES80's eight processors and four IO7s and a sixteen-
   processor GS1280 run (M6c); 64 processors would need 64 host threads
   and is not attempted.

## Rules

As in [TEMPLATE.md](TEMPLATE.md). Also:

- The `marvel-probe` row is retired: the `es47` row runs the console on
  emulated EV7s. Nothing about Marvel goes into a Tsunami row.
- vmspal fast paths stay off for EV7 until compared against the EV7
  PALcode.
