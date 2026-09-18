/* Alphabox Alpha Emulator
 * Copyright (C) 2020 Tomáš Glozar
 * Copyright (C) 2026 Artur Goulão
 * Website: https://github.com/lenticularis39/axpbox
 *          https://github.com/artur/alphabox
 *
 * Forked from: ES40 emulator
 * Copyright (C) 2007-2008 by the ES40 Emulator Project
 * Copyright (C) 2007 by Camiel Vanderhoeven
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301,
 * USA.
 *
 * Although this is not required, the author would appreciate being notified of,
 * and receiving any modifications you may make to the source code that might
 * serve the general public.
 */

/**
 * \file
 * Symbios 53C8xx family: The register file: BAR access and the registers with
 * side effects.
 **/
#include "Disk.hpp"
#include "SCSIBus.hpp"
#include "StdAfx.hpp"
#include "Sym53C8xx.hpp"
#include "System.hpp"

#include "Sym53C8xxRegs.hpp"

/**
 * Write data to one of the PCI BAR (relocatable) address ranges.
 *
 * The function number is the channel: each of them decodes a register file
 * and a SCRIPTS RAM of its own.
 **/
void CSym53C8xx::WriteMem_Bar(int func, int bar, u32 address, int dsize,
                              u32 data) {
  if (func < m_chip.channels)
    channels[func]->bar_write(bar, address, dsize, data);
}

/**
 * Read data from one of the PCI BAR (relocatable) address ranges.
 **/
u32 CSym53C8xx::ReadMem_Bar(int func, int bar, u32 address, int dsize) {
  if (func >= m_chip.channels)
    return 0;
  return channels[func]->bar_read(bar, address, dsize);
}

/**
 * Write to this channel's registers or SCRIPTS RAM.
 **/
void CSym53C8xx::CChannel::bar_write(int bar, u32 address, int dsize,
                                     u32 data) {
  void *p;

  // One PCI transaction = one critical section: a 16/32-bit access must not
  // interleave with a SCRIPTS step between its byte lanes (e.g. DSP must not
  // start SCRIPTS before all four bytes are committed). This also covers the
  // PCI config-space reflection, which calls in with the full width.
  std::lock_guard<std::recursive_mutex> lock(myRegLock);

  switch (bar) {
  case 0:
  case 1:
    address &= m_chip.reg_bytes - 1;
    switch (dsize) {
    case 8:
#if defined(DEBUG_SYM_REGS)
      printf("SYM: Write to register %02x: %02x.   \n", address, data);
#endif
      if (address >= R_SCRATCHB) {
        state.regs.reg8[address] = (u8)data;
        break;
      }

      switch (address) {

        // SIMPLE CASES: JUST WRITE
      case R_SXFER:        // 05
      case R_SFBR:         // 08
      case R_SOCL:         // 09
      case R_DSA:          // 10
      case R_DSA + 1:      // 11
      case R_DSA + 2:      // 12
      case R_DSA + 3:      // 13
      case R_CTEST0:       // 18
      case R_TEMP:         // 1C
      case R_TEMP + 1:     // 1D
      case R_TEMP + 2:     // 1E
      case R_TEMP + 3:     // 1F
      case R_DSP:          // 2C
      case R_DSP + 1:      // 2D
      case R_DSP + 2:      // 2E
      case R_DSPS:         // 30
      case R_DSPS + 1:     // 31
      case R_DSPS + 2:     // 32
      case R_DSPS + 3:     // 33
      case R_SCRATCHA:     // 34
      case R_SCRATCHA + 1: // 35
      case R_SCRATCHA + 2: // 36
      case R_SCRATCHA + 3: // 37
      case R_DMODE:        // 38
      case R_SBR:          // 3A     // 810
      case R_SLPAR:        // 44
      case R_SWIDE:        // 45
      case R_GPCNTL:       // 47
      case R_STIME0:       // 48
      case R_RESPID:       // 4A
      case R_STEST0:       // 4C
      case R_SODL:         // 54
        state.regs.reg8[address] = (u8)data;
        break;

      case R_SCNTL0: // 00
        // side effects: start arbitration bit
        write_b_scntl0((u8)data);
        break;

      case R_SCNTL1: // 01
        // side effects: start immediate arbitration bit
        write_b_scntl1((u8)data);
        break;

      case R_SCNTL2: // 02
        // WSS/WSR (wide parts) are cleared by writing 1
        WRMW1C_R8(SCNTL2, (u8)data);
        break;

      case R_SCNTL3: // 03
        // side effects: clearing EWS
        write_b_scntl3((u8)data);
        break;

      case R_SCID: // 04
        WRM_R8(SCID, (u8)data);
        break;

      case R_SDID: // 06
        WRM_R8(SDID, (u8)data);
        break;

      case R_GPREG: // 07
        WRM_R8(GPREG, (u8)data);
        break;

      case R_ISTAT: // 14
        write_b_istat((u8)data);
        break;

      case R_ISTAT1: // 15
        // The 896 generation added a second interrupt-status register and
        // two mailboxes here; on the older parts nothing decodes at 15-17,
        // and the writes NT and the Linux generic driver make are ignored.
        if (m_chip.reg_bytes > 128) {
          WRM_R8(ISTAT1, (u8)data);
          eval_interrupts(); // SIRQD gates the interrupt pin
        }
        break;

      case R_MBOX0:  // 16
      case R_MBOX1:  // 17
      case R_CCNTL0: // 56
      case R_CCNTL1: // 57
        if (m_chip.reg_bytes > 128)
          state.regs.reg8[address] = (u8)data;
        break;

      case R_CTEST3: // 1B
        write_b_ctest3((u8)data);
        break;

      case R_CTEST4: // 21
        write_b_ctest4((u8)data);
        break;

      case R_CTEST5: // 22
        write_b_ctest5((u8)data);
        break;

      case R_DSP + 3: // 2F
        state.regs.reg8[address] = (u8)data;
        post_dsp_write();
        break;

      case R_DIEN: // 39
        WRM_R8(DIEN, (u8)data);
        eval_interrupts();
        break;

      case R_DCNTL: // 3B
        write_b_dcntl((u8)data);
        break;

      case R_SIEN0: // 40
        R8(SIEN0) = (u8)data;
        eval_interrupts();
        break;

      case R_SIEN1: // 41
        WRM_R8(SIEN1, (u8)data);
        eval_interrupts();
        break;

      case R_MACNTL: // 46     // 810
        // Read-only on the 896, where it is the chip-type register.
        if (m_chip.reg_bytes == 128)
          WRM_R8(MACNTL, (u8)data);
        break;

      case R_STIME1: // 49
        WRM_R8(STIME1, (u8)data);
        state.gen_timer = (R8(STIME1) & R_STIME1_GEN) * 30;
        break;

      case R_STEST1: // 4D
        WRM_R8(STEST1, (u8)data);
        break;

      case R_STEST2: // 4E
        write_b_stest2((u8)data);
        break;

      case R_STEST3: // 4F
        write_b_stest3((u8)data);
        break;

      case R_CTEST2: // 1A
        // Read only, except SRTCH on the parts with SCRIPTS RAM.
        if (m_chip.ram_bytes)
          SB_R8(CTEST2, SRTCH, (data & R_CTEST2_SRTCH) != 0);
        break;

      case R_RESPID + 1: // 4B
        // RESPID1: reselection IDs 8-15 on wide parts
        if (m_chip.id_mask == 0x0f)
          state.regs.reg8[address] = (u8)data;
        break;

      case R_DSTAT:  // 0C
      case R_SSTAT0: // 0D
      case R_SSTAT1: // 0E
      case R_SSTAT2: // 0F
      case R_SIST0:  // 42
      case R_SIST1:  // 43
        // printf("SYM: Write to read-only register at %02x. FreeBSD driver
        // cache test.\n", address);
        break;

      default:
        printf("SYM: Write to unknown register at %02x with %08x.\n", address,
               data);
      }

      break;

    case 16:
      bar_write(1, address + 0, 8, (data >> 0) & 0xff);
      bar_write(1, address + 1, 8, (data >> 8) & 0xff);
      break;

    case 32:
      bar_write(1, address + 0, 8, (data >> 0) & 0xff);
      bar_write(1, address + 1, 8, (data >> 8) & 0xff);
      bar_write(1, address + 2, 8, (data >> 16) & 0xff);
      bar_write(1, address + 3, 8, (data >> 24) & 0xff);
      break;
    }
    break;

  case 2:
    // SCRIPTS RAM; accesses past its end are not decoded.
    if (address + dsize / 8 > m_chip.ram_bytes)
      break;
    p = (u8 *)state.ram + address;
    switch (dsize) {
    case 8:
      *((u8 *)p) = (u8)data;
      break;
    case 16:
      *((u16 *)p) = (u16)data;
      break;
    case 32:
      *((u32 *)p) = (u32)data;
      break;
    }
    break;
  }
}

/**
 * Read from this channel's registers or SCRIPTS RAM.
 **/
u32 CSym53C8xx::CChannel::bar_read(int bar, u32 address, int dsize) {
  u32 data = 0;
  void *p;

  // Whole-transaction lock: no torn DSP/DSPS/TEMP/DBC reads against a
  // concurrent SCRIPTS step (see WriteMem_Bar).
  std::lock_guard<std::recursive_mutex> lock(myRegLock);

  switch (bar) {
  case 0:
  case 1:
    address &= m_chip.reg_bytes - 1;
    switch (dsize) {
    case 8:
      if (address >= R_SCRATCHB + 4) {
        data = state.regs.reg8[address];
        // With PCI configuration information enabled the part answers with
        // its own identity in SFS, as SCRATCHA and SCRATCHB answer with the
        // BAR bases; it is how a driver tells an 896 from its successors,
        // the chip-type nibble no longer naming a part.
        if (address - R_SFS < 4 && m_chip.reg_bytes > 128 &&
            TB_R8(CTEST2, SRTCH)) {
          const u32 id =
              (u32(m_chip.pci_revision) << 16) | m_chip.pci_device_id;
          data = u8(id >> ((address - R_SFS) * 8));
        }
        break;
      }

      switch (address) {
      case R_SCNTL0: // 00
      case R_SCNTL1: // 01
      case R_SCNTL2: // 02
      case R_SCNTL3: // 03
      case R_SCID:   // 04
      case R_SXFER:  // 05
      case R_SDID:   // 06
      case R_GPREG:  // 07
      case R_SFBR:   // 08
      case R_SOCL:   // 09
      case R_SSID:   // 0A
        data = state.regs.reg8[address];
        break;

      case R_SBCL:                        // 0B
        data = R8(SSTAT1) & R_SBCL_PHASE; // Return current phase signals
        break;

      case R_SSTAT0: // 0D
      case R_SSTAT1: // 0E
        data = state.regs.reg8[address];
        break;

      case R_SSTAT2: // 0F
        data = TB_R8(SCNTL1, CON) ? 0x00 : R_SSTAT2_LDSC;
        break;

      case R_DSA:     // 10
      case R_DSA + 1: // 11
      case R_DSA + 2: // 12
      case R_DSA + 3: // 13
        data = state.regs.reg8[address];
        break;

      case R_ISTAT: // 14
        // CON mirrors the connection state (SCNTL1 CON). Drivers check it
        // in their interrupt handlers: the Windows 2000 symc8xx driver
        // resets the bus on a message interrupt that finds it clear.
        data =
            (R8(ISTAT) & ~R_ISTAT_CON) | (TB_R8(SCNTL1, CON) ? R_ISTAT_CON : 0);
        break;

      case R_CTEST0: // 18
        data = 0xff; // DMA FIFO content byte
        break;

      case R_MBOX0:        // 16
      case R_MBOX1:        // 17
      case R_CTEST1:       // 19
      case R_CTEST3:       // 1B
      case R_TEMP:         // 1C
      case R_TEMP + 1:     // 1D
      case R_TEMP + 2:     // 1E
      case R_TEMP + 3:     // 1F
      case R_CTEST4:       // 21
      case R_CTEST5:       // 22
      case R_DBC:          // 24  // 810
      case R_DBC + 1:      // 25  // 810
      case R_DBC + 2:      // 26  // 810
      case R_DCMD:         // 27  // 810
      case R_DNAD:         // 28  // 810
      case R_DNAD + 1:     // 29  // 810
      case R_DNAD + 2:     // 2A  // 810
      case R_DNAD + 3:     // 2B  // 810
      case R_DSP:          // 2C
      case R_DSP + 1:      // 2D
      case R_DSP + 2:      // 2E
      case R_DSP + 3:      // 2F
      case R_DSPS:         // 30
      case R_DSPS + 1:     // 31
      case R_DSPS + 2:     // 32
      case R_DSPS + 3:     // 33
      case R_DMODE:        // 38
      case R_DIEN:         // 39
      case R_SBR:          // 3A     // 810
      case R_DCNTL:        // 3B
      case R_ADDER:        // 3C
      case R_ADDER + 1:    // 3D
      case R_ADDER + 2:    // 3E
      case R_ADDER + 3:    // 3F
      case R_SIEN0:        // 40
      case R_SIEN1:        // 41
      case R_SLPAR:        // 44
      case R_SWIDE:        // 45
      case R_MACNTL:       // 46     // 810
      case R_GPCNTL:       // 47
      case R_STIME0:       // 48
      case R_STIME1:       // 49
      case R_RESPID:       // 4A
      case R_STEST0:       // 4C
      case R_STEST1:       // 4D
      case R_STEST2:       // 4E
      case R_STEST3:       // 4F
      case R_SIDL:         // 50
      case R_SODL:         // 54
        data = state.regs.reg8[address];
        break;

      case R_SBDL: // 58
        if ((R8(SSTAT1) & R_SSTAT1_PHASE) == SCSI_PHASE_MSG_IN)
          data = state.regs.reg8[R_SIDL];
        else
          data = state.regs.reg8[address];
        break;

      case R_DSTAT: // 0C
        data = read_b_dstat();
        break;

      case R_CTEST2: // 1A
        data = read_b_ctest2();
        break;

      case R_DFIFO:            // 20
        data = R8(DBC) & 0x7f; // 810 - fake the DFIFO count
        break;

      case R_SIST0: // 42
      case R_SIST1: // 43
        data = read_b_sist(address - R_SIST0);
        break;

      case R_SCRATCHA:     // 34
      case R_SCRATCHA + 1: // 35
      case R_SCRATCHA + 2: // 36
      case R_SCRATCHA + 3: // 37
      case R_SCRATCHB:     // 5C
      case R_SCRATCHB + 1: // 5D
      case R_SCRATCHB + 2: // 5E
      case R_SCRATCHB + 3: // 5F
        data = read_b_scratch(address);
        break;

      case R_RESPID + 1: // 4B
        data = (m_chip.id_mask == 0x0f) ? state.regs.reg8[address] : 0;
        break;

      case R_STEST4: // 52
        data = m_chip.stest4;
        break;

      case R_ISTAT1: // 15
        // SRUN says a SCRIPTS program is running. ISTAT0/ISTAT1 and the
        // mailboxes are the registers a driver may read while it does.
        if (m_chip.reg_bytes > 128)
          data = (R8(ISTAT1) & R_ISTAT1_SIRQD) |
                 (state.executing ? R_ISTAT1_SRUN : 0);
        break;

      case R_CCNTL0: // 56
      case R_CCNTL1: // 57
        if (m_chip.reg_bytes > 128)
          data = state.regs.reg8[address];
        break;

      case 0x59: // ??? Linux wants this.
      case 0x23: // CTEST6 NT wants this.
      case 0x51:
      case 0x53:
      case 0x55:
      case 0x5a:
      case 0x5b:
        // printf("SYM: Read from non-existing register at %02x. Linux generic
        // driver.\n", address);
        data = 0;
        break;

      default:
        FAILURE_2(
            NotImplemented,
            "SYM: Attempt to read %i bytes from unknown register at %02" PRIx32
            "\n",
            dsize, address);
      }

#if defined(DEBUG_SYM_REGS)
      printf("SYM: Read from register %02x: %02x.   \n", address, data);
#endif
      break;

    case 16:
      data = (bar_read(1, address + 0, 8) << 0) & 0x00ff;
      data |= (bar_read(1, address + 1, 8) << 8) & 0xff00;
      break;

    case 32:
      data = (bar_read(1, address + 0, 8) << 0) & 0x000000ff;
      data |= (bar_read(1, address + 1, 8) << 8) & 0x0000ff00;
      data |= (bar_read(1, address + 2, 8) << 16) & 0x00ff0000;
      data |= (bar_read(1, address + 3, 8) << 24) & 0xff000000;
      break;
    }
    break;

  case 2:
    if (address + dsize / 8 > m_chip.ram_bytes)
      return 0;
    p = (u8 *)state.ram + address;
    switch (dsize) {
    case 8:
      return *((u8 *)p);
    case 16:
      return *((u16 *)p);
    case 32:
      return *((u32 *)p);
    }
    break;
  }

  return data;
}

/**
 * Write a byte to the SCSI Control 0 register.
 *
 * This is a normal masked write operation; implemented as a separate
 * function, because there are some bits in here (START and TRG) that
 * we should do something with if a driver sets these, but that we
 * don't implement.
 *
 * START: When this bit is set, the controller should start the
 * arbitration seqence indicated by the arbitration mode bits. Used
 * only in low-level mode. UNIMPLEMENTED.
 *
 * TRG: When this bit is set, the controller is a target device.
 * UNIMPLEMENTED.
 **/
void CSym53C8xx::CChannel::write_b_scntl0(u8 value) {
  bool old_start = TB_R8(SCNTL0, START);

  WRM_R8(SCNTL0, value);

  if (TB_R8(SCNTL0, START) && !old_start)
    printf("SYM: START sequence requested via SCNTL0=%02x but low-level "
           "arbitration is not implemented; continuing.\n",
           R8(SCNTL0));

  if (TB_R8(SCNTL0, TRG))
    printf("SYM: target mode requested via SCNTL0=%02x; continuing without "
           "target-mode support.\n",
           R8(SCNTL0));
}

/**
 * Write a byte to the SCSI Control 1 register.
 *
 * This is implemented as a separate function, because there are
 * quite a few side-effects that occur when writing to this register
 *
 * CON (Connected): This bit is automatically set any time the
 * controller is connected to the SCSI bus. The CPU can force a
 * connection or disconnection by setting or clearing this bit.
 * UNIMPLEMENTED.
 *
 * RST: Asserts the SCSI RST/ signal. Has the "side-effect" of
 * resetting the SCSI bus. This effects a couple of other registers.
 *
 * \todo: Implement real reset of the SCSI bus.
 **/
void CSym53C8xx::CChannel::write_b_scntl1(u8 value) {
  bool old_iarb = TB_R8(SCNTL1, IARB);
  bool old_con = TB_R8(SCNTL1, CON);
  bool old_rst = TB_R8(SCNTL1, RST);

  R8(SCNTL1) = value;

  //  if (TB_R8(SCNTL1,CON) != old_con)
  //    printf("SYM: Don't know how to forcibly connect or disconnect\n");
  if (TB_R8(SCNTL1, RST) != old_rst) {
    SB_R8(SSTAT0, SDP0, false);
    SB_R8(SSTAT1, SDP1, false);
    R16(SBDL) = 0;
    R8(SBCL) = 0;

    SB_R8(SSTAT0, RST, !old_rst);

    //    printf("SYM: %s SCSI bus reset.\n",old_rst?"end":"start");
    if (!old_rst) {
      // A bus reset ends any connection: the target drops off and CON
      // clears (drivers write SCNTL1 back read-modify-write, so a stale
      // CON would otherwise survive the reset).
      dev.scsi_bus[index]->reset_bus();
      SB_R8(SCNTL1, CON, false);
      RAISE(SIST0, RST);
    }
  }
}

/**
 * Write a byte to the SCSI Interrupt Status.
 *
 * This is implemented as a separate function, because there are
 * quite a few side-effects that occur when writing to this register
 *
 * ABRT (Abort Operation): Aborts the currently executing SCRIP, and
 * generate an interrupt.
 *
 * SRST (Software Reset): Resets the SCSI chipset.
 *
 * SIGP (Signal Process): Aborts a Wait for (Re)Selection instruction
 * by jumping to the alternate address immediately.
 *
 * Since interrupt state is affected, call eval_interrupts.
 **/
void CSym53C8xx::CChannel::write_b_istat(u8 value) {
  bool old_srst = TB_R8(ISTAT, SRST);
  bool old_sem = TB_R8(ISTAT, SEM);
  bool old_sigp = TB_R8(ISTAT, SIGP);

  WRMW1C_R8(ISTAT, value);

  if (TB_R8(ISTAT, ABRT)) {

    //    printf("SYM: Aborting on request.\n");
    RAISE(DSTAT, ABRT);
  }

  if (TB_R8(ISTAT, SRST) && !old_srst) {

    //    printf("SYM: Resetting on request.\n");
    chip_reset();
  }

  //  if (TB_R8(ISTAT,SEM) != old_sem)
  //    printf("SYM: SEM %s.\n",old_sem?"reset":"set");
  //  if (TB_R8(ISTAT,SIGP) != old_sigp)
  //    printf("SYM: SIGP %s.\n",old_sigp?"reset":"set");
  bool resumed = false;
  if (TB_R8(ISTAT, SIGP)) {
    if (state.wait_reselect) {

      //      printf("SYM: SIGP while wait_reselect. Jumping...\n");
      R32(DSP) = state.wait_jump;
      state.wait_reselect = false;
      start_scripts();
      resumed = true;
    }
#if defined(DEBUG_SYM_START)
    else if (!old_sigp)
      printf("SYM: SIGP set while not in WAIT RESELECT (executing %d, #%lu)\n",
             state.executing, ++dbg_sigp_not_waiting);
#endif
  }

  eval_interrupts();

  // Run after eval_interrupts() so a pending fatal interrupt still halts the
  // resumed program before it executes, as it did when the start was
  // deferred to the thread.
  if (resumed)
    run_scripts_inline();
}

/**
 * Reads a byte from the Chip Test 2 register.
 *
 * This is implemented as a separate function, because:
 *   - The SIGP flag read by this register comes from the ISTAT
 *     register.
 *   - The CIO (configured as I/O) and CM (configured as memory)
 *     flags are determined from PCI Configuration space.
 *   - Reading this register has the side effect of clearing the
 *     SIGP flag.
 *   .
 **/
u8 CSym53C8xx::CChannel::read_b_ctest2() {
  SB_R8(CTEST2, CIO, dev.pci_state.config_data[index][4] != 0);
  SB_R8(CTEST2, CM, dev.pci_state.config_data[index][5] != 0);
  SB_R8(CTEST2, SIGP, TB_R8(ISTAT, SIGP));
  SB_R8(ISTAT, SIGP, false);

  //  printf("SYM: SIGP cleared by CTEST2 read.\n");
  return R8(CTEST2);
}

/**
 * Write a byte to the Chip Test 3 register.
 *
 * This is implemented as a separate function, because there are
 * some unimplemented bits that probably should have a function if
 * a driver ever decides to use these.
 *
 * FM (Fetch Pin Mode): When set, this bit causes the FETCH/ pin to
 * deassert during indirect and table indirect read operations.
 * FETCH/ will only be active during the op codde portion of an
 * instruction fetch. This allows SCRIPTS to be stored in a PROM
 * while data tables are stored in RAM. UNIMPLEMENTED.
 **/
void CSym53C8xx::CChannel::write_b_ctest3(u8 value) {
  WRM_R8(CTEST3, value);

  // if ((value>>3) & 1)
  //   printf("SYM: Don't know how to flush DMA FIFO\n");
  // if ((value>>2) & 1)
  //   printf("SYM: Don't know how to clear DMA FIFO\n");
  if ((value >> 1) & 1)
    printf("SYM: Don't know how to handle FM mode\n");
}

/**
 * Write a byte to the Chip Test 4 register.
 *
 * This is implemented as a separate function, because there are
 * some unimplemented bits that probably should have a function if
 * a driver ever decides to use these.
 *
 * SRTM: Shadow Register Test Mode. Access shadow copies of TEMP and
 * DSA. Used for manufacturing diagnostics only. UNIMPLEMENTED.
 **/
void CSym53C8xx::CChannel::write_b_ctest4(u8 value) {
  R8(CTEST4) = value;

  if ((value >> 4) & 1)
    printf("SYM: Don't know how to handle SRTM mode\n");
}

/**
 * Write a byte to the Chip Test 5 register.
 *
 * This is implemented as a separate function, because there are
 * some unimplemented bits that probably should have a function if
 * a driver ever decides to use these.
 *
 * ADCK (Clock Address Incrementor): Setting this bit increments the
 * DNAD register. The DNAD register is incremented based on the DNAD
 * contents and the current DBC value. This bit automatically clears
 * itself after incrementing the  DNAD register. UNIMPLEMENTED.
 *
 * BBCK (Clock Byte Counter): Setting this bit decrements the byte
 * count contained in the 24-bit DBC register. It is decremented
 * based on the DBC contents and the current DNAD value. This bit
 * automatically clears itself after decrementing the DBC register.
 * UNIMPLEMENTED.
 **/
void CSym53C8xx::CChannel::write_b_ctest5(u8 value) {
  WRM_R8(CTEST5, value);

  if ((value >> 7) & 1)
    printf("SYM: Don't know how to do Clock Address increment\n");

  if ((value >> 6) & 1)
    printf("SYM: Don't know how to do Clock Byte Counter decrement\n");
}

/**
 * Read a byte from the DSTAT register.
 *
 * This is implemented as a separate function, because it requires
 * interrupt re-evaluation.
 **/
u8 CSym53C8xx::CChannel::read_b_dstat() {
  u8 retval = R8(DSTAT);

  RDCLR_R8(DSTAT);

  // printf("Read DSTAT --> eval int\n");
  eval_interrupts();

  // printf("Read DSTAT <-- eval int; retval: %02x; dstat:
  // %02x.\n",retval,R8(DSTAT));
  return retval;
}

/**
 * Read a byte from the SIST0 or SIST1 register.
 *
 * This is implemented as a separate function, because it requires
 * interrupt re-evaluation.
 **/
u8 CSym53C8xx::CChannel::read_b_sist(int id) {
  u8 retval = state.regs.reg8[R_SIST0 + id];

  if (id)
    RDCLR_R8(SIST1);
  else
    RDCLR_R8(SIST0);

  eval_interrupts();

  return retval;
}

/**
 * Write a byte to the DMA Control register.
 *
 * This is implemented as a separate function, because there are
 * some side-effects.
 *
 * STD (Start DMA Operation): Start executing SCSI SCRIPT (inline burst,
 * then the thread).
 *
 * IRQD (IRQ Disable): disables the IRQ pin. Requires interrupt
 * re-evaluation.
 **/
void CSym53C8xx::CChannel::write_b_dcntl(u8 value) {
  WRM_R8(DCNTL, value);

  // start operation
  if (value & R_DCNTL_STD)
    start_scripts();

  // IRQD bit...
  eval_interrupts();

  if (value & R_DCNTL_STD)
    run_scripts_inline();
}

/**
 * Write a byte to the SCSI Test 2 register.
 *
 * This is implemented as a separate function, because there are
 * some unimplemented bits that probably should have a function if
 * a driver ever decides to use these.
 *
 * LOW (Low-level-mode). Switches the SCSI controller to low-level
 * mode operation. No SCRIPTS processor, but raw manipulation of
 * SCSI registers. Yuck. UNIMPLEMENTED.
 **/
void CSym53C8xx::CChannel::write_b_stest2(u8 value) {
  WRM_R8(STEST2, value);

  //  if (value & R_STEST2_ROF)
  //    printf("SYM: Don't know how to reset SCSI offset!\n");
  if (TB_R8(STEST2, LOW))
    printf("SYM: I don't like LOW level mode\n ");
}

/**
 * Write a byte to the SCSI Control 3 register.
 *
 * Disabling wide SCSI (EWS) also clears the Wide SCSI Receive flag.
 **/
void CSym53C8xx::CChannel::write_b_scntl3(u8 value) {
  WRM_R8(SCNTL3, value);

  if (m_chip.id_mask == 0x0f && !TB_R8(SCNTL3, EWS))
    SB_R8(SCNTL2, WSR, false);
}

/**
 * Read a byte of SCRATCHA or SCRATCHB.
 *
 * With CTEST2 SRTCH set (parts with SCRIPTS RAM only), SCRATCHA reads the
 * memory-mapped base of the operating registers (BAR1) and SCRATCHB the
 * base of the RAM (BAR2); the scratch contents are kept.
 **/
u8 CSym53C8xx::CChannel::read_b_scratch(u32 address) {
  if (m_chip.ram_bytes && TB_R8(CTEST2, SRTCH)) {
    const bool a = address < R_SCRATCHB;
    const int byte = int(address - (a ? R_SCRATCHA : R_SCRATCHB));
    const u32 bar = dev.pci_state.config_data[index][a ? 5 : 6];
    return u8(bar >> (byte * 8));
  }
  return state.regs.reg8[address];
}

/**
 * Write a byte to the SCSI Test 3 register.
 *
 * This is implemented as a separate function, because there are
 * some unimplemented bits that probably should have a function if
 * a driver ever decides to use these.
 **/
void CSym53C8xx::CChannel::write_b_stest3(u8 value) {
  WRM_R8(STEST3, value);

  // CSF (Clear SCSI FIFO) clears itself once the FIFO is empty, which with
  // no modelled FIFO is at once. Drivers poll for it: the Windows 2000
  // symc8xx driver resets the chip when it stays set.
  SB_R8(STEST3, CSF, false);
}
