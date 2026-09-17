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
 * Intel 8255x family: construction, PCI configuration, the receive thread,
 * resets, EEPROM contents and the state file.
 **/
#include "I8255x.hpp"
#include "Configurator.hpp"
#include "NicAddress.hpp"
#include "StdAfx.hpp"
#include "System.hpp"

#include "I8255xRegs.hpp"

#include <chrono>

// Configure command defaults after reset (Intel's documented values, as
// every driver's template starts from).
static const u8 default_config[CFG_BYTES] = {
    0x16, 0x08, 0x00, 0x00, 0x00, 0x00, 0x32, 0x03, 0x01, 0x00, 0x2e,
    0x00, 0x60, 0x00, 0xf2, 0x48, 0x00, 0x40, 0xf2, 0x80, 0x3f, 0x05};

// EEPROM words
#define EE_ADDRESS 0    // words 0-2: station address, low byte first
#define EE_CONTROLLER 5 // bits 15-8: controller type (1 82557 ... 3 82559)
#define EE_PHY 6        // primary PHY: bits 13-8 type, 7-0 address
#define EE_ID 10        // bits 15-14 = 01: valid signature
#define EE_SUBSYS_ID 11
#define EE_SUBSYS_VENDOR 12
#define EE_WORDS 64
#define EE_CHECKSUM_SUM 0xbaba
#define PHY_TYPE_82555 7

CI8255x::CI8255x(CConfigurator *cfg, CSystem *c, int pcibus, int pcidev,
                 const i8255x_chip_config &chip)
    : CPCIDevice(cfg, c, pcibus, pcidev), m_chip(chip) {}

CI8255x::~CI8255x() {
  stop_threads();
  if (net_backend) {
    net_backend->close();
    delete net_backend;
  }
}

void CI8255x::init() {
  u32 cfg_data[64] = {};
  u32 cfg_mask[64] = {};

  cfg_data[0x00 >> 2] = u32(m_chip.pci_device_id) << 16 | PCI_VENDOR_INTEL;
  // Status: medium DEVSEL, fast back-to-back.
  cfg_data[0x04 >> 2] = 0x02800000;
  cfg_data[0x08 >> 2] = 0x02000000 | m_chip.pci_revision; // Ethernet
  cfg_data[0x10 >> 2] = 0x00000008; // CSRs, prefetchable memory
  cfg_data[0x14 >> 2] = 0x00000001; // CSRs, I/O
  cfg_data[0x18 >> 2] = 0x00000000; // flash, memory
  cfg_data[0x2c >> 2] = u32(m_chip.subsys_id) << 16 | m_chip.subsys_vendor_id;
  cfg_data[0x3c >> 2] = 0x180801ff; // MAX_LAT 0x18, MIN_GNT 0x08, INTA#
  cfg_mask[0x04 >> 2] = 0x00000157;
  cfg_mask[0x0c >> 2] = 0x0000ff00; // latency timer
  cfg_mask[0x10 >> 2] = 0xfffff000; // 4 KB
  cfg_mask[0x14 >> 2] = ~u32(CSR_SIZE - 1);
  cfg_mask[0x18 >> 2] = 0xfff00000; // 1 MB
  cfg_mask[0x3c >> 2] = 0x000000ff;
  if (m_chip.pm_capabilities) {
    cfg_data[0x04 >> 2] |= 0x00100000; // capability list
    cfg_data[0x34 >> 2] = 0xdc;
    cfg_data[0xdc >> 2] = u32(m_chip.pm_capabilities) << 16 | 0x0001;
    cfg_mask[0xe0 >> 2] = 0x00000103; // power state, PME enable
  }
  add_function(0, cfg_data, cfg_mask);

  net_backend = create_network_backend(myCfg);
  if (!net_backend)
    FAILURE(Runtime, "Failed to create network backend");
  if (!net_backend->init(devid_string, myCfg))
    FAILURE(Runtime, "Failed to initialize network backend");

  memset(&state, 0, sizeof(state));
  nic_station_address(myCfg, devid_string, state.station);
  state.eeprom.init(6); // 93C46
  build_eeprom();
  state.phy.reset();

  ResetPCI();

  printf("%s: Intel %s (%s)\n", devid_string, m_chip.part, m_chip.name);
}

/**
 * Fill the EEPROM the way the board ships: station address, PHY record,
 * subsystem IDs, and the checksum word that makes all 64 words sum to
 * 0xBABA.
 **/
void CI8255x::build_eeprom() {
  u16 *w = state.eeprom.data;
  for (int i = 0; i < CEeprom93cx6::MAX_WORDS; i++)
    w[i] = 0xffff;
  for (int i = 0; i < EE_WORDS - 1; i++)
    w[i] = 0;
  for (int i = 0; i < 3; i++)
    w[EE_ADDRESS + i] =
        u16(state.station[2 * i] | state.station[2 * i + 1] << 8);
  w[EE_CONTROLLER] = u16((m_chip.generation - 6) << 8);
  w[EE_PHY] = u16(PHY_TYPE_82555 << 8 | CI8255xPhy::ADDRESS);
  w[EE_ID] = 0x4000;
  w[EE_SUBSYS_ID] = m_chip.subsys_id;
  w[EE_SUBSYS_VENDOR] = m_chip.subsys_vendor_id;

  u16 sum = 0;
  for (int i = 0; i < EE_WORDS - 1; i++)
    sum += w[i];
  w[EE_WORDS - 1] = u16(EE_CHECKSUM_SUM - sum);
}

/**
 * Receive thread: bring host frames in and let a long command list run on.
 * Frames only arrive while the receive unit is ready; the others are
 * counted as resource errors and dropped, as the chip does.
 **/
void CI8255x::run() {
  try {
    std::unique_lock<std::mutex> lock(myLock);
    while (!StopThread) {
      const u8 *frame = nullptr;
      int len = 0;
      while (net_backend->receive(&frame, &len) > 0)
        receive_frame(frame, len);
      if (state.cu_pending)
        run_command_list();
      update_irq();
      myWake.wait_for(lock, std::chrono::milliseconds(10));
    }
  } catch (std::exception &e) {
    printf("Exception in %s thread: %s.\n", m_chip.name, e.what());
    myThreadDead.store(true);
  }
}

void CI8255x::start_threads() {
  if (!myThread) {
    printf(" %s", m_chip.name);
    {
      std::lock_guard<std::mutex> lock(myLock);
      StopThread = false;
    }
    myThread = std::make_unique<std::thread>([this]() { this->run(); });
  }
}

void CI8255x::stop_threads() {
  {
    std::lock_guard<std::mutex> lock(myLock);
    StopThread = true;
  }
  myWake.notify_all();
  if (myThread) {
    printf(" %s", m_chip.name);
    myThread->join();
    myThread = nullptr;
  }
}

void CI8255x::check_state() {
  if (myThreadDead.load())
    FAILURE_1(Thread, "%s thread has died", m_chip.name);
}

void CI8255x::ResetPCI() {
  CPCIDevice::ResetPCI();
  std::lock_guard<std::mutex> lock(myLock);
  software_reset();
}

/**
 * PORT software reset (and PCI reset): everything back to power-on,
 * interrupts unmasked. The EEPROM and the PHY keep their contents.
 **/
void CI8255x::software_reset() {
  memset(state.csr, 0, sizeof(state.csr));
  memcpy(state.config, default_config, sizeof(state.config));
  memset(state.ia, 0, sizeof(state.ia));
  memset(state.hash, 0, sizeof(state.hash));
  state.cu_base = 0;
  state.ru_base = 0;
  state.stats_addr = 0;
  memset(state.stats, 0, sizeof(state.stats));
  state.csr[CSR_MDI + 3] = MDI_READY >> 24;
  if (m_chip.generation >= 8)
    state.csr[CSR_GSTAT] = GSTAT_LINK | GSTAT_100 | GSTAT_FDX;
  state.csr[CSR_EEPROM] = EEPROM_DO;
  selective_reset();
  update_host_filter();
}

/**
 * PORT selective reset: command and receive units idle, nothing pending;
 * configuration, addresses and bases survive.
 **/
void CI8255x::selective_reset() {
  state.cu_offset = 0;
  state.ru_offset = 0;
  state.rbd_offset = 0;
  state.rbd_valid = false;
  state.cu_pending = false;
  state.csr[SCB_STATUS] = 0;
  state.csr[SCB_STATACK] = 0;
  state.csr[SCB_COMMAND] = 0;
  update_irq();
}

/**
 * Tell the backend which frames the guest wants, so a capture filter can
 * drop the rest early. The receive unit filters again either way.
 **/
void CI8255x::update_host_filter() {
  NetworkFilter f;
  memset(&f, 0, sizeof(f));
  memcpy(f.own_mac, state.station, 6);
  memcpy(f.mac_list[0], state.ia, 6);
  f.promiscuous = (state.config[15] & CFG15_PROMISC) != 0 ||
                  (state.config[20] & CFG20_MULTI_IA) != 0;
  bool any_hash = false;
  for (u8 b : state.hash)
    any_hash |= b != 0;
  f.pass_multicast = any_hash || (state.config[21] & CFG21_MC_ALL);
  net_backend->set_filter(f);
}

u8 CI8255x::dma_read8(u32 address) {
  u8 b;
  do_pci_read(address, &b, 1, 1);
  return b;
}

u16 CI8255x::dma_read16(u32 address) {
  u8 b[2];
  do_pci_read(address, b, 1, 2);
  return u16(b[0] | b[1] << 8);
}

u32 CI8255x::dma_read32(u32 address) {
  u8 b[4];
  do_pci_read(address, b, 1, 4);
  return u32(b[0]) | u32(b[1]) << 8 | u32(b[2]) << 16 | u32(b[3]) << 24;
}

void CI8255x::dma_write16(u32 address, u16 value) {
  u8 b[2] = {u8(value), u8(value >> 8)};
  do_pci_write(address, b, 1, 2);
}

void CI8255x::dma_write32(u32 address, u32 value) {
  u8 b[4] = {u8(value), u8(value >> 8), u8(value >> 16), u8(value >> 24)};
  do_pci_write(address, b, 1, 4);
}

static const u32 i8255x_magic1 = 0x82558A00;
static const u32 i8255x_magic2 = 0x0082558A;

int CI8255x::SaveState(FILE *f) {
  long ss = sizeof(state);
  int res;

  if ((res = CPCIDevice::SaveState(f)))
    return res;

  std::lock_guard<std::mutex> lock(myLock);
  fwrite(&i8255x_magic1, sizeof(u32), 1, f);
  fwrite(&ss, sizeof(long), 1, f);
  fwrite(&state, sizeof(state), 1, f);
  fwrite(&i8255x_magic2, sizeof(u32), 1, f);
  printf("%s: %d bytes saved.\n", devid_string, (int)ss);
  return 0;
}

int CI8255x::RestoreState(FILE *f) {
  long ss;
  u32 m1;
  u32 m2;
  int res;

  if ((res = CPCIDevice::RestoreState(f)))
    return res;

  if (fread(&m1, sizeof(u32), 1, f) != 1 || m1 != i8255x_magic1) {
    printf("%s: MAGIC 1 does not match!\n", devid_string);
    return -1;
  }
  if (fread(&ss, sizeof(long), 1, f) != 1 || ss != sizeof(state)) {
    printf("%s: STRUCT SIZE does not match!\n", devid_string);
    return -1;
  }
  std::lock_guard<std::mutex> lock(myLock);
  if (fread(&state, sizeof(state), 1, f) != 1) {
    printf("%s: unexpected end of file!\n", devid_string);
    return -1;
  }
  if (fread(&m2, sizeof(u32), 1, f) != 1 || m2 != i8255x_magic2) {
    printf("%s: MAGIC 2 does not match!\n", devid_string);
    return -1;
  }
  update_host_filter();
  printf("%s: %d bytes restored.\n", devid_string, (int)ss);
  return 0;
}
