/* Alphabox Alpha Emulator
 * Copyright (C) 2026 Artur Goulão
 * Website: https://github.com/artur/alphabox
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
 */

/**
 * \file
 * QLogic ISP10x0: construction, PCI configuration, the register file,
 * resets, the NVRAM, the worker thread and the state file.
 **/
#include "Isp1040.hpp"
#include "Disk.hpp"
#include "SCSIBus.hpp"
#include "StdAfx.hpp"
#include "System.hpp"

#include "Isp1040Regs.hpp"

#include <chrono>

#if defined(DEBUG_ISP)
#define TRACE_ISP(...) printf(__VA_ARGS__)
#else
#define TRACE_ISP(...)
#endif

CIsp1040::CIsp1040(CConfigurator *cfg, CSystem *c, int pcibus, int pcidev,
                   const isp_chip_config &chip)
    : CPCIDevice(cfg, c, pcibus, pcidev),
      CDiskController(1, chip.wide ? 16 : 8), m_chip(chip) {
  CSCSIBus *bus = new CSCSIBus(cfg, c);
  scsi_register(0, bus, 7); // the adapter's own place on the bus
}

CIsp1040::~CIsp1040() {
  stop_threads();
  scsi_bus[0] = 0;
}

void CIsp1040::init() {
  u32 data[64] = {};
  u32 mask[64] = {};

  data[0x00 >> 2] = u32(m_chip.device_id) << 16 | 0x1077; // QLogic
  data[0x04 >> 2] = 0x02800000;                           // medium DEVSEL
  data[0x08 >> 2] = 0x01000000 | m_chip.revision;         // SCSI controller
  data[0x10 >> 2] = 0x00000001;                           // registers, I/O
  data[0x14 >> 2] = 0x00000000;                           // registers, memory
  data[0x3c >> 2] = 0x281401ff;
  mask[0x04 >> 2] = 0x00000157;
  mask[0x0c >> 2] = 0x0000ffff;
  mask[0x10 >> 2] = ~u32(ISP_REG_SIZE - 1);
  mask[0x14 >> 2] = ~u32(ISP_REG_SIZE - 1);
  mask[0x3c >> 2] = 0x000000ff;
  add_function(0, data, mask);

  memset(&state, 0, sizeof(state));
  state.nvram_eeprom.init(ISP_NVRAM_ADDRESS_BITS);
  build_nvram();

  ResetPCI();

  printf("%s: QLogic %s\n", devid_string, m_chip.part);
}

/**
 * The adapter's own settings, as its NVRAM holds them: the header the
 * drivers check ("ISP"), the version they require, this adapter's SCSI
 * address, and per-target entries that allow everything the part can do.
 * The last byte makes the bytes sum to zero, which is the check they
 * apply.
 **/
void CIsp1040::build_nvram() {
  u8 nv[ISP_NVRAM_BYTES];
  memset(nv, 0, sizeof(nv));

  nv[0] = 'I';
  nv[1] = 'S';
  nv[2] = 'P';
  nv[3] = 0;
  nv[4] = 2; // the version the drivers require of this family
  // Byte 5: FIFO threshold in the low bits, this adapter's SCSI address in
  // the top nibble, and the adapter enabled.
  nv[5] = u8(0x02 | (1 << 3) | (state.initiator_id << 4));
  nv[6] = 3;            // bus reset delay, seconds
  nv[7] = 4;            // retry count
  nv[8] = 5;            // retry delay
  nv[9] = 0x06;         // asynchronous data setup time
  nv[10] = 8;           // tag age limit
  nv[11] = 0x03 | 0x20; // both terminators on, automatic termination
  nv[12] = 250;         // selection timeout, milliseconds
  nv[13] = 0;
  nv[14] = 32; // maximum queue depth
  nv[15] = 0;
  nv[16] = u8((m_chip.wide ? 1 : 0) | (m_chip.ultra ? 0x04 : 0));

  for (int t = 0; t < 16; t++) {
    u8 *e = nv + 28 + 6 * t;
    // Renegotiate, auto request sense, tagged queueing, synchronous, wide
    // where the part is wide, parity, disconnect.
    e[0] =
        u8(0x01 | 0x04 | 0x08 | 0x10 | (m_chip.wide ? 0x20 : 0) | 0x40 | 0x80);
    e[1] = 16;                         // execution throttle
    e[2] = m_chip.ultra ? 0x0c : 0x19; // synchronous period
    e[3] = 0x0f | 0x10;                // offset, device enabled
  }

  u8 sum = 0;
  for (int i = 0; i < ISP_NVRAM_BYTES - 1; i++)
    sum = u8(sum + nv[i]);
  nv[ISP_NVRAM_BYTES - 1] = u8(-sum);

  // The part is addressed in words, low byte first.
  for (int w = 0; w < ISP_NVRAM_BYTES / 2; w++)
    state.nvram_eeprom.data[w] = u16(nv[w * 2] | nv[w * 2 + 1] << 8);
}

void CIsp1040::register_disk(CDisk *dsk, int bus, int dev) {
  CDiskController::register_disk(dsk, bus, dev);
  dsk->scsi_register(0, scsi_bus[0], dev);
}

void CIsp1040::ResetPCI() {
  CPCIDevice::ResetPCI();
  std::lock_guard<std::recursive_mutex> lock(myLock);
  chip_reset(false);
}

/**
 * Reset: the RISC stops, the queues are forgotten and the interrupt line
 * drops. A soft reset through the interrupt control register keeps the
 * parameters a driver has set; a power-on reset does not.
 **/
void CIsp1040::chip_reset(bool keep_parameters) {
  state.icr = 0;
  state.isr = 0;
  state.sema = 0;
  state.risc_paused = false;
  state.risc_reset = true;
  state.firmware_running = false;
  state.request_base = 0;
  state.request_length = 0;
  state.request_in = 0;
  state.request_out = 0;
  state.response_base = 0;
  state.response_length = 0;
  state.response_in = 0;
  state.response_out = 0;
  state.queue_pending = false;
  memset(state.mailbox, 0, sizeof(state.mailbox));
  memset(state.mailbox_out, 0, sizeof(state.mailbox_out));

  if (!keep_parameters) {
    state.initiator_id = 7;
    build_nvram();
  }
  if (state.irq_asserted) {
    do_pci_interrupt(0, false);
    state.irq_asserted = false;
  }
}

/**
 * The interrupt line: raised while the RISC has something for the driver
 * -- a mailbox result, an asynchronous event, or entries in the response
 * queue -- and enabled in the interrupt control register.
 **/
void CIsp1040::update_irq() {
  const bool pending = (state.isr & ISP_ISR_RISC_INT) != 0;
  const bool enabled =
      (state.icr & ISP_ICR_ENABLE_ALL) && (state.icr & ISP_ICR_ENABLE_RISC);
  const bool assert = pending && enabled;

  if (assert != state.irq_asserted && do_pci_interrupt(0, assert))
    state.irq_asserted = assert;
}

/**
 * Report an asynchronous event, which arrives in mailbox 0 with the
 * semaphore set, the same way a mailbox result does -- so it has to wait
 * its turn: a result the driver has not read yet would be overwritten,
 * and the driver would never see its command complete.
 **/
void CIsp1040::raise_async(u16 event) {
  if (state.sema & ISP_SEMA_LOCK) {
    state.pending_async = event;
    return;
  }
  state.mailbox_out[0] = event;
  state.sema = ISP_SEMA_LOCK;
  state.isr |= ISP_ISR_RISC_INT | ISP_ISR_IPEND;
  update_irq();
}

void CIsp1040::nvram_pins(u16 value) {
  state.nvram = value;
  state.nvram_eeprom.set_pins((value & ISP_NVRAM_CHIP_SELECT) != 0,
                              (value & ISP_NVRAM_CLOCK) != 0,
                              (value & ISP_NVRAM_DATA_OUT) != 0);
}

u32 CIsp1040::ReadMem_Bar(int func, int bar, u32 address, int dsize) {
  std::lock_guard<std::recursive_mutex> lock(myLock);
  const u32 offset = address & (ISP_REG_SIZE - 1);
  u16 value = 0;

  switch (offset) {
  case ISP_BIU_ID_LO:
    value = ISP_BIU_ID_MAGIC_LO;
    break;
  case ISP_BIU_ID_HI:
    value = ISP_BIU_ID_MAGIC_HI;
    break;
  case ISP_BIU_CONF0:
    value = m_chip.revision;
    break;
  case ISP_BIU_CONF1:
    value = state.conf1;
    break;
  case ISP_BIU_ICR:
    value = state.icr;
    break;
  case ISP_BIU_ISR:
    value = state.isr;
    break;
  case ISP_BIU_SEMA:
    value = state.sema;
    break;
  case ISP_BIU_NVRAM:
    value = u16((state.nvram & ~ISP_NVRAM_DATA_IN) |
                (state.nvram_eeprom.data_out() ? ISP_NVRAM_DATA_IN : 0));
    break;
  case ISP_BIU_REQINP:
    value = state.request_in;
    break;
  case ISP_BIU_REQOUTP:
    value = state.request_out;
    break;
  case ISP_BIU_RSPINP:
    value = state.response_in;
    break;
  case ISP_BIU_RSPOUTP:
    value = state.response_out;
    break;
  case ISP_HCCR:
    value = u16(state.risc_paused ? ISP_HCCR_PAUSE : 0);
    break;
  default:
    if (offset >= ISP_MBOX(0) && offset < ISP_MBOX(ISP_MBOX_COUNT))
      value = state.mailbox_out[(offset - ISP_MBOX(0)) / 2];
    break;
  }

  TRACE_ISP("%s: read  %02x = %04x\n", devid_string, offset, value);
  return dsize == 32 ? value : value & 0xffff;
}

void CIsp1040::WriteMem_Bar(int func, int bar, u32 address, int dsize,
                            u32 data) {
  std::lock_guard<std::recursive_mutex> lock(myLock);
  const u32 offset = address & (ISP_REG_SIZE - 1);
  const u16 value = u16(data);

  TRACE_ISP("%s: write %02x = %04x\n", devid_string, offset, value);

  switch (offset) {
  case ISP_BIU_ICR:
    if (value & ISP_ICR_SOFT_RESET) {
      chip_reset(true);
      return;
    }
    state.icr = value;
    update_irq();
    return;

  case ISP_BIU_CONF1:
    state.conf1 = value;
    return;

  case ISP_BIU_SEMA:
    state.sema = value & (ISP_SEMA_LOCK | ISP_SEMA_STATUS);
    // The driver reads the mailbox and then releases the semaphore, so
    // this is the first moment an event held back for it may go.
    if (!(state.sema & ISP_SEMA_LOCK) && state.pending_async) {
      const u16 event = state.pending_async;
      state.pending_async = 0;
      raise_async(event);
    }
    return;

  case ISP_BIU_NVRAM:
    nvram_pins(value);
    return;

  case ISP_BIU_REQINP:
    // The driver has queued entries for us.
    state.request_in = value;
    run_request_queue();
    return;

  case ISP_BIU_RSPOUTP:
    state.response_out = value;
    return;

  case ISP_HCCR:
    switch (value & ISP_HCCR_CMD_MASK) {
    case ISP_HCCR_CMD_RESET:
      chip_reset(true);
      state.risc_reset = true;
      return;
    case ISP_HCCR_CMD_PAUSE:
      state.risc_paused = true;
      return;
    case ISP_HCCR_CMD_RELEASE:
      state.risc_paused = false;
      return;
    case ISP_HCCR_CMD_SET_HOST_INT:
      // The driver has filled the mailboxes and is asking the RISC to
      // read them.
      mailbox_command();
      return;
    case ISP_HCCR_CMD_CLEAR_RISC_INT:
      state.isr &= ~(ISP_ISR_RISC_INT | ISP_ISR_IPEND);
      update_irq();
      return;
    default:
      return;
    }

  default:
    if (offset >= ISP_MBOX(0) && offset < ISP_MBOX(ISP_MBOX_COUNT)) {
      const int n = (offset - ISP_MBOX(0)) / 2;
      state.mailbox[n] = value;
      // This family carries the queue pointers in mailboxes 4 and 5: the
      // driver announces new commands by writing where it stopped, and
      // reports how far it has consumed the response queue the same way.
      // (A console that runs the adapter's own firmware never issues
      // EXEC FIRMWARE, so the queues are what says the adapter is in use,
      // not any "firmware running" state.)
      if (n == 4 && state.request_length) {
        state.request_in = value;
        run_request_queue();
      } else if (n == 5 && state.response_length) {
        state.response_out = value;
      }
    }
    return;
  }
}

/**
 * Thread: runs queue entries the register write could not finish, so a
 * driver that queues a great many commands at once never waits inside its
 * own write.
 **/
void CIsp1040::run() {
  try {
    std::unique_lock<std::recursive_mutex> lock(myLock);
    while (!StopThread) {
      if (state.queue_pending)
        run_request_queue();
      myWake.wait_for(lock, std::chrono::milliseconds(10));
    }
  } catch (std::exception &e) {
    printf("Exception in ISP thread: %s.\n", e.what());
    myThreadDead.store(true);
  }
}

void CIsp1040::start_threads() {
  if (!myThread) {
    printf(" isp");
    {
      std::lock_guard<std::recursive_mutex> lock(myLock);
      StopThread = false;
    }
    myThread = std::make_unique<std::thread>([this]() { this->run(); });
  }
}

void CIsp1040::stop_threads() {
  {
    std::lock_guard<std::recursive_mutex> lock(myLock);
    StopThread = true;
  }
  myWake.notify_all();
  if (myThread) {
    printf(" isp");
    myThread->join();
    myThread = nullptr;
  }
}

void CIsp1040::check_state() {
  if (myThreadDead.load())
    FAILURE(Thread, "ISP thread has died");
}

u16 CIsp1040::dma_read16(u32 address) {
  u8 b[2];
  do_pci_read(address, b, 1, 2);
  return u16(b[0] | b[1] << 8);
}

u32 CIsp1040::dma_read32(u32 address) {
  u8 b[4];
  do_pci_read(address, b, 1, 4);
  return u32(b[0]) | u32(b[1]) << 8 | u32(b[2]) << 16 | u32(b[3]) << 24;
}

void CIsp1040::dma_write16(u32 address, u16 value) {
  u8 b[2] = {u8(value), u8(value >> 8)};
  do_pci_write(address, b, 1, 2);
}

void CIsp1040::dma_write32(u32 address, u32 value) {
  u8 b[4] = {u8(value), u8(value >> 8), u8(value >> 16), u8(value >> 24)};
  do_pci_write(address, b, 1, 4);
}

static const u32 isp_magic1 = 0x10401077;
static const u32 isp_magic2 = 0x77104010;

int CIsp1040::SaveState(FILE *f) {
  long ss = sizeof(state);
  int res;

  if ((res = CPCIDevice::SaveState(f)))
    return res;

  std::lock_guard<std::recursive_mutex> lock(myLock);
  fwrite(&isp_magic1, sizeof(u32), 1, f);
  fwrite(&ss, sizeof(long), 1, f);
  fwrite(&state, sizeof(state), 1, f);
  fwrite(&isp_magic2, sizeof(u32), 1, f);
  printf("%s: %d bytes saved.\n", devid_string, (int)ss);
  return 0;
}

int CIsp1040::RestoreState(FILE *f) {
  long ss;
  u32 m1;
  u32 m2;
  int res;

  if ((res = CPCIDevice::RestoreState(f)))
    return res;

  if (fread(&m1, sizeof(u32), 1, f) != 1 || m1 != isp_magic1) {
    printf("%s: MAGIC 1 does not match!\n", devid_string);
    return -1;
  }
  if (fread(&ss, sizeof(long), 1, f) != 1 || ss != sizeof(state)) {
    printf("%s: STRUCT SIZE does not match!\n", devid_string);
    return -1;
  }
  std::lock_guard<std::recursive_mutex> lock(myLock);
  if (fread(&state, sizeof(state), 1, f) != 1) {
    printf("%s: unexpected end of file!\n", devid_string);
    return -1;
  }
  if (fread(&m2, sizeof(u32), 1, f) != 1 || m2 != isp_magic2) {
    printf("%s: MAGIC 2 does not match!\n", devid_string);
    return -1;
  }
  printf("%s: %d bytes restored.\n", devid_string, (int)ss);
  return 0;
}
