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
 */

#include "DiskRam.hpp"
#include "StdAfx.hpp"

CDiskRam::CDiskRam(CConfigurator *cfg, CSystem *sys, CDiskController *c,
                   int idebus, int idedev)
    : CDisk(cfg, sys, c, idebus, idedev) {
  byte_size = myCfg->get_num_value("size", false, 512 * 1024 * 1024);

  // A file to start from (es40.cfg): the disk takes its size, in whole
  // blocks, and its contents. What the guest writes stays in memory.
  FILE *preload = nullptr;
  if (const char *file = myCfg->get_text_value("file")) {
    preload = fopen_large(file, "rb");
    if (!preload) {
      printf("%s: %s cannot be opened (%s); the RAMDISK starts blank.\n",
             devid_string, file, strerror(errno));
    } else {
      if (fseek_large(preload, 0, SEEK_END) != 0 ||
          (byte_size = ftell_large(preload)) < 0 ||
          fseek_large(preload, 0, SEEK_SET) != 0)
        FAILURE_2(File, "%s: cannot read %s", devid_string, file);
      byte_size = (byte_size + state.block_size - 1) / state.block_size *
                  state.block_size;
    }
  }

  if (byte_size <= 0)
    FAILURE_1(Configuration, "%s: a RAMDISK of 0 bytes", devid_string);
  // Zeroed: a blank disk reads as zeros, as a new image file does.
  CHECK_ALLOCATION(ramdisk = calloc(1, (size_t)byte_size));
  if (preload) {
    const size_t got = fread(ramdisk, 1, (size_t)byte_size, preload);
    fclose(preload);
    printf("%s: RAMDISK preloaded with %zu bytes of %s.\n", devid_string, got,
           myCfg->get_text_value("file"));
  }

  state.byte_pos = 0;

  sectors = 32;
  heads = 8;

  // calc_cylinders();
  determine_layout();

  model_number = myCfg->get_text_value("model_number", "ES40RAMDISK");

  printf("%s: Mounted RAMDISK, %" PRId64 " %zd-byte blocks, %" PRId64 "/%ld/%ld.\n",
         devid_string, byte_size / state.block_size, state.block_size,
         cylinders, heads, sectors);
}

CDiskRam::~CDiskRam(void) {
  if (ramdisk) {
    printf("%s: RAMDISK freed.\n", devid_string);
    free(ramdisk);
    ramdisk = 0;
  }
}

/**
 * Returns false, as CDiskFile does, for a position outside the disk: the
 * caller turns that into a SCSI or ATA error for the guest, which reached
 * it, rather than a stop of the machine.
 **/
bool CDiskRam::seek_byte(off_t_large byte) {
  if (byte < 0 || byte >= byte_size)
    return false;

  state.byte_pos = byte;
  return true;
}

size_t CDiskRam::read_bytes(void *dest, size_t bytes) {
  if (state.byte_pos >= byte_size)
    return 0;

  // Clamp to the bytes left in the image. The old loop stopped one byte short
  // (">="), so a transfer ending exactly at the end of the image lost its last
  // byte.
  const off_t_large available = byte_size - state.byte_pos;
  if ((off_t_large)bytes > available)
    bytes = (size_t)available;

  memcpy(dest, &(((char *)ramdisk)[state.byte_pos]), bytes);
  state.byte_pos += (unsigned long)bytes;
  return bytes;
}

static const u32 ramdisk_magic = 0x5241D15C;
static const size_t ramdisk_chunk = 64 * 1024;

/**
 * The disk's state, then its contents: a RAM disk has no image to come
 * back from, so a snapshot without them restored whatever malloc gave.
 * The contents go in 64 KiB chunks, each a flag byte and, unless the chunk
 * is all zeros, its bytes: an idle RAM disk costs a few bytes per chunk.
 **/
int CDiskRam::SaveState(FILE *f) {
  if (CDisk::SaveState(f))
    return -1;
  const u64 size = (u64)byte_size;
  fwrite(&ramdisk_magic, sizeof(u32), 1, f);
  fwrite(&size, sizeof(u64), 1, f);
  static const char zero[ramdisk_chunk] = {0};
  for (u64 at = 0; at < size; at += ramdisk_chunk) {
    const size_t n = (size_t)std::min<u64>(ramdisk_chunk, size - at);
    const char *p = (const char *)ramdisk + at;
    const u8 has_data = memcmp(p, zero, n) != 0;
    fwrite(&has_data, 1, 1, f);
    if (has_data)
      fwrite(p, 1, n, f);
  }
  fwrite(&ramdisk_magic, sizeof(u32), 1, f);
  return 0;
}

int CDiskRam::RestoreState(FILE *f) {
  if (CDisk::RestoreState(f))
    return -1;
  // A state file from before the contents were saved has the next
  // component's record here: leave the disk as it is, and say so.
  u32 m = 0;
  if (fread(&m, sizeof(u32), 1, f) != 1 || m != ramdisk_magic) {
    fseek(f, -(long)sizeof(u32), SEEK_CUR);
    printf("%s: the state file has no RAM disk contents; the disk keeps "
           "its current ones.\n",
           devid_string);
    return 0;
  }
  u64 size = 0;
  if (fread(&size, sizeof(u64), 1, f) != 1 || size != (u64)byte_size) {
    printf("%s: the saved RAM disk is %" PRIu64 " bytes, this one %" PRId64
           ".\n",
           devid_string, size, (s64)byte_size);
    return -1;
  }
  for (u64 at = 0; at < size; at += ramdisk_chunk) {
    const size_t n = (size_t)std::min<u64>(ramdisk_chunk, size - at);
    char *p = (char *)ramdisk + at;
    u8 has_data = 0;
    if (fread(&has_data, 1, 1, f) != 1 ||
        (has_data && fread(p, 1, n, f) != n)) {
      printf("%s: unexpected end of file!\n", devid_string);
      return -1;
    }
    if (!has_data)
      memset(p, 0, n);
  }
  if (fread(&m, sizeof(u32), 1, f) != 1 || m != ramdisk_magic) {
    printf("%s: RAM disk contents end in the wrong place!\n", devid_string);
    return -1;
  }
  printf("%s: %" PRIu64 " bytes of RAM disk restored.\n", devid_string, size);
  return 0;
}

size_t CDiskRam::write_bytes(void *src, size_t bytes) {
  // A read-only RAM disk is not written, whoever asks: the SCSI path
  // refuses first, and any other caller gets the short count.
  if (read_only || state.byte_pos >= byte_size)
    return 0;

  // Clamp to the bytes left in the image. The old loop stopped one byte short
  // (">="), so a transfer ending exactly at the end of the image lost its last
  // byte.
  const off_t_large available = byte_size - state.byte_pos;
  if ((off_t_large)bytes > available)
    bytes = (size_t)available;

  memcpy(&(((char *)ramdisk)[state.byte_pos]), src, bytes);
  state.byte_pos += (unsigned long)bytes;
  return bytes;
}
