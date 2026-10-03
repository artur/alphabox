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

/* The CP microcode images the model knows (RadeonMicrocode.cpp), and what
 * an image a driver loads into the micro-engine says about the packets the
 * CP will parse. The images themselves are not here: they are identified
 * by CRC-32. docs/radeon-microcode.md is the reference.
 */

#if !defined(INCLUDED_RADEON_MICROCODE_H)
#define INCLUDED_RADEON_MICROCODE_H

#include <functional>
#include <string>

#include "datatypes.hpp"

namespace radeon {

struct ChipInfo;

/// A microcode image found in the open sources (docs/radeon-microcode.md).
struct MicrocodeImage {
  u32 crc;            ///< CRC-32 as the model logs it (entries {H, L}, LE)
  u32 crc_file;       ///< CRC-32 of the linux-firmware style file (BE)
  const char *name;   ///< e.g. "R100_cp"
  const char *family; ///< the parts Linux loads it on, e.g. "R100"
  const char *origin; ///< where it was first published, and when
  /// Width of the micro-engine word's register field: 11 (Rage 128), 12
  /// (R100/R200), 13 (R300 and later) [inference: the images].
  u8 reg_bits;
  /// The last type-3 opcode the image's dispatch table covers (it starts
  /// at 0x10, NOP), and which opcodes have a handler of their own (bit
  /// op - 0x10); the others go to NOP's handler, which skips the body.
  u8 table_last;
  u64 handled;
};

/// What the model makes of the image in the micro-engine RAM.
struct MicrocodeId {
  u32 crc = 0;
  const MicrocodeImage *known = nullptr;
  /// The register-field width entries 2 and 3 (two jumps) give, 0 if they
  /// are not jumps of any known width.
  u8 reg_bits = 0;
  /// The image is for another generation of micro-engine than the part's.
  bool wrong_width = false;
};

/// The known image with this CRC (as logged), or nullptr.
const MicrocodeImage *find_microcode(u32 crc);

/// Identify an image (entries of {DATAH, DATAL}).
MicrocodeId identify_microcode(const u32 (*me)[2], u32 entries,
                               const ChipInfo &chip);

/// Print what identify_microcode found, as the card's log line(s).
void log_microcode(const MicrocodeId &id, const ChipInfo &chip, u32 entries,
                   const char *devid);

/// Whether the CP parses type-3 opcode `op` (bit 7, GUI_CONTROL present,
/// ignored) with this microcode:
///   1  the image's dispatch table gives it a handler of its own;
///   0  the table sends it to NOP's handler: the packet is skipped;
///  -1  not known (an unknown image, or past the end of its table): the
///      part's row decides, as before the image was identified.
int microcode_packet(const MicrocodeId &id, u8 op);

/// ALPHABOX_RADEON_ME_DUMP=<file>: write the image in the linux-firmware
/// layout (entries of DATAH then DATAL, big-endian), for
/// test/tools/radeon_me_dis.py.
void dump_microcode(const u32 (*me)[2], u32 entries, const char *devid);

/// The self-test's checks of the lookups (ALPHABOX_RADEON_SELFTEST).
void selftest_microcode(const ChipInfo &chip,
                        const std::function<void(const std::string &, bool,
                                                 const std::string &)> &report);

} // namespace radeon

#endif // !defined(INCLUDED_RADEON_MICROCODE_H)
