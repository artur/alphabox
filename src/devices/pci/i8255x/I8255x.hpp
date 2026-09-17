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

/* Intel 8255x 10/100 Ethernet controller family (82557, 82558, 82559), the
 * chip on the DE600 and DE602 and on Intel's PRO/100 boards.
 *
 * One controller model for every part; what differs (PCI identity,
 * extended TCBs and counters, the 82558+ registers) is data in an
 * i8255x_chip_config row (I8255xChips.cpp). Split by concern:
 *
 *   I8255x.cpp           construction, PCI header, the receive thread,
 *                        resets, EEPROM contents, state file
 *   I8255xRegisters.cpp  the CSRs: SCB, PORT, EEPROM and MDI control,
 *                        and the interrupt line
 *   I8255xCommands.cpp   the command unit: action commands, transmit,
 *                        statistical counters
 *   I8255xReceive.cpp    the receive unit: address filtering, RFDs, RBDs
 *   I8255xPhy.cpp        the 82555 PHY behind the MDI
 *   Eeprom93cx6.cpp      the serial EEPROM
 *   I8255xRegs.hpp       register and descriptor definitions
 *   I8255xChips.cpp      the parts
 *
 * Documentation consulted:
 *  - Intel 8255x 10/100 Mbps Ethernet Controller Family Open Source
 *    Software Developer Manual
 *  - QEMU hw/net/eepro100.c, Linux drivers/net/ethernet/intel/e100.c,
 *    NetBSD sys/dev/ic/i82557.c
 *  - the ES40 SRM console: its "ei" driver and PCI table (device 0x1229,
 *    Compaq subsystem 0xb144 = DE600-AA, 0xb0dd = DE602-AA)
 */
#if !defined(INCLUDED_I8255X_H_)
#define INCLUDED_I8255X_H_

#include "Eeprom93cx6.hpp"
#include "Ethernet.hpp"
#include "I8255xPhy.hpp"
#include "NetworkBackend.hpp"
#include "PCIDevice.hpp"

#include <condition_variable>
#include <mutex>

/**
 * \brief What distinguishes one 8255x part (and board) from another.
 **/
struct i8255x_chip_config {
  const char *name;     ///< config class and message name, e.g. "de600"
  const char *part;     ///< the controller, e.g. "82558"
  u16 pci_device_id;    ///< PCI config 0x02
  u8 pci_revision;      ///< PCI config 0x08: 82557 1-3, 82558 4-5, 82559 8
  u16 subsys_vendor_id; ///< PCI config 0x2c (0: none, as on early 82557s)
  u16 subsys_id;        ///< PCI config 0x2e
  u8 generation;        ///< 7 = 82557, 8 = 82558, 9 = 82559
  u16 pm_capabilities;  ///< PCI power management PMC (0: no capability)
};

/**
 * \brief Emulated Intel 8255x Ethernet controller.
 **/
class CI8255x : public CPCIDevice {
public:
  CI8255x(CConfigurator *cfg, class CSystem *c, int pcibus, int pcidev,
          const i8255x_chip_config &chip);
  virtual ~CI8255x();

  virtual void init();
  virtual void start_threads();
  virtual void stop_threads();
  virtual void check_state();
  virtual void ResetPCI();
  virtual int SaveState(FILE *f);
  virtual int RestoreState(FILE *f);

  virtual u32 ReadMem_Bar(int func, int bar, u32 address, int dsize);
  virtual void WriteMem_Bar(int func, int bar, u32 address, int dsize,
                            u32 data);

  /// The part named `name` ("de600", "82558", ...), or nullptr.
  static const i8255x_chip_config *find_chip(const char *name);

private:
  const i8255x_chip_config m_chip;

  // I8255x.cpp
  void run();
  void software_reset();
  void selective_reset();
  void build_eeprom();
  void update_host_filter();

  // I8255xRegisters.cpp
  u8 read_csr(u32 offset);
  void write_csr(u32 offset, u8 value);
  u32 csr_long(u32 offset) const;
  void csr_written(u32 first, u32 last);
  void scb_command(u8 value);
  void port_command();
  void eeprom_pins();
  void mdi_command();
  void raise(u8 causes);
  void update_irq();
  void set_cu_state(int s);
  void set_ru_state(int s);
  int cu_state() const;
  int ru_state() const;

  // I8255xCommands.cpp
  void cu_start(u32 offset);
  void cu_resume();
  void run_command_list();
  bool action_command(u32 cb);
  void transmit(u32 cb, u16 command);
  void setup_multicast(u32 cb);
  void dump_statistics(bool reset);
  int statistics_size() const;
  bool extended_tcb() const;

  // I8255xReceive.cpp
  void ru_command(u8 command, u32 pointer);
  bool accept_frame(const u8 *frame, int len, u16 *status);
  void receive_frame(const u8 *frame, int len);
  int store_frame_rbds(const u8 *data, int len, u32 *first_rbd);
  static int hash_index(const u8 *address);

  // DMA helpers (little-endian guest structures)
  u8 dma_read8(u32 address);
  u16 dma_read16(u32 address);
  u32 dma_read32(u32 address);
  void dma_write16(u32 address, u16 value);
  void dma_write32(u32 address, u32 value);

  CNetworkBackend *net_backend = nullptr;

  std::unique_ptr<std::thread> myThread;
  std::atomic_bool myThreadDead{false};
  /// Serializes the CSRs, the command and receive units, and the backend.
  std::mutex myLock;
  std::condition_variable myWake;
  bool StopThread = false; ///< guarded by myLock

  /// Everything the state file keeps.
  struct SI8255x_state {
    u8 csr[32];    ///< CSR bytes (CSR_SIZE) as last written or updated
    u8 config[22]; ///< configure command bytes
    u8 ia[6];      ///< individual address (IA setup)
    u8 hash[8];    ///< multicast / multiple-IA hash filter
    u32 cu_base;
    u32 cu_offset; ///< next command block, relative to cu_base
    u32 ru_base;
    u32 ru_offset; ///< next RFD, relative to ru_base
    u32 rbd_offset;
    bool rbd_valid; ///< rbd_offset names the next free RBD
    u32 stats_addr;
    u32 stats[20];   ///< counters (STATS_WORDS), in dump order
    bool cu_pending; ///< command list continues on the thread
    bool irq_asserted;
    CEeprom93cx6 eeprom;
    CI8255xPhy phy;
    u8 station[6]; ///< the board's address, as in the EEPROM
  } state;
};

#endif // !defined(INCLUDED_I8255X_H_)
