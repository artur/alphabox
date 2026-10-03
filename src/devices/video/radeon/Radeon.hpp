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

/* ATI Radeon 7500 (RV200, 1002:5157), the AGP card HP shipped in the
 * ES47, ES80 and GS1280 (and offered for the ES45 and DS25).
 *
 * Standard VGA -- register decode, planar memory, rendering, the render
 * thread, option ROM and I/O routing -- comes from CVGACard/CVGA. This
 * class adds what is Radeon, one concern per translation unit:
 *
 *   Radeon.cpp         construction, init(), PCI header (with the AGP
 *                      capability), BAR routing, state file
 *   RadeonControl.cpp  the register aperture: MM_INDEX/MM_DATA, the PLLs,
 *                      memory controller, CRTC status, DAC and palette,
 *                      DDC lines, interrupts, configuration mirrors
 *   RadeonMemory.cpp   the framebuffer aperture and the 0xa0000 window
 *   RadeonEngine.cpp   the 2D engine (GUI master control, fills, blits,
 *                      host data, lines) and its FIFO and idle status
 *   RadeonCP.cpp       the command processor: the ring buffer, indirect
 *                      buffers and the packets they carry
 *   RadeonDisplay.cpp  the primary CRTC's extended modes, the hardware
 *                      cursor, the 8-bit palette
 *   Radeon3D.cpp, RadeonTcl.cpp, RadeonRaster.cpp
 *                      the 3D engine (CRadeon3D, Radeon3D.hpp): vertex
 *                      fetch and the 3D packets, TCL, rasteriser and
 *                      pixel pipeline
 *
 * Every register the card has is kept in one 64 KB register file and
 * reads back what was written, unless this model gives it a meaning:
 * the BIOS and drivers touch many registers (memory timing, power
 * management, the TV-out and TMDS blocks) whose effect is analogue and
 * none of an emulator's business.
 *
 * The register meanings come from the open drivers -- Linux's radeonfb
 * and radeon DRM headers, X.org's xf86-video-ati -- and from QEMU's
 * ati-vga (hw/display/ati*.c, GPL-2.0-or-later, BALATON Zoltan), which
 * models the Rage 128 Pro and the Radeon RV100. The code is this
 * project's own; where it follows QEMU's reading of a register it says so.
 * The 3D engine's sources are listed in Radeon3D.hpp.
 */

#if !defined(INCLUDED_RADEON_H)
#define INCLUDED_RADEON_H

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "Radeon3D.hpp"
#include "RadeonRegs.hpp"
#include "VGACard.hpp"
#include "i2c_spd.hpp"

/// The card's own microsecond clock (vertical position, engine timing).
inline long long radeon_clock_us() {
  using clock = std::chrono::steady_clock;
  static const auto t0 = clock::now();
  return std::chrono::duration_cast<std::chrono::microseconds>(clock::now() -
                                                               t0)
      .count();
}

class CRadeon : public CVGACard {
  friend class CRadeon3D;

public:
  CRadeon(CConfigurator *cfg, class CSystem *c, int pcibus, int pcidev);
  virtual ~CRadeon();

  virtual void init() override;

  virtual u32 ReadMem_Bar(int func, int bar, u32 address, int dsize) override;
  virtual void WriteMem_Bar(int func, int bar, u32 address, int dsize,
                            u32 data) override;

  uint32_t screen_update(bitmap_rgb32 &bitmap,
                         const rectangle &cliprect) override;

protected:
  // --- CVGACard hooks (Radeon.cpp) ---------------------------------------
  const char *card_name() const override { return "Radeon"; }
  const char *thread_tag() const override { return " radeon"; }
  u32 state_magic1() const override { return 0x52414445; } // 'RADE'
  u32 state_magic2() const override { return 0x45444152; }
  u8 io_read_b(u32 address) override;
  void io_write_b(u32 address, u8 data) override;
  void crtc_map(address_map &map) override {}
  void sequencer_map(address_map &map) override {}
  void gc_map(address_map &map) override {}
  void attribute_map(address_map &map) override {}
  int save_card_state(FILE *f) override;
  int restore_card_state(FILE *f) override;
  void post_restore() override;
  void recompute_params() override { state.vga_mem_updated = 1; }
  u32 config_read_custom(int func, u32 address, int dsize, u32 data) override;
  void config_write_custom(int func, u32 address, int dsize, u32 old_data,
                           u32 new_data, u32 data) override;

  // --- the register aperture (RadeonControl.cpp) -------------------------
  u32 reg_read(u32 offset, int bytes);
  void reg_write(u32 offset, int bytes, u32 data);
  /// A dword register's value as a read returns it (side effects included).
  u32 reg_read32(u32 reg);
  /// Store a dword register (the bytes written merged in by the caller)
  /// and act on it; `old` is its value before.
  void reg_write32(u32 reg, u32 data, u32 old, u32 byte_mask);
  u32 &R(u32 reg) {
    return m_regs[(reg & (radeon::REG_APERTURE_BYTES - 1)) >> 2];
  }
  u32 R(u32 reg) const {
    return m_regs[(reg & (radeon::REG_APERTURE_BYTES - 1)) >> 2];
  }
  u32 pll_read(u8 index);
  void pll_write(u8 index, u32 data);
  void palette_write_data(u32 data);
  u32 palette_read_data();
  void ddc_attach_monitor();
  void ddc_drive();
  u32 gpio_read(u32 reg) const;
  void card_tick() override;
  void update_int_line();
  /// The vertical position the CRTC is at now, from the card's clock and
  /// the current mode's totals; `in_vblank` set past the displayed lines.
  u32 current_vline(bool *in_vblank) const;

  // --- memory (RadeonMemory.cpp) -------------------------------------------
  u32 fb_read(u32 offset, int dsize);
  void fb_write(u32 offset, int dsize, u32 data);
  u32 vram_read(u32 addr, int bytes) const;
  void vram_write(u32 addr, int bytes, u32 data);
  u32 vram_mask() const { return m_vram_bytes - 1; }
  /// A memory-controller address (as the CRTC, cursor and engine take
  /// them) as an offset into VRAM: MC_FB_LOCATION places the framebuffer.
  u32 mc_to_vram(u32 mc) const;
  void refresh_direct_aperture();
  bool direct_framebuffer_active() const override { return m_direct_size != 0; }
  uint64_t direct_view_hash() const override;

  // --- the 2D engine (RadeonEngine.cpp) ------------------------------------
  /// True for registers the engine owns (0x1400-0x17ff and the host data
  /// ports); the write is handed to engine_write.
  static bool is_engine_reg(u32 reg);
  void engine_write(u32 reg, u32 data);
  u32 engine_read(u32 reg);
  void engine_reset();
  void engine_master_cntl(u32 data);
  void engine_rect();
  void engine_line(u32 reg);
  void engine_host_data(u32 data, bool last);
  void engine_pixel(int x, int y, u32 src, bool src_opaque);
  /// The engine finishes a command inside the write that starts it.
  bool engine_busy() const { return false; }

  // --- the command processor (RadeonCP.cpp) --------------------------------
  bool cp_reg_write(u32 reg, u32 data);
  bool cp_reg_read(u32 reg, u32 *v);
  bool cp_translate(u32 mc, bool *is_vram, u32 *addr) const;
  u32 cp_read32(u32 mc);
  void cp_write32(u32 mc, u32 data);
  void cp_run_ring();
  void cp_run_buffer(u32 mc, u32 dwords);
  void cp_feed(u32 d);
  void cp_packet3(u8 op, const std::vector<u32> &payload);
  /// A type-3 packet for the 3D engine; false when it is not one.
  bool r3d_packet3(u8 op, const std::vector<u32> &payload);

  // --- display (RadeonDisplay.cpp) -------------------------------------------
  bool native_crtc_active() const override;
  void determine_screen_dimensions(unsigned *height, unsigned *width) override;
  void palette_update() override;
  uint64_t hw_cursor_signature() const override;
  void render_native(bitmap_rgb32 &bitmap);
  void draw_hw_cursor(bitmap_rgb32 &bitmap);
  unsigned native_width() const;
  unsigned native_height() const;
  unsigned native_bytes_pp() const;
  unsigned native_pitch_bytes() const;
  u32 native_start() const;

public:
  /// The 2D engine's decoded state, kept beside the register file (which
  /// holds what was written, for read-back and the state file).
  struct engine_t {
    u32 dst_offset, dst_pitch; ///< bytes, MC address and bytes per line
    u32 src_offset, src_pitch;
    int dst_x, dst_y, src_x, src_y, width, height;
    int sc_left, sc_top, sc_right, sc_bottom; ///< inclusive
    int bpp;     ///< bytes a pixel of the destination
    u32 gmc;     ///< DP_GUI_MASTER_CNTL as last written
    u32 dp_cntl; ///< <0> left to right, <1> top to bottom
    // host data in flight
    bool host_active;
    int hx, hy; ///< the next pixel, relative to the rectangle
    bool host_mono;
    int line_pat; ///< a line's pattern position, -1 outside a line
  };

protected:
  // --- state ---------------------------------------------------------------
  u32 m_vram_bytes = 0;
  u32 m_regs[radeon::REG_APERTURE_BYTES / 4];
  u32 m_pll[radeon::PLL_REGS];
  engine_t eng;

  /// The packet in progress, across the dwords the CP is fed.
  struct cp_parser {
    u32 header = 0, remaining = 0, reg = 0, reg1 = 0;
    std::vector<u32> payload;
  };
  cp_parser m_cp;
  /// The 3D engine.
  std::unique_ptr<CRadeon3D> m_3d;
  int m_cp_depth = 0, m_cp_ib_depth = 0;
  u32 m_me_ram[256][2] = {}; ///< the CP microcode, kept for read-back
  u32 m_me_index = 0;
  bool m_cp_unknown_seen[256] = {};
  int m_cp_bad_reads = 0;

  /// The INTA line as this card last drove it, and the lock deciding it.
  bool m_int_asserted = false;
  std::mutex m_int_lock;
  long long m_last_vblank_frame = 0;

  /// The monitor on GPIO_VGA_DDC (a 24C02 with its EDID).
  I2CBus m_ddc;

  /// The framebuffer offered to the CPUs as plain memory while the first
  /// aperture is decoded and SURFACE_CNTL leaves it unswapped.
  u64 m_direct_base = 0, m_direct_size = 0;

  /// ALPHABOX_TRACE_RADEON: register accesses (bring-up aid). "1" prints
  /// each, capped at m_trace_budget lines; "new" only the first read and
  /// the first write of each register; "file:<path>" as "1", but only
  /// while <path> exists (polled every tick).
  bool m_trace = false;
  std::string m_trace_file;
  bool m_trace_new = false;
  long m_trace_budget = 0;
  u8 m_seen[2][radeon::REG_APERTURE_BYTES / 4] = {};
  const char *m_trace_path = "?";
  void trace_access(bool write, u32 reg, int bytes, u32 data);
};

#endif // !defined(INCLUDED_RADEON_H)
