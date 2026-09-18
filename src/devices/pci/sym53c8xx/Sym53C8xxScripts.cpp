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
 * Symbios 53C8xx family: The SCRIPTS processor: start/stop, SCSI phase checks
 * and the instruction set.
 **/
#include "Disk.hpp"
#include "SCSIBus.hpp"
#include "StdAfx.hpp"
#include "Sym53C8xx.hpp"
#include "System.hpp"

#include "Sym53C8xxRegs.hpp"

/**
 * Called after the DMA Scripts Pointer register has been written.
 *
 * Start executing SCSI SCRIPT (inline burst, then the thread).
 **/
void CSym53C8xx::CChannel::post_dsp_write() {
  if (!TB_R8(DMODE, MAN)) {
    start_scripts();
    run_scripts_inline();

    // printf("SYM: Execution started @ %08x.\n",R32(DSP));
  }
}

/**
 * Mark SCRIPTS as executing. Caller holds myRegLock.
 *
 * A guest-initiated start (DSP write, DCNTL.STD, SIGP resume of WAIT
 * RESELECT) gets a fresh runaway budget. When the write comes from SCRIPTS
 * itself (Load/Store or R/W into DSP/DCNTL), the running program simply
 * continues from the new DSP and keeps its budget, so a self-restarting loop
 * is still caught.
 **/
void CSym53C8xx::CChannel::start_scripts() {
  state.executing = true;
  if (!scripts_running)
    state.insn_processed = 0;
}

/**
 * Execute a bounded SCRIPTS burst on the register-writing thread.
 * Caller holds myRegLock.
 *
 * The real controller starts fetching as soon as DSP or STD is written.
 * Deferring every fetch to the device thread left a window in which a later
 * SIGP write (or a CTEST2 read clearing SIGP) could overtake the start before
 * SCRIPTS reached WAIT RESELECT (upstream ES40 issue #133; worse with SMP).
 *
 * The burst stops when SCRIPTS halt or park in WAIT RESELECT, before any
 * Block Move or Memory Move (target data transfer / disk I/O, bulk DMA), or
 * after SYM_INLINE_INSN_LIMIT instructions; the device thread takes over from
 * there. Nested calls from SCRIPTS itself are no-ops.
 **/
void CSym53C8xx::CChannel::run_scripts_inline() {
  if (scripts_running || !state.executing)
    return;

  int n = 0;
  while (state.executing && n < SYM_INLINE_INSN_LIMIT &&
         inline_can_execute_next()) {
    step_scripts();
    n++;
  }

  if (state.executing) {
#if defined(DEBUG_SYM_START)
    printf("SYM: inline start handed to thread at DSP %08x after %d insns "
           "(#%lu)\n",
           R32(DSP), n, ++dbg_inline_handoffs);
#endif
    scriptsWake.notify_one();
  }
}

/**
 * Peek at the next SCRIPTS instruction: may it run inline?
 *
 * Block Moves hand data to the SCSI target (scsi_xfer_done() performs the
 * disk I/O) and Memory Moves copy up to 1 MiB of guest memory; neither
 * belongs on a CPU thread inside an MMIO write. Everything else (I/O,
 * R/W, Transfer Control, Load/Store) only touches controller and bus state.
 **/
bool CSym53C8xx::CChannel::inline_can_execute_next() {
  u32 insn;
  try {
    dev.do_pci_read(R32(DSP), &insn, 4, 1);
  } catch (...) {
    return false; // let the thread's execute() hit and report it
  }

  u8 dcmd = (u8)(insn >> 24);
  switch ((dcmd >> 6) & 3) {
  case 0: // Block Move
    return false;
  case 3: // Memory Move (bit 5 clear) or Load/Store
    return (dcmd & 0x20) != 0;
  default: // I/O, R/W, Transfer Control
    return true;
  }
}

/**
 * Execute one SCRIPTS instruction. Caller holds myRegLock.
 *
 * Exceptions never propagate: this may run on a CPU (interpreter/JIT) thread
 * inside an MMIO write. SCRIPTS are halted and the failure is raised by
 * the main thread, which collects it in poll().
 **/
void CSym53C8xx::CChannel::step_scripts() {
  scripts_running = true;
  try {
    execute();
  } catch (CException &e) {
    halt_scripts_on_failure(e.displayText());
  } catch (std::exception &e) {
    halt_scripts_on_failure(e.what());
  } catch (...) {
    halt_scripts_on_failure("unknown exception");
  }
  scripts_running = false;
}

/**
 * Halt SCRIPTS after an exception and record it for poll().
 * Caller holds myRegLock.
 **/
void CSym53C8xx::CChannel::halt_scripts_on_failure(const std::string &msg) {
  printf("SYM: exception while executing SCRIPTS at DSP %08x: %s\n", R32(DSP),
         msg.c_str());
  state.executing = false;
  state.wait_reselect = false;
  if (scripts_error.empty())
    scripts_error = msg;
}

/**
 * Check SCSI Bus Phase.
 *
 * Returns -1 on timeout or similar, 0 on different phase, and 1 on same phase
 **/

int CSym53C8xx::CChannel::check_phase(int chk_phase) {
  int real_phase = dev.scsi_get_phase(index);

  if (real_phase == SCSI_PHASE_ARBITRATION) {
#if defined(DEBUG_SYM_SCRIPTS)
    printf("Phase check... selection time-out!\n");
#endif
    RAISE(SIST1, STO); // select time-out
    dev.scsi_free(index);
    state.select_timeout = false;
    return -1;
  }

  if (real_phase == SCSI_PHASE_FREE && state.disconnected) {
#if defined(DEBUG_SYM_SCRIPTS)
    printf("Phase check... disconnected!\n");
#endif
    state.disconnected = 1;
    R32(DSP) -= 8;
    return -1;
  }

  // Always update SSTAT1 with the real phase
  R8(SSTAT1) = (R8(SSTAT1) & ~R_SSTAT1_PHASE) | (real_phase & R_SSTAT1_PHASE);

  if (real_phase == chk_phase)
    return 1;
  else
    return 0;
}

/**
 * Execute one SCRIPTS Block Move instruction
 *
 * The Block Move instruction moves data between system memory and the
 * SCSI Bus. This is a two DWORD instruction. The instruction format in
 * the DCMD register is as follows:
 *
 * \code
 * +---+-+-+-+-----+
 * |7 6|5|4|3|2 1 0| DCMD Register
 * +---+-+-+-+-----+
 *   |  | | |   +- 0..2: SCSI Phase (I/O, C/D and MSG/ signals):
 *   |  | | |            The data transfer only occurs if these bits
 *   |  | | |            match the actual SCSI bus phase.
 *   |  | | +- 3: Op Code: IGNORED
 *   |  | +- 4: Table Indirect Adressing:
 *   |  |         0: The DSPS register contains the address of the data,
 *   |  |            and the DBC register contains the number of bytes to
 *   |  |            transfer.
 *   |  |         1: The DSPS register contains a 24-bit signed offset
 *   |  |            that is added to the DSA register to get a pointer
 *   |  |            to a data structure that contains the address and
 *   |  |            byte count. This structure looks as follows:
 *   |  |                           +----+------------+
 *   |  |               DSA+DSPS:   | 00 | Byte Count |
 *   |  |                           +----+------------+
 *   |  |               DSA+DSPS+4: |  Data Address   |
 *   |  |                           +-----------------+
 *   |  +- 5: Indirect Addressing:
 *   |          0: The DSPS register contains the address of the data
 *   |          1: The DSPS register contains the address of a 32-bit
 *   |             pointer to the data.
 *   +- 6..7: Instruction Type: 00 = Block Move
 * \endcode
 **/
void CSym53C8xx::CChannel::execute_bm_op() {
  bool indirect = (R8(DCMD) >> 5) & 1;
  bool table_indirect = (R8(DCMD) >> 4) & 1;
  int opcode = (R8(DCMD) >> 3) & 1;
  int scsi_phase = (R8(DCMD) >> 0) & 7;

#if defined(DEBUG_SYM_SCRIPTS)
  printf("SYM: INS = Block Move (i %d, t %d, opc %d, phase %d\n", indirect,
         table_indirect, opcode, scsi_phase);
#endif
  // Check for delayed select timeout
  if (state.regs.reg8[R_SIST1] & R_SIST1_STO) {
#if defined(DEBUG_SYM_SCRIPTS)
    printf("SYM: Delayed select timeout on block move\n");
#endif
    state.executing = false;
    return;
  }

  // Where the instruction and its operands came from. A phase-mismatch
  // jump hands these to the SCRIPTS routine that has to take the transfer
  // up again, and it has to hand them over even when the phase never
  // matched -- a target that disconnects before the first byte is the case
  // the whole mechanism exists for -- so the operands are read before the
  // phase is compared. Nothing is committed to DNAD or DBC until it is.
  const u32 insn_addr = R32(DSP) - 8;
  u32 entry_addr = insn_addr;
  u32 start;
  u32 count;
  u8 count_top = R8(DCMD);

  if (table_indirect) {
    entry_addr = (R32(DSA) + sext_u32_24(R32(DSPS))) & ~0x03u; // 810
#if defined(DEBUG_SYM_SCRIPTS)
    printf("SYM: Reading table at DSA(%08x)+DSPS(%08x) = %08x.\n", R32(DSA),
           R32(DSPS), entry_addr);
#endif
    u32 entry;
    dev.do_pci_read(entry_addr, &entry, 4, 1);
    count = entry & 0x00ffffff;
    count_top = (u8)(entry >> 24);
    dev.do_pci_read(entry_addr + 4, &start, 4, 1);
  } else if (indirect) {
    dev.do_pci_read(R32(DSPS), &start, 4, 1);
    count = GET_DBC();
  } else {
    start = R32(DSPS);
    count = GET_DBC();
  }

  // Compare phase
  int phase_result = check_phase(scsi_phase);

  if (phase_result < 0) {
    // Timeout or disconnect — check_phase already raised the
    // appropriate interrupt
    return;
  }

  if (phase_result == 0) {
    // Phase mismatch — interrupt, or jump, before a single byte moved
#if defined(DEBUG_SYM_SCRIPTS)
    printf("SYM: Phase mismatch! Expected %d, got different.\n", scsi_phase);
#endif
    // Update SSTAT1 with the actual phase from the SCSI bus
    int real_phase = dev.scsi_get_phase(index);
    R8(SSTAT1) = (R8(SSTAT1) & ~R_SSTAT1_PHASE) | (real_phase & R_SSTAT1_PHASE);

    phase_mismatch(scsi_phase, insn_addr, entry_addr, count_top, count, start,
                   0);
    return;
  }

  // Phase matches — proceed with data transfer
#if defined(DEBUG_SYM_SCRIPTS)
  printf("SYM: Ready for transfer.\n");
  printf("SYM: %08x: MOVE Start/count %x, %x\n", insn_addr, start, count);
#endif
  R32(DNAD) = start;
  SET_DBC(count); // page 5-32

  if (count == 0) {

    // printf("SYM: Count equals zero!\n");
    RAISE(DSTAT, IID); // page 5-32
    return;
  }

  u32 moved = 0; // what this Block Move put on the bus, for SBC and CSBC

  for (;;) {
    size_t expected = dev.scsi_expected_xfer(index);
    u32 remaining = GET_DBC();
    u32 xfer = remaining;

    if ((size_t)xfer > expected) {
#if defined(DEBUG_SYM_SCRIPTS)
      printf("SYM: xfer %u bytes, max %zu expected, in phase %d.\n", xfer,
             expected, scsi_phase);
#endif
      xfer = (u32)expected;
    }

    if (xfer == 0) {
      // Target has nothing to provide/accept in this phase but DBC > 0.
      // Report a phase mismatch so SCRIPTS can save the residual.
      phase_mismatch(scsi_phase, insn_addr, entry_addr, count_top, remaining,
                     R32(DNAD), moved);
      return;
    }

    u8 *scsi_data_ptr = (u8 *)dev.scsi_xfer_ptr(index, xfer);
    u8 *org_sdata_ptr = scsi_data_ptr;

    switch (scsi_phase) {
    case SCSI_PHASE_COMMAND:
    case SCSI_PHASE_DATA_OUT:
    case SCSI_PHASE_MSG_OUT:
      dev.do_pci_read(R32(DNAD), scsi_data_ptr, 1, xfer);
      R32(DNAD) += xfer;
      break;

    case SCSI_PHASE_STATUS:
    case SCSI_PHASE_DATA_IN:
    case SCSI_PHASE_MSG_IN:
      dev.do_pci_write(R32(DNAD), scsi_data_ptr, 1, xfer);
      R32(DNAD) += xfer;
      break;
    }

    SET_DBC(remaining - xfer);
    moved += xfer;
    R8(SFBR) = *org_sdata_ptr;

    // Update SIDL with last byte received during MSG_IN
    if (scsi_phase == SCSI_PHASE_MSG_IN)
      state.regs.reg8[R_SIDL] = scsi_data_ptr[xfer - 1];

    if (GET_DBC() == 0) {
      // Clean completion; reflect just-completed phase in SSTAT1 for
      // SCRIPTS that read it before the next check_phase.
      R8(SSTAT1) =
          (R8(SSTAT1) & ~R_SSTAT1_PHASE) | (scsi_phase & R_SSTAT1_PHASE);
      dev.scsi_xfer_done(index);
      count_scsi_bytes(scsi_phase, moved);
      return;
    }

    // Residual remains. Hand the slice back to the target and re-check
    // phase before continuing.
    dev.scsi_xfer_done(index);

    phase_result = check_phase(scsi_phase);
    if (phase_result <= 0) {
      // phase_result < 0: check_phase already raised STO/disconnect.
      // phase_result == 0: the phase shifted under the transfer; report the
      //                    mismatch with what is left of it.
      if (phase_result == 0)
        phase_mismatch(scsi_phase, insn_addr, entry_addr, count_top, GET_DBC(),
                       R32(DNAD), moved);
      return;
    }
  }
}

/**
 * Is this part jumping on a phase mismatch rather than interrupting?
 **/
bool CSym53C8xx::CChannel::pm_jump() const {
  return m_chip.reg_bytes > 128 && TB_R8(CCNTL0, ENPMJ);
}

/**
 * Account for what a Block Move moved across the SCSI bus.
 *
 * SBC counts one instruction, CSBC counts data phases until the driver
 * reloads it; the phase-mismatch routines work out a residual from them.
 * Only the parts that jump on a mismatch keep these counts.
 **/
void CSym53C8xx::CChannel::count_scsi_bytes(int phase, u32 moved) {
  if (!pm_jump())
    return;

  R32(SBC) = moved & 0x00ffffff;
  if (phase == SCSI_PHASE_DATA_IN || phase == SCSI_PHASE_DATA_OUT)
    R32(CSBC) += moved;
}

/**
 * A Block Move met a phase the SCRIPTS program did not ask for.
 *
 * The older parts can only interrupt with MA and leave the driver to work
 * out from DBC and DNAD where the transfer stopped. The 896 keeps that
 * state in registers of its own -- what is left of the byte count, where
 * the data would have gone next, and where the instruction and its operands
 * live -- and jumps to a SCRIPTS routine instead, so a target that
 * disconnects mid-transfer never reaches the host at all. Drivers turn it
 * on with ENPMJ in CCNTL0 and name the two routines in PMJAD1 and PMJAD2:
 * one for the phases the initiator drives and one for the phases the target
 * drives, unless PMJCTL asks for them to be chosen by the wide residue.
 **/
void CSym53C8xx::CChannel::phase_mismatch(int phase, u32 insn_addr,
                                          u32 entry_addr, u8 count_top,
                                          u32 remaining, u32 address,
                                          u32 moved) {
  if (!pm_jump()) {
    RAISE(SIST0, MA);
    return;
  }

  count_scsi_bytes(phase, moved);
  R32(RBC) = (remaining & 0x00ffffff) | (u32(count_top) << 24);
  R32(UA) = address;
  R32(ESA) = entry_addr;
  R32(IA) = insn_addr;

  const bool outbound = phase == SCSI_PHASE_DATA_OUT ||
                        phase == SCSI_PHASE_COMMAND ||
                        phase == SCSI_PHASE_MSG_OUT;
  const bool second = TB_R8(CCNTL0, PMJCTL) ? TB_R8(SCNTL2, WSR) : !outbound;
  R32(DSP) = second ? R32(PMJAD2) : R32(PMJAD1);

#if defined(DEBUG_SYM_SCRIPTS)
  printf("SYM: phase mismatch in phase %d: jumping to %08x, %u of %u bytes "
         "left at %08x.\n",
         phase, R32(DSP), remaining, remaining + moved, address);
#endif
}

/* Execute one SCRIPTS I/O instruction
 *
 * The I/O instructions perform common SCSI hardware sequences, like
 * Selection and reselection. The instruction format in
 * the DCMD and DBC register is as follows:
 *
 * \code
 * +---+-----+-+-+-+
 * |7 6|5 4 3|2|1|0| DCMD Register
 * +---+-----+-+-+-+
 *   |    |   | | +- 0: Select with ATN/:
 *   |    |   | |       Valid only for Select instruction. Assert ATN/ during
 *   |    |   | |       selection
 *   |    |   | +- 1: Table Indirect Mode:
 *   |    |   |         0: All information is taken from the instruction,
 *   |    |   |            and the contents of the SCNTL3 and SXFER registers.
 *   |    |   |         1: The DSPS register contains a 24-bit signed offset
 *   |    |   |            that is added to the DSA register to get a pointer
 *   |    |   |            to a data structure that contains the destination
 *   |    |   |            ID, SCNTL3 bits, and SXFER bits. This structure
 *   |    |   |            is 32 bits long and looks as follows:
 *   |    |   |                        +--------+--------+--------+--------+
 *   |    |   |            DSA+DSPS:   | SCNTL3 | ID     | SXFER  |        |
 *   |    |   |                        +--------+--------+--------+--------+
 *   |    |   +- 2: Relative Addrressing:
 *   |    |           0: The value in the DNAD register is an absolute address.
 *   |    |           1: The value in the DNAD register is a 24-bit signed
 *   |    |              displacement from the current DSP address.
 *   |    +- 3..5: Op Code
 *   +- 6..7 Instruction Type: 01 = I/O
 *
 * +-------+-------++--------+--+-+-++-+-+---+-+-----+
 * |       |19   16||        |10|9| || |6|   |3|     | DBC Register
 * +-------+-------++--------+--+-+-++-+-+---+-+-----+
 *             |              |  |      |     +- 3: Set/Clear ATN
 *             |              |  |      +- 6: Set/Clear ACK
 *             |              |  +- 9: Set/Clear Target Mode
 *             |              +- 10: Set/Clear Carry
 *             +- 16..19: Destination ID
 * \endcode
 *
 * The Opcode determines the actual instruction:
 * \code
 * +-----+-----------------+-------------+
 * | OPC | Initiator mode  | Target Mode |
 * +-----+-----------------+-------------+
 * | 000 | Select          | Reselect    |
 * | 001 | Wait Disconnect | Disconnect  |
 * | 010 | Wait Reselect   | Wait Select |
 * | 011 | Set             | Set         |
 * | 100 | Clear           | Clear       |
 * +-----+-----------------+-------------+
 * \endcode
 *
 * Select:
 *   - Arbitrate for the SCSI bus until arbitration is won.
 *   - If arbitration is won, try to select the destination ID
 *   - If the controller is selected or reselected before winning
 *     arbitration, jump to the address in the DNAD register.
 *   .
 *
 * Wait Disconnect:
 *   - Wait for the target to disconnect from the SCSI bus.
 *   .
 *
 * Wait Reselect:
 *   - If the controller is reselected, go to the next instruction.
 *   - If the controller is selected before being reselected, or if
 *     the CPU sets the SIGP flag, jump to the address in the DNAD
 *     register
 *   .
 *
 * Set/Clear:
 *   - Set or Clear the flags whose Set/Clear bits are set in the
 *     instruction.
 *   .
 **/
void CSym53C8xx::CChannel::execute_io_op() {
  int opcode = (R8(DCMD) >> 3) & 7;
  bool relative = (R8(DCMD) >> 2) & 1;
  bool table_indirect = (R8(DCMD) >> 1) & 1;
  bool atn = (R8(DCMD) >> 0) & 1;
  int destination = (GET_DBC() >> 16) & 0x0f;
  bool sc_carry = (GET_DBC() >> 10) & 1;
  bool sc_target = (GET_DBC() >> 9) & 1;
  bool sc_ack = (GET_DBC() >> 6) & 1;
  bool sc_atn = (GET_DBC() >> 3) & 1;

  R32(DNAD) = R32(DSPS);

  u32 dest_addr = R32(DNAD);

  if (relative)
    dest_addr = R32(DSP) + sext_u32_24(R32(DNAD));

#if defined(DEBUG_SYM_SCRIPTS)
  printf("SYM: INS = I/O (opc %d, r %d, t %d, a %d, dest %d, sc %d%d%d%d\n",
         opcode, relative, table_indirect, atn, destination, sc_carry,
         sc_target, sc_ack, sc_atn);
#endif
  if (table_indirect) {
    u32 io_addr = R32(DSA) + sext_u32_24(GET_DBC());
    io_addr &= ~3; // 810
#if defined(DEBUG_SYM_SCRIPTS)
    printf("SYM: Reading table at DSA(%08x)+DBC(%08x) = %08x.\n", R32(DSA),
           sext_u32_24(GET_DBC()), io_addr);
#endif

    u32 io_struc;
    dev.do_pci_read(io_addr, &io_struc, 4, 1);
    destination = (io_struc >> 16) & 0x0f;
#if defined(DEBUG_SYM_SCRIPTS)
    printf("SYM: table indirect. io_struct = %08x, new dest = %d.\n", io_struc,
           destination);
#endif
  }

  switch (opcode) {
  case 0:
#if defined(DEBUG_SYM_SCRIPTS)
    printf("SYM: %08x: SELECT %d.\n", R32(DSP) - 8, destination);
#endif
    SET_DEST(destination);

    // Check if already connected (reselected before arb won)
    if (TB_R8(SCNTL1, CON)) {
#if defined(DEBUG_SYM_SCRIPTS)
      printf("SYM: Already connected, jumping to alternate address\n");
#endif
      R32(DSP) = dest_addr;
      return;
    }

    if (!dev.scsi_arbitrate(index)) {

      // scsi bus busy, try again next clock...
      printf("scsi bus busy...\n");
      R32(DSP) -= 8;
      return;
    }

    // Set Won Arbitration, clear Immediate Arbitration
    SB_R8(SSTAT0, WOA, true);
    SB_R8(SCNTL1, IARB, false);

    state.select_timeout = !dev.scsi_select(index, destination);

    if (!state.select_timeout) // select ok
    {
      // Set Connected bit
      SB_R8(SCNTL1, CON, true);

      // Set ATN if select-with-ATN
      if (atn)
        SB_R8(SOCL, ATN, true);

      // Set phase to MSG OUT after successful select
      R8(SSTAT1) = (R8(SSTAT1) & ~R_SSTAT1_PHASE) | SCSI_PHASE_MSG_OUT;

      SB_R8(SCNTL2, SDU, true); // don't expect a disconnect
    }
    return;

  case 1:
#if defined(DEBUG_SYM_SCRIPTS)
    printf("SYM: %08x: WAIT DISCONNECT\n", R32(DSP) - 8);
#endif
    // Clear Connected bit on disconnect
    SB_R8(SCNTL1, CON, false);
    // Clear phase bits
    R8(SSTAT1) &= ~R_SSTAT1_PHASE;
    {
      int cur_phase = dev.scsi_get_phase(index);
      if (cur_phase == SCSI_PHASE_ARBITRATION) {
        // We won arbitration; the initiator may free the bus.
        dev.scsi_free(index);
      } else if (cur_phase != SCSI_PHASE_FREE) {
        // free_bus() only lets the selected target release a connected
        // bus, and our passive targets never drop BSY on their own --
        // release on the target's behalf instead of aborting.
        printf("SYM: WAIT DISCONNECT with bus still connected (phase %d, "
               "target %d); releasing.\n",
               cur_phase, GET_DEST());
        dev.scsi_bus[index]->free_bus(GET_DEST());
      }
    }
    return;

  case 2:
#if defined(DEBUG_SYM_SCRIPTS)
    printf("SYM: %08x: WAIT RESELECT\n", R32(DSP) - 8);
#endif
    if (TB_R8(ISTAT, SIGP)) {
#if defined(DEBUG_SYM_SCRIPTS)
      printf("SYM: SIGP set before wait reselect; jumping!\n");
#endif
      R32(DSP) = dest_addr;
    } else {
      state.wait_reselect = true;
      state.wait_jump = dest_addr;
      state.executing = false;
    }

    return;

  case 3:
#if defined(DEBUG_SYM_SCRIPTS)
    printf("SYM: %08x: SET %s%s%s%s\n", R32(DSP) - 8, sc_carry ? "carry " : "",
           sc_target ? "target " : "", sc_ack ? "ack " : "",
           sc_atn ? "atn " : "");
#endif
    if (sc_ack)
      SB_R8(SOCL, ACK, true);
    if (sc_atn) {
      if (!TB_R8(SOCL, ATN)) {
        SB_R8(SOCL, ATN, true);

        // printf("SET ATN.\n");
        // printf(">");
        // getchar();
      }
    }

    if (sc_target)
      SB_R8(SCNTL0, TRG, true);
    if (sc_carry)
      state.alu.carry = true;
    return;

  case 4:
#if defined(DEBUG_SYM_SCRIPTS)
    printf("SYM: %08x: CLEAR %s%s%s%s\n", R32(DSP) - 8,
           sc_carry ? "carry " : "", sc_target ? "target " : "",
           sc_ack ? "ack " : "", sc_atn ? "atn " : "");
#endif
    if (sc_ack)
      SB_R8(SOCL, ACK, false);
    if (sc_atn) {
      if (TB_R8(SOCL, ATN)) {
        SB_R8(SOCL, ATN, false);

        // printf("RESET ATN.\n");
        // printf(">");
        // getchar();
      }
    }

    if (sc_target)
      SB_R8(SCNTL0, TRG, false);
    if (sc_carry)
      state.alu.carry = false;
    return;

    break;
  }
}

/**
 * Execute one SCRIPTS R/W instruction
 *
 * The R/W instructions perform arithmetic or logic operations on
 * registers. The instruction format in the DCMD and DBC register is as follows:
 *
 * \code
 * +---+-----+-----+
 * |7 6|5 4 3|2 1 0| DCMD Register
 * +---+-----+-----+
 *   |    |     +- 2..0: operator:
 *   |    |                000: data8
 *   |    |                001: reg << 1
 *   |    |                010: reg | data8
 *   |    |                011: reg ^ data8
 *   |    |                100: reg & data8
 *   |    |                101: reg >> 1
 *   |    |                110: reg + data8
 *   |    |                111: reg + data8 + carry
 *   |    +- 3..5: Op Code
 *   |               101: regA = operator(SFBR, data8)
 *   |               110: SFBR = operator(RegA, data8)
 *   |               111: regA = operator(RegA, data8)
 *   +- 6..7 Instruction Type: 01 = R/W
 *
 * +--+------------++---------------++-+-------------+
 * |23|22        16||15            8||7|             | DBC Register
 * +--+------------++---------------++-+-------------+
 *  |  A6--------A0         |         A7
 *  |        +--------------|---------+- 7,22..16: RegA address
 *  |                       |
 *  |                       +- 15..8: Immediate data
 *  +- 23: Use data8/SFBR
 *         0: data8 = Immediate data
 *         1: data8 = SFBR
 * \endcode
 */
void CSym53C8xx::CChannel::execute_rw_op() {
  int opcode = (R8(DCMD) >> 3) & 7;
  int oper = (R8(DCMD) >> 0) & 7;
  bool use_data8_sfbr = (GET_DBC() >> 23) & 1;
  int reg_address = ((GET_DBC() >> 16) & 0x7f) | (GET_DBC() & 0x80);
  u8 imm_data = (u8)(GET_DBC() >> 8) & 0xff;
  u8 op_data;

#if defined(DEBUG_SYM_SCRIPTS)
  printf("SYM: INS = R/W (opc %d, oper %d, use %d, add %d, imm %02x\n", opcode,
         oper, use_data8_sfbr, reg_address, imm_data);
#endif
  if (use_data8_sfbr)
    imm_data = R8(SFBR);

  if (oper != 0) {
    if (opcode == 5 || reg_address == 0x08) {
      op_data = R8(SFBR);
#if defined(DEBUG_SYM_SCRIPTS)
      printf("SYM: %08x: sfbr (%02x) ", R32(DSP) - 8, op_data);
#endif
    } else {
      op_data = (u8)bar_read(1, reg_address, 8);
#if defined(DEBUG_SYM_SCRIPTS)
      printf("SYM: %08x: reg%02x (%02x) ", R32(DSP) - 8, reg_address, op_data);
#endif
    }
  }

  u16 tmp16;

  switch (oper) {
  case 0:
    op_data = imm_data;
#if defined(DEBUG_SYM_SCRIPTS)
    printf("SYM: %08x: %02x ", R32(DSP) - 8, imm_data);
#endif
    break;

  case 1:
    tmp16 = (op_data << 1) + (state.alu.carry ? 1 : 0);
    state.alu.carry = (tmp16 >> 8) & 1;
    op_data = tmp16 & 0xff;
#if defined(DEBUG_SYM_SCRIPTS)
    printf("<< 1 = %02x ", op_data);
#endif
    break;

  case 2:
    op_data |= imm_data;
#if defined(DEBUG_SYM_SCRIPTS)
    printf("| %02x = %02x ", imm_data, op_data);
#endif
    break;

  case 3:
    op_data ^= imm_data;
#if defined(DEBUG_SYM_SCRIPTS)
    printf("^ %02x = %02x ", imm_data, op_data);
#endif
    break;

  case 4:
    op_data &= imm_data;
#if defined(DEBUG_SYM_SCRIPTS)
    printf("& %02x = %02x ", imm_data, op_data);
#endif
    break;

  case 5:
    tmp16 = (op_data >> 1) + (state.alu.carry ? 0x80 : 0x00);
    state.alu.carry = op_data & 1;
    op_data = tmp16 & 0xff;
#if defined(DEBUG_SYM_SCRIPTS)
    printf(">> 1 = %02x ", op_data);
#endif
    break;

  case 6:
    tmp16 = op_data + imm_data;
    state.alu.carry = (tmp16 > 0xff);
    op_data = tmp16 & 0xff;
#if defined(DEBUG_SYM_SCRIPTS)
    printf("+ %02x = %02x (carry %d) ", imm_data, op_data, state.alu.carry);
#endif
    break;

  case 7:
    tmp16 = op_data + imm_data + (state.alu.carry ? 1 : 0);
    state.alu.carry = (tmp16 > 0xff);
    op_data = tmp16 & 0xff;
#if defined(DEBUG_SYM_SCRIPTS)
    printf("+ %02x (w/carry) = %02x (carry %d) ", imm_data, op_data,
           state.alu.carry);
#endif
    break;
  }

  if (opcode == 6 || reg_address == 0x08) {
#if defined(DEBUG_SYM_SCRIPTS)
    printf("-> sfbr.\n");
#endif
    R8(SFBR) = op_data;
  } else {
#if defined(DEBUG_SYM_SCRIPTS)
    printf("-> reg%02x.\n", reg_address);
#endif
    bar_write(1, reg_address, 8, op_data);
  }
}

/**
 * Execute one SCRIPTS Transfer Control instruction
 *
 * The Transfer Control instructions perform conditional jumps, calls,
 * returns and interrupts. The instruction format in the DCMD and DBC
 * registers is as follows:
 *
 * \code
 * +---+-----+-----+
 * |7 6|5 4 3|2 1 0| DCMD Register
 * +---+-----+-----+
 *   |    |     +- 0..2: SCSI Phase (I/O, C/D and MSG/ signals):
 *   |    |              The actual SCSI phase is compared against these
 *   |    |              bits.
 *   |    +- 3..5: Op Code
 *   |               000: Jump
 *   |               001: Call
 *   |               010: Return
 *   |               011: Interrupt
 *   |               1xx: reserved
 *   +- 6..7 Instruction Type: 10 = Transfer Control
 *
 * +--+-+--+--+--+--+--+--++---------------++---------------+
 * |23| |21|20|19|18|17|16||15            8||7             0| DBC Register
 * +--+-+--+--+--+--+--+--++---------------++---------------+
 *  |    |  |  |  |  |  |          |             +- 7..0: Data to compare
 *  |    |  |  |  |  |  |          |                      against SFBR
 *  |    |  |  |  |  |  |          +- 15..8: Mask that determines what bits
 *  |    |  |  |  |  |  |                    to compare against SFBR.
 *  |    |  |  |  |  |  +- 16: Wait for valid SCSI phase
 *  |    |  |  |  |  +- 17: Compare Phase
 *  |    |  |  |  +- 18: Compare SFBR data
 *  |    |  |  +- Jump if:
 *  |    |  |       0: Jump/Call/return/Interrupt if the equation is true
 *  |    |  |       0: Jump/Call/return/Interrupt if the equation is false
 *  |    |  +- Interrupt on the Fly
 *  |    +- Carry Test
 *  +- relative Addressing:
 *       0: The value in the DSPS register is an absolute address.
 *       1: The value in the DSPS register is a 24-bit signed
 *          displacement from the current DSP address.
 * \endcode
 *
 * The equation evaluated is one of the following:
 *   - the value of the carry bit (if the Carry Test bit is set)
 *   - equality comparisons of SCSI phase and/or SFBR register data
 *   - true (if none of the compare/carry test bits are set)
 *   .
 *
 * An action is taken when the equation evaluates to either true or false
 * as determined by the "Jump if" bit.
 *
 * Jump:
 *   - Jump to the instruction addressed by the DSPS register.
 *   .
 *
 * Call:
 *   - Store the current DSP register value to the TEMP register.
 *   - Jump to the instruction addressed by the DSPS register.
 *   .
 *
 * Return:
 *   - Jump to the instruction addressed by the TEMP register.
 *   .
 *
 * Interrupt:
 *   - If the Interrupt on the Fly bit is set, raise the INTF interrupt.
 *   - Otherwise:
 *       - Raise the SIR interrupt
 *       - Terminate SCRIPTS execution
 *       - The DSPS value is used as an interrupt vector for the driver.
 *       .
 *   .
 **/
void CSym53C8xx::CChannel::execute_tc_op() {
  int opcode = (R8(DCMD) >> 3) & 7;
  int scsi_phase = (R8(DCMD) >> 0) & 7;
  bool relative = (GET_DBC() >> 23) & 1;
  bool carry_test = (GET_DBC() >> 21) & 1;
  bool interrupt_fly = (GET_DBC() >> 20) & 1;
  bool jump_if = (GET_DBC() >> 19) & 1;
  bool cmp_data = (GET_DBC() >> 18) & 1;
  bool cmp_phase = (GET_DBC() >> 17) & 1;
  int cmp_mask = (GET_DBC() >> 8) & 0xff;
  int cmp_dat = (GET_DBC() >> 0) & 0xff;
  u32 dest_addr;

  // wait_valid can be safely ignored, phases are always valid in this ideal
  // world... bool wait_valid = (GET_DBC()>>16) & 1;

  // Check for delayed select timeout
  if (state.regs.reg8[R_SIST1] & R_SIST1_STO) {
#if defined(DEBUG_SYM_SCRIPTS)
    printf("SYM: Delayed select timeout in Transfer Control\n");
#endif
    state.executing = false;
    return;
  }

  // If no comparison flags are set at all, this is a NOP
  if (!(GET_DBC() & 0x002e0000)) {
#if defined(DEBUG_SYM_SCRIPTS)
    printf("SYM: Transfer Control NOP\n");
#endif
    return;
  }

  // We'll keep modifying this variable until we know what the result of the
  // comparisons is.
  bool do_it;

  // Relative jump or not? (no effect on Return or Interrupt)
  if (relative)
    dest_addr = R32(DSP) + sext_u32_24(R32(DSPS));
  else
    dest_addr = R32(DSPS);

#if defined(DEBUG_SYM_SCRIPTS)
  printf("SYM: %08x: if (", R32(DSP) - 8);
#endif
  if (carry_test) {
    // All we need to check is the CARRY flag
#if defined(DEBUG_SYM_SCRIPTS)
    printf("(%scarry)", jump_if ? "" : "!");
#endif
    do_it = (state.alu.carry == jump_if);
  } else if (cmp_data || cmp_phase) {
    // We need to compare data and/or phase
    do_it = true;
    if (cmp_data) {
      // compare data
#if defined(DEBUG_SYM_SCRIPTS)
      printf("((data & 0x%02x) %s 0x%02x)", (~cmp_mask) & 0xff,
             jump_if ? "==" : "!=", cmp_dat & ~cmp_mask);
#endif
      if (((R8(SFBR) & ~cmp_mask) == (cmp_dat & ~cmp_mask)) != jump_if)
        do_it = false;
#if defined(DEBUG_SYM_SCRIPTS)
      if (cmp_phase)
        printf(" && ");
#endif
    }

    if (cmp_phase) {
      // Compare phase
#if defined(DEBUG_SYM_SCRIPTS)
      printf("(phase %s %d)", jump_if ? "==" : "!=", scsi_phase);
#endif
      if ((check_phase(scsi_phase) > 0) != jump_if)
        do_it = false;
    }
  } else {

    // no comparison
    do_it = jump_if;
  }

#if defined(DEBUG_SYM_SCRIPTS)
  printf(") ");
#endif
  switch (opcode) {
  case 0:
#if defined(DEBUG_SYM_SCRIPTS)
    printf("jump %x\n", R32(DSPS));
#endif
    if (do_it) {
#if defined(DEBUG_SYM_SCRIPTS)
      printf("SYM: Jumping %08x...\n", dest_addr);
#endif
      R32(ADDER) = dest_addr;
      R32(DSP) = dest_addr;
    }

    return;
    break;

  case 1:
#if defined(DEBUG_SYM_SCRIPTS)
    printf("call %d\n", R32(DSPS));
#endif
    if (do_it) {
#if defined(DEBUG_SYM_SCRIPTS)
      printf("SYM: Calling %08x...\n", dest_addr);
#endif
      R32(TEMP) = R32(DSP);
      R32(ADDER) = dest_addr;
      R32(DSP) = dest_addr;
    }

    return;
    break;

  case 2:
#if defined(DEBUG_SYM_SCRIPTS)
    printf("return %d\n", R32(DSPS));
#endif
    if (do_it) {
#if defined(DEBUG_SYM_SCRIPTS)
      printf("SYM: Returning %08x...\n", R32(TEMP));
#endif
      R32(DSP) = R32(TEMP);
    }

    return;
    break;

  case 3:
#if defined(DEBUG_SYM_SCRIPTS)
    printf("interrupt%s.\n", interrupt_fly ? " on the fly" : "");
#endif
    if (do_it) {
#if defined(DEBUG_SYM_SCRIPTS)
      printf("SYM: Interrupt with vector %x...\n", R32(DSPS));
#endif
      if (interrupt_fly)
        RAISE(ISTAT, INTF);
      else
        RAISE(DSTAT, SIR);
    }

    return;
    break;

  default:
    FAILURE_1(NotImplemented,
              "SYM: Transfer Control Instruction with opcode %d is RESERVED.\n",
              opcode);
  }
}

/**
 * Execute one SCRIPTS Load/Store instruction
 *
 * The Load/Store instruction moves data between registers and memory.
 * The memory range could very well map back to the registers through
 * the PCI bus! The instruction format in the DCMD and DBC registers
 * is as follows:
 *
 * \code
 * +-----+-+---+-+-+
 * |7 6 5|4|   |1|0| DCMD Register
 * +-----+-+---+-+-+
 *    |   |     | +- 0: Load/Store
 *    |   |     |         0: Store (register -> memory)
 *    |   |     |         1: Load (memory -> register)
 *    |   |     +- 1: No Flush (no effect)
 *    |   +- 4: DSA Relative
 *    |           0: The value in the DSPS register is absolute.
 *    |           1: The value in the DSPS register is a 24-bit
 *    |              signed offset from DSA.
 *    +- 7..5 Instruction Type: 111 = Load/Store
 *
 * +---------------++---------------++---------------+
 * |23           16||               ||         |2   0| DBC Register
 * +---------------++---------------++---------------+
 *          |                                     +- 2..0: Byte Count
 *          +- 23..16: Register Address
 * \endcode
 *
 * This instructions moves up to 4 bytes between registers and memory.
 **/
void CSym53C8xx::CChannel::execute_ls_op() {
  bool is_load = (R8(DCMD) >> 0) & 1;
  bool no_flush = (R8(DCMD) >> 1) & 1;
  bool dsa_relative = (R8(DCMD) >> 4) & 1;
  // Load/Store carries the whole register address in DBC[23:16], so it
  // reaches the upper half of a 256-byte register file without the bit-7
  // trick the Read/Write instruction needs.
  int regaddr = (GET_DBC() >> 16) & (m_chip.reg_bytes - 1);
  int byte_count = (GET_DBC() >> 0) & 7;
  u32 memaddr;

  // Relative Addressing
  if (dsa_relative)
    memaddr = R32(DSA) + sext_u32_24(R32(DSPS));
  else
    memaddr = R32(DSPS);

#if defined(DEBUG_SYM_SCRIPTS)
  printf("SYM: dsa_rel: %d, DSA: %04x, DSPS: %04x, mem %04x.\n", dsa_relative,
         R32(DSA), R32(DSPS), memaddr);
#endif
  if (is_load) {
#if defined(DEBUG_SYM_SCRIPTS)
    printf("SYM: %08x: Load reg%02x", R32(DSP) - 8, regaddr);
    if (byte_count > 1)
      printf("..%02x", regaddr + byte_count - 1);
    printf("from %x.\n", memaddr);
#endif
    // Perform Load Operation
    for (int i = 0; i < byte_count; i++) {
      u8 dat;
      dev.do_pci_read(memaddr + i, &dat, 1, 1);
#if defined(DEBUG_SYM_SCRIPTS)
      printf("SYM: %02x -> reg%02x\n", dat, regaddr + i);
#endif
      bar_write(1, regaddr + i, 8, dat);
    }
  } else {
#if defined(DEBUG_SYM_SCRIPTS)
    printf("SYM: %08x: Store reg%02x", R32(DSP) - 8, regaddr);
    if (byte_count > 1)
      printf("..%02x", regaddr + byte_count - 1);
    printf("to %x.\n", memaddr);
#endif
    // Perform Store Operation
    for (int i = 0; i < byte_count; i++) {
      u8 dat = (u8)bar_read(1, regaddr + i, 8);
#if defined(DEBUG_SYM_SCRIPTS)
      printf("SYM: %02x <- reg%02x\n", dat, regaddr + i);
#endif
      dev.do_pci_write(memaddr + i, &dat, 1, 1);
    }
  }
}

/**
 * Execute one SCRIPTS Memory Move instruction
 *
 * The Memory Move instruction is used to transfer data from one
 * region in host memory to another region through the SCSI
 * controller's. DMA. The instruction format in the DCMD register
 * is as follows:
 *
 * \code
 * +-----+-------+-+
 * |7 6 5|       |0| DCMD Register
 * +-----+-------+-+
 *    |           +- No Flush (no effect)
 *    +- 7..5 Instruction Type: 110 = Memory Move
 * \endcode
 *
 * This is a 3-DWORD instruction.
 *
 * The DBC register holds the number of bytes to be moved.
 * The DSPS register holds the source address.
 * The TEMP register holds the destination address.
 */
void CSym53C8xx::CChannel::execute_mm_op() {
  u32 temp_shadow;
  dev.do_pci_read(R32(DSP), &temp_shadow, 4, 1);
  R32(DSP) += 4;

#if defined(DEBUG_SYM_SCRIPTS)
  printf("SYM: %08x: Memory Move %06x bytes from %08x to %08x.\n",
         R32(DSP) - 12, GET_DBC(), R32(DSPS), temp_shadow);
#endif

  const u32 dbc = GET_DBC();
  if (dbc == 0 ||
      dbc > 0x100000) // 1 MiB cap — larger than any legit Memory Move
  {
    printf("SYM: Memory Move DBC=%u out of range; aborting.\n", dbc);
    state.executing = false;
    RAISE(DSTAT, ABRT);
    return;
  }

  void *buf = malloc(dbc);
  if (!buf) {
    printf("SYM: Memory Move malloc(%u) failed; aborting.\n", dbc);
    state.executing = false;
    RAISE(DSTAT, ABRT);
    return;
  }

  dev.do_pci_read(R32(DSPS), buf, 1, dbc);
  dev.do_pci_write(temp_shadow, buf, 1, dbc);
  free(buf);
  return;
}

/**
 * Execute one SCRIPTS instruction.
 *
 * DSP (DMA Scripts Pointer) contains the address of the next instruction.
 * For each instruction, two DWORDS are read into the 8-bit DCMD (DMA Command),
 * 24-bit DBC (DMA Byte Counter), and 32-bit DSPS (DMA Scripts Pointer Save)
 * registers. For some commands, a third DWORD is read into the 32-bit TEMP
 * register:
 *
 * \code
 *        +--------+------------------------+
 * DSP  : |  DCMD  |          DBC           |
 *        +--------+------------------------+
 * DSP+4: |              DSPS               |
 *        +---------------------------------+
 * DSP+8: |/ / / / / / / TEMP  / / / / / / /|
 *        +---------------------------------+
 * \endcode
 **/
void CSym53C8xx::CChannel::execute() {
  int optype;
  int opcode;
  bool is_load_store;

  if (++state.insn_processed > SYM_MAX_INSN_PER_BURST) {
    printf("SYM: SCRIPTS runaway (> %d instructions without halt); aborting.\n",
           SYM_MAX_INSN_PER_BURST);
    state.executing = false;
    RAISE(DSTAT, ABRT);
    return;
  }

#if defined(DEBUG_SYM_SCRIPTS)
  printf("SYM: INS @ %x   \n", R32(DSP));
#endif

  // Read 2 DWORDS into the DCMD, DBC and DSPS registers.
  dev.do_pci_read(R32(DSP), &R32(DBC), 4, 1);
  dev.do_pci_read(R32(DSP) + 4, &R32(DSPS), 4, 1);

  // Increase DSP to point to the next instruction
  R32(DSP) += 8;

#if defined(DEBUG_SYM_SCRIPTS)
  printf("SYM: INS = %x, %x, %x   \n", R8(DCMD), GET_DBC(), R32(DSPS));
#endif
  /* The two most significant bits of the DCMD register determine the operation
   * type. These are:
   *   00: Block Move
   *   01: I/O or R/W
   *   10: Transfer Control
   *   11: Memory Move or Load And Store
   */
  optype = (R8(DCMD) >> 6) & 3;
  switch (optype) {
  case 0:
    execute_bm_op();
    break;

  case 1:
    opcode = (R8(DCMD) >> 3) & 7;
    if (opcode < 5)
      execute_io_op();
    else
      execute_rw_op();
    break;

  case 2:
    execute_tc_op();
    break;

  case 3:
    is_load_store = (R8(DCMD) >> 5) & 1;
    if (is_load_store)
      execute_ls_op();
    else
      execute_mm_op();
    break;
  }

  // single step mode
  if (TB_R8(DCNTL, SSM)) {
#if defined(DEBUG_SYM_SCRIPTS)
    printf("SYM: Single step...\n");
#endif
    RAISE(DSTAT, SSI);
  }
}
