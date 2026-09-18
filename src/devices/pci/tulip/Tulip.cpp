/* Alphabox Alpha Emulator
 * Copyright (C) 2020 Tomáš Glozar
 * Copyright (C) 2021 Dietmar M. Zettl
 * Website: https://github.com/lenticularis39/axpbox
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
 *
 * Parts of this file based upon GXemul, which is Copyright (C) 2004-2007
 * Anders Gavare.  All rights reserved.
 */

/**
 * \file
 * The Tulip Ethernet controllers: the device itself -- how it appears
 * on the PCI bus, its register file, reset, the state file, and the
 * thread that keeps frames moving.
 **/
#include "StdAfx.hpp"

#if defined(HAVE_PCAP) || defined(__linux__)
#include "Tulip.hpp"
#include "System.hpp"
#include "NicAddress.hpp"
#include <string.h>

//#define DEBUG_NIC_IRQ

/**
 * Thread entry point.
 **/
void CTulip::run() {
  try {
    for (;;) {
      if (StopThread)
        return;

      receive_process();

      bool asserted;

      if ((state.reg[CSR_OPMODE / 8] & OPMODE_ST))
        while (dec21143_tx())
          ;

      /* Normal and Abnormal interrupt summary (align with 21143/QEMU). */
      {
        u32 ie = state.reg[CSR_STATUS / 8] & state.reg[CSR_INTEN / 8];
        state.reg[CSR_STATUS / 8] &= ~(STATUS_NIS | STATUS_AIS);
        /* NIS if any normal event is enabled + set */
        if (ie & (STATUS_TI | STATUS_TU | STATUS_RI | STATUS_TM | STATUS_ER))
          state.reg[CSR_STATUS / 8] |= STATUS_NIS;
        /* AIS if any abnormal event is enabled + set */
        if (ie & (STATUS_LC | STATUS_GPPI | STATUS_SE | STATUS_LNF |
                  STATUS_ETI | STATUS_RWT | STATUS_RPS | STATUS_RU |
                  STATUS_UNF | STATUS_LNPANC | STATUS_TJT | STATUS_TPS))
          state.reg[CSR_STATUS / 8] |= STATUS_AIS;
        asserted = (state.reg[CSR_STATUS / 8] & state.reg[CSR_INTEN / 8] &
                    (STATUS_AIS | STATUS_NIS)) != 0;
      }

      if (asserted != state.irq_was_asserted) {
        if (do_pci_interrupt(0, asserted))
          state.irq_was_asserted = asserted;
      }

      mySemaphore.tryWait(10);
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
    : CPCIDevice(confg, c, pcibus, pcidev), m_chip(chip), mySemaphore(0, 1) {}

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
    mySemaphore.tryWait(0);
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

  u32 oldreg = 0;
  int regnr = (int)(address >> 3);

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
 * Write to the NIC registers.
 **/
void CTulip::nic_write(u32 address, int dsize, u32 data) {
  uint32_t oldreg = 0;

  int regnr = (int)(address >> 3);

#if defined(DEBUG_NIC)
  printf("21143: nic_write - CSR(%d), value: %08x\n", regnr, data);
#endif

  // printf("21143: rx_queue->name= %s\n", rx_queue->name);
  if ((address & 7) == 0 && regnr < 32) {
    oldreg = state.reg[regnr];
    switch (regnr) {
    case CSR_STATUS / 8: /*  CSR5: Write-1-to-clear */
      /* Clear all write-1-to-clear events like QEMU. */
      state.reg[regnr] &=
          ~((u32)data &
            (STATUS_TI | STATUS_TPS | STATUS_TU | STATUS_TJT | STATUS_LNPANC |
             STATUS_UNF | STATUS_RI | STATUS_RU | STATUS_RPS | STATUS_RWT |
             STATUS_ETI | STATUS_LNF | STATUS_SE | STATUS_ER | STATUS_AIS |
             STATUS_NIS | STATUS_GPPI | STATUS_LC));
#if defined(DEBUG_NIC_IRQ)
      printf("21143: CSR5 W1C=%08x -> CSR5=%08x CSR7=%08x\n", data,
             state.reg[CSR_STATUS / 8], state.reg[CSR_INTEN / 8]);
#endif
      /* Drop INTx immediately if that cleared the cause(s). */
      update_irq();
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
      ResetNIC();
      data &= ~BUSMODE_SWR;
    }

    // calculate descriptor skip length in bytes
    state.descr_skip = ((data & BUSMODE_DSL) >> 2) * 4;
    break;

  case CSR_TXPOLL: /*  csr1  */
    /* CaVa interpretation... */
    state.reg[CSR_STATUS / 8] &= ~STATUS_TU;
    state.tx.suspend = false;
    mySemaphore.tryWait(0);
    mySemaphore.set();
    break;

  case CSR_RXPOLL: /*  csr2  */
    mySemaphore.tryWait(0);
    mySemaphore.set();
    break;

  case CSR_RXLIST: /*  csr3  */
    /* debug("[ dec21143: setting RXLIST to 0x%x ]\n", (int)data); */
    if (data & 0x3)
      printf("[ dec21143: WARNING! RXLIST not aligned? (0x%x) ]\n", data);
    data &= ~0x3;
    state.rx.cur_addr = data;
    break;

  case CSR_TXLIST: /*  csr4  */
    /* debug("[ dec21143: setting TXLIST to 0x%x ]\n", (int)data); */
    if (data & 0x3)
      printf("[ dec21143: WARNING! TXLIST not aligned? (0x%x) ]\n", data);
    data &= ~0x3;
    state.tx.cur_addr = data;
    break;

  case CSR_STATUS: /* csr5: handled above; just ensure IRQ is updated */
    update_irq();
    break;

  case CSR_INTEN: /* csr7: interrupt mask */
    /* NOTE: state.reg[CSR_INTEN/8] was already written via default case above.
                     We just need to update the line level right now. */
#if defined(DEBUG_NIC_IRQ)
    printf("21143: CSR7 write %08x (mask)\n", state.reg[CSR_INTEN / 8]);
#endif
    update_irq();
    break;

  case CSR_OPMODE: /*  csr6:  */
    /* MBO (bit 25) is hardware-driven on real silicon and always reads
     * back as 1, regardless of what the driver wrote (HRM 3.2.2.6,
     * Table 3-42). Force it set in the stored value so a write/read-back
     * verify by the driver (e.g. AlphaBIOS adapter self-test) succeeds
     * even when the write omitted MBO. */
    state.reg[regnr] |= 0x02000000;
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
        set_tx_state(STATUS_TS_SUSPENDED);
        /* transmitter running -> clear 'process stopped' */
        state.reg[CSR_STATUS / 8] &= ~STATUS_TPS;
        mySemaphore.tryWait(0);
        mySemaphore.set();
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
        mySemaphore.tryWait(0);
        mySemaphore.set();
      } else { // SR went low
        /* BUGFIX: this must change the RX state, not TX */
        set_rx_state(STATUS_RS_STOPPED);
        state.reg[CSR_STATUS / 8] |= STATUS_RPS;
        /* When receiver is stopped, drop any queued frames to avoid growth
         * while SR=0. */
        if (rx_queue) {
          rx_queue->flush();
        }
      }
      /* Mode & status changed → recompute IRQ level now. */
      update_irq();
    }

    /* If mode bits affecting reception/filtering changed, rebuild BPF. */
    if ((data ^ oldreg) & (OPMODE_PR | OPMODE_IF | OPMODE_PM | OPMODE_RA)) {
      SetupFilter();
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
 * after power-up. This does not affect the PCI configuration.
 *
 * Code for computing the SROM checksum is taken from [T64].
 **/
void CTulip::ResetNIC() {
  int leaf;

  // Drop any queued inbound frames; a real 21143 loses its RX FIFO on reset.
  if (rx_queue)
    rx_queue->flush();

  // Clear any previously programmed perfect-filter setup frame.
  memset(state.setup_filter, 0, sizeof(state.setup_filter));

  // Reset derived/internal soft state that is not covered by the CSR array.
  state.descr_skip = 0;
  state.rx.current.len = 0;
  state.rx.current.used = 0;
  state.rx.cur_offset = 0;
  state.rx.cur_buf_len = 0;
  state.tx.cur_buf_len = 0;
  state.tx.suspend = false;

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
  SetupFilter();

  // Real reset deasserts the interrupt line.
  (void)do_pci_interrupt(0, false);
  state.irq_was_asserted = false;
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

  printf("%s: %li bytes restored.\n", devid_string, ss);
  return 0;
}

/* ----- IRQ recompute: set/clear INTx instantly (level-triggered) ----- */
void CTulip::update_irq() {
  /* Rebuild NIS/AIS like the background loop does, but do it now. */
  const u32 csr5_before = state.reg[CSR_STATUS / 8];
  const u32 csr7 = state.reg[CSR_INTEN / 8];

  /* Summary recompute follows QEMU logic: only enabled events contribute. */
  u32 ie = csr5_before & csr7;
  u32 csr5 = csr5_before & ~(STATUS_NIS | STATUS_AIS);

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
  if (asserted != state.irq_was_asserted) {
#if defined(DEBUG_NIC_IRQ)
    printf("21143: IRQ %s  CSR5=%08x CSR7=%08x pendN=%08x pendA=%08x\n",
           asserted ? "ASSERT" : "DEASSERT", csr5, csr7, (ie & normal),
           (ie & abnormal));
#endif
    if (do_pci_interrupt(0, asserted))
      state.irq_was_asserted = asserted;
  }
}

#endif // defined(HAVE_PCAP) || defined(__linux__)
