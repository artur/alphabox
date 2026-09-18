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
 * Symbios 53C8xx family: Construction, PCI configuration, threads, reset, state
 * file and the main-thread timers.
 **/
#include "Sym53C8xx.hpp"
#include "Disk.hpp"
#include "SCSIBus.hpp"
#include "StdAfx.hpp"
#include "System.hpp"

#include "Sym53C8xxRegs.hpp"

CSym53C8xx::CChannel::CChannel(CSym53C8xx &dev, int index)
    : dev(dev), m_chip(dev.m_chip), index(index) {}

/**
 * Thread entry point.
 *
 * Repeat:
 *   - Waiting until the semaphore is set
 *   - Executing SCRIPTS code until execution ends.
 *   .
 **/
void CSym53C8xx::CChannel::run() {
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
 * Set up a SCSI bus per channel, and defer the rest of initialization to
 * CSym53C8xx::init.
 **/
CSym53C8xx::CSym53C8xx(CConfigurator *cfg, CSystem *c, int pcibus, int pcidev,
                       const sym_chip_config &chip)
    : CPCIDevice(cfg, c, pcibus, pcidev),
      // One disk bus per channel; narrow parts address targets 0-6, wide
      // ones 0-15.
      CDiskController(chip.channels, chip.id_mask == 0x0f ? 16 : 7),
      m_chip(chip) {

  for (int i = 0; i < m_chip.channels; i++) {
    // create scsi bus
    CSCSIBus *a = new CSCSIBus(cfg, c);
    scsi_register(i, a, 7); // scsi id 7 by default
    channels[i].reset(new CChannel(*this, i));
  }
}

/**
 * Initialize the Symbios device.
 *
 * Reset PCI structures, reset the chipset, and set up locks.
 **/
void CSym53C8xx::init() {
  // PCI header: I/O BAR0 and memory BAR1 for the register file, BAR2 for the
  // on-chip SCRIPTS RAM on the parts that have it. A part with more than one
  // channel gives each of them a function of its own; they are identical
  // save for the interrupt pin and the multi-function bit.
  for (int f = 0; f < m_chip.channels; f++) {
    u32 cfg_data[64] = {};
    u32 cfg_mask[64] = {};
    cfg_data[0x00 >> 2] = (u32(m_chip.pci_device_id) << 16) | 0x1000;
    cfg_data[0x04 >> 2] = 0x02000001;
    cfg_data[0x08 >> 2] = 0x01000000 | m_chip.pci_revision;
    // Header type bit 7: configuration software only looks for functions
    // past 0 when the device says it has them.
    if (m_chip.channels > 1)
      cfg_data[0x0c >> 2] = 0x00800000;
    cfg_data[0x10 >> 2] = 0x00000001;
    // Each channel has an interrupt pin of its own, counting from INTA, so
    // the console can give the two of them separate interrupt lines.
    cfg_data[0x3c >> 2] = 0x401100ff | (u32(f + 1) << 8);
    cfg_mask[0x04 >> 2] = 0x00000157;
    cfg_mask[0x0c >> 2] = 0x0000ffff;
    cfg_mask[0x10 >> 2] = 0xffffff00;
    cfg_mask[0x14 >> 2] = 0xffffff00;
    if (m_chip.ram_bytes)
      cfg_mask[0x18 >> 2] = ~(m_chip.ram_bytes - 1);
    cfg_mask[0x3c >> 2] = 0x000000ff;
    add_function(f, cfg_data, cfg_mask);
  }

  ResetPCI();

  for (int i = 0; i < m_chip.channels; i++)
    channels[i]->init();

  if (m_chip.channels > 1)
    printf("%s: Symbios %s, %d channels\n", devid_string, m_chip.name,
           m_chip.channels);
  else
    printf("%s: Symbios %s\n", devid_string, m_chip.name);
}

void CSym53C8xx::CChannel::init() {
  // chip_reset() only lowers the PCI line if irq_asserted says it is up, so
  // start from a known state rather than whatever the allocation held.
  memset(&state, 0, sizeof(state));
  chip_reset();

  myThread = nullptr;
}

/**
 * Create the threads, and start executing them.
 **/
void CSym53C8xx::start_threads() {
  for (int i = 0; i < m_chip.channels; i++)
    channels[i]->start_thread();
}

void CSym53C8xx::CChannel::start_thread() {
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
 * Stop and destroy the threads.
 **/
void CSym53C8xx::stop_threads() {
  for (int i = 0; i < m_chip.channels; i++)
    channels[i]->stop_thread();
}

void CSym53C8xx::CChannel::stop_thread() {
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
 * Kill threads if still running, and destroy the SCSI busses.
 **/
CSym53C8xx::~CSym53C8xx() {
  stop_threads();
  for (int i = 0; i < m_chip.channels; i++)
    scsi_bus[i] = 0;
}

/**
 * Reset the chipset.
 *
 * Initialize all registers to their default values.
 **/
void CSym53C8xx::CChannel::chip_reset() {
  // SCRIPTS bookkeeping lives outside the register array and must survive
  // neither power-on initialization nor an ISTAT software reset: stale
  // stacked interrupts would drain into SIST0/DSTAT, and a stale disconnect
  // countdown would raise a false UDC from poll().
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
  const u32 pci_rev = dev.pci_state.config_data[index][2];
  R8(CTEST3) = (u8)(pci_rev << 4) & R_CTEST3_REV; // Chip rev.
  R8(MACNTL) = m_chip.macntl;                     // chip type
  R8(GPCNTL) = 0x0F;                              // 810
  R8(STEST0) = 0x03;                              // 810

  // Reset clears the interrupt state, so the IRQ/ pin must drop as well;
  // eval_interrupts() only signals level changes and would otherwise leave
  // the line stuck high.
  if (state.irq_asserted)
    dev.do_pci_interrupt(index, false);
  state.irq_asserted = false;
}

/**
 * Register a disk
 *
 * Attach the disk to the SCSI bus of the channel it is configured on: on a
 * two-channel part disk0.* is the first channel and disk1.* the second.
 **/
void CSym53C8xx::register_disk(class CDisk *dsk, int bus, int dev) {
  CDiskController::register_disk(dsk, bus, dev);
  dsk->scsi_register(0, scsi_bus[bus], dev);
}

// The 53C810's values, so its existing state files still restore.
static u32 sym_magic1 = 0x53C810CC;
static u32 sym_magic2 = 0xCC53C810;

/**
 * Save state to a Virtual Machine State file.
 *
 * One record per channel, in order, so a single-channel part writes exactly
 * what it always has.
 **/
int CSym53C8xx::SaveState(FILE *f) {
  int res;

  if ((res = CPCIDevice::SaveState(f)))
    return res;

  for (int i = 0; i < m_chip.channels; i++)
    if ((res = channels[i]->save(f, devid_string)))
      return res;
  return 0;
}

int CSym53C8xx::CChannel::save(FILE *f, const char *devid) {
  long ss = sizeof(state);

  fwrite(&sym_magic1, sizeof(u32), 1, f);
  fwrite(&ss, sizeof(long), 1, f);
  fwrite(&state, sizeof(state), 1, f);
  fwrite(&sym_magic2, sizeof(u32), 1, f);
  printf("%s: %d bytes saved.\n", devid, (int)ss);
  return 0;
}

/**
 * Restore state from a Virtual Machine State file.
 **/
int CSym53C8xx::RestoreState(FILE *f) {
  int res;

  if ((res = CPCIDevice::RestoreState(f)))
    return res;

  for (int i = 0; i < m_chip.channels; i++)
    if ((res = channels[i]->restore(f, devid_string)))
      return res;
  return 0;
}

int CSym53C8xx::CChannel::restore(FILE *f, const char *devid) {
  long ss;
  u32 m1;
  u32 m2;
  size_t r;

  r = fread(&m1, sizeof(u32), 1, f);
  if (r != 1) {
    printf("%s: unexpected end of file!\n", devid);
    return -1;
  }

  if (m1 != sym_magic1) {
    printf("%s: MAGIC 1 does not match!\n", devid);
    return -1;
  }

  r = fread(&ss, sizeof(long), 1, f);
  if (r != 1) {
    printf("%s: unexpected end of file!\n", devid);
    return -1;
  }

  if (ss != sizeof(state)) {
    printf("%s: STRUCT SIZE does not match!\n", devid);
    return -1;
  }

  r = fread(&state, sizeof(state), 1, f);
  if (r != 1) {
    printf("%s: unexpected end of file!\n", devid);
    return -1;
  }

  r = fread(&m2, sizeof(u32), 1, f);
  if (r != 1) {
    printf("%s: unexpected end of file!\n", devid);
    return -1;
  }

  if (m2 != sym_magic2) {
    printf("%s: MAGIC 2 does not match!\n", devid);
    return -1;
  }

  printf("%s: %d bytes restored.\n", devid, (int)ss);
  return 0;
}

/**
 * Override PCI Configuration Space read action.
 *
 * Lower 80 bytes are normal, upper 80 bytes reflect into the register space
 * -- on the parts whose register file is small enough to fit there. The 896
 * has twice as many registers and stops answering configuration cycles
 * after its power-management capability.
 **/
u32 CSym53C8xx::config_read_custom(int func, u32 address, int dsize, u32 data) {
  if (address >= 0x80 && m_chip.reg_bytes == 128)
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
  if (address >= 0x80 && m_chip.reg_bytes == 128)
    WriteMem_Bar(func, 1, address - 0x80, dsize, data);
}

/**
 * Check if threads are still running.
 **/
void CSym53C8xx::check_state() {
  for (int i = 0; i < m_chip.channels; i++) {
    if (channels[i]->thread_died())
      FAILURE_1(Thread, "SYM thread has died (channel %d)", i);

    const std::string msg = channels[i]->poll();
    if (!msg.empty())
      FAILURE_2(Thread, "SYM SCRIPTS failed (channel %d): %.1024s", i,
                msg.c_str());
  }
}

/**
 * Advance this channel's main-thread timers, and report a SCRIPTS failure
 * the thread left behind.
 **/
std::string CSym53C8xx::CChannel::poll() {
  // Runs on the main thread: take myRegLock so the GP-timer RAISE() below
  // doesn't race SCRIPTS' eval_interrupts(). RAII unlock covers the early
  // returns below.
  std::lock_guard<std::recursive_mutex> regLock(myRegLock);

  if (!scripts_error.empty()) {
    std::string msg;
    msg.swap(scripts_error);
    return msg;
  }

  if (state.gen_timer) {
    state.gen_timer--;
    if (!state.gen_timer) {
      state.gen_timer = (R8(STIME1) & R_STIME1_GEN) * 30;
      RAISE(SIST1, GEN);
      return std::string();
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
      return std::string();
    }

    state.disconnected--;
    if (!state.disconnected) {

      // printf("SYM: Disconnect unexpected. raising interrupt!\n");
      // printf(">");
      // getchar();
      RAISE(SIST0, UDC);
      return std::string();
    }
  }

  return std::string();
}
