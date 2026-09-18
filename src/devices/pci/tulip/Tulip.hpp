/* Alphabox Alpha Emulator
 * Copyright (C) 2020 Tomáš Glozar
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
 * Contains the definitions for the emulated DEC Tulip NIC devices.
 **/
#if !defined(INCLUDED_TULIP_H_)
#define INCLUDED_TULIP_H_

#if defined(DEBUG_NIC)
#define DEBUG_NIC_FILTER
#define DEBUG_NIC_SROM
#endif

#include "TulipMii.hpp"
#include "TulipRegs.hpp"
#include "Ethernet.hpp"
#include "NetworkBackend.hpp"
#include "PCIDevice.hpp"
#include "base/Semaphore.hpp"

/**
 * \brief How a part hands the driver the station address.
 **/
enum tulip_id_rom {
  /** The 21040 has no serial ROM. Its station address sits in a plain
      parallel ROM that the chip reads for the driver: a write to CSR9
      rewinds it, and each read hands back the next byte, with bit 31 set
      while the byte is not ready yet. */
  TULIP_ID_ADDRESS_ROM,
  /** Everything after the 21040 bit-bangs a Microwire serial ROM through
      the clock, chip-select and data pins of CSR9. */
  TULIP_ID_SERIAL_ROM,
};

/**
 * \brief Which serial ROM layout the part's drivers expect.
 **/
enum tulip_srom_form {
  TULIP_SROM_NONE,  ///< 21040: no serial ROM at all
  TULIP_SROM_21041, ///< the old format: station address, no info leaf
  TULIP_SROM_21140, ///< an info leaf of 21140 GPR/MII media blocks
  TULIP_SROM_21143, ///< an info leaf of extended SIA/SYM/MII blocks
};

/**
 * \brief What the part has between the CSRs and the wire.
 **/
enum tulip_media {
  /** 21040: the original 10 Mb SIA. No autonegotiation; CSR12 reports the
      link, CSR13/14/15 set the port up. */
  TULIP_MEDIA_SIA_21040,
  /** 21041: a 10 Mb SIA that can autonegotiate, reporting the result in
      CSR12's autonegotiation state field. */
  TULIP_MEDIA_SIA_21041,
  /** 21140: no SIA. CSR12 is the general purpose port, eight pins the
      board wires to whatever it uses to pick a medium. */
  TULIP_MEDIA_GPR_21140,
  /** 21143: SIA plus the internal 100 Mb symbol path, and the CSR12
      autonegotiation the 21143 code already implements. */
  TULIP_MEDIA_SIA_21143,
};

/**
 * \brief What distinguishes one Tulip from another.
 *
 * What the parts have in common is the whole data path: the same
 * descriptor rings, the same setup frame, the same interrupt summary. What
 * they do not share is how a driver learns which board it is looking at
 * and how it picks a medium -- which is why those, and almost nothing
 * else, are what a row here carries.
 **/
struct tulip_chip_config {
  const char *name;     ///< configuration class, e.g. "dec21140"
  const char *part;     ///< the part, for messages
  u16 device_id;        ///< PCI config 0x02
  u8 revision;          ///< PCI config 0x08
  u16 subsys_vendor;    ///< PCI config 0x2c, 0 if the part has no subsystem
  u16 subsys_id;        ///< PCI config 0x2e
  tulip_id_rom id_rom;  ///< where the station address comes from
  tulip_srom_form srom; ///< what that ROM looks like
  tulip_media media;    ///< what drives the wire
  u32 csr12_reset;      ///< CSR12 after reset (SIA status, or the GPR)
  u32 csr13_reset;      ///< CSR13 after reset
  u32 csr14_reset;      ///< CSR14 after reset
  u32 csr15_reset;      ///< CSR15 after reset
};

/// Fill in the PCI configuration header for this part (TulipChips.cpp).
void tulip_config_space(const tulip_chip_config &chip, u32 *data, u32 *mask);

/**
 * \brief Emulated DEC Tulip NIC device (21040, 21041, 21140, 21143).
 *
 * Documentation consulted:
 *  - 21143 PCI/Cardbus 10/100Mb/s Ethernet LAN Controller Hardware Reference
 *Manual  [HRM]. (http://download.intel.com/design/network/manuals/27807401.pdf)
 *  - 21040, 21041 and 21140A hardware reference manuals, for the parts that
 *    came before it.
 *  - "DECchip 21X4 Serial ROM Format" (Digital, version 4.05) [SROM].
 *  - Tru64 Device Driver Kit Version 2 (Ethernet sample = tu driver!) [T64].
 *(http://h30097.www3.hp.com/docs/dev_doc/DOCUMENTATION/HTML/dev_docs_r2.html)
 *  .
 **/
class CTulip : public CPCIDevice {
public:
  virtual int SaveState(FILE *f);
  virtual int RestoreState(FILE *f);
  void instant_tick();

  //    void interrupt(int number);
  virtual void check_state();
  virtual void WriteMem_Bar(int func, int bar, u32 address, int dsize,
                            u32 data);
  virtual u32 ReadMem_Bar(int func, int bar, u32 address, int dsize);

  CTulip(CConfigurator *confg, class CSystem *c, int pcibus, int pcidev,
         const tulip_chip_config &chip);
  virtual ~CTulip();

  /// The part named `name` ("dec21040" ... "dec21143"), or nullptr.
  static const tulip_chip_config *find_chip(const char *name);

  virtual void ResetPCI();
  void ResetNIC();
  void SetupFilter();
  void receive_process();
  virtual void run();
  virtual void init();
  virtual void start_threads();
  virtual void stop_threads();
  void update_irq();

private:
  const tulip_chip_config m_chip;
  u32 cfg_data[64] = {};
  u32 cfg_mask[64] = {};

  std::unique_ptr<std::thread> myThread;
  std::atomic_bool myThreadDead{false};
  bool StopThread;
  CSemaphore mySemaphore;
  /** serializes NIC thread vs CPU-thread CSR access (recursive: nic_read /
   * nic_write paths may re-enter through the interrupt plumbing) */

  u32 nic_read(u32 address, int dsize);
  void nic_write(u32 address, int dsize, u32 data);
  void mii_access(uint32_t oldreg, uint32_t idata);
  void build_srom();
  void build_address_rom();
  void srom_access(uint32_t oldreg, uint32_t idata);
  u32 address_rom_read();
  u32 gpr_read();
  void gpr_write(u32 data);
  void sia_connect(u32 csr13);
  void complete_sia_autoneg();
  void trace_packet(const char *dir, const u8 *frame, int len);

  int dec21143_rx();
  int dec21143_tx();
  void set_tx_state(int tx_state);
  void set_rx_state(int rx_state);

  /// A descriptor word the other way round, for CSR0's DBO.
  static u32 bswap32_local(u32 v) {
    return ((v & 0x000000ffU) << 24) | ((v & 0x0000ff00U) << 8) |
           ((v & 0x00ff0000U) >> 8) | ((v & 0xff000000U) >> 24);
  }

  CPacketQueue *rx_queue;
  CNetworkBackend *net_backend;
  bool calc_crc;
  bool trace_packets;

  /// The state structure contains all elements that need to be saved to the
  /// statefile.
  struct SNIC_state {
    bool irq_was_asserted; /**< remember state of IRQ */

    u8 mac[6];            /**< ethernet address */
    u8 setup_filter[192]; /**< filter for perfect filtering */
    int descr_skip;       // Descriptor Skip Length [DSL] (in bytes)

    /// SROM emulation. The 21040 has no serial ROM, but its address ROM is
    /// the same 128 bytes seen through a different door, so it lives here
    /// too, with `addr` as the byte the next read hands over.
    struct SNIC_srom {
      u8 data[1 << (7)];
      int curbit;
      int opcode;
      int opcode_has_started;
      int addr;
    } srom;

    /// The 21140's general purpose port (CSR12): eight pins, each of which
    /// the chip either drives or reads.
    struct SNIC_gpr {
      u8 dir; ///< 1 = the chip drives the pin
      u8 out; ///< what it drives there
    } gpr;

    /// MII PHY emulation
    struct SNIC_mii {
      u16 phy_reg[MII_NPHY * 32];
      int state;
      int bit;
      int opcode;
      int phyaddr;
      int regaddr;
    } mii;

    u32 reg[32]; /**< 21143 registers */

    /// Internal TX state
    struct SNIC_tx {
      u32 cur_addr;
      unsigned char *cur_buf;
      int cur_buf_len;
      bool suspend;
    } tx;

    /// Internal RX state
    struct SNIC_rx {
      u32 cur_addr;
      unsigned char *cur_buf;
      int cur_buf_len;
      int cur_offset;
      eth_packet current;
    } rx;
  } state;
};
#endif // !defined(INCLUDED_TULIP_H_)
