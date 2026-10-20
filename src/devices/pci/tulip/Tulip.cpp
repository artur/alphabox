/* Alphabox Alpha Emulator
 * Copyright (C) 2020 Tomáš Glozar
 * Copyright (C) 2021 Dietmar M. Zettl
 * Website: https://github.com/lenticularis39/axpbox
 *
 * Forked from: ES40 emulator
 * Copyright (C) 2007-2008 by the ES40 Emulator Project
 * Copyright (C) 2007 by Camiel Vanderhoeven
 *
 * Contains code from ES40-Emu:
 * Copyright (C) 2025-2026 gdwnldsKSC and the ES40-Emu contributors
 * Website: https://github.com/ES40-Emu/es40
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
 *
 * Parts of this file based upon GXemul, which is Copyright (C) 2004-2007
 * Anders Gavare.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/**
 * \file
 * The Tulip Ethernet controllers: the device itself -- how it appears
 * on the PCI bus, its register file, reset, the state file, and the
 * thread that keeps frames moving.
 **/
#include "StdAfx.hpp"

#include "Tulip.hpp"
#include "System.hpp"
#include "NicAddress.hpp"
#include <string.h>

//#define DEBUG_NIC_IRQ

/*
 * Threads and locking
 *
 * A Tulip is driven from two sides. The CPU threads read and write its CSRs
 * (ReadMem_Bar, WriteMem_Bar); the NIC's own thread (run) moves frames
 * between the back end and the descriptor rings in guest memory, which is
 * the chip's DMA. Both change the same registers, CSR5 above all, where the
 * chip sets bits while the driver clears others, so `state` and the receive
 * queue are guarded by one mutex, m_lock.
 *
 * m_lock is a leaf: nothing else is taken while it is held, and it is held
 * only for plain computation on the state. It is never held across
 *  - a DMA transfer (do_pci_read, do_pci_write), which takes the memory
 *    system's DMA writer gate and can wait for the CPUs;
 *  - the interrupt line (do_pci_interrupt), which takes the PCI device's
 *    and the chipset's interrupt locks;
 *  - the back end (send, receive, set_filter), whose calls can block.
 * The lock this replaces (29d0ac0, removed in 0f7c160 after it caused
 * regressions upstream) was held over the NIC thread's whole pass, all of
 * the above included: a CSR access could wait for a full receive drain, and
 * every lock taken inside those calls nested under it.
 *
 * What that costs, and how it is paid:
 *  - DMA. The NIC thread takes what it needs from the state under the lock
 *    (the descriptor's address, DBO, the frame), lets go for the transfer,
 *    and commits the result under the lock again. A reset or a new list
 *    address (CSR3, CSR4) meanwhile bumps m_rx_epoch or m_tx_epoch, and a
 *    descriptor fetched before it is then not committed. A stop (SR or ST
 *    cleared) does not: the descriptor already written back is passed on
 *    and its interrupt raised, as the chip finishes the frame in hand, but
 *    the process state field stays as the stop set it. The bytes of the
 *    frame in hand (rx.current.frame, tx.cur_buf) are written by the NIC
 *    thread alone, so it reads them during a transfer without the lock.
 *  - The interrupt line. update_irq() computes the level under the lock.
 *    One thread at a time drives the line (m_irq_busy); one that finds it
 *    busy leaves the work to the other, which looks again once it has
 *    driven the line. The line so ends at the level the last change asks
 *    for, and no thread waits for another's interrupt delivery.
 *  - The back end. Only the NIC thread calls it (and init, before there is
 *    one). A change of filter (CSR6, a setup frame, a reset) sets
 *    m_filter_dirty and wakes the thread, which hands the new host filter
 *    to the back end (apply_filter).
 */

/**
 * Thread entry point. One pass: the host filter, frames from the back end
 * into the receive ring, the transmit ring onto the back end, the interrupt
 * line.
 **/
void CTulip::run() {
  try {
    while (!StopThread.load()) {
      apply_filter();
      const bool more = receive_process();
      while (dec21143_tx())
        ;
      update_irq();
      // A back end with frames still waiting gets the next pass at once.
      if (!more)
        mySemaphore.try_wait_for(std::chrono::milliseconds(10));
    }
  }

  catch (CException &e) {
    printf("Exception in NIC thread: %s.\n", e.displayText().c_str());
    myThreadDead.store(true);

    // Let the thread die...
  }
}


/**
 * Constructor.
 **/
CTulip::CTulip(CConfigurator *confg, CSystem *c, int pcibus, int pcidev,
               const tulip_chip_config &chip)
    : CPCIDevice(confg, c, pcibus, pcidev), m_chip(chip) {}

/**
 * Initialize the network device.
 **/
void CTulip::init() {
  tulip_config_space(m_chip, cfg_data, cfg_mask);
  add_function(0, cfg_data, cfg_mask);

  net_backend = create_network_backend(myCfg);
  if (!net_backend)
    FAILURE(Runtime, "Failed to create network backend");

  if (!net_backend->init(devid_string, myCfg))
    FAILURE(Runtime, "Failed to initialize network backend");

  nic_station_address(myCfg, devid_string, state.mac);

  rx_queue = new CPacketQueue("rx_queue",
                              (int)myCfg->get_num_value("queue", false, 1024));
  calc_crc = myCfg->get_bool_value("crc", false);
  trace_packets = myCfg->get_bool_value("trace_packets", false);

  state.rx.cur_buf = NULL;
  /* Use a 2KB TX scratch buffer like QEMU's tulip (tx_frame[2048]) to avoid
   * overflows. */
  state.tx.cur_buf = (unsigned char *)malloc(2048);
  state.irq_was_asserted = false;

  ResetPCI();
  ResetNIC(); // explicit one-shot internal reset; previously implicit via
              // ResetPCI

  myThread = nullptr;

  printf("%s: DECchip %s network interface.\n", devid_string, m_chip.part);
}

void CTulip::start_threads() {
  if (!myThread) {
    printf(" nic");
    StopThread = false;
    myThread = std::make_unique<std::thread>([this]() { this->run(); });
  }
}

void CTulip::stop_threads() {
  StopThread = true;
  if (myThread) {
    mySemaphore.set();
    printf(" nic");
    myThread->join();
    myThread = nullptr;
  }
}

/**
 * Destructor.
 **/
CTulip::~CTulip() {
  stop_threads();

  if (net_backend) {
    net_backend->close();
    delete net_backend;
  }
  delete rx_queue;
  /* Free TX scratch on device teardown (not on ResetNIC, which expects it
   * alive). */
  if (state.tx.cur_buf) {
    free(state.tx.cur_buf);
    state.tx.cur_buf = nullptr;
  }
}

u32 CTulip::ReadMem_Bar(int func, int bar, u32 address, int dsize) {
  switch (bar) {
  case 0: // CBIO
  case 1: // CBMA
    return nic_read(address, dsize);
  }

  printf("21143: ReadMem_Bar: unsupported bar %d\n", bar);
  return 0;
}

void CTulip::WriteMem_Bar(int func, int bar, u32 address, int dsize,
                             u32 data) {
  switch (bar) {
  case 0: // CBIO
  case 1: // CBMA
    nic_write(address, dsize, (u32)endian_bits(data, dsize));
    return;
  }

  printf("21143: WriteMem_Bar: unsupported bar %d\n", bar);
}

/**
 * Check if threads are still running.
 **/
void CTulip::check_state() {
  if (myThreadDead.load())
    FAILURE(Thread, "NIC thread has died");
}


/**
 * Read from the NIC registers.
 **/
u32 CTulip::nic_read(u32 address, int dsize) {
  u32 data = 0;
  int regnr = (int)(address >> 3);

  // A read can move state too: CSR8 clears, CSR9 clocks the ROM.
  std::lock_guard<std::mutex> lock(m_lock);
  if ((address & 7) == 0 && regnr < 32) {
    data = state.reg[regnr];
    // The CSR8 counters clear when read; drivers accumulate them (Linux tulip
    // adds CSR8 & 0xffff to rx_missed_errors on every statistics read). The
    // top three bits keep their reset value.
    if (regnr == CSR_MISSED / 8)
      state.reg[regnr] &= 0xE0000000;
    // On the 21040 a read of CSR9 is not a register read at all: it is how
    // the address ROM is clocked out, a byte per read.
    if (regnr == CSR_MIIROM / 8 && m_chip.id_rom == TULIP_ID_ADDRESS_ROM)
      data = address_rom_read();
    // On the 21140 CSR12 is not the SIA status but eight pins.
    if (regnr == CSR_SIASTAT / 8 && m_chip.media == TULIP_MEDIA_GPR_21140)
      data = gpr_read();
  } else
    printf("dec21143: WARNING! unaligned access (0x%x) \n", (int)address);
#if defined(DEBUG_NIC)
  printf("21143: nic_read - CSR(%d), value: %08x\n", regnr, data);
#endif
  return data;
}

/**
 * Write to the NIC registers: the change under the lock, then the wake-up
 * of the NIC thread and the interrupt line outside it.
 **/
void CTulip::nic_write(u32 address, int dsize, u32 data) {
  bool wake;
  {
    std::lock_guard<std::mutex> lock(m_lock);
    wake = nic_write_locked(address, data);
  }
  if (wake)
    mySemaphore.set();
  update_irq();
}

/**
 * The register side of a CSR write, under m_lock. Returns whether the NIC
 * thread has something to look at: a poll demand, a start, a new filter.
 **/
bool CTulip::nic_write_locked(u32 address, u32 data) {
  uint32_t oldreg = 0;
  bool wake = false;

  int regnr = (int)(address >> 3);

#if defined(DEBUG_NIC)
  printf("21143: nic_write - CSR(%d), value: %08x\n", regnr, data);
#endif

  if ((address & 7) == 0 && regnr < 32) {
    oldreg = state.reg[regnr];
    switch (regnr) {
    case CSR_STATUS / 8: /*  CSR5: Write-1-to-clear */
      /* Bits 0 to 16, 26 and 27 clear when written with 1; the process
         states and the error bits are read only (HRM Table 3-70). */
      state.reg[regnr] &=
          ~((u32)data &
            (STATUS_TI | STATUS_TPS | STATUS_TU | STATUS_TJT | STATUS_LNPANC |
             STATUS_UNF | STATUS_RI | STATUS_RU | STATUS_RPS | STATUS_RWT |
             STATUS_ETI | STATUS_TM | STATUS_LNF | STATUS_SE | STATUS_ER |
             STATUS_AIS | STATUS_NIS | STATUS_GPPI | STATUS_LC));
#if defined(DEBUG_NIC_IRQ)
      printf("21143: CSR5 W1C=%08x -> CSR5=%08x CSR7=%08x\n", data,
             state.reg[CSR_STATUS / 8], state.reg[CSR_INTEN / 8]);
#endif
      break;

    case CSR_MISSED / 8: /*  Read only  */
      break;

    default:
      state.reg[regnr] = (u32)data;
    }
  } else
    printf("[ dec21143: WARNING! unaligned access (0x%x) ]\n", (int)address);

  switch (address) {
  case CSR_BUSMODE: /*  csr0  */
    if (data & BUSMODE_SWR) {
      reset_locked();
      wake = true; // the host filter follows the reset
      data &= ~BUSMODE_SWR;
    }

    // calculate descriptor skip length in bytes
    state.descr_skip = ((data & BUSMODE_DSL) >> 2) * 4;
    break;

  case CSR_TXPOLL: /*  csr1  */
    /* A poll demand resumes a suspended transmitter. TU is the driver's to
       clear (HRM Table 3-70), and a ring that is still empty sends the
       process back to suspended without raising it again (Table 3-46). */
    if (state.tx.suspend)
      m_tx_poll = true;
    state.tx.suspend = false;
    wake = true;
    break;

  case CSR_RXPOLL: /*  csr2  */
    m_rx_poll = true;
    wake = true;
    break;

  case CSR_RXLIST: /*  csr3  */
    /* debug("[ dec21143: setting RXLIST to 0x%x ]\n", (int)data); */
    if (data & 0x3)
      printf("[ dec21143: WARNING! RXLIST not aligned? (0x%x) ]\n", data);
    data &= ~0x3;
    state.rx.cur_addr = data;
    m_rx_epoch++;
    break;

  case CSR_TXLIST: /*  csr4  */
    /* debug("[ dec21143: setting TXLIST to 0x%x ]\n", (int)data); */
    if (data & 0x3)
      printf("[ dec21143: WARNING! TXLIST not aligned? (0x%x) ]\n", data);
    data &= ~0x3;
    state.tx.cur_addr = data;
    state.tx.suspend = false; // a new list is polled afresh at the start
    m_tx_epoch++;
    break;

  case CSR_STATUS: /* csr5: handled above; nic_write updates the line */
    break;

  case CSR_INTEN: /* csr7: interrupt mask, stored above */
#if defined(DEBUG_NIC_IRQ)
    printf("21143: CSR7 write %08x (mask)\n", state.reg[CSR_INTEN / 8]);
#endif
    break;

  case CSR_OPMODE: /*  csr6:  */
    /* MBO (bit 25) is hardware-driven on real silicon and always reads
     * back as 1, regardless of what the driver wrote (HRM 3.2.2.6,
     * Table 3-42). Force it set in the stored value so a write/read-back
     * verify by the driver (e.g. AlphaBIOS adapter self-test) succeeds
     * even when the write omitted MBO. */
    state.reg[regnr] |= 0x02000000;
    /* HP, HO and IF are read only: they report the filtering mode the last
       setup frame loaded (21143 HRM Table 3-71, CSR6 bits 0, 2, 4). */
    state.reg[regnr] = (state.reg[regnr] & ~OPMODE_FILTER_MODE) |
                       (oldreg & OPMODE_FILTER_MODE);
    if (data & 0x02000000) {

      /*  A must-be-one bit.  */
      data &= ~0x02000000;
    }

    if (data & OPMODE_ST) {

      // data &= ~OPMODE_ST;
    } else {

      /*  Turned off TX? Then idle:  */

      //      state.reg[CSR_STATUS/8] |= STATUS_TPS;
    }

    if (data & OPMODE_SR) {

      // data &= ~OPMODE_SR;
    } else {

      /*  Turned off RX? Then go to stopped state:  */

      // state.reg[CSR_STATUS/8] &= ~STATUS_RS;
    }

    // Did Start/Stop Transmission change state ?
    if ((data ^ oldreg) & OPMODE_ST) {
      if (data & OPMODE_ST) { // ST went high
        /* The process runs and fetches from the current position; an
           empty ring then suspends it with TU (HRM Table 3-71, ST, and
           Table 4-14). */
        set_tx_state(STATUS_TS_FETCH);
        state.tx.suspend = false;
        m_tx_poll = false;
        /* transmitter running -> clear 'process stopped' */
        state.reg[CSR_STATUS / 8] &= ~STATUS_TPS;
        wake = true;
      } else { // ST went low
        set_tx_state(STATUS_TS_STOPPED);
        /* transmitter stopped -> advertise idle */
        state.reg[CSR_STATUS / 8] |= STATUS_TPS;
      }
    }

    // Did Start/Stop Receive change state ?
    if ((data ^ oldreg) & OPMODE_SR) {
      if (data & OPMODE_SR) { // SR went high
        set_rx_state(STATUS_RS_WAIT);
        state.reg[CSR_STATUS / 8] &= ~STATUS_RPS;
        wake = true;
      } else { // SR went low
        /* BUGFIX: this must change the RX state, not TX */
        set_rx_state(STATUS_RS_STOPPED);
        state.reg[CSR_STATUS / 8] |= STATUS_RPS;
        /* The frames still in the FIFO go, and so does the rest of a frame
         * the ring had no room for: the next start begins with a new frame
         * at the retained position (HRM Table 4-13). */
        if (rx_queue) {
          rx_queue->flush();
        }
        state.rx.current.used = state.rx.current.len;
      }
    }

    /* If mode bits affecting reception/filtering changed, the NIC thread
       hands the back end a new host filter. */
    if ((state.reg[regnr] ^ oldreg) & (OPMODE_PR | OPMODE_PM | OPMODE_RA)) {
      m_filter_dirty = true;
      wake = true;
      if (trace_packets)
        printf("%s: CSR6 filtering:%s%s%s\n", devid_string,
               (state.reg[regnr] & OPMODE_PR) ? " promiscuous" : "",
               (state.reg[regnr] & OPMODE_PM) ? " all-multicast" : "",
               (state.reg[regnr] & OPMODE_RA) ? " receive-all" : "");
    }
    break;

  case CSR_MISSED: /*  csr8  */
    break;

  case CSR_MIIROM: /*  csr9  */
    /* On the 21040 CSR9 is the address ROM's port and nothing else: a write
       of any value rewinds it to its first byte, and the reads that follow
       walk through it. */
    if (m_chip.id_rom == TULIP_ID_ADDRESS_ROM) {
      state.srom.addr = 0;
      break;
    }
    if (data & MIIROM_MDC)
      mii_access(oldreg, (u32)data);
    else
      srom_access(oldreg, (u32)data);
    break;

  case CSR_SIASTAT: /*  csr12  */
    if (m_chip.media == TULIP_MEDIA_GPR_21140) {
      gpr_write((u32)data);
      state.reg[CSR_SIASTAT / 8] = oldreg;
    } else if (((data & SIASTAT_ANS) == SIASTAT_ANS_START) &&
               (state.reg[CSR_SIATXRX / 8] & SIATXRX_ANE)) {
      complete_sia_autoneg();
    } else {
      state.reg[CSR_SIASTAT / 8] = oldreg;
    }
    break;

  case CSR_SIATXRX: /*  csr14  */
    /* The 21040 has no autonegotiation to enable, and bit 7 of its CSR14
       means something else, so only the parts that have it look here. */
    if ((m_chip.media == TULIP_MEDIA_SIA_21041 ||
         m_chip.media == TULIP_MEDIA_SIA_21143) &&
        (data & SIATXRX_ANE) && (state.reg[CSR_SIACONN / 8] & SIACONN_SRL))
      complete_sia_autoneg();
    break;

  case CSR_SIACONN: /*  csr13  */
    /* A 21143 comes up through autonegotiation and nothing else. The older
       SIAs report a link as soon as the driver lets the port out of reset,
       whether or not it asked to negotiate. */
    if (m_chip.media == TULIP_MEDIA_SIA_21143) {
      if ((data & SIACONN_SRL) && (state.reg[CSR_SIATXRX / 8] & SIATXRX_ANE))
        complete_sia_autoneg();
    } else if (m_chip.media != TULIP_MEDIA_GPR_21140) {
      sia_connect((u32)data);
    }
    break;

  case CSR_SIAGEN: /*  csr15  */
    break;

  default:
    printf("[ dec21143: write to unimplemented 0x%02x: 0x%02x ]\n",
           (int)address, (int)data);
  }
  return wake;
}

void CTulip::set_tx_state(int tx_state) {
  state.reg[CSR_STATUS / 8] &= ~STATUS_TS;
  state.reg[CSR_STATUS / 8] |= (tx_state & STATUS_TS);
}

void CTulip::set_rx_state(int rx_state) {
  state.reg[CSR_STATUS / 8] &= ~STATUS_RS;
  state.reg[CSR_STATUS / 8] |= (rx_state & STATUS_RS);
}


/**
 * Reset the network interface internals to their condition immediately
 * after power-up, including the PCI configuration.
 **/
void CTulip::ResetPCI() {
  CPCIDevice::ResetPCI();

  // Do NOT call ResetNIC() here. PCI bus reset (pchip 0x800, fired by LFU)
  // must preserve the chip's internal CSRs to match qemu's tulip behavior.
  // SRM does a partial NIC config (CSR0/13/6/7) immediately before triggering
  // the LFU self-IPI and expects that config to survive the bus reset, so it
  // skips re-init on the LFU restart path. Internal reset still happens via:
  //   - explicit ResetNIC() in init() (one-shot at construction)
  //   - BUSMODE_SWR (CSR0=1), which calls ResetNIC() inline in nic_write()
}

/**
 * Reset the network interface internals to their condition immediately
 * after power-up. This does not affect the PCI configuration. For init(),
 * before the NIC thread exists; a software reset from CSR0 is
 * reset_locked() and the rest done by nic_write and the NIC thread.
 **/
void CTulip::ResetNIC() {
  {
    std::lock_guard<std::mutex> lock(m_lock);
    reset_locked();
  }
  apply_filter();
  update_irq();
}

/**
 * The reset itself, under m_lock. The host filter (m_filter_dirty) and the
 * interrupt line, which a reset drops, are left to the caller.
 *
 * Code for computing the SROM checksum is taken from [T64].
 **/
void CTulip::reset_locked() {
  // A descriptor the NIC thread is working on belongs to the old rings.
  m_rx_epoch++;
  m_tx_epoch++;

  // Drop any queued inbound frames; a real 21143 loses its RX FIFO on reset.
  if (rx_queue)
    rx_queue->flush();

  // Clear any previously programmed perfect-filter setup frame.
  memset(state.setup_filter, 0, sizeof(state.setup_filter));
  m_setup_loaded = false;

  // Reset derived/internal soft state that is not covered by the CSR array.
  state.descr_skip = 0;
  state.rx.current.len = 0;
  state.rx.current.used = 0;
  state.rx.cur_offset = 0;
  state.rx.cur_buf_len = 0;
  state.tx.cur_buf_len = 0;
  state.tx.suspend = false;
  m_tx_poll = false;
  m_rx_poll = false;

  // Drop any partially assembled RX buffer.
  if (state.rx.cur_buf != NULL)
    free(state.rx.cur_buf);

  //  if (state.tx.cur_buf != NULL)
  //        free(state.tx.cur_buf);
  state.rx.cur_buf = /*state.tx.cur_buf = */ NULL;

  memset(state.reg, 0, sizeof(uint32_t) * 32);

  // Reset the whole SROM/MII state machines (not just their data), and the
  // general purpose port, whose pins come out of reset as inputs.
  memset(&state.srom, 0, sizeof(state.srom));
  memset(&state.mii, 0, sizeof(state.mii));
  memset(&state.gpr, 0, sizeof(state.gpr));

  /*  Register values at reset, per HRM tables 3-27/41/47/49/51/57/59/61/63/66:
   */
  state.reg[CSR_BUSMODE / 8] = 0xFE000000; /* csr0  */
  state.reg[CSR_TXPOLL / 8] = 0xFFFFFFFF;  /* csr1  */
  state.reg[CSR_RXPOLL / 8] = 0xFFFFFFFF;  /* csr2  */
  state.reg[CSR_STATUS / 8] = 0xF0000000;  /* csr5  */
  state.reg[CSR_OPMODE / 8] = 0x32000040;  /* csr6  - includes MBO */
  state.reg[CSR_INTEN / 8] = 0xF3FE0000;   /* csr7  */
  state.reg[CSR_MISSED / 8] = 0xE0000000;  /* csr8  */
  state.reg[CSR_MIIROM / 8] = 0xFFF483FF;  /* csr9  */
  state.reg[CSR_GPT / 8] = 0xFFFE0000;     /* csr11 */
  /* csr12 to csr15 are where the parts stop resembling one another: what
     each of them holds at reset is in the part's own row. */
  state.reg[CSR_SIASTAT / 8] = m_chip.csr12_reset;
  state.reg[CSR_SIACONN / 8] = m_chip.csr13_reset;
  state.reg[CSR_SIATXRX / 8] = m_chip.csr14_reset;
  state.reg[CSR_SIAGEN / 8] = m_chip.csr15_reset;

  state.rx.cur_addr = state.tx.cur_addr = 0;

  build_srom();

  // Make sure the host-side capture filter matches the reset state (SRM relies
  // on this).
  m_filter_dirty = true;
}

static u32 nic_magic1 = 0xDEC21143;
static u32 nic_magic2 = 0x21143DEC;

/**
 * Save state to a Virtual Machine State file.
 **/
int CTulip::SaveState(FILE *f) {
  long ss = sizeof(state);
  int res;

  if ((res = CPCIDevice::SaveState(f)))
    return res;

  // The threads are stopped while the state is saved; the lock is only
  // there to say what it guards.
  std::lock_guard<std::mutex> lock(m_lock);

  // state carries two heap pointers (tx/rx cur_buf) that mean nothing
  // outside this process, so write them as null with the partial frame they
  // describe dropped. A restore then cannot install a foreign address, and
  // the file no longer leaks one.
  SNIC_state saved = state;
  saved.tx.cur_buf = nullptr;
  saved.tx.cur_buf_len = 0;
  saved.rx.cur_buf = nullptr;
  saved.rx.cur_buf_len = 0;

  fwrite(&nic_magic1, sizeof(u32), 1, f);
  fwrite(&ss, sizeof(long), 1, f);
  fwrite(&saved, sizeof(saved), 1, f);
  fwrite(&nic_magic2, sizeof(u32), 1, f);
  printf("%s: %li bytes saved.\n", devid_string, ss);
  return 0;
}

/**
 * Restore state from a Virtual Machine State file.
 **/
int CTulip::RestoreState(FILE *f) {
  long ss;
  u32 m1;
  u32 m2;
  int res;
  size_t r;

  if ((res = CPCIDevice::RestoreState(f)))
    return res;

  std::lock_guard<std::mutex> lock(m_lock);
  r = fread(&m1, sizeof(u32), 1, f);
  if (r != 1) {
    printf("%s: unexpected end of file!\n", devid_string);
    return -1;
  }

  if (m1 != nic_magic1) {
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

  // Keep this process's own buffers: tx.cur_buf is the lifetime allocation
  // made in init(), while the pointers in the file belong to whichever
  // process wrote it (files written before this fix hold a real address).
  // An in-flight partial frame is dropped rather than restored.
  unsigned char *tx_buf = state.tx.cur_buf;
  unsigned char *rx_buf = state.rx.cur_buf;

  r = fread(&state, sizeof(state), 1, f);
  if (r != 1) {
    printf("%s: unexpected end of file!\n", devid_string);
    return -1;
  }

  state.tx.cur_buf = tx_buf;
  state.tx.cur_buf_len = 0;
  state.rx.cur_buf = rx_buf;
  state.rx.cur_buf_len = 0;

  r = fread(&m2, sizeof(u32), 1, f);
  if (r != 1) {
    printf("%s: unexpected end of file!\n", devid_string);
    return -1;
  }

  if (m2 != nic_magic2) {
    printf("%s: MAGIC 2 does not match!\n", devid_string);
    return -1;
  }

  sanitize_state_locked();

  // Whatever the rings were doing belongs to the restored state now.
  m_rx_epoch++;
  m_tx_epoch++;
  m_filter_dirty = true;
  m_setup_loaded = (state.reg[CSR_OPMODE / 8] & OPMODE_HP) != 0;
  for (u8 b : state.setup_filter)
    m_setup_loaded |= b != 0;

  printf("%s: %li bytes restored.\n", devid_string, ss);
  return 0;
}

/**
 * Bring what a state file says back into range where it is used as an
 * index or a length: a damaged or foreign file must not reach outside the
 * arrays. Under m_lock.
 **/
void CTulip::sanitize_state_locked() {
  auto clamp = [](int &v, int lo, int hi) {
    if (v < lo)
      v = lo;
    if (v > hi)
      v = hi;
  };
  clamp(state.rx.current.len, 0, ETH_MAX_PACKET_CRC);
  clamp(state.rx.current.used, 0, state.rx.current.len);
  clamp(state.tx.cur_buf_len, 0, 2048);
  clamp(state.descr_skip, 0, 31 * 4); // CSR0 DSL: five bits of longwords
  clamp(state.srom.addr, 0, (int)sizeof(state.srom.data) - 1);
  clamp(state.srom.curbit, 0, 3 + 6 + 16);
  clamp(state.mii.phyaddr, 0, MII_NPHY - 1);
  clamp(state.mii.regaddr, 0, 31);
  clamp(state.mii.bit, 0, 31);
}

/**
 * Bring the interrupt line to the level CSR5 and CSR7 ask for, recomputing
 * the summary bits on the way (HRM 3.2.2.8: NIS and AIS are the OR of their
 * enabled groups; the line is their OR). Called after every CSR write and
 * at the end of every pass of the NIC thread.
 *
 * The line is driven outside m_lock, by one thread at a time: a thread that
 * finds another driving it returns, and the other computes the level again
 * once its call is done, so the change is not lost.
 **/
void CTulip::update_irq() {
  std::unique_lock<std::mutex> lock(m_lock);
  if (m_irq_busy)
    return;
  for (;;) {
    const u32 csr7 = state.reg[CSR_INTEN / 8];
    const u32 ie = state.reg[CSR_STATUS / 8] & csr7;
    u32 csr5 = state.reg[CSR_STATUS / 8] & ~(STATUS_NIS | STATUS_AIS);

    const u32 normal =
        (STATUS_TI | STATUS_TU | STATUS_RI | STATUS_TM | STATUS_ER);
    const u32 abnormal = (STATUS_LC | STATUS_GPPI | STATUS_SE | STATUS_LNF |
                          STATUS_ETI | STATUS_RWT | STATUS_RPS | STATUS_RU |
                          STATUS_UNF | STATUS_LNPANC | STATUS_TJT | STATUS_TPS);
    if (ie & normal)
      csr5 |= STATUS_NIS;
    if (ie & abnormal)
      csr5 |= STATUS_AIS;
    state.reg[CSR_STATUS / 8] = csr5;

    const bool asserted = (csr5 & csr7 & (STATUS_AIS | STATUS_NIS)) != 0;
    if (asserted == state.irq_was_asserted)
      return;
#if defined(DEBUG_NIC_IRQ)
    printf("21143: IRQ %s  CSR5=%08x CSR7=%08x pendN=%08x pendA=%08x\n",
           asserted ? "ASSERT" : "DEASSERT", csr5, csr7, (ie & normal),
           (ie & abnormal));
#endif
    m_irq_busy = true;
    lock.unlock();
    // false: the slot has no interrupt wiring, and there is no line to drive.
    (void)do_pci_interrupt(0, asserted);
    lock.lock();
    m_irq_busy = false;
    state.irq_was_asserted = asserted;
  }
}
