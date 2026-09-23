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

/* 3Dlabs Permedia 2 (TI TVP4020): an SVGA, a 2D/3D graphics processor and
 * a RAMDAC on one chip, sharing up to 8 MB of SGRAM.
 *
 * Standard VGA -- register decode, planar memory, rendering, the render
 * thread, option ROM and I/O routing -- comes from CVGACard/CVGA. This
 * class adds what is Permedia 2, one concern per translation unit:
 *
 *   Permedia2.cpp         construction, init(), PCI header (and its
 *                         indirect window onto the regions), the SVGA
 *                         extensions, region routing, state file
 *   Permedia2Control.cpp  region 0: control status, memory control, the
 *                         video timing generator, the RAMDAC, DDC
 *   Permedia2Memory.cpp   the two 8 MB memory apertures and the 0xa0000
 *                         window
 *   Permedia2Display.cpp  the graphics processor's display: the timing
 *                         generator's frame through the RAMDAC, and the
 *                         RAMDAC's hardware cursor
 *
 * The SVGA and the graphics processor's display are exclusive: VGAControl
 * (sequencer index 5) bit 3 hands the RAMDAC to one or the other. The
 * SVGA reaches no further than standard VGA plus two 256-colour VESA
 * modes; everything beyond is the graphics processor's, programmed
 * through region 0.
 *
 * Register semantics are those of the TVP4020 Hardware Reference Manual
 * and the Permedia 2 Programmer's Reference Manual (3Dlabs).
 */

#if !defined(INCLUDED_PERMEDIA2_H)
#define INCLUDED_PERMEDIA2_H

#include <mutex>

#include "Permedia2Regs.hpp"
#include "VGACard.hpp"
#include "i2c_spd.hpp"

class CPermedia2 : public CVGACard {
public:
  CPermedia2(CConfigurator *cfg, class CSystem *c, int pcibus, int pcidev);
  virtual ~CPermedia2();

  virtual void init() override;

  virtual u32 ReadMem_Bar(int func, int bar, u32 address, int dsize) override;
  virtual void WriteMem_Bar(int func, int bar, u32 address, int dsize,
                            u32 data) override;
  u32 config_read_custom(int func, u32 address, int dsize, u32 data) override;
  void config_write_custom(int func, u32 address, int dsize, u32 old_data,
                           u32 new_data, u32 data) override;

  uint32_t screen_update(bitmap_rgb32 &bitmap,
                         const rectangle &cliprect) override;

protected:
  // --- CVGACard hooks (Permedia2.cpp) --------------------------------------
  const char *card_name() const override { return "Permedia2"; }
  const char *thread_tag() const override { return " permedia2"; }
  u32 state_magic1() const override { return 0x50324D50; } // 'PM2P'
  u32 state_magic2() const override { return 0x504D3250; }
  u8 io_read_b(u32 address) override;
  void io_write_b(u32 address, u8 data) override;
  void crtc_map(address_map &map) override {}
  void sequencer_map(address_map &map) override;
  void gc_map(address_map &map) override;
  void attribute_map(address_map &map) override {}
  int save_card_state(FILE *f) override;
  int restore_card_state(FILE *f) override;
  void post_restore() override;
  void recompute_params() override { state.vga_mem_updated = 1; }

  /// A region access by its PCI region number (0, 1, 2; 7 is the ROM), as
  /// the BARs and the configuration-space window both address them.
  u32 region_read(int region, u32 offset, int dsize);
  void region_write(int region, u32 offset, int dsize, u32 data);

  // --- region 0 (Permedia2Control.cpp) --------------------------------------
  /// `offset` within the 64 KB control space, a whole access; the caller
  /// has undone the byte-swapped mirror.
  u32 control_read(u32 offset, int bytes);
  void control_write(u32 offset, int bytes, u32 data);
  u32 control_read_dword(u32 offset);
  void control_write_dword(u32 offset, u32 data);
  u8 ramdac_read(int reg);
  void ramdac_write(int reg, u8 data);
  u8 ramdac_indexed_read(u8 index) const;
  void ramdac_indexed_write(u8 index, u8 data);
  u32 line_count() const;
  u32 display_data_read() const;
  void ddc_attach_monitor();
  void card_tick() override;
  void update_int_line();

  // --- memory (Permedia2Memory.cpp) ----------------------------------------
  u32 aperture_read(int which, u32 offset, int dsize);
  void aperture_write(int which, u32 offset, int dsize, u32 data);
  u32 vram_read(u32 addr, int bytes) const;
  void vram_write(u32 addr, int bytes, u32 data);
  u32 vram_mask() const { return m_vram_bytes - 1; }
  uint8_t mem_r(offs_t offset) override;
  void mem_w(offs_t offset, uint8_t data) override;
  void refresh_direct_aperture();

  // --- display (Permedia2Display.cpp) ----------------------------------------
  bool native_crtc_active() const override;
  bool display_enabled() const override { return true; }
  void determine_screen_dimensions(unsigned *height, unsigned *width) override;
  void palette_update() override;
  uint64_t direct_view_hash() const override;
  bool direct_framebuffer_active() const override { return m_direct_size != 0; }
  uint64_t hw_cursor_signature() const override;
  unsigned native_width() const;
  unsigned native_height() const;
  unsigned native_bits_per_pixel() const;
  void render_native(bitmap_rgb32 &bitmap);
  void draw_hw_cursor(bitmap_rgb32 &bitmap);

  u32 reg(u32 offset) const { return r.ctl[offset >> 2]; }

  /// Written to the state file verbatim.
  struct {
    /// Region 0's dwords below the graphics processor (0x0000-0x7fff),
    /// by offset / 4: control status, memory control, video timing, video
    /// streams. The RAMDAC and SVGA blocks are not stored here.
    u32 ctl[permedia2::R0_CONTROL_WORDS];
    /// The graphics processor's registers (0x8000-0xffff), by offset / 4.
    u32 gp[(permedia2::REGION0_HALF - permedia2::R0_GP) / 4];

    // The SVGA's two extended registers.
    u8 vga_control; ///< SR5
    u8 mode640;     ///< GR9

    // The RAMDAC. Its palette is the VGA core's (vga.dac): both paths reach
    // the same RAM, the SVGA's through ports 0x3c6-0x3c9.
    u8 rd_index; ///< the write address as the index of the indirect file
    u8 rd_indexed[256];
    u8 cursor_ram[1024]; ///< plane 0 at 0x000, plane 1 at 0x200
    u8 cursor_color[4][3];
    u8 cursor_color_addr;
    u8 cursor_color_state; ///< which of R, G, B comes next
    u8 cursor_x_lo, cursor_x_hi, cursor_y_lo, cursor_y_hi;

    u32 vblank_seen; ///< the last frame whose vertical retrace was flagged
  } r;

  u32 m_vram_bytes = 0;
  I2CBus m_ddc;
  u64 m_direct_base = 0;
  u64 m_direct_size = 0;
  /// IntFlags is set from the render thread (card_tick) and acknowledged
  /// from the CPUs'.
  std::mutex m_int_lock;
  bool m_int_asserted = false;
  bool m_trace = false;
};

#endif // !defined(INCLUDED_PERMEDIA2_H)
