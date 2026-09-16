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

/**
 * \file
 * Contains the code for the emulated Floppy Controller devices.
 **/
#include "FloppyController.hpp"
#include "AliM1543C.hpp"
#include "DMA.hpp"
#include "Disk.hpp"
#include "StdAfx.hpp"
#include "System.hpp"

#include <memory>

#if defined(DEBUG_FDC)
#define FDC_DEBUG(...) printf(__VA_ARGS__)
#else
#define FDC_DEBUG(...) ((void)0)
#endif

/**
 * Constructor.
 **/
CFloppyController::CFloppyController(CConfigurator *cfg, CSystem *c, int id)
    : CSystemComponent(cfg, c), CDiskController(1, 2) {
  c->RegisterMemory(this, 1536, U64(0x00000801fc0003f0) - (0x80 * id), 6);
  c->RegisterMemory(this, 1537, U64(0x00000801fc0003f7) - (0x80 * id), 1);

  memset(&state, 0, sizeof(state));
  state.status.rqm = true;
  state.dma = true;
  state.dor = 0x0C;

  printf("%s: $Id$\n", devid_string);
}

/**
 * Destructor.
 **/
CFloppyController::~CFloppyController() {}

#if defined(DEBUG_FDC)
static const char *const datarate_name[4] = {"500 Kb/S MFM", "300 Kb/S MFM",
                                             "250 Kb/S MFM", "1 Mb/S MFM"};
#endif

struct SFdcCommandInfo {
  u8 parms;   ///< Command bytes including the opcode (0 = invalid opcode).
  u8 returns; ///< Result-phase bytes.
  const char *name;
};

static const SFdcCommandInfo cmdinfo[32] = {
    {0, 0, ""},
    {0, 0, ""},
    {9, 7, "Read Track"},
    {3, 0, "Specify"},
    {2, 1, "Sense Drive Status"},
    {9, 7, "Write Data"},
    {9, 7, "Read Data"},
    {2, 0, "Recalibrate"},
    {1, 2, "Sense Interrupt Status"},
    {9, 7, "Write Deleted Data"},
    {2, 7, "Read ID"},
    {0, 0, ""},
    {9, 7, "Read Deleted"},
    {6, 7, "Format Track"},
    {1, 10, "DumpReg"},
    {3, 0, "Seek"},
    {1, 1, "Version"},
    {9, 7, "Scan Equal"},
    {2, 0, "Perpendicular Mode"},
    {4, 0, "Configure"},
    {1, 1, "Lock"},
    {0, 0, ""},
    {9, 7, "Verify"},
    {0, 0, ""},
    {0, 0, ""},
    {9, 7, "Scan Low or Equal"},
    {0, 0, ""},
    {0, 0, ""},
    {0, 0, ""},
    {9, 7, "Scan High or Equal"},
    {0, 0, ""},
    {0, 0, ""},
};

void CFloppyController::reset_controller(bool raise_irq) {
  memset(&state.pio, 0, sizeof(state.pio));
  memset(state.cmd_parms, 0, sizeof(state.cmd_parms));
  memset(state.cmd_res, 0, sizeof(state.cmd_res));
  state.cmd_parms_ptr = 0;
  state.cmd_res_ptr = 0;
  state.cmd_res_max = 0;
  state.status.rqm = true;
  state.status.dio = false;
  state.status.nondma = false;
  state.status.busy = false;
  state.status.seeking[0] = false;
  state.status.seeking[1] = false;
  state.drive[0].seeking = 0;
  state.drive[1].seeking = 0;
  state.dma = true;
  // Leaving reset in polling mode raises one interrupt that the guest
  // acknowledges with four Sense Interrupt Status commands, one per drive.
  state.reset_sense_cnt = raise_irq ? 4 : 0;
  clear_interrupt();
  if (raise_irq)
    do_interrupt();
}

bool CFloppyController::get_geometry(int drive, SFloppyGeometry *geometry) {
  struct SGeometryEntry {
    int cylinders;
    int heads;
    int sectors;
    u8 data_rate;
    off_t_large byte_size;
  };

  static const SGeometryEntry geometries[] = {
      {40, 1, 8, 2, 40 * 1 * 8 * 512},   // 160K,  250 Kb/s
      {40, 1, 9, 2, 40 * 1 * 9 * 512},   // 180K,  250 Kb/s
      {40, 2, 8, 2, 40 * 2 * 8 * 512},   // 320K,  250 Kb/s
      {40, 2, 9, 2, 40 * 2 * 9 * 512},   // 360K,  250 Kb/s
      {40, 2, 10, 2, 40 * 2 * 10 * 512}, // 400K,  250 Kb/s
      {80, 2, 8, 2, 80 * 2 * 8 * 512},   // 640K,  250 Kb/s
      {80, 2, 9, 2, 80 * 2 * 9 * 512},   // 720K,  250 Kb/s
      {80, 2, 10, 2, 80 * 2 * 10 * 512}, // 800K,  250 Kb/s
      {80, 2, 15, 0, 80 * 2 * 15 * 512}, // 1.2M,  500 Kb/s
      {80, 2, 18, 0, 80 * 2 * 18 * 512}, // 1.44M, 500 Kb/s
      {80, 2, 21, 0, 80 * 2 * 21 * 512}, // 1.68M, 500 Kb/s (DMF)
      {80, 2, 36, 3, 80 * 2 * 36 * 512}, // 2.88M, 1 Mb/s
  };

  CDisk *disk = FDISK(drive);
  if (disk == NULL || !disk->media_present() || geometry == NULL)
    return false;

  off_t_large media_size = disk->get_byte_size();
  const int geometry_count = (int)(sizeof(geometries) / sizeof(geometries[0]));
  int best = 0;
  off_t_large best_delta = media_size >= geometries[0].byte_size
                               ? media_size - geometries[0].byte_size
                               : geometries[0].byte_size - media_size;

  for (int i = 1; i < geometry_count; i++) {
    off_t_large delta = media_size >= geometries[i].byte_size
                            ? media_size - geometries[i].byte_size
                            : geometries[i].byte_size - media_size;
    if (delta < best_delta) {
      best = i;
      best_delta = delta;
    }
  }

  // Raw images have no track metadata.  Exact listed sizes win first.
  // Otherwise, prefer a 40/80-cylinder track layout that packs the image
  // into complete cylinders; the cylinder count itself need not be standard
  // because real drives can step beyond tracks 40/80.
  bool exact_nominal = false;
  int extended_cylinders = 0;
  for (int i = 0; i < geometry_count; i++) {
    if (media_size == geometries[i].byte_size) {
      best = i;
      exact_nominal = true;
      break;
    }
  }

  if (!exact_nominal) {
    int extended_best = -1;
    int least_extra = 0;
    for (int i = 0; i < geometry_count; i++) {
      off_t_large cylinder_size =
          (off_t_large)geometries[i].heads * geometries[i].sectors * 512;
      if (media_size % cylinder_size != 0)
        continue;

      off_t_large cylinders = media_size / cylinder_size;
      if (cylinders <= geometries[i].cylinders || cylinders > 256)
        continue;

      int extra = (int)cylinders - geometries[i].cylinders;
      if (extended_best < 0 || extra < least_extra) {
        extended_best = i;
        extended_cylinders = (int)cylinders;
        least_extra = extra;
      }
    }

    if (extended_best >= 0)
      best = extended_best;
  }

  geometry->heads = geometries[best].heads;
  geometry->sectors = geometries[best].sectors;
  geometry->data_rate = geometries[best].data_rate;
  off_t_large cylinder_size =
      (off_t_large)geometry->heads * geometry->sectors * 512;
  off_t_large cylinders =
      extended_cylinders > 0 ? extended_cylinders : geometries[best].cylinders;
  if (cylinders < 1)
    cylinders = 1;
  if (cylinders > 256)
    cylinders = 256;
  geometry->cylinders = (int)cylinders;
  geometry->byte_size = cylinders * cylinder_size;
  return true;
}

void CFloppyController::prepare_rw_result(int drive, int head, int eot,
                                          const SFloppyGeometry &geometry,
                                          bool multi_track, bool result_is_next,
                                          size_t count) {
  int sectors = (int)(count / 512);
  bool ended_in_partial_sector = (count % 512) != 0;
  int address_advances = sectors;
  if (!result_is_next && !ended_in_partial_sector && address_advances > 0)
    address_advances--;
  int last_head = head;

  for (int i = 0; i < address_advances; i++) {
    last_head = state.cmd_parms[3];
    state.cmd_parms[4]++;
    if (state.cmd_parms[4] > eot) {
      state.cmd_parms[4] = 1;
      if (multi_track) {
        state.cmd_parms[3]++;
        if (state.cmd_parms[3] >= geometry.heads) {
          state.cmd_parms[3] = 0;
          state.cmd_parms[2]++;
        }
      } else {
        // Table 5-6: MT=0 advances C but leaves H unchanged.
        state.cmd_parms[2]++;
      }
    }
  }
  if (!result_is_next || ended_in_partial_sector)
    last_head = state.cmd_parms[3];

  state.cmd_res[0] = drive | (last_head << 2);
  state.cmd_res[1] = 0;
  state.cmd_res[2] = 0;
  state.cmd_res[3] = state.cmd_parms[2];
  state.cmd_res[4] = state.cmd_parms[3];
  state.cmd_res[5] = state.cmd_parms[4];
  state.cmd_res[6] = state.cmd_parms[5];
  state.drive[drive].seeking = 1;
}

bool CFloppyController::format_track(int drive, int head, u8 sector_size,
                                     u8 sector_count, u8 fill,
                                     const u8 *sector_ids, size_t id_bytes) {
  u8 result_cylinder =
      drive >= 0 && drive < 2 ? (u8)state.drive[drive].cylinder : 0;
  auto set_result = [&](u8 st0, u8 st1, const u8 *id) {
    state.cmd_res[0] = st0;
    state.cmd_res[1] = st1;
    state.cmd_res[2] = 0;
    state.cmd_res[3] = id ? id[0] : result_cylinder;
    state.cmd_res[4] = id ? id[1] : (u8)head;
    state.cmd_res[5] = id ? id[2] : 1;
    state.cmd_res[6] = id ? id[3] : sector_size;
  };

  CDisk *disk = drive >= 0 && drive < 2 ? FDISK(drive) : NULL;
  if (disk == NULL || !disk->media_present()) {
    set_result(0x40 | ST0_NR | (head << 2) | drive, 0, NULL);
    return false;
  }
  if (disk->ro()) {
    set_result(0x40 | (head << 2) | drive, ST1_WP, NULL);
    return false;
  }

  SFloppyGeometry geometry;
  if (!get_geometry(drive, &geometry) || sector_size != 2 ||
      sector_count == 0 || sector_count > geometry.sectors ||
      sector_ids == NULL || id_bytes < (size_t)sector_count * 4 ||
      state.drive[drive].cylinder < 0 ||
      state.drive[drive].cylinder >= geometry.cylinders || head < 0 ||
      head >= geometry.heads) {
    set_result(0x40 | (head << 2) | drive, ST1_ND, NULL);
    return false;
  }

  u8 sector_data[512];
  memset(sector_data, fill, sizeof(sector_data));
  const u8 *last_id = NULL;
  for (unsigned i = 0; i < sector_count; i++) {
    const u8 *id = sector_ids + i * 4;
    last_id = id;

    // Raw images cannot preserve deliberately unusual C/H ID fields or
    // physical interleave.  R still identifies the logical sector to fill;
    // C/H are retained in the result packet, matching normal formatters.
    if (id[2] < 1 || id[2] > geometry.sectors || id[3] != 2) {
      set_result(0x40 | (head << 2) | drive, ST1_ND, id);
      return false;
    }

    off_t_large sector_index =
        ((off_t_large)state.drive[drive].cylinder * geometry.heads + head) *
            geometry.sectors +
        id[2] - 1;
    off_t_large offset = sector_index * 512;
    if (offset < 0 || offset > geometry.byte_size - 512 ||
        offset > disk->get_byte_size() - 512 || !disk->seek_byte(offset) ||
        disk->write_bytes(sector_data, sizeof(sector_data)) !=
            sizeof(sector_data)) {
      set_result(0x40 | (head << 2) | drive, ST1_ND, id);
      return false;
    }
  }

  disk->flush();
  set_result((head << 2) | drive, 0, last_id);
  return true;
}

void CFloppyController::finish_pio_transfer(bool ok) {
  CDisk *disk = FDISK(state.pio.drive);
  bool was_write = state.pio.write;
  bool was_format = state.pio.format;
  u32 transferred = state.pio.pos;
  u32 requested = state.pio.size;

  if (ok && was_format) {
    ok = format_track(state.pio.drive, state.pio.head, state.pio.format_n,
                      state.pio.format_sc, state.pio.format_fill,
                      state.pio.data, state.pio.size);
  } else if (ok && was_write) {
    u32 first_size = state.pio.first_size <= state.pio.size
                         ? state.pio.first_size
                         : state.pio.size;
    u32 second_size = state.pio.size - first_size;
    ok = disk != NULL && first_size > 0 && disk->seek_byte(state.pio.offset) &&
         disk->write_bytes(state.pio.data, first_size) == first_size;
    if (ok && second_size > 0) {
      ok = disk->seek_byte(state.pio.second_offset) &&
           disk->write_bytes(state.pio.data + first_size, second_size) ==
               second_size;
    }
    if (ok)
      disk->flush();
  }

  state.pio.active = false;
  state.pio.write = false;
  state.pio.format = false;
  state.pio.format_n = 0;
  state.pio.format_sc = 0;
  state.pio.format_fill = 0;
  state.pio.offset = 0;
  state.pio.second_offset = 0;
  state.pio.size = 0;
  state.pio.first_size = 0;
  state.pio.pos = 0;
  state.status.nondma = false;
  state.status.rqm = true;
  state.status.dio = true;
  state.cmd_res_ptr = 0;
  state.cmd_res_max = 7;

  if (!ok && !was_format) {
    state.cmd_res[0] = (state.cmd_res[0] & (ST0_DS | ST0_HA)) | 0x40;
    state.cmd_res[1] = ST1_ND;
    state.cmd_res[2] = 0;
  }

  FDC_DEBUG("FDC [PIO]: %s %s (%u/%u bytes)\n",
            was_format ? "format" : (was_write ? "write" : "read"),
            ok ? "complete" : "failed", transferred, requested);
  (void)transferred;
  (void)requested;

  clear_interrupt();
  do_interrupt();
}

void CFloppyController::WriteMem(int index, u64 address, int dsize, u64 data) {
  MediaRelease released[2]; // destroyed after the lock below is released
  std::lock_guard<std::mutex> lock(controller_mutex);

  if (index == 1537)
    address += 7;

  switch (address) {
  case FDC_REG_STATUS_A:
  case FDC_REG_STATUS_B:
    FDC_DEBUG("FDC: Read only register %" PRId64 " written.\n", address);
    break;

  case FDC_REG_DOR: {
    u8 old_dor = state.dor;
    state.dor = (u8)data;
    // bit 4 = drive 0 motor, bit 5 = drive 1 motor
    // bit 3 = IRQ/DMA output gate
    // bit 2 = 1: fdc enable, 0: hold at reset
    // bits 1-0: drive select (only drives 0 and 1 exist)

    state.drive[0].motor = (data & 0x10) >> 4;
    state.drive[1].motor = (data & 0x20) >> 5;
    state.drive_select = data & 0x03;

    FDC_DEBUG("FDC: motor a: %s, motor b: %s, dma/irq gate: %s, drive: %d\n",
              state.drive[0].motor ? "on" : "off",
              state.drive[1].motor ? "on" : "off", (data & 0x08) ? "on" : "off",
              state.drive_select);

    if ((data & 0x04) == 0) {
      reset_controller(false);
    } else if ((old_dor & 0x04) == 0) {
      reset_controller(true);
    } else if (((old_dor ^ data) & 0x08) && state.interrupt && theAli) {
      // DOR bit 3 gates the external IRQ pin; it does not select the
      // transfer mode, which comes from SPECIFY's ND bit.
      if (data & 0x08)
        theAli->pic_interrupt(0, 6);
      else
        theAli->pic_deassert(0, 6);
    }
    break;
  }

  case FDC_REG_TAPE:
    FDC_DEBUG("FDC: Tape register written with %" PRIx64 "\n", data);
    break;

  case FDC_REG_STATUS: // write = data rate selector
    // bit 7 = software reset (self clearing)
    // bit 6 = power down
    // bit 5 = reserved (0)
    // bit 4-2 = write precomp (000 = default)
    // bit 1-0 = data rate select
    state.datarate = data & 0x03;
    state.write_precomp = (data & 0x1c) >> 2;
    FDC_DEBUG("FDC: data rate %s, precomp: %d\n", datarate_name[state.datarate],
              state.write_precomp);

    if (data & 0x80)
      reset_controller(true);
    break;

  case FDC_REG_COMMAND:
    // A wider access is only the low byte at 0x3F5; the ISA bridge would
    // route the upper bytes to 0x3F6 and beyond, which the FDC does not own.
    write_data((u8)data);
    break;

  case FDC_REG_DIR:
    // PC/AT, PS/2
    //    bits 7-2 = reserved
    //    bit 0-1 = MFM data rate
    state.datarate = data & 0x03;
    FDC_DEBUG("FDC: data rate %s\n", datarate_name[state.datarate]);
    break;
  }

  released[0] = std::move(media_release[0]);
  released[1] = std::move(media_release[1]);
}

/**
 * One byte written to the data (FIFO) register.  Caller holds
 * controller_mutex.
 **/
void CFloppyController::write_data(u8 data) {
  if (state.pio.active) {
    if (!state.pio.write) {
      FDC_DEBUG("FDC: write to data register during non-DMA read phase.\n");
      return;
    }

    state.pio.data[state.pio.pos++] = data;
    // A non-DMA execution request is acknowledged by each FIFO access.
    // The next ready byte generates another request; the last byte instead
    // transitions to the independently-signalled result phase.
    state.status.rqm = false;
    clear_interrupt();
    if (state.pio.pos >= state.pio.size) {
      finish_pio_transfer(true);
    } else {
      state.status.rqm = true;
      do_interrupt();
    }
    return;
  }

  if (state.status.dio) {
    FDC_DEBUG("FDC: unrequested data byte to command port, discarded.\n");
    return;
  }

  state.cmd_parms[state.cmd_parms_ptr++] = data;
  int cmd = state.cmd_parms[0] & 0x1F;
  if (cmdinfo[cmd].parms != 0 && state.cmd_parms_ptr < cmdinfo[cmd].parms)
    return;

#if defined(DEBUG_FDC)
  printf("FDC: command %s(", cmdinfo[cmd].name);
  for (int i = 1; i < state.cmd_parms_ptr; i++)
    printf("%s%x", i > 1 ? " " : "", state.cmd_parms[i]);
  printf(")\n");
#endif

  state.cmd_res_max = cmdinfo[cmd].returns;
  state.cmd_res_ptr = 0;
  state.status.rqm = false;

  execute_command(cmd);

  state.status.rqm = true;
  if (state.cmd_res_max > 0 && !state.pio.active)
    state.status.dio = true;
  state.cmd_parms_ptr = 0;
}

void CFloppyController::execute_command(int cmd) {
  // A command is starting and no data transfer is in flight: a safe point to
  // apply pending operator media changes (they set the disk-change line).
  for (int i = 0; i < 2; i++)
    if (CDisk *d = FDISK(i))
      media_release[i] = d->service_media_request();

  // The post-reset polling interrupts are only reported until the guest
  // starts issuing other commands; abandoning them also drops their INT.
  if (cmd != 8 && state.reset_sense_cnt > 0) {
    state.reset_sense_cnt = 0;
    clear_interrupt();
  }

  switch (cmd) {
  case 3: // specify
    // We don't care about the step rate, head unload and head load times,
    // but the ND bit selects the execution data path.
    state.dma = (state.cmd_parms[2] & 0x01) == 0;
    break;

  case 4: // Sense Drive Status
  {
    int drive_idx = state.cmd_parms[1] & 3;
    int head = (state.cmd_parms[1] >> 2) & 1;
    CDisk *disk = FDISK(drive_idx); // NULL for drives 2-3
    u8 st3 = (head << 2) | drive_idx;
    if (disk != NULL) {
      // The M1543C defines ST3 bit 5 as fixed one and bit 3 as fixed zero.
      // OpenVMS treats bit 5 as the legacy READY bit; returning it clear
      // makes an otherwise present disk appear offline.  Absent drives
      // report neither READY nor TRACK0/WP, so guests don't see four drives.
      st3 |= 0x20;
      if (state.drive[drive_idx].cylinder == 0)
        st3 |= ST3_TZ;
      if (disk->media_present() && disk->ro())
        st3 |= ST3_WP;
    }
    state.cmd_res[0] = st3;
    break;
  }

  case 5: // write data
  case 6: // read data
    cmd_read_write(cmd);
    break;

  case 13: // Format Track
    cmd_format();
    break;

  case 7: // recalibrate
  {
    int drive_idx = state.cmd_parms[1] & 3;
    CDisk *disk = FDISK(drive_idx); // NULL for drives 2-3
    if (disk != NULL) {
      state.drive[drive_idx].seeking = 3; // wait 3 status reads to finish.
      state.drive[drive_idx].cylinder = 0;
      disk->acknowledge_media_change();
      state.seek_st0 = ST0_SE | drive_idx;
    } else {
      // No drive answers: abnormal termination, seek end, equipment check.
      state.seek_st0 = 0x40 | ST0_SE | ST0_ECE | drive_idx;
    }
    do_interrupt();
    break;
  }

  case 8: // sense interrupt status
    if (state.reset_sense_cnt > 0) {
      // ST0 interrupt code 11 (ready changed by polling), drives 0-3.
      int drive_idx = 4 - state.reset_sense_cnt;
      state.reset_sense_cnt--;
      state.cmd_res[0] = 0xC0 | drive_idx;
      state.cmd_res[1] =
          drive_idx < 2 ? (u8)state.drive[drive_idx].cylinder : 0;
      clear_interrupt();
    } else if (!state.interrupt) {
      state.cmd_res[0] = 0x80;
      state.cmd_res[1] = 0;
    } else {
      int drive_idx = state.seek_st0 & ST0_DS;
      state.cmd_res[0] = state.seek_st0;
      state.cmd_res[1] =
          drive_idx < 2 ? (u8)state.drive[drive_idx].cylinder : 0;
      clear_interrupt();
    }
    break;

  case 10: // Read ID
  {
    int drive_idx = state.cmd_parms[1] & 3;
    int head = (state.cmd_parms[1] >> 2) & 1;
    CDisk *disk = FDISK(drive_idx);
    SFloppyGeometry geometry;
    bool ready = disk != NULL && get_geometry(drive_idx, &geometry);
    state.cmd_res[1] = 0;
    state.cmd_res[2] = 0;
    state.cmd_res[3] = drive_idx < 2 ? (u8)state.drive[drive_idx].cylinder : 0;
    state.cmd_res[4] = head;
    state.cmd_res[5] = 1;
    state.cmd_res[6] = 2; // 512 bytes
    if (!ready) {
      state.cmd_res[0] = 0x40 | ST0_NR | (head << 2) | drive_idx;
      state.cmd_res[1] = ST1_MAM;
    } else if (state.datarate != geometry.data_rate) {
      state.cmd_res[0] = 0x40 | (head << 2) | drive_idx;
      state.cmd_res[1] = ST1_MAM;
    } else {
      state.cmd_res[0] = drive_idx | (head << 2);
    }
    do_interrupt();
    break;
  }

  case 14: // DumpReg
    // we're software, we don't care (I think)
    break;

  case 15: // seek
  {
    // args:
    // 0: opcode
    // 1: bit 2 = HDS (head), 1 = DS1, 0 = DS0
    // 2: NCN = new cylinder number
    int drive_idx = state.cmd_parms[1] & 3;
    CDisk *disk = FDISK(drive_idx); // NULL for drives 2-3
    if (disk != NULL) {
      state.drive[drive_idx].seeking = 3; // wait 3 status reads to finish.
      state.drive[drive_idx].cylinder = state.cmd_parms[2];
      disk->acknowledge_media_change();
      state.seek_st0 = ST0_SE | drive_idx;
    } else {
      // No drive answers: abnormal termination, seek end, equipment check.
      state.seek_st0 = 0x40 | ST0_SE | ST0_ECE | drive_idx;
    }
    do_interrupt();
    break;
  }

  case 16:                   // Version
    state.cmd_res[0] = 0x90; // 82077 compatible
    break;

  case 18: // perpendicular mode
    // We really don't care, somehow
    break;

  case 19: // configure
    // we're software, we don't care (I think)
    break;

  case 20:                                               // Lock
    state.cmd_res[0] = (state.cmd_parms[0] >> 3) & 0x10; // per the datasheet
    break;

  default:
    FDC_DEBUG("FDC: unsupported command %d = %s\n", cmd, cmdinfo[cmd].name);
    // An unsupported opcode terminates in the result phase with ST0's
    // invalid-command indication; guest input must never terminate the
    // emulator process.
    state.cmd_res[0] = 0x80;
    state.cmd_res_max = 1;
    break;
  }
}

/**
 * READ DATA / WRITE DATA.
 *
 * args:
 * 0: bit 7 = MT (multitrack), 6 = MFM, 5 = SK (skip flag)
 * 1: bit 2 = HDS (head), 1 = DS1, 0 = DS0
 * 2: C = cyl
 * 3: H = head address
 * 4: R = sector
 * 5: N = sector size, 2 = 512b
 * 6: EOT = last sector number on the track (18 for 1.44MB)
 * 7: GPL = gap length
 * 8: DTL = sector size (if N = 0)
 **/
void CFloppyController::cmd_read_write(int cmd) {
  int drive_idx = state.cmd_parms[1] & 0x03;
  int head = (state.cmd_parms[1] >> 2) & 1;
  CDisk *disk = FDISK(drive_idx);
  auto fail_rw = [&](u8 st0_extra, u8 st1) {
    state.cmd_res[0] = 0x40 | st0_extra | (head << 2) | drive_idx;
    state.cmd_res[1] = st1;
    state.cmd_res[2] = 0;
    state.cmd_res[3] = state.cmd_parms[2];
    state.cmd_res[4] = state.cmd_parms[3];
    state.cmd_res[5] = state.cmd_parms[4];
    state.cmd_res[6] = state.cmd_parms[5];
    do_interrupt();
  };

  // The FDC and a physical drive can exist without inserted media.  A data
  // command to an absent drive (including drives 2-3) or an empty drive
  // terminates abnormally with Not Ready instead of accessing an image.
  SFloppyGeometry geometry;
  if (disk == NULL || !get_geometry(drive_idx, &geometry)) {
    FDC_DEBUG("FDC [CMD %02x]: drive %d not ready (no media)\n", cmd,
              drive_idx);
    fail_rw(ST0_NR, 0);
    return;
  }

  int cyl = state.cmd_parms[2];
  int sector = state.cmd_parms[4];
  int eot = state.cmd_parms[6];
  off_t_large media_size = disk->get_byte_size();
  if (state.datarate != geometry.data_rate) {
    FDC_DEBUG("FDC [%s]: data-rate mismatch: controller=%s, media=%s\n",
              state.dma ? "DMA" : "PIO", datarate_name[state.datarate],
              datarate_name[geometry.data_rate]);
    // At the wrong clock rate no valid ID address mark can be decoded from
    // the medium.  Density probes depend on this failure before trying the
    // next supported data rate.
    fail_rw(0, ST1_MAM);
    return;
  }
  if (eot == 0)
    eot = geometry.sectors;

  int pos = (cyl * geometry.heads + head) * geometry.sectors + sector - 1;

  bool mt = (state.cmd_parms[0] & 0x80) != 0;
  int sectors_to_read;
  if (mt && head == 0 && geometry.heads > 1)
    sectors_to_read = (eot - sector + 1) + eot;
  else
    sectors_to_read = eot - sector + 1;
  if (sectors_to_read <= 0)
    sectors_to_read = 1;

  size_t count = (size_t)sectors_to_read * 512;
  if (state.dma) {
    size_t dma_count = theDMA ? theDMA->get_transfer_size(2) : 0;
    if (dma_count < count)
      count = dma_count;
  }

  FDC_DEBUG("FDC [CMD %02x]: CHS=(%d/%d/%d) EOT=%d MT=%d drive=%d "
            "geometry=%d/%d/%d media=%" PRId64 "\n",
            cmd, cyl, head, sector, eot, mt, drive_idx, geometry.cylinders,
            geometry.heads, geometry.sectors, (s64)media_size);
  FDC_DEBUG("FDC [%s]: transfer size %zu bytes (%zu sectors), LBA %d\n",
            state.dma ? "DMA" : "PIO", count, count / 512, pos);

  bool sector_valid = sector >= 1 && sector <= geometry.sectors &&
                      eot >= sector && eot <= geometry.sectors;

  off_t_large byte_offset = (off_t_large)pos * 512;
  size_t first_count = count;
  off_t_large second_offset = 0;
  if (sector_valid && mt && head == 0 && geometry.heads > 1 &&
      eot < geometry.sectors) {
    size_t first_track_count = (size_t)(eot - sector + 1) * 512;
    if (first_track_count < count) {
      first_count = first_track_count;
      second_offset =
          (off_t_large)(cyl * geometry.heads + 1) * geometry.sectors * 512;
    }
  }
  size_t second_count = count - first_count;
  auto range_fits = [&](off_t_large offset, size_t length) {
    return offset >= 0 && offset <= geometry.byte_size &&
           offset <= media_size &&
           (off_t_large)length <= geometry.byte_size - offset &&
           (off_t_large)length <= media_size - offset;
  };
  bool range_valid =
      range_fits(byte_offset, first_count) &&
      (second_count == 0 || range_fits(second_offset, second_count));
  if (state.cmd_parms[5] != 2 || !sector_valid || cyl >= geometry.cylinders ||
      head >= geometry.heads || state.cmd_parms[3] != head || !range_valid ||
      count == 0 || count > sizeof(xfer_buffer) ||
      count > sizeof(state.pio.data)) {
    if (state.dma && count == 0) {
      // No DMA controller to service the request.
      fail_rw(0, ST1_OR);
      return;
    }
    FDC_DEBUG("FDC [%s]: unsupported request C/H/R/N/EOT=%d/%d/%d/%d/%d, "
              "media=%" PRId64 " bytes\n",
              state.dma ? "DMA" : "PIO", cyl, head, sector, state.cmd_parms[5],
              eot, (s64)media_size);
    fail_rw(0, ST1_ND);
    return;
  }

  if (cmd == 5 && disk->ro()) {
    fail_rw(0, ST1_WP);
    return;
  }

  auto read_image = [&](u8 *buffer) {
    bool ok = disk->seek_byte(byte_offset) &&
              disk->read_bytes(buffer, first_count) == first_count;
    if (ok && second_count > 0)
      ok = disk->seek_byte(second_offset) &&
           disk->read_bytes(buffer + first_count, second_count) == second_count;
    return ok;
  };

  if (!state.dma) {
    // With no TC input, reaching EOT is the controller's implicit terminator.
    // The M1543 reports abnormal termination/End of Track and advances the
    // result CHRN to the next logical sector.
    prepare_rw_result(drive_idx, head, eot, geometry, mt, true, count);
    state.cmd_res[0] = (state.cmd_res[0] & (ST0_DS | ST0_HA)) | 0x40;
    state.cmd_res[1] = ST1_EOC;
    state.pio.active = true;
    state.pio.write = cmd == 5;
    state.pio.format = false;
    state.pio.drive = (u8)drive_idx;
    state.pio.head = (u8)head;
    state.pio.offset = byte_offset;
    state.pio.second_offset = second_offset;
    state.pio.size = (u32)count;
    state.pio.first_size = (u32)first_count;
    state.pio.pos = 0;
    state.cmd_res_ptr = 0;
    state.cmd_res_max = 0;
    state.status.rqm = true;
    state.status.dio = !state.pio.write;
    state.status.nondma = true;

    if (!state.pio.write) {
      memset(state.pio.data, 0, count);
      if (!read_image(state.pio.data)) {
        finish_pio_transfer(false);
        return;
      }
    }

    // In non-DMA mode INT and RQM signal that execution data is ready.
    do_interrupt();
    return;
  }

  // Partial-sector transfers (a DMA count that is not a multiple of 512) are
  // passed through as-is; real media would still read/write whole sectors.
  bool terminal_count;
  if (cmd == 6) {
    if (!read_image(xfer_buffer)) {
      fail_rw(0, ST1_ND);
      return;
    }
    CDMA::SDMA_result r = theDMA->send_data(2, xfer_buffer, count);
    terminal_count = r.terminal_count;
    if (r.blocked || (!r.verify && r.transferred < count)) {
      // Nobody accepted the data: the FDC's data register overran.
      FDC_DEBUG("FDC [DMA]: read overrun (%s, %zu/%zu bytes)\n",
                r.blocked ? "blocked" : "short", r.transferred, count);
      fail_rw(0, ST1_OR);
      return;
    }
  } else {
    CDMA::SDMA_result r = theDMA->recv_data(2, xfer_buffer, count);
    terminal_count = r.terminal_count;
    if (r.blocked || r.verify || r.transferred < count) {
      // No valid data arrived; never write a zeroed/partial buffer to the
      // image.
      FDC_DEBUG("FDC [DMA]: write overrun (%s, %zu/%zu bytes)\n",
                r.blocked ? "blocked" : (r.verify ? "verify mode" : "short"),
                r.transferred, count);
      fail_rw(0, ST1_OR);
      return;
    }
    bool ok = disk->seek_byte(byte_offset) &&
              disk->write_bytes(xfer_buffer, first_count) == first_count;
    if (ok && second_count > 0)
      ok = disk->seek_byte(second_offset) &&
           disk->write_bytes(xfer_buffer + first_count, second_count) ==
               second_count;
    if (!ok) {
      fail_rw(0, ST1_ND);
      return;
    }
    disk->flush();
  }

  prepare_rw_result(drive_idx, head, eot, geometry, mt, true, count);
  if (!terminal_count) {
    // Like the non-DMA path: reaching EOT before TC ends the command with
    // abnormal termination and End of Cylinder.
    state.cmd_res[0] = (state.cmd_res[0] & (ST0_DS | ST0_HA)) | 0x40;
    state.cmd_res[1] = ST1_EOC;
  }
  do_interrupt();
}

/**
 * FORMAT TRACK.  Command parameters are HDS/DS, N, SC, GPL and the fill byte;
 * the execution phase then supplies SC four-byte C/H/R/N IDs.
 **/
void CFloppyController::cmd_format() {
  int drive_idx = state.cmd_parms[1] & 0x03;
  int head = (state.cmd_parms[1] >> 2) & 1;
  u8 sector_size = state.cmd_parms[2];
  u8 sector_count = state.cmd_parms[3];
  u8 fill = state.cmd_parms[5];
  CDisk *disk = FDISK(drive_idx);

  auto fail_format = [&](u8 st0, u8 st1) {
    state.cmd_res[0] = st0;
    state.cmd_res[1] = st1;
    state.cmd_res[2] = 0;
    state.cmd_res[3] = drive_idx < 2 ? (u8)state.drive[drive_idx].cylinder : 0;
    state.cmd_res[4] = (u8)head;
    state.cmd_res[5] = 1;
    state.cmd_res[6] = sector_size;
    do_interrupt();
  };

  if (disk == NULL || !disk->media_present()) {
    fail_format(0x40 | ST0_NR | (head << 2) | drive_idx, 0);
    return;
  }
  if (disk->ro()) {
    fail_format(0x40 | (head << 2) | drive_idx, ST1_WP);
    return;
  }
  if (sector_size != 2 || sector_count == 0) {
    fail_format(0x40 | (head << 2) | drive_idx, ST1_ND);
    return;
  }

  size_t id_bytes = (size_t)sector_count * 4;
  if (state.dma) {
    u8 sector_ids[256 * 4];
    CDMA::SDMA_result r = {0, true, false, false, false};
    if (theDMA && theDMA->get_transfer_size(2) >= id_bytes)
      r = theDMA->recv_data(2, sector_ids, id_bytes);
    if (r.blocked || r.verify || r.transferred < id_bytes) {
      fail_format(0x40 | (head << 2) | drive_idx, ST1_OR);
      return;
    }
    format_track(drive_idx, head, sector_size, sector_count, fill, sector_ids,
                 id_bytes);
    do_interrupt();
  } else {
    state.pio.active = true;
    state.pio.write = true;
    state.pio.format = true;
    state.pio.drive = (u8)drive_idx;
    state.pio.head = (u8)head;
    state.pio.format_n = sector_size;
    state.pio.format_sc = sector_count;
    state.pio.format_fill = fill;
    state.pio.size = (u32)id_bytes;
    state.pio.first_size = 0;
    state.pio.pos = 0;
    state.cmd_res_ptr = 0;
    state.cmd_res_max = 0;
    state.status.rqm = true;
    state.status.dio = false;
    state.status.nondma = true;
    do_interrupt();
  }
}

/**
 * One byte read from the data (FIFO) register.  Returns false if no byte is
 * available.  Caller holds controller_mutex.
 **/
bool CFloppyController::read_data(u8 *value) {
  if (state.pio.active) {
    if (state.pio.write) {
      FDC_DEBUG("FDC: read from data register during non-DMA write phase.\n");
      return false;
    }

    *value = state.pio.data[state.pio.pos++];
    state.status.rqm = false;
    clear_interrupt();
    if (state.pio.pos >= state.pio.size) {
      finish_pio_transfer(true);
      return true;
    }
    state.status.rqm = true;
    do_interrupt();
    return true;
  }

  if (!state.status.dio || state.cmd_res_max == 0)
    return false;

  bool first_result_byte = state.cmd_res_ptr == 0;
  *value = state.cmd_res[state.cmd_res_ptr++];
  // Result-phase INT is acknowledged by the first result-byte read, not by
  // draining the entire result packet.
  if (first_result_byte)
    clear_interrupt();
  if (state.cmd_res_ptr >= state.cmd_res_max) {
    state.status.rqm = true;
    state.status.dio = false;
    state.cmd_res_ptr = 0;
    state.cmd_res_max = 0;
    return true;
  }
  state.status.rqm = true;
  return true;
}

u64 CFloppyController::ReadMem(int index, u64 address, int dsize) {
  MediaRelease released[2]; // destroyed after the lock below is released
  std::lock_guard<std::mutex> lock(controller_mutex);

  u64 data = 0;

  if (index == 1537)
    address += 7;

  switch (address) {
  case FDC_REG_STATUS_A:
  case FDC_REG_STATUS_B:
    break;

  case FDC_REG_DOR:
  case FDC_REG_TAPE:
    FDC_DEBUG("FDC: Write only register %" PRId64 " read.\n", address);
    break;

  case FDC_REG_STATUS:
    data = get_status();
    break;

  case FDC_REG_COMMAND: {
    // Only one FIFO byte per access, whatever the access width (see WriteMem).
    u8 value = 0;
    if (read_data(&value))
      data = value;
    break;
  }

  case FDC_REG_DIR: {
    // PS/2 mode:
    //    bit 7 = diskette change
    //    bits 6-3 = 1
    //    bit 2 = datarate select 1
    //    bit 1 = datarate select 0
    //    bit 0 = high density select
    CDisk *disk = FDISK(state.drive_select & 3);
    // DIR polls are how guests notice a media change: apply pending ones
    // here too, unless a command is being received or a non-DMA transfer
    // is still using the image.
    if (disk != NULL && !state.pio.active && state.cmd_parms_ptr == 0)
      media_release[state.drive_select & 1] = disk->service_media_request();
    if (disk != NULL && disk->media_present())
      data = disk->media_change_pending() ? 0x80 : 0x00;
    else
      data = 0x80;
    break;
  }
  }

  released[0] = std::move(media_release[0]);
  released[1] = std::move(media_release[1]);
  return data;
}

static u32 fdc_magic1 = 0x0fdc0fdc;
static u32 fdc_magic2 = 0xfdc0fdc0;

int CFloppyController::SaveState(FILE *f) {
  std::lock_guard<std::mutex> lock(controller_mutex);
  long ss = sizeof(state);

  if (fwrite(&fdc_magic1, sizeof(u32), 1, f) != 1 ||
      fwrite(&ss, sizeof(long), 1, f) != 1 ||
      fwrite(&state, sizeof(state), 1, f) != 1 ||
      fwrite(&fdc_magic2, sizeof(u32), 1, f) != 1) {
    printf("fdc: error writing state file!\n");
    return -1;
  }

  printf("fdc: %ld bytes saved.\n", ss);
  return 0;
}

int CFloppyController::RestoreState(FILE *f) {
  long ss = 0;
  u32 m1 = 0;
  u32 m2 = 0;
  std::unique_ptr<SFDC_state> restored(new SFDC_state());

  if (fread(&m1, sizeof(u32), 1, f) != 1) {
    printf("fdc: unexpected end of file!\n");
    return -1;
  }

  if (m1 != fdc_magic1) {
    printf("fdc: MAGIC 1 does not match!\n");
    return -1;
  }

  if (fread(&ss, sizeof(long), 1, f) != 1) {
    printf("fdc: unexpected end of file!\n");
    return -1;
  }

  if (ss != sizeof(SFDC_state)) {
    printf("fdc: STRUCT SIZE does not match!\n");
    return -1;
  }

  if (fread(restored.get(), sizeof(SFDC_state), 1, f) != 1) {
    printf("fdc: unexpected end of file!\n");
    return -1;
  }

  if (fread(&m2, sizeof(u32), 1, f) != 1) {
    printf("fdc: unexpected end of file!\n");
    return -1;
  }

  if (m2 != fdc_magic2) {
    printf("fdc: MAGIC 2 does not match!\n");
    return -1;
  }

  // Indices used for array access must be in range.
  const SFDC_state &s = *restored;
  if (s.cmd_parms_ptr >= sizeof(s.cmd_parms) ||
      s.cmd_res_max > sizeof(s.cmd_res) || s.cmd_res_ptr > s.cmd_res_max ||
      s.datarate > 3 || s.drive_select > 3 || s.reset_sense_cnt > 4 ||
      (s.pio.active &&
       (s.pio.drive > 1 || s.pio.size > sizeof(s.pio.data) ||
        s.pio.pos >= s.pio.size || s.pio.first_size > s.pio.size))) {
    printf("fdc: invalid controller state in file!\n");
    return -1;
  }

  std::lock_guard<std::mutex> lock(controller_mutex);
  state = s;

  // The ALi restores its own PIC IRR/ISR (before the FDC), so raising IRQ 6
  // here would latch a duplicate edge.  Only make sure the line is low when
  // the restored controller is not driving it.
  if (theAli && !(state.interrupt && (state.dor & 0x08)))
    theAli->pic_deassert(0, 6);

  printf("fdc: %ld bytes restored.\n", ss);
  return 0;
}

void CFloppyController::init() {
  if (theAli) {
    bool hasA = (FDISK(0) != NULL);
    bool hasB = (FDISK(1) != NULL);
    theAli->set_floppy_presence(hasA, hasB);
  }
}

/**
 * Idle check (main thread, ~10 Hz): apply pending operator media changes when
 * the controller is between commands, so a change lands even while the guest
 * is not polling the drive.
 **/
void CFloppyController::check_state() {
  MediaRelease released[2]; // destroyed after the lock below is released
  std::lock_guard<std::mutex> lock(controller_mutex);
  if (state.pio.active || state.cmd_parms_ptr != 0)
    return;
  for (int i = 0; i < 2; i++)
    if (CDisk *d = FDISK(i))
      released[i] = d->service_media_request();
}

void CFloppyController::do_interrupt() {
  state.interrupt = true;
  if (theAli && (state.dor & 0x08))
    theAli->pic_interrupt(0, 6);
}

void CFloppyController::clear_interrupt() {
  state.interrupt = false;
  if (theAli)
    theAli->pic_deassert(0, 6);
}

u8 CFloppyController::get_status() {
  // bit 7 = RQM data register is ready (0: no access is permitted)
  // bit 6 = 1: transfer from controller to system, 0: sys to controller
  // bit 5 = non dma mode
  // bit 4 = diskette controller is busy
  // bit 3-2 reserved
  // bit 1 = drive 1 is busy (seeking)
  // bit 0 = drive 0 is busy (seeking)

  for (int i = 0; i < 2; i++) {
    if (state.drive[i].seeking > 0)
      state.drive[i].seeking--;
    state.status.seeking[i] = state.drive[i].seeking != 0;
  }

  // CMD BUSY is separate from the per-drive seek/recalibrate busy bits.  For
  // commands without a result phase it clears after the last command byte.
  state.status.nondma = state.pio.active;
  state.status.busy = state.pio.active ||
                      (state.status.dio && state.status.rqm) ||
                      (state.cmd_parms_ptr > 0);

  u8 data =
      (state.status.rqm ? 0x80 : 0x00) | (state.status.dio ? 0x40 : 0x00) |
      (state.status.nondma ? 0x20 : 0x00) | (state.status.busy ? 0x10 : 0x00) |
      (state.status.seeking[1] ? 0x02 : 0x00) |
      (state.status.seeking[0] ? 0x01 : 0x00);

  FDC_DEBUG("FDC: status %02x\n", data);
  return data;
}
