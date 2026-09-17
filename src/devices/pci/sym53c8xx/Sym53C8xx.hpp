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

/* Symbios (NCR/LSI) 53C8xx PCI-SCSI I/O processor family.
 *
 * One SCRIPTS processor and register file for every part; the differences
 * between parts (wide SCSI, on-chip RAM, register masks, PCI identity) are
 * data in a sym_chip_config, so a chip is a table row (Sym53C8xxChips.cpp).
 * The code is split by concern:
 *
 *   Sym53C8xx.cpp            construction, PCI header, threads, reset,
 *                            state file, main-thread timers
 *   Sym53C8xxRegisters.cpp   the register file and its side effects
 *   Sym53C8xxScripts.cpp     the SCRIPTS processor
 *   Sym53C8xxInterrupts.cpp  interrupt raising, stacking, the IRQ line
 *   Sym53C8xxRegs.hpp        register definitions (family units only)
 *   Sym53C8xxChips.cpp       the parts
 */
#if !defined(INCLUDED_SYM53C8XX_H_)
#define INCLUDED_SYM53C8XX_H_

#include "DiskController.hpp"
#include "PCIDevice.hpp"
#include "SCSIDevice.hpp"

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>

/**
 * \brief What distinguishes one 53C8xx part from another.
 **/
struct sym_chip_config {
  const char *name;  ///< part name for messages, e.g. "53C810"
  u16 pci_device_id; ///< PCI config 0x02
  u8 pci_revision;   ///< PCI config 0x08; its low nibble also reads in CTEST3
  u8 macntl;         ///< MACNTL reset value (bits 7..4: chip type)
  u32 ram_bytes;     ///< on-chip SCRIPTS RAM behind BAR2 (0: none)

  // SCSI IDs: 3 bits on narrow parts, 4 on wide ones.
  u8 id_mask;

  // Writable bits of the registers whose layout differs between parts.
  u8 scntl2_mask;
  u8 scntl2_w1c;
  u8 scntl3_mask;
  u8 scid_mask;
  u8 gpreg_mask;
  u8 ctest5_mask;
  u8 sien1_mask;
  u8 sist1_rc;    ///< SIST1 bits cleared by reading
  u8 sist1_fatal; ///< SIST1 bits that interrupt even when masked
  u8 stime1_mask;
  u8 stest1_mask;
  u8 stest2_mask;
  u8 stest3_mask;
};

/**
 * \brief Symbios 53C8xx SCSI disk controller.
 *
 * \bug Exception below ASTDEL during OpenVMS boot when booting from SCSI.
 *
 * Documentation consulted:
 *  - SCSI 2 (http://www.t10.org/ftp/t10/drafts/s2/s2-r10l.pdf)
 *  - SCSI 3 Multimedia Commands (MMC)
 *(http://www.t10.org/ftp/t10/drafts/mmc/mmc-r10a.pdf)
 *  - SYM53C810A PCI-SCSI I/O Processor
 *(http://ftp.netbsd.org/pub/NetBSD/arch/bebox/doc/810a.pdf)
 *  - Symbios SCSI SCRIPTS Processors Programming Guide
 *(http://la.causeuse.org/hauke/macbsd/symbios_53cXXX_doc/lsilogic-53cXXX-scripts.pdf)
 *  .
 **/
class CSym53C8xx : public CPCIDevice,
                   public CDiskController,
                   public CSCSIDevice {
public:
  virtual int SaveState(FILE *f);
  virtual int RestoreState(FILE *f);
  virtual void check_state();

  virtual void run(); // Poco Thread entry point
  virtual void init();
  virtual void start_threads();
  virtual void stop_threads();

  virtual void WriteMem_Bar(int func, int bar, u32 address, int dsize,
                            u32 data);
  virtual u32 ReadMem_Bar(int func, int bar, u32 address, int dsize);

  virtual u32 config_read_custom(int func, u32 address, int dsize, u32 data);
  virtual void config_write_custom(int func, u32 address, int dsize,
                                   u32 old_data, u32 new_data, u32 data);

  virtual void register_disk(class CDisk *dsk, int bus, int dev);

  CSym53C8xx(CConfigurator *cfg, class CSystem *c, int pcibus, int pcidev,
             const sym_chip_config &chip);
  virtual ~CSym53C8xx();

  /// The part named `name` ("810", "895", ...), or nullptr.
  static const sym_chip_config *find_chip(const char *name);

private:
  const sym_chip_config m_chip;

  void write_b_scntl0(u8 value);
  void write_b_scntl1(u8 value);
  void write_b_istat(u8 value);
  u8 read_b_ctest2();
  void write_b_ctest3(u8 value);
  void write_b_ctest4(u8 value);
  void write_b_ctest5(u8 value);
  void write_b_stest2(u8 value);
  void write_b_stest3(u8 value);
  u8 read_b_dstat();
  u8 read_b_sist(int id);
  void write_b_dcntl(u8 value);

  void post_dsp_write();

  void start_scripts();
  void run_scripts_inline();
  bool inline_can_execute_next();
  void step_scripts();
  void halt_scripts_on_failure(const std::string &msg);

  int check_phase(int chk_phase);
  void execute_io_op();
  void execute_rw_op();
  void execute_ls_op();
  void execute_mm_op();
  void execute_tc_op();
  void execute_bm_op();
  void execute();

  void eval_interrupts();
  void set_interrupt(int reg, u8 interrupt);
  void chip_reset();

  std::unique_ptr<std::thread> myThread;
  std::atomic_bool myThreadDead{false};

  /// Serializes the register file and SCRIPTS execution. Recursive because
  /// SCRIPTS Load/Store and R/W instructions re-enter ReadMem_Bar/WriteMem_Bar
  /// (and through DSP/ISTAT/DCNTL writes, the start logic) with it held.
  std::recursive_mutex myRegLock;
  /// Wakes the SCRIPTS thread; predicate is StopThread || state.executing.
  std::condition_variable_any scriptsWake;
  bool StopThread = false;      ///< guarded by myRegLock
  bool scripts_running = false; ///< a SCRIPTS instruction is executing on the
                                ///< thread holding myRegLock
  std::string scripts_error;    ///< SCRIPTS failure for check_state() to raise
#if defined(DEBUG_SYM_START)
  unsigned long dbg_sigp_not_waiting = 0;
  unsigned long dbg_inline_handoffs = 0;
#endif

  /// The state structure contains all elements that need to be saved to the
  /// statefile.
  struct SSym_state {
    bool irq_asserted;

    union USym_regs {
      u8 reg8[128];
      u16 reg16[64];
      u32 reg32[64];
    } regs;

    struct SSym_alu {
      bool carry;
    } alu;

    u8 ram[4096];

    bool executing;

    bool wait_reselect;
    bool select_timeout;
    int disconnected;
    u32 wait_jump;

    u8 dstat_stack;
    u8 sist0_stack;
    u8 sist1_stack;

    long gen_timer;

    // Instruction counter for runaway SCRIPTS protection
    int insn_processed;

    // SCSI phase tracked by the controller (SSTAT1 bits [2:0])
    int scsi_phase;

    // Current SCSI status byte from command completion
    u8 status;

    // Message-in buffer and length
    u8 msg[8];
    int msg_len;

    // Message action: what to do after MSG IN phase completes
    // 0 = COMMAND, 1 = disconnect, 2 = DATA OUT, 3 = DATA IN
    int msg_action;

    // Current LUN (set by IDENTIFY message)
    u8 current_lun;

    // Command completion pending flag
    int command_complete;
  } state;
};
#endif // !defined(INCLUDED_SYM53C8XX_H_)
