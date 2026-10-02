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
 */

/**
 * \file
 * The 21272 Tsunami/Typhoon chipset, as the ES40, DS20E, DS10 and DS20L
 * have it. One class, split over files by chip:
 *
 *  - Tsunami.cpp: the physical address decode, interrupts, reset and state;
 *  - TsunamiCchip.cpp: the Cchip's CSRs (MISC, DIMn/DIRn/DRIR, AARn, MPD)
 *    and the Dchips';
 *  - TsunamiPchip.cpp: the Pchips' CSRs and DMA address translation;
 *  - TsunamiTig.cpp: the TIG bus registers (halt lines, the PALcode's MP
 *    handshake, the system reset request);
 *  - TsunamiMemory.cpp: the memory arrays the Cchip reports (AARn) and the
 *    serial-presence-detect EEPROMs on its MPD I2C pins.
 *
 * Documentation consulted:
 *  - Tsunami/Typhoon 21272 Chipset Hardware Reference Manual  [HRM]
 *(http://download.majix.org/dec/tsunami_typhoon_21272_hrm.pdf)
 *  - AlphaServer ES40 and AlphaStation ES40 Service Guide [SG]
 *(http://www.dec-store.com/PD_00158.aspx)
 *  - Tru64 include file dc104x.h [T64]
 *(http://samy.pl/packet/MISC/tru64/usr/include/alpha/dc104x.h)
 *  .
 *
 * The configuration modelled, whatever the board:
 *   - 1 x 21274-C1 Cchip (controller chip) - The Cchip controls the other chips
 *in the chipset, as well as the DRAM memory array in a system. The Cchip
 *interfaces with the CPU's command and address buses.
 *   - 8 x 21274-D1 Dchip (data slice chip) - The Dchips interface with the
 *system data bus and provide the data path between the CPU, DRAM memory, and
 *the Pchip(s).
 *   - 2 x 21272-P1 Pchip (peripheral interface chip) - The interface to the PCI
 *bus. Both answer on every board, the one-hose DS10 included, because that
 *is what the emulator always did and its console has never been watched
 *without the second.
 *   .
 **/
#if !defined(INCLUDED_TSUNAMI_H_)
#define INCLUDED_TSUNAMI_H_

#include "Chipset.hpp"
#include "DimmModel.hpp"
#include "i2c_spd.hpp"
#include <mutex>
#include <vector>

class CTsunami : public CChipset {
public:
  explicit CTsunami(CSystem *sys);
  ~CTsunami() override = default;

  chipset_kind kind() const override { return CHIPSET_TSUNAMI; }
  const char *name() const override { return "Tsunami/Typhoon 21272"; }

  u64 phys_mask() const override { return U64(0x00000807ffffffff); }
  u64 read_io(u64 a, u64 raw, int dsize, CSystemComponent *source) override;
  void write_io(u64 a, u64 raw, int dsize, u64 data,
                CSystemComponent *source) override;

  void interrupt(int number, bool assert) override;
  void interval_tick() override { interrupt(-1, true); }
  void ack_interval_timer(int cpu) override;
  void ack_ipi(int cpu) override;

  u64 pci_space_base(int hose, pci_space space) const override;
  u64 pci_phys(int hose, u32 address) override;

  void reset() override;
  void save_state(FILE *f) override;
  bool restore_state(FILE *f) override;

  // --- For the ES40 console's native PALcode (cpu/AlphaCPU_vmspal.cpp) ----
  // It replaces PALcode that reads these Cchip registers.
  u64 misc() const { return state.cchip.misc; }
  u64 dir(int cpu) const { return state.cchip.drir & state.cchip.dim[cpu]; }
  u64 dim(int cpu) const { return state.cchip.dim[cpu]; }
  /// Write a processor's device interrupt mask the way the Cchip register
  /// does: under the lock that serialises with interrupt(), and re-driving
  /// that processor's lines afterwards. The bare assignment this used to be
  /// raced with interrupt() and left a masked line asserted until some
  /// unrelated event happened to re-drive it. Only the native PALcode
  /// reaches it; the CSR path always did this.
  void set_dim(int cpu, u64 value);

  // --- The memory the Cchip reports (TsunamiMemory.cpp) -------------------
  const dimm_population *dimms() const override { return &m_dimms; }

private:
  u64 cchip_csr_read(u32 address, CSystemComponent *source);
  void cchip_csr_write(u32 address, u64 data, CSystemComponent *source);
  u64 pchip_csr_read(int num, u32 address);
  void pchip_csr_write(int num, u32 address, u64 data);
  u8 dchip_csr_read(u32 address);
  void dchip_csr_write(u32 address, u8 data);
  u8 tig_read(u32 address);
  void tig_write(u32 address, u8 data);
  void tig_update_halt_lines();
  u64 pci_phys_direct_mapped(u32 address, u64 wsm, u64 tba);
  u64 pci_phys_scatter_gather(u32 address, u64 wsm, u64 tba);
  void power_on_state();

  /// Build SPD images that match the configured memory.
  void init_spd(uint32_t total_mb);
  dimm_population m_dimms;

  /// Host-side open-drain drivers for the Cchip's MPD I2C pins.
  struct MPDState {
    // Host open-drain drivers (1 = released high, 0 = pulling low)
    bool cks_out = true; // SCL
    bool ds_out = true;  // SDA
  } m_mpd;
  I2CBus m_mpd_bus;

  // Serializes DRIR read-modify-write and line delivery in interrupt()
  // across device threads, and every other change to MISC, DIMn and the TIG
  // halt lines that re-drives a processor's interrupt pins. Not part of the
  // saved state.
  std::mutex m_lock;

public:
  /**
   * The chipset's saved state, laid out exactly as it was inside CSystem's
   * state structure, which follows it in the state file: the file format is
   * unchanged by the move (a 40-byte system part, then this).
   **/
  struct SState {
    /**
     * TIGbus state data
     *
     * More details in: HRM, 6.3; T64. Detailed information is hard to find...
     *
     * The TIGbus (TTL Integrated Glue Logic) is the interface between the
     *chipset and the interrupt controller, flash ROM, and possibly some other
     *system components.
     **/
    struct SSys_tig {
      u8 FwWrite;
      u8 HaltA;
      u8 HaltB;
      u8 ModInfo;
      u8 ipcr[5]; ///< ipcr0-4 (0xa00-0xb00): PALcode MP restart handshake
    } tig;

    /**
     * CCHIP state data
     *
     * More details in: HRM, 1.2.1.
     *
     * The 21274-C1 Cchip (controller chip) is the heart of the ES40's Typhoon
     *chipset. It interfaces directly with the CPU's through the System address
     *ports, it issues controls to the Dchips (data slice chips) and Pchips
     *(peripheral interface chips) using the Dchip control ports, and the CAPbus
     *(C-And-P-chip bus). It controls memory using the DRAM command and address
     *ports. It also controls the TIGbus.
     **/
    struct SSys_cchip {

      /**
       * DIM: Device Interrupt Mask Registers.
       *
       * These mask registers control which interrupts are allowed to go through
       *to the CPUs. No interrupt in DRIR will get through to the masked
       *interrupt registers (and on to interrupt the CPUs) unless the
       *corresponding mask bit is set in DIMn. All bits are initialized to 0 at
       *reset.
       **/
      u64 dim[4];

      /**
       * DRIR: Device Raw Interrupt Request Register.
       *
       * DRIR indicates which of the 64 possible device interrupts is asserted.
       *
       * \code
       * +---------+---------+---------+------+-------------------------------------+
       * | Field   | Bits    | Type    | Init | Description |
       * +---------+---------+---------+------+-------------------------------------+
       * | ERR     | <63:58> | RO      | 0    | IRQ0 error interrupts | | | | |
       *|    <63> Chip detected MISC<NXM>     | |         |         |         |
       *|    <62> hookup to Pchip0 error      | |         |         |         |
       *|    <61> hookup to Pchip1 errror     |
       * +---------+---------+---------+------+-------------------------------------+
       * | RES     | <57:56> | RO      | 0    | Reserved |
       * +---------+---------+---------+------+-------------------------------------+
       * | DEV     | <55:0>  | RO      | 0    | PCI interrupts pending to the
       *CPU   |
       * +---------+---------+---------+------+-------------------------------------+
       * \endcode
       *
       * Combined with DIM[n] to form DIR[n]:
       *
       * DIR: Device Interrupt Request Registers.
       *
       * These registers indicate which interrupts are pending to the CPUs. If a
       *raw request bit is set and the corresponding mask bit is set, then the
       *corresponding bit in this register will be set and the appropriate CPU
       *will be interrupted.
       **/
      u64 drir;

      /**
       * Miscellaneous Register (MISC - RW).
       *
       * +---------+---------+---------+------+-------------------------------------+
       * | Field   | Bits    | Type    | Init | Description |
       * +---------+---------+---------+------+-------------------------------------+
       * | RES     | <63:44> | MBZ,RAZ | 0    | Reserved. | | DEVSUP  | <43:40>
       *| WO      | 0    | Suppress IRQ1 interrupts to the CPU | |         | |
       *|      | corresponding to a 1 in this field  | |         |         | |
       *| until the interrupt polling machine | |         |         |         |
       *| has completed a poll of all PCI     | |         |         |         |
       *| devices.                            |
       * +---------+---------+---------+------+-------------------------------------+
       * | REV     | <39:32> | RO      | 8    | Latest revision of Cchip |
       * +---------+---------+---------+------+-------------------------------------+
       * | NXS     | <31:29> | RO      | 0    | NXM source - Device that caused
       *NXM | |         |         |         |      | - UNPREDICTABLE if NXM is
       *not set.  | |         |         |         |      |   Value Source | | |
       *|         |      |   0..3  CPU 0..3                    | |         | |
       *|      |   4..5  Pchip 0..1                  |
       * +---------+---------+---------+------+-------------------------------------+
       * | NXM     | <28>    | R,W1C   | 0    | Nonexistent memory address
       *detected.| |         |         |         |      | Sets DRIR<63> and
       *locks the NXS     | |         |         |         |      | field until
       *it is cleared.          |
       * +---------+---------+---------+------+-------------------------------------+
       * | RES     | <27:25> | MBZ,RAZ | 0    | Reserved. |
       * +---------+---------+---------+------+-------------------------------------+
       * | ACL     | <24>    | WO      | 0    | Arbitration clear - writing a 1
       *to  | |         |         |         |      | this bit clears ABT and ABW
       *fields. |
       * +---------+---------+---------+------+-------------------------------------+
       * | ABT     | <23:20> | R,W1S   | 0    | Arbitration try - writing a 1 to
       *| |         |         |         |      | these bits sets them. |
       * +---------+---------+---------+------+-------------------------------------+
       * | ABW     | <19:16> | R,W1S   | 0    | Arbitration won - writing a 1 to
       *| |         |         |         |      | these bits sets them unless one
       *is  | |         |         |         |      | already set, in which case
       *the      | |         |         |         |      | write is ignored. |
       * +---------+---------+---------+------+-------------------------------------+
       * | IPREQ   | <15:12> | WO      | 0    | Interprocessor interrupt request
       *-  | |         |         |         |      | write a 1 to the bit
       *corresponding  | |         |         |         |      | to the CPU you
       *want to interrupt.   | |         |         |         |      | Writing a
       *1 here sets the corres-   | |         |         |         |      |
       *ponding bit in IPINTR.              |
       * +---------+---------+---------+------+-------------------------------------+
       * | IPINTR  | <11:8>  | R,W1C   | 0    | Interprocessor interrupt pending
       *-  | |         |         |         |      | one bit per CPU. Pin irq<3>
       *is      | |         |         |         |      | asserted to the CPU
       *corresponding   | |         |         |         |      | to a 1 in this
       *field.               |
       * +---------+---------+---------+------+-------------------------------------+
       * | ITINTR  | <7:4>   | R,W1C   | 0    | Interval timer interrupt pending
       *-  | |         |         |         |      | one bit per CPU. Pin irq<2>
       *is      | |         |         |         |      | asserted to the CPU
       *corresponding   | |         |         |         |      | to a 1 in this
       *field.               |
       * +---------+---------+---------+------+-------------------------------------+
       * | RES     | <3:2>   | MBZ,RAZ | 0    | Reserved. |
       * +---------+---------+---------+------+-------------------------------------+
       * | CPUID   | <1:0>   | RO      |      | ID of the CPU performing the
       *read.  |
       * +---------+---------+---------+------+-------------------------------------+
       * \endcode
       **/
      u64 misc;
      u64 csc;
    } cchip;

    /**
     * DCHIP state data
     *
     * More details in: HRM, 1.2.2.
     *
     * The ES40 contains eight 21274-D1 Dchips (data slice chips). Each Dchip is
     * responsible for handling 8 bits of the 64-bit data bus (in the ES40,
     *other configurations using less Dchips are possible). Each Dchip
     *interfaces with the Cchip for control, with each of the Pchips, with each
     *of the CPU's and with each of the DRAM arrays.
     **/
    struct SSys_dchip {
      u8 drev;
      u8 dsc;
      u8 dsc2;
      u8 str;
    } dchip;

    /**
     * PCHIP state data
     *
     * More details in: HRM, 1.2.3.
     *
     * The ES40 contains two 21272-P1 Pchips (peripheral interface chips). Each
     *Pchip controls one 64-bit PCI bus, and interfaces it to the Cchip and the
     *Dchips.
     *
     * On PIO transfers from the CPU's (or PTP transfers from the other PCI
     *bus), the Pchip acts as bus master on the PCI bus.
     *
     * On DMA or PTP transfers from a PCI device, the Pchip acts as target on
     *the PCI bus. To determine on which addresses to respond, each Pchip
     *contains 4 DMA/PTP windows, that support both direct mapped and
     *scatter-gather DMA/PTP memory access.
     **/
    struct SSys_pchip {
      u64 plat;
      u64 perr;
      u64 perrmask;
      u64 pctl;
      u64 wsba[4];
      u64 wsm[4];
      u64 tba[4];
    } pchip[2];

    u32 cf8_address[2];
  } state;
};

// The state file is the old CSystem state structure byte for byte: its
// 40-byte LL/SC part, then this. A layout change here is a file-format
// change and must bump the version in CSystem::SaveState.
static_assert(sizeof(CTsunami::SState) == 344,
              "Tsunami state layout changed: the state file format with it");

/* constants for P-Chip CSR's */
#define PCI_PCTL_HOLE U64(0x0000000000000020) /* <5>     */
#define PCI_PCTL_HOLE_START 0x00080000
#define PCI_PCTL_HOLE_END 0x000fffff

/* constants for pci-to-phys-address-mapping */
#define PCI_WSM_MASK U64(0x00000000fff00000)     /* <31:20> */
#define PCI_ADD_MASK U64(0x00000000000fffff)     /* <19:0>  */
#define PCI_TBA_MASK U64(0x00000007fff00000)     /* <34:20> */
#define PCI_PTE_ADD_MASK U64(0x00000000000fe000) /* <19:13> */
#define PCI_PTE_ADD_SHIFT 10
#define PCI_PTE_TBA_MASK U64(0x00000007fffffc00) /* <34:10> */
#define PCI_PTE_MASK U64(0x00000007ffffe000)     /* <34:13> */
#define PCI_PTE_SHIFT 12
#define PCI_PTE_ADD2_MASK U64(0x0000000000001fff) /* <12:0>  */
#define PCI_PTE_PEER_BIT U64(0x0000000090000000)  /* <31,28> */

#define PHYS_PIO_ACCESS U64(0x0000080000000000) /* <43>    */

#endif // !defined(INCLUDED_TSUNAMI_H_)
