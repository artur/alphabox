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

/* Cirrus Logic GD54xx family base.
 *
 * Standard VGA -- register decode, planar memory, rendering, the render
 * thread, option ROM and I/O routing -- comes from CVGACard/CVGA. This
 * class adds only what is Cirrus, one concern per translation unit:
 *
 *   CirrusGD54xx.cpp     construction, init(), PCI, I/O port routing
 *   CirrusSequencer.cpp  SR extended registers (lock, clocks, straps)
 *   CirrusGraphics.cpp   GR extended registers (banking, write-mode colours,
 *                        BitBLT register block)
 *   CirrusBlitter.cpp    BitBLT engine
 *   CirrusCRTC.cpp       CR extended registers (offsets, interlace, chip id)
 *   CirrusMemory.cpp     banked 0xa0000 window, linear aperture, extended
 *                        write modes 4/5
 *   CirrusDAC.cpp        hidden DAC, extended palette, colour depth
 *   CirrusCursor.cpp     hardware cursor
 *
 * A concrete chip (CirrusGD5430, CirrusGD5434, ...) supplies only a
 * cirrus_chip_config; the PCI header is built from it.
 */

#if !defined(INCLUDED_CIRRUS_GD54XX_H)
#define INCLUDED_CIRRUS_GD54XX_H

#include "CirrusBlitter.hpp"
#include "CirrusRegs.hpp"
#include "VGACard.hpp"

/**
 * \brief Per-chip parameters. A variant fills this in and the base does the
 * rest, so adding a GD5436 or GD5446 is a table entry rather than a new
 * device.
 */
struct cirrus_chip_config {
  const char *part;        ///< human-readable part, for startup messages
  u8 chip_id;              ///< CR27 value (id in bits 7..2, revision 1..0)
  u16 pci_device_id;       ///< PCI config space 0x02
  u32 vram_bytes;          ///< installed framebuffer memory (power of two)
  u32 linear_bytes;        ///< PCI BAR0 aperture size (power of two)
  u8 revision;             ///< PCI revision id
  u8 sr0f_strap;           ///< SR0F power-on value (DRAM configuration)
  u8 sr17_strap;           ///< SR17 power-on value (bus type straps)
  u8 sr1f_mclk;            ///< SR1F power-on memory clock
  const char *default_rom; ///< option ROM file when "rom" is not set
};

class CCirrusGD54xx : public CVGACard {
public:
  CCirrusGD54xx(CConfigurator *cfg, class CSystem *c, int pcibus, int pcidev,
                const cirrus_chip_config &chip);
  virtual ~CCirrusGD54xx();

  virtual void init() override;

  virtual u32 ReadMem_Bar(int func, int bar, u32 address, int dsize) override;
  virtual void WriteMem_Bar(int func, int bar, u32 address, int dsize,
                            u32 data) override;

  uint32_t screen_update(bitmap_rgb32 &bitmap,
                         const rectangle &cliprect) override;

protected:
  // --- CVGACard hooks (CirrusGD54xx.cpp) --------------------------------
  const char *card_name() const override { return "Cirrus"; }
  const char *thread_tag() const override { return " cirrus"; }
  u32 state_magic1() const override { return 0xC1A5D454; }
  u32 state_magic2() const override { return 0x454DA5C1; }
  u8 io_read_b(u32 address) override;
  void io_write_b(u32 address, u8 data) override;
  void attribute_map(address_map &map) override {}
  void recompute_params() override;
  uint16_t offset() override;
  bool get_interlace_mode() override;

  // --- sequencer (CirrusSequencer.cpp) ----------------------------------
  void sequencer_map(address_map &map) override;
  void sequencer_reset();

  // --- graphics controller (CirrusGraphics.cpp) -------------------------
  void gc_map(address_map &map) override;
  void graphics_reset();

  // --- CRTC (CirrusCRTC.cpp) --------------------------------------------
  void crtc_map(address_map &map) override;
  void crtc_reset();

  // --- memory apertures (CirrusMemory.cpp) ------------------------------
  uint8_t mem_r(offs_t offset) override;
  void mem_w(offs_t offset, uint8_t data) override;
  uint8_t mem_linear_r(offs_t offset) override;
  void mem_linear_w(offs_t offset, uint8_t data) override;
  void update_banks();
  u32 aperture_offset(u32 offset) const;
  void write_packed(u32 offset, u8 data);
  bool mmio_enabled_legacy() const;
  bool mmio_enabled_linear() const;
  u8 mmio_read(u32 offset);
  void mmio_write(u32 offset, u8 data);
  u32 vram_mask() const { return m_chip.vram_bytes - 1; }

  // --- BitBLT engine (CirrusBlitter.cpp) --------------------------------
  /// Mark the screen dirty if the blitter drew.
  void blt_sync() {
    if (m_blitter.take_drawn())
      state.vga_mem_updated = 1;
  }

  // --- DAC (CirrusDAC.cpp) ----------------------------------------------
  u8 dac_mask_read();
  void dac_mask_write(u8 data);
  bool dac_data_read(u8 &data);
  bool dac_data_write(u8 data);
  void define_video_mode();
  u32 ext_palette_color(int index) const;

  // --- hardware cursor (CirrusCursor.cpp) -------------------------------
  uint64_t hw_cursor_signature() const override;
  void draw_hw_cursor(bitmap_rgb32 &bitmap, const rectangle &cliprect);
  u16 cursor_x() const;
  u16 cursor_y() const;

  // --- state ------------------------------------------------------------
  const cirrus_chip_config m_chip;

  /// Extended graphics controller registers. GR00/GR01 are kept here at
  /// their full 8 bits (the standard handler keeps only the low nibble)
  /// because extended write modes 4/5 use them as colours.
  u8 m_gr[256] = {};

  /// The two 32 KB windows of the 0xa0000 aperture (GR09/GR0A, GR0B).
  u32 m_bank_base[2] = {0, 0};
  u32 m_bank_limit[2] = {0, 0};

  /// Hidden DAC: reads of the pixel mask arm it; see CirrusDAC.cpp.
  int m_dac_arm = 0;
  u8 m_hidden_dac = 0;

  /// Extended palette (SR12 bit 1): 16 entries, of which 0 and 15 are the
  /// hardware cursor background and foreground.
  u8 m_ext_palette[16 * 3] = {};

  /// Low three bits of the cursor position, taken from the SR10/SR11
  /// index used for the last write.
  u8 m_cursor_x_low = 0;
  u8 m_cursor_y_low = 0;

  /// The BitBLT engine works on m_gr and the VRAM. Not part of the state
  /// file: a blit completes within one register write or, for
  /// system-to-screen transfers, within the guest's following aperture
  /// writes.
  CCirrusBlitter m_blitter{m_gr, "cirrus"};

  /// Port 0x3c3 (video subsystem enable). Stored and read back only.
  u8 m_port_3c3 = 0x01;
};

#endif // !defined(INCLUDED_CIRRUS_GD54XX_H)
