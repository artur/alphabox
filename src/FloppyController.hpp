/* AXPbox Alpha Emulator
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
 */

/**
 * \file
 * Contains the definitions for the emulated Floppy Controller devices.
 **/
#if !defined(INCLUDED_FLOPPYCONTROLLER_H)
#define INCLUDED_FLOPPYCONTROLLER_H

#include "DMA.hpp"
#include "DiskController.hpp"
#include "SystemComponent.hpp"
#include <mutex>

/**
 * \brief Emulated floppy-drive controller (82077AA-compatible FDC of the
 * ALi M1543C).
 *
 * Commands execute synchronously on the guest's port I/O thread; DMA data is
 * moved in one CDMA::send_data()/recv_data() call, non-DMA (PIO) data one
 * byte per data-register access.
 **/
class CFloppyController : public CSystemComponent, public CDiskController {
public:
  virtual u64 ReadMem(int index, u64 address, int dsize);
  virtual void WriteMem(int index, u64 address, int dsize, u64 data);
  CFloppyController(class CConfigurator *cfg, class CSystem *c, int id);
  virtual ~CFloppyController();
  virtual int RestoreState(FILE *f);
  virtual int SaveState(FILE *f);
  virtual void init();

private:
  struct SFloppyGeometry {
    int cylinders;
    int heads;
    int sectors;
    u8 data_rate;
    off_t_large byte_size;
  };

  void write_data(u8 data);
  bool read_data(u8 *value);
  void execute_command(int cmd);
  void cmd_read_write(int cmd);
  void cmd_format();
  void reset_controller(bool raise_irq);
  void do_interrupt();
  void clear_interrupt();
  u8 get_status();
  bool get_geometry(int drive, SFloppyGeometry *geometry);
  void prepare_rw_result(int drive, int head, int eot,
                         const SFloppyGeometry &geometry, bool multi_track,
                         bool result_is_next, size_t count);
  bool format_track(int drive, int head, u8 sector_size, u8 sector_count,
                    u8 fill, const u8 *sector_ids, size_t id_bytes);
  void finish_pio_transfer(bool ok);

  /// Serializes guest port I/O (any CPU thread) and state save/restore.
  /// Lock order: controller_mutex, then the DMA's own mutex.
  std::mutex controller_mutex;

  /// Scratch buffer for DMA transfers (not part of the saved state).
  u8 xfer_buffer[65536];

  /// The state structure contains all elements that need to be saved to the
  /// statefile.
  struct SFDC_state {
    struct {
      int seeking;
      int cylinder;
      bool motor;
    } drive[2];

    u8 write_precomp;
    u8 drive_select;
    bool dma; ///< SPECIFY ND bit clear: execution phase uses DMA.
    u8 datarate;

    struct {
      bool rqm;
      bool dio;
      bool nondma;
      bool busy;
      bool seeking[2];
    } status;

    u8 cmd_parms[16];
    u8 cmd_parms_ptr;
    u8 cmd_res[16];
    u8 cmd_res_ptr;
    u8 cmd_res_max;

    bool interrupt;
    u8 dor;
    u8 reset_sense_cnt; ///< Pending post-reset Sense Interrupt Status polls.
    u8 seek_st0; ///< ST0 of the last SEEK/RECALIBRATE, for Sense Interrupt.

    /// Non-DMA execution phase.
    struct {
      bool active;
      bool write;
      bool format;
      u8 drive;
      u8 head;
      u8 format_n;
      u8 format_sc;
      u8 format_fill;
      off_t_large offset;
      off_t_large second_offset;
      u32 size;
      u32 first_size;
      u32 pos;
      u8 data[65536];
    } pio;
  } state;
};

#define FDC_REG_STATUS_A 0
#define FDC_REG_STATUS_B 1
#define FDC_REG_DOR 2
#define FDC_REG_TAPE 3
#define FDC_REG_STATUS 4
#define FDC_REG_COMMAND 5
#define FDC_REG_DIR 7

/// Disk attached to drive i (0-1), or NULL (also for i = 2-3).
#define FDISK(i) get_disk(0, i)

//
// These defines were stolen from the Linux 1.0 fdreg.h file :)
//
/* Bits of FD_ST0 */
#define ST0_DS 0x03   /* drive select mask */
#define ST0_HA 0x04   /* Head (Address) */
#define ST0_NR 0x08   /* Not Ready */
#define ST0_ECE 0x10  /* Equipment chech error */
#define ST0_SE 0x20   /* Seek end */
#define ST0_INTR 0xC0 /* Interrupt code mask */

/* Bits of FD_ST1 */
#define ST1_MAM 0x01 /* Missing Address Mark */
#define ST1_WP 0x02  /* Write Protect */
#define ST1_ND 0x04  /* No Data - unreadable */
#define ST1_OR 0x10  /* OverRun */
#define ST1_CRC 0x20 /* CRC error in data or addr */
#define ST1_EOC 0x80 /* End Of Cylinder */

/* Bits of FD_ST2 */
#define ST2_MAM 0x01 /* Missing Addess Mark (again) */
#define ST2_BC 0x02  /* Bad Cylinder */
#define ST2_SNS 0x04 /* Scan Not Satisfied */
#define ST2_SEH 0x08 /* Scan Equal Hit */
#define ST2_WC 0x10  /* Wrong Cylinder */
#define ST2_CRC 0x20 /* CRC error in data field */
#define ST2_CM 0x40  /* Control Mark = deleted */

/* Bits of FD_ST3 */
#define ST3_HA 0x04 /* Head (Address) */
#define ST3_TZ 0x10 /* Track Zero signal (1=track 0) */
#define ST3_WP 0x40 /* Write Protect */

#endif // !defined(INCLUDED_FLOPPYCONTROLLER_H)
