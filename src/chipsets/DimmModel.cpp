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
 * The DIMM model the Cchips with MPD pins share (DimmModel.hpp), moved
 * unchanged out of the Tsunami's TsunamiMemory.cpp for the Titan.
 **/
#include "DimmModel.hpp"
#include "i2c_spd.hpp"
#include <memory>

std::vector<uint8_t> build_sdram_spd(uint32_t mb, bool registered_ecc) {
  // ES40-typical: Registered ECC PC100 SDRAM (168-pin), CL=2/3 supported.
  // Geometry chosen to make capacity math coherent for 64/128/256/512/1024 MB.
  struct Geo {
    uint8_t rows, cols, ranks;
  } g{};

  switch (mb) {
  case 16: // 16/32 MB DIMMs only occur in configurations below 256 MB
    g = {11, 8, 1};
    break;
  case 32:
    g = {11, 9, 1};
    break;
  case 64:
    g = {12, 9, 1};
    break; // 8Mx8 devices, 1 rank
  case 128:
    g = {13, 9, 1};
    break; // 16Mx8, 1 rank
  case 256:
    g = {13, 10, 2};
    break; // 16Mx8, 2 ranks
  case 512:
    g = {14, 10, 2};
    break; // 32Mx8, 2 ranks
  case 1024:
    g = {14, 10, 2};
    break; // 32Mx8, 2 ranks (denser parts)
  default:
    g = {13, 10, 2};
    break;
  }

  std::vector<uint8_t> b(256, 0x00);
  b[0] = 0x80;   // bytes used
  b[1] = 0x08;   // SPD rev 1.3 (0x08 is commonly used)
  b[2] = 0x04;   // SDR SDRAM
  b[3] = g.rows; // Row address bits
  b[4] = g.cols; // Column address bits
  // Byte 5: module attributes - bit1 Registered, bit5 ECC
  b[5] = (registered_ecc ? 0x20 : 0x00) | 0x02; // ECC + Registered
  b[6] = 0x04;                                  // SDRAM device banks (4)
  // Data width 64, ECC width 8 -> 72-bit module (ES40 expects ECC)
  b[7] = 64;
  b[8] = 0;
  b[11] = 8;
  b[12] = 0;
  b[17] = g.ranks; // module ranks
  // Conservative PC100 timings (CL=2/3). Units are ns.
  b[9] = 20;  // tAA (CL=2) 20 ns
  b[10] = 2;  // tWR (~2ns; not used by SRM)
  b[18] = 20; // tRCD 20 ns
  b[19] = 20; // tRP  20 ns
  b[20] = 10; // tCK min at highest supported CL (10 ns => 100 MHz)
  b[21] = 10; // tCK at CL=2 also 10 ns (safe)
  b[22] = 45; // tRAS 45 ns (common PC100 value)
  // Byte 23: supported CAS latencies bitmask: bit1=CL2, bit2=CL3
  b[23] = 0x06; // CL=2 and CL=3 supported

  // Compute checksum over bytes 0..62
  uint8_t sum = 0;
  for (int i = 0; i <= 62; i++)
    sum = (uint8_t)(sum + b[i]);
  b[63] = (uint8_t)(0x100 - sum);
  return b;
}

dimm_population model_dimms(uint32_t total_mb) {
  dimm_population p;
  const uint32_t dimm_mb = (total_mb / 4 > 1024) ? 1024 : total_mb / 4;
  const uint32_t n_dimms = total_mb / dimm_mb;
  const int n_arrays = (int)((n_dimms + 7) / 8);
  for (int a = 0; a < n_arrays && a < 4; a++) {
    p.array[a].dimms = (int)(n_dimms / n_arrays);
    p.array[a].dimm_mb = dimm_mb;
    p.array[a].base = a * p.array[a].bytes();
    p.array[a].spd = build_sdram_spd(dimm_mb, /*registered_ecc*/ true);
  }
  return p;
}

const char *dimm_array_refusal(uint32_t mb) {
  if (!mb)
    return nullptr;
  if (mb & (mb - 1))
    return "is not a power of two";
  if (mb < 64)
    return "is less than 64 MB (four DIMMs of 16 MB)";
  if (mb > 8192)
    return "is more than 8192 MB, the largest array the Cchip describes";
  return nullptr;
}

dimm_population model_dimm_arrays(const uint32_t mb[4]) {
  dimm_population p;
  uint64_t next = 0;
  // Largest first; among equals, the lower array first.
  for (uint32_t size = 8192; size >= 64; size >>= 1)
    for (int a = 0; a < 4; a++) {
      if (mb[a] != size)
        continue;
      p.array[a].dimms = (size > 4096) ? 8 : 4;
      p.array[a].dimm_mb = size / p.array[a].dimms;
      p.array[a].base = next;
      p.array[a].spd = build_sdram_spd(p.array[a].dimm_mb, true);
      next += p.array[a].bytes();
    }
  return p;
}

void attach_dimm_spd(I2CBus &bus, const dimm_population &d) {
  // The I2C bus does not carry every DIMM (HRM 9.10): one representative
  // EEPROM per 4-DIMM set, at 0x50 + array * 2 + set.
  for (int a = 0; a < 4; a++)
    for (int s = 0; s < d.array[a].dimms / 4; s++)
      bus.attach(std::make_shared<Eeprom24C02>(uint8_t(0x50 + a * 2 + s),
                                               d.array[a].spd));
}
