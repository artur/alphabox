# Marvel (AlphaServer ES47 / ES80 / GS1280) work packet

The EV7 machines. This is phase 0, which covers research, the plan and the
first contact with the firmware. It is a separate project in the sense of
[platforms.md](../platforms.md): the processor is not an EV6-family part,
and there is no chipset in the Tsunami sense. Each EV7 carries its own
memory controllers, interrupt logic and a router, and reaches the I/O
through an IO7 bridge.

**Branch**: `platform/marvel` (own worktree) · **Config**:
`platform = "marvel-probe";` (experimental, see below) · **Status**: L1,
reached on an EV68 core with the ES40's devices. The console's PALcode runs
its reset path to completion, and the console proper starts executing C
code. It then waits, without a timeout, for an answer from the management
port (GIO). Nothing is printed: the console terminal is reached through a
management interface that does not exist yet.

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
| South bridge | the ES47's embedded I/O has a CMD 649 IDE controller, USB, an AIC-7892 SCSI controller and DEGXA gigabit Ethernet (real `show config`). The console also carries an "Acer Labs M1543C" driver (with IDE, PMU, USB), but which Marvel I/O drawer has one is **not established** |
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
  - **Open**: the chip-ID values per revision. **[guess]** They are small
    integers, not the EV68CB's 0x21.
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
MHz" with 1.50 MB. **[inference]** EV7z is a CPU row: a chip ID, a type
minor and a cache size. Whether the console calls a part "EV7 rev 3.0" or
"EV79" by chip ID is open.

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
    cache saves. **Consequence: a second boot from the cache would lose
    the console.** The cache needs to grow (or be keyed on the image)
    before Marvel uses it. This is not fixed here; the probe always runs
    in a fresh directory.
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
  framing above the GIO register level is **not known**: it is the main
  reverse-engineering task of this project.

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
  processors, not Alpha code.
- What the firmware requires that the emulator does not have yet:
  - a 44-bit EV7 physical address decode;
  - the EV7 CSR block;
  - a GIO/CMM endpoint;
  - IO7s;
  - a decompressed-image cache larger than 2 MB.

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
| L1 | the console image loads, its PALcode runs the reset path, the console proper starts | **reached on the probe row** (EV68 core, no Marvel hardware) |
| L2 | `P00>>>` on the telnet port, through the emulated CMM's GIO terminal | needs packets M1-M4 |
| L3 | `show config` matches the real ES47 listing in structure: "PID 0 ... EV7 rev x, NNN MHz", "Memory 0 ...", "IO7 0 ...", "PCI Bus 0 Hose 0 ...", PID 1 "No Local I/O", the RIMM table; `show cpu` "Type Major 15"; `show mem` with PID 1 at 400000000 | Real listings exist only for other configurations and console versions (V7.3-11 against our V7.3-1), so L3 is a structural match. Device lines depend on which I/O we emulate behind the IO7 |
| L4 | `test` and console network boot through a NIC on an IO7 hose; disk boot of a CD | the console's interrupt numbers in INTERRUPT_LINE are the answer key for the LSI wiring |
| L5 | OpenVMS 8.4 boots from `lab/ALPHA084.ISO` | |
| L6 | the ES40 console-log check is clean, the JIT cross-check is 0 | after every packet that touches shared code (M0, M1) |

## Plan

The staged implementation, in packets. Sizes are rough estimates of new or
moved lines of code, **[guess]**.

| # | Packet | Contents | Size | Level | Status |
| --- | --- | --- | --- | --- | --- |
| P | L1 probe | experimental `marvel-probe` board row (EV68, ES40 devices); the trace reports unclaimed accesses with the full address and folds repeated accesses | <50 | L1 | **done** |
| M0 | **Separate the chipset from CSystem** | Today `CSystem` *is* the Tsunami: its `ReadMem`/`WriteMem` mask to 0x807_ffff_ffff and decode Cchip/Dchip/Pchip/TIG, `interrupt()` is the Cchip's DIR/DIM, and the PCI hoses are the Pchips. Split a `Chipset` interface out (non-memory decode, interrupt delivery, timer, PCI hose access, DMA translation), keep Tsunami as the first implementation with no behaviour change, and let the board row pick it. Titan (ES45) needs the same split, so it is shared work with the Tsunami-family effort | 1.5-3 k moved, ~500 new | L6 | open, **the dependency for everything below** |
| M1 | **EV7 CPU model** | `cpu_model` grows a family field (EV6 / EV7). An EV7 row: chip ID, type 15, minor per revision, EV7z row (type 16 if the console agrees). Physical addresses masked to 44 bits. Per-CPU PID (WHAMI). Interrupt delivery from the Rbox instead of the EIR pins. vmspal fast paths and JIT PAL shortcuts off for EV7 until verified. Kept apart from the EV6 code wherever the PALcode interface differs | 500-1000 | L1 | open |
| M2 | **EV7 on-chip CSR block** | per EV7 a device answering its 4 MB CSR window: Rbox (WHOAMI, INT/IMASK/IREQ/INTQ/INTA, the IT interval timer, SCRATCH, routing CFG reading as configured), Cbox, Zbox (memory configuration consistent with the board row, errors clean), GIO registers and the 0x80000 lock | 1-2 k | L1+ | open |
| M3 | **SROM/XSROM replacement** | a loader that leaves each EV7 as the XSROM leaves it: registers, CSR state, memory per PID, secondaries waiting on RBOX_SCRATCH for a jump address, and the console placed by the existing LFU loader (plus the >2 MB cache) | 300-600 | L1+ | open; needs the XSROM handoff disassembled |
| M4 | **CMM/MBM replacement (GIO protocol)** | the console's GIO protocol, reverse-engineered from its `cmm_*`/`giott`/`get_mbm_configuration` code and the CMM/MBM firmware as references; a terminal on telnet; the configuration, partition database, FRU, TOY and NVRAM answers | 1.5-3 k, **most uncertain** | L2 | open |
| M5 | **IO7 module** | port 7 CSRs, four ports (3 PCI/PCI-X + AGP), config/mem/IO windows, SG DMA (reusing the Pchip window logic once M0 has separated it), LSI/MSI control routing IIDs to an EV7's Rbox, the error registers clean; existing PCI devices on IO7 hoses | 2-3 k | L3-L4 | open |
| M6 | **Board rows** | `es47` (2 EV7, 1 IO7), then `es80` (up to 8, router mesh) and `gs1280` (up to 64, multiple IO7s, partitions); the PID-to-memory placement, the hose numbering, which I/O sits behind the ES47's embedded IO7 | 200-400 each | L3 | open |
| M7 | **Guests** | OpenVMS 8.4 from the ISO: GCT, HWRPB checks, interrupts end to end, TOY through cserve | ? | L5 | open |

The order is M0, then M1+M2+M3 together (the console reaches the GIO
conversation with real answers to its CSR reads), then M4 (the prompt),
M5 (devices) and M6/M7. M4 can start as research in parallel at any time:
it is disassembly of the console and of the CMM firmware, no emulator
code.

## Findings

### The L1 probe (2026-10-01)

**Setup.** The board row `marvel-probe` (`src/platforms/Platforms.cpp`,
marked experimental) is the ES40 with the Marvel console image: one EV68CB
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

1. **The GIO/CMM/MBM protocol** is the critical path to a prompt. No
   public documentation is known. It has to be reverse-engineered from
   the console (`cmm_*`, `giott`, `wait_for_gio`) and, for the meaning of
   the answers, from the CMM and MBM firmware images, which run on other
   processors with an unknown architecture **[guess: the MBM is x86 or
   PowerPC, which matters for disassembly]**. If the console insists on
   data we cannot reconstruct (partition database, FRU checksums), L2
   stalls.
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
7. **Scale**: the GS1280's 64 processors and multiple IO7s are not a
   goal. The ES47 with 2 CPUs is. The design should not preclude more,
   but nothing is built for them before the ES47 runs.

## Rules

As in [TEMPLATE.md](TEMPLATE.md). Also:

- The `marvel-probe` row is experimental and says so. It is not the ES47
  board row, and nothing about Marvel may be added to it except what the
  probe needs.
- vmspal fast paths stay off for EV7 until compared against the EV7
  PALcode.
