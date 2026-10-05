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
 * The memory a 21272-family Cchip (Tsunami/Typhoon, Titan) reports: DIMMs in
 * sets of four, a set per memory motherboard slot bank, an array per
 * populated MMB, and the serial-presence-detect EEPROMs the console reads on
 * the Cchip's MPD I2C pins. The same model serves every chipset that has
 * those pins, and the management processor's copy of it (the ES40's and
 * ES45's DPR) reads it from here (CChipset::dimms()).
 **/
#if !defined(INCLUDED_DIMM_MODEL_H_)
#define INCLUDED_DIMM_MODEL_H_

#include "StdAfx.hpp"
#include <vector>

class I2CBus;

/// One memory array: a set of four identical DIMMs, or two sets (eight).
struct dimm_array {
  int dimms = 0;            ///< 0 (not populated), 4, or 8 (twice-split)
  uint32_t dimm_mb = 0;     ///< capacity of each (identical) DIMM
  uint64_t base = 0;        ///< where the array's memory starts, in bytes
  std::vector<uint8_t> spd; ///< SPD image shared by the array's DIMMs
  uint64_t bytes() const { return ((uint64_t)dimms * dimm_mb) << 20; }
};

/// The modelled DIMM population (model_dimms, model_dimm_arrays): array n
/// is the DIMMs of memory motherboard n.
struct dimm_population {
  dimm_array array[4];
};

/// An SPD image for a registered ECC PC100 SDRAM DIMM of `dimm_mb`.
std::vector<uint8_t> build_sdram_spd(uint32_t dimm_mb,
                                     bool registered_ecc = true);

/**
 * Choose the DIMM population for `total_mb` of memory. Sets of 4 identical
 * DIMMs fill one MMB's slot set (J1-J4, then J5-J8) and each populated MMB is
 * one memory array (at most 8 DIMMs, within 8 GB per array). The DIMM size is
 * total/4 capped at 1 GB, so every configuration is at least one 4-DIMM set
 * (4-way interleave) and memory above 4 GB spills into more sets and arrays.
 **/
dimm_population model_dimms(uint32_t total_mb);

/**
 * Why an array of `mb` megabytes cannot be modelled, or nullptr when it can
 * (0 is an empty array): four or eight identical DIMMs of 16 MB to 1 GB,
 * and a size the Cchip's AARn can hold (a power of two, at most 8 GB).
 **/
const char *dimm_array_refusal(uint32_t mb);

/**
 * The DIMM population for arrays of the given sizes (`memory.arrays`, in
 * megabytes, 0 for an empty array): four DIMMs of a quarter of the array
 * each, eight of 1 GB for an 8 GB array. The bases are given out from 0,
 * to the largest array first and to arrays of one size in their own order,
 * so that every array starts at a multiple of its size (the Cchip compares
 * an address with AARn's base under the size's mask) and memory has no
 * hole. [Inferred: the serial ROM, which is not emulated, assigns the
 * bases on a real machine. The real listings agree where they say: arrays
 * 0 and 2 of one size in that order (DS15, ES45), and the DS25's 512 MB
 * array 0 above its 1024 MB array 2.]
 **/
dimm_population model_dimm_arrays(const uint32_t mb[4]);

/// Attach the population's SPD EEPROMs to the Cchip's MPD bus: one
/// representative EEPROM per 4-DIMM set, at 0x50 + array * 2 + set.
void attach_dimm_spd(I2CBus &bus, const dimm_population &dimms);

#endif // !defined(INCLUDED_DIMM_MODEL_H_)
