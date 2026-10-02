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

/// The modelled DIMM population (model_dimms).
struct dimm_population {
  int n_arrays = 1;         ///< populated arrays = populated MMBs (1, 2 or 4)
  int dimms_per_array = 4;  ///< 8 (twice-split) or 4 (lower slot set only)
  uint32_t dimm_mb = 0;     ///< capacity of each (identical) DIMM
  std::vector<uint8_t> spd; ///< SPD image shared by all modelled DIMMs
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

/// Attach the population's SPD EEPROMs to the Cchip's MPD bus: one
/// representative EEPROM per 4-DIMM set, at 0x50 + array * 2 + set.
void attach_dimm_spd(I2CBus &bus, const dimm_population &dimms);

#endif // !defined(INCLUDED_DIMM_MODEL_H_)
