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

/* What a Radeon generation's 3D engine is to the rest of the card, and
 * what the card is to it.
 *
 * The family's common code (the radeon/ directory: the register file,
 * the display, the memory controller and GART, the command FIFO and the
 * CP, the 2D engine) is the same from the R100 to the R300; the 3D
 * engine is each generation's own (r100/, and r200/, r300/ beside it
 * later). The chip row (RadeonChip.hpp) names the part's generation, and
 * the generation builds its engine (radeon::Generation::make_3d). The
 * common code reaches the engine only through CRadeonEngine3D; the engine
 * reaches the card only through CRadeonEngineBus.
 *
 * Threads. An engine runs on whatever thread drives the card's engine --
 * the engine thread that drains the command FIFO and the CP's streams
 * (RadeonQueue.cpp), or a CPU's thread when the engine runs inline
 * (ALPHABOX_RADEON_SYNC=1, a write that cannot be queued, the state file,
 * the self-test) -- and always with the card's execution lock
 * (CRadeon::m_exec_mx) held. Every entry point below is called under that
 * lock, so an engine keeps its state without locks of its own, never
 * starts threads, never blocks waiting for the CPU, and touches the card
 * only through the bus while it is called. The one exception is
 * pixels(), which the queue reads under the same lock too, before and
 * after each command.
 */

#if !defined(INCLUDED_RADEON_ENGINE3D_H)
#define INCLUDED_RADEON_ENGINE3D_H

#include <cstdint>
#include <cstdio>
#include <vector>

#include "datatypes.hpp"

class CRadeon;
namespace radeon {
struct ChipInfo;
struct SelfTest;
} // namespace radeon

/**
 * The card as a 3D engine sees it: the register file, VRAM, the memory
 * controller's view of the bus for the engine's own fetches, the part's
 * row. A reference to the card, copied into the engine; the definitions
 * are inline, after CRadeon (Radeon.hpp).
 **/
class CRadeonEngineBus {
public:
  explicit CRadeonEngineBus(CRadeon &card) : c(card) {}

  /// A register as it was last written (the register file; no side
  /// effects).
  u32 &R(u32 reg);
  u32 R(u32 reg) const;
  /// VRAM by offset (the framebuffer's own addresses, not the MC's).
  u32 vram_read(u32 addr, int bytes) const;
  void vram_write(u32 addr, int bytes, u32 data);
  u8 vram_byte(u32 addr) const;
  /// A memory-controller address of the framebuffer as a VRAM offset
  /// (MC_FB_LOCATION).
  u32 mc_to_vram(u32 mc) const;
  /// A memory-controller address the engine fetches by bus mastering:
  /// the framebuffer, the AGP window or the PCI GART (RadeonGart.cpp).
  bool bm_translate(u32 mc, bool *is_vram, u32 *addr);
  /// A dword read at such an address (CRadeon::cp_read32).
  u32 bm_read32(u32 mc);
  /// A command-buffer dword at such an address: as bm_read32, with the
  /// ring's BUF_SWAP in host memory (CRadeon::cp_fetch).
  u32 bm_fetch(u32 mc);
  /// The part's row.
  const radeon::ChipInfo &chip() const;
  /// The device's name for messages.
  const char *devid() const;

private:
  CRadeon &c;
};

/**
 * A generation's 3D engine, as the common code calls it.
 *
 * Register writes and reads reach the engine after the card's own
 * registers have had their turn (RadeonControl.cpp): the engine takes the
 * ones it acts on and leaves the rest to the register file, which it
 * reads back through the bus when it draws. Type-3 packets the CP does
 * not know reach packet3() (RadeonCP.cpp).
 **/
class CRadeonEngine3D {
public:
  virtual ~CRadeonEngine3D() = default;

  /// A register write the engine acts on; true when it consumed it
  /// (plain state registers are left to the register file).
  virtual bool reg_write(u32 reg, u32 data) = 0;
  /// A register read the engine answers; true when it did.
  virtual bool reg_read(u32 reg, u32 *v) = 0;
  /// A type-3 packet the engine owns; false for any other.
  virtual bool packet3(u8 op, const std::vector<u32> &d) = 0;
  /// The engine's own state back to power-on (not the register file's).
  virtual void reset() = 0;
  /// The state file: the engine's block after the card's own. Each
  /// generation writes its own magic first, and restore() returns false,
  /// leaving the file where it was, when the next block is not its own
  /// (a state file older than the engine), after which the card calls
  /// reset().
  virtual void save(FILE *f) const = 0;
  virtual bool restore(FILE *f) = 0;
  /// Pixels the engine has written since it was made (the busy-time
  /// model's input; RadeonQueue.cpp).
  virtual u64 pixels() const = 0;
  /// The self-test's 3D scenes (ALPHABOX_RADEON_SELFTEST), run between
  /// the common checks with the means RadeonSelfTest.hpp gives them.
  virtual void selftest_scenes(radeon::SelfTest &t) = 0;
};

#endif // !defined(INCLUDED_RADEON_ENGINE3D_H)
