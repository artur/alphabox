/* AXPbox Alpha Emulator
 * Copyright (C) 2020 Tomáš Glozar
 * Copyright (C) 2026 Artur Goulão
 * Website: https://github.com/lenticularis39/axpbox
 *          https://github.com/artur/axpbox
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
 * Symbios 53C8xx family: Construction, PCI configuration, threads, reset, state
 * file and the main-thread timers.
 **/
#include "Sym53C8xx.hpp"
#include "Disk.hpp"
#include "SCSIBus.hpp"
#include "StdAfx.hpp"
#include "System.hpp"

#include "Sym53C8xxRegs.hpp"

/**
 * Thread entry point.
 *
 * Repeat:
 *   - Waiting until the semaphore is set
 *   - Executing SCRIPTS code until execution ends.
 *   .
 **/
void CSym53C8xx::run() {
  try {
    std::unique_lock<std::recursive_mutex> lock(myRegLock);
    for (;;) {
      scriptsWake.wait(lock, [this] { return StopThread || state.executing; });
      if (StopThread)
        return;
      step_scripts();
      if (state.executing) {
        // Let register accesses interleave between instructions.
        lock.unlock();
        lock.lock();
      }
    }
  } catch (std::exception &e) {
    printf("Exception in SYM thread: %s.\n", e.what());
    myThreadDead.store(true);

    // Let the thread die...
  }
}

/**
 * Constructor.
 *
 * Set up the SCSI bus, and defer the rest of initialization to
 * CSym53C8xx::init.
 **/
CSym53C8xx::CSym53C8xx(CConfigurator *cfg, CSystem *c, int pcibus, int pcidev,
                       const sym_chip_config &chip)
    : CPCIDevice(cfg, c, pcibus, pcidev),
      // Narrow parts address targets 0-6, wide ones 0-15.
      CDiskController(1, chip.id_mask == 0x0f ? 16 : 7), m_chip(chip) {

  // create scsi bus
  CSCSIBus *a = new CSCSIBus(cfg, c);
  scsi_register(0, a, 7); // scsi id 7 by default
}

/**
 * Initialize the Symbios device.
 *
 * Reset PCI structures, reset the chipset, and set up locks.
 **/
void CSym53C8xx::init() {
  // PCI header: I/O BAR0 and memory BAR1 for the 128-byte register file,
  // BAR2 for the on-chip SCRIPTS RAM on the parts that have it.
  u32 cfg_data[64] = {};
  u32 cfg_mask[64] = {};
  cfg_data[0x00 >> 2] = (u32(m_chip.pci_device_id) << 16) | 0x1000;
  cfg_data[0x04 >> 2] = 0x02000001;
  cfg_data[0x08 >> 2] = 0x01000000 | m_chip.pci_revision;
  cfg_data[0x10 >> 2] = 0x00000001;
  cfg_data[0x3c >> 2] = 0x401101ff;
  cfg_mask[0x04 >> 2] = 0x00000157;
  cfg_mask[0x0c >> 2] = 0x0000ffff;
  cfg_mask[0x10 >> 2] = 0xffffff00;
  cfg_mask[0x14 >> 2] = 0xffffff00;
  if (m_chip.ram_bytes)
    cfg_mask[0x18 >> 2] = ~(m_chip.ram_bytes - 1);
  cfg_mask[0x3c >> 2] = 0x000000ff;
  add_function(0, cfg_data, cfg_mask);

  ResetPCI();

  // chip_reset() only lowers the PCI line if irq_asserted says it is up, so
  // start from a known state rather than whatever the allocation held.
  memset(&state, 0, sizeof(state));
  chip_reset();

  myThread = nullptr;

  printf("%s: Symbios %s\n", devid_string, m_chip.name);
}

/**
 * Create the thread, and start executing it.
 **/
void CSym53C8xx::start_threads() {
  if (!myThread) {
    printf(" sym");
    {
      std::lock_guard<std::recursive_mutex> lock(myRegLock);
      StopThread = false;
    }
    // The thread checks state.executing before its first wait, so a restored
    // running SCRIPTS program resumes without an explicit wake.
    myThread = std::make_unique<std::thread>([this]() { this->run(); });
  }
}

/**
 * Stop and destroy the thread.
 **/
void CSym53C8xx::stop_threads() {
  {
    std::lock_guard<std::recursive_mutex> lock(myRegLock);
    StopThread = true;
  }
  scriptsWake.notify_all();
  if (myThread) {
    printf(" sym");
    myThread->join();
    myThread = nullptr;
  }
}

/**
 * Destructor.
 *
 * Kill thread if still running, and destroy the SCSI bus.
 **/
CSym53C8xx::~CSym53C8xx() {
  stop_threads();
  scsi_bus[0] = 0;
}

/**
 * Reset the chipset.
 *
 * Initialize all registers to their default values.
 **/
void CSym53C8xx::chip_reset() {
  // SCRIPTS bookkeeping lives outside the register array and must survive
  // neither power-on initialization nor an ISTAT software reset: stale
  // stacked interrupts would drain into SIST0/DSTAT, and a stale disconnect
  // countdown would raise a false UDC from check_state().
  state.executing = false;
  state.wait_reselect = false;
  state.select_timeout = false;
  state.disconnected = 0;
  state.wait_jump = 0;
  state.alu.carry = false;
  state.dstat_stack = 0;
  state.sist0_stack = 0;
  state.sist1_stack = 0;
  state.gen_timer = 0;
  state.insn_processed = 0;
  state.scsi_phase = SCSI_PHASE_FREE;
  state.status = 0;
  memset(state.msg, 0, sizeof(state.msg));
  state.msg_len = 0;
  state.msg_action = 0;
  state.current_lun = 0;
  state.command_complete = 0;
  memset(state.regs.reg32, 0, sizeof(state.regs.reg32));
  R8(SCNTL0) = R_SCNTL0_ARB1 | R_SCNTL0_ARB0; // 810
  R8(DSTAT) = R_DSTAT_DFE;                    // DMA FIFO empty // 810

  //  R8(SSTAT2) = R_SSTAT2_LDSC; // 810
  R8(CTEST1) = R_CTEST1_FMT;  // 810
  R8(CTEST2) = R_CTEST2_DACK; // 810
  R8(CTEST3) =
      (u8)(pci_state.config_data[0][2] << 4) & R_CTEST3_REV; // Chip rev.
  R8(MACNTL) = m_chip.macntl;                                // chip type
  R8(GPCNTL) = 0x0F;                                         // 810
  R8(STEST0) = 0x03;                                         // 810

  // Reset clears the interrupt state, so the IRQ/ pin must drop as well;
  // eval_interrupts() only signals level changes and would otherwise leave
  // the line stuck high.
  if (state.irq_asserted)
    do_pci_interrupt(0, false);
  state.irq_asserted = false;
}

/**
 * Register a disk
 *
 * Attach the disk to the SCSI bus.
 **/
void CSym53C8xx::register_disk(class CDisk *dsk, int bus, int dev) {
  CDiskController::register_disk(dsk, bus, dev);
  dsk->scsi_register(0, scsi_bus[0], dev);
}

// The 53C810's values, so its existing state files still restore.
static u32 sym_magic1 = 0x53C810CC;
static u32 sym_magic2 = 0xCC53C810;

/**
 * Save state to a Virtual Machine State file.
 **/
int CSym53C8xx::SaveState(FILE *f) {
  long ss = sizeof(state);
  int res;

  if ((res = CPCIDevice::SaveState(f)))
    return res;

  fwrite(&sym_magic1, sizeof(u32), 1, f);
  fwrite(&ss, sizeof(long), 1, f);
  fwrite(&state, sizeof(state), 1, f);
  fwrite(&sym_magic2, sizeof(u32), 1, f);
  printf("%s: %d bytes saved.\n", devid_string, (int)ss);
  return 0;
}

/**
 * Restore state from a Virtual Machine State file.
 **/
int CSym53C8xx::RestoreState(FILE *f) {
  long ss;
  u32 m1;
  u32 m2;
  int res;
  size_t r;

  if ((res = CPCIDevice::RestoreState(f)))
    return res;

  r = fread(&m1, sizeof(u32), 1, f);
  if (r != 1) {
    printf("%s: unexpected end of file!\n", devid_string);
    return -1;
  }

  if (m1 != sym_magic1) {
    printf("%s: MAGIC 1 does not match!\n", devid_string);
    return -1;
  }

  r = fread(&ss, sizeof(long), 1, f);
  if (r != 1) {
    printf("%s: unexpected end of file!\n", devid_string);
    return -1;
  }

  if (ss != sizeof(state)) {
    printf("%s: STRUCT SIZE does not match!\n", devid_string);
    return -1;
  }

  r = fread(&state, sizeof(state), 1, f);
  if (r != 1) {
    printf("%s: unexpected end of file!\n", devid_string);
    return -1;
  }

  r = fread(&m2, sizeof(u32), 1, f);
  if (r != 1) {
    printf("%s: unexpected end of file!\n", devid_string);
    return -1;
  }

  if (m2 != sym_magic2) {
    printf("%s: MAGIC 2 does not match!\n", devid_string);
    return -1;
  }

  printf("%s: %d bytes restored.\n", devid_string, (int)ss);
  return 0;
}

/**
 * Override PCI Configuration Space read action.
 *
 * Lower 80 bytes are normal, upper 80 bytes reflect into the
 * register space.
 **/
u32 CSym53C8xx::config_read_custom(int func, u32 address, int dsize, u32 data) {
  if (address >= 0x80)
    return ReadMem_Bar(func, 1, address - 0x80, dsize);
  else
    return data;
}

/**
 * Override PCI Configuration Space write action.
 *
 * Lower 80 bytes are normal, upper 80 bytes reflect into the
 * register space.
 **/
void CSym53C8xx::config_write_custom(int func, u32 address, int dsize,
                                     u32 old_data, u32 new_data, u32 data) {
  if (address >= 0x80)
    WriteMem_Bar(func, 1, address - 0x80, dsize, data);
}

/**
 * Check if threads are still running.
 **/
void CSym53C8xx::check_state() {
  if (myThreadDead.load())
    FAILURE(Thread, "SYM thread has died");

  // Runs on the main thread: take myRegLock so the GP-timer RAISE() below
  // doesn't race SCRIPTS' eval_interrupts(). RAII unlock covers the early
  // returns (and the throw) below.
  std::lock_guard<std::recursive_mutex> regLock(myRegLock);

  if (!scripts_error.empty()) {
    std::string msg;
    msg.swap(scripts_error);
    FAILURE_1(Thread, "SYM SCRIPTS failed: %.1024s", msg.c_str());
  }

  if (state.gen_timer) {
    state.gen_timer--;
    if (!state.gen_timer) {
      state.gen_timer = (R8(STIME1) & R_STIME1_GEN) * 30;
      RAISE(SIST1, GEN);
      return;
    }
  }

  /**

  if (state.wait_reselect && PT.disconnected)
  {
    state.executing = true;
    state.wait_reselect = false;
    PT.disconnected = false;
    //PT.disconnect_priv = false;
    //PT.will_disconnect = false;
    PT.reselected = true;
    state.phase = 7; // msg in //PT.disconnect_phase;
    R8(SSID) = GET_DEST() | R_SSID_VAL; // valid scsi selector id
    if (TB_R8(DCNTL,COM))
          R8(SFBR) = GET_DEST();
    // don't expect a disconnect.
    SB_R8(SCNTL2,SDU,true);
    //RAISE(SIST0,RSL);
    return 0;
  }

**/
  if (state.disconnected) {
    if (!TB_R8(SCNTL2, SDU)) {

      // disconnect expected
      // printf("SYM: Disconnect expected. stopping disconnect timer at
      // %d.\n",state.disconnected);
      state.disconnected = 0;
      return;
    }

    state.disconnected--;
    if (!state.disconnected) {

      // printf("SYM: Disconnect unexpected. raising interrupt!\n");
      // printf(">");
      // getchar();
      RAISE(SIST0, UDC);
      return;
    }
  }
}
