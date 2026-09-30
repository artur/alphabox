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

/* S3 ViRGE family (86C325 ViRGE, 86C988 ViRGE/VX, 86C375 ViRGE/DX and
 * 86C385 ViRGE/GX, 86C357 ViRGE/GX2): an S3 SVGA with an integrated
 * RAMDAC and clock synthesiser, a streams processor for video overlay,
 * and the S3d engine -- 2D BitBLT, lines and polygons, and 3D triangles
 * with Z buffer, texture, fog and alpha blending -- all driven through a
 * 64 KB register window.
 *
 * Standard VGA -- register decode, planar memory, rendering, the render
 * thread, option ROM and I/O routing -- comes from CVGACard/CVGA. This
 * class adds what is ViRGE, one concern per translation unit:
 *
 *   S3Virge.cpp          chip table, construction, init(), PCI header,
 *                        the S3 CRTC and sequencer extensions and their
 *                        locks, the VGA ports, state file
 *   S3VirgeMemory.cpp    BAR0 (linear framebuffer and MMIO window), the
 *                        banked 0xa0000 window and old-style MMIO there,
 *                        the direct framebuffer offer
 *   S3VirgeControl.cpp   the MMIO register window: subsystem status and
 *                        interrupts, advanced function control, the
 *                        streams processor's registers, the serial port
 *                        (DDC), command DMA
 *   S3VirgeDisplay.cpp   the extended-mode display (CRTC or streams
 *                        processor), the clock synthesiser, the hardware
 *                        cursor, the secondary stream (overlay)
 *   S3VirgeEngine.cpp    the S3d engine's 2D commands: BitBLT (screen,
 *                        host colour and host monochrome sources),
 *                        rectangle fill, line, polygon fill, ROPs,
 *                        patterns, clipping, host data
 *   S3Virge3D.cpp        the S3d engine's 3D commands: Gouraud, textured
 *                        and lit triangles and 3D lines, Z buffer, fog,
 *                        alpha blending
 *
 * The S3 SVGA register set (CR2D-CR6F, SR08-SR18) is the Trio64's
 * descendant; the ViRGE drops the Trio's 8514-style drawing engine and
 * its I/O ports, which is why this is its own card and not a Trio64 with
 * extras. Register semantics are those of the ViRGE/DX and ViRGE/GX data
 * books; where those were unclear the behaviour follows 86Box's
 * vid_s3_virge.c (GPL-2.0-or-later) and MAME's s3virge.cpp (BSD-3-Clause)
 * as references -- this code is written anew -- and where the Windows 2000
 * driver's register traffic disagrees with any of them, the driver wins.
 */

#if !defined(INCLUDED_S3VIRGE_H)
#define INCLUDED_S3VIRGE_H

#include <mutex>
#include <set>

#include "S3VirgeRegs.hpp"
#include "VGACard.hpp"
#include "i2c_spd.hpp"

/// One row per part of the family ("chip" in the config).
struct virge_chip_config {
  const char *name; ///< config value
  const char *part; ///< for messages
  u16 pci_device_id;
  u8 revision;
  u32 vram_bytes; ///< default memory
  const char *default_rom;
  bool vx;  ///< ViRGE/VX: 8 MB decode, 32 MB BAR, its own memory straps
  bool dx;  ///< DX/GX or later: 3-bit PLL R, 2 VCLK per 16-bit pixel
  bool gx2; ///< GX2: streams processor always on, different overlay
};

const virge_chip_config *virge_chip_by_name(const char *name);

/// What a 2D command draws with, decoded from its registers when it starts.
struct virge_draw {
  u32 cmd;
  int bytes; ///< per pixel
  u32 pmask; ///< a pixel's bits
  u32 dest_base, src_base;
  u32 dest_stride, src_stride;
  s32 clip_l, clip_r, clip_t, clip_b;
  bool clip;
  u8 rop;
  /// Rectangle fills and lines: the pattern is forced to its foreground
  /// colour, whatever the command says (data book 15.4.4.2-3).
  bool solid;
};

class CS3Virge : public CVGACard {
public:
  CS3Virge(CConfigurator *cfg, class CSystem *c, int pcibus, int pcidev,
           const virge_chip_config &chip);
  virtual ~CS3Virge();

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
  // --- CVGACard hooks (S3Virge.cpp) ----------------------------------------
  const char *card_name() const override { return "ViRGE"; }
  const char *thread_tag() const override { return " virge"; }
  u32 state_magic1() const override { return 0x56495247; } // 'VIRG'
  u32 state_magic2() const override { return 0x47524956; }
  u8 io_read_b(u32 address) override;
  void io_write_b(u32 address, u8 data) override;
  void crtc_map(address_map &map) override;
  void sequencer_map(address_map &map) override;
  void gc_map(address_map &map) override {}
  void attribute_map(address_map &map) override {}
  void apply_extended_timing(int &h, int &v) override;
  bool atc_palette_locked() const override;
  int save_card_state(FILE *f) override;
  int restore_card_state(FILE *f) override;
  void post_restore() override;
  void recompute_params() override { state.vga_mem_updated = 1; }

  u8 cr(u8 index) const { return vga.crtc.data[index]; }
  u8 sr(u8 index) const { return vga.sequencer.data[index]; }
  bool cr_unlocked_s3() const { return (cr(CR_LOCK_1_IDX) & 0xcc) == 0x48; }
  bool cr_unlocked_ext() const { return (cr(CR_LOCK_2_IDX) & 0xe0) == 0xa0; }
  void crtc_ext_write(u8 index, u8 data);
  u8 crtc_ext_read(u8 index);

  // --- memory (S3VirgeMemory.cpp) ------------------------------------------
  u32 bar0_read(u32 offset, int dsize);
  void bar0_write(u32 offset, int dsize, u32 data);
  bool lfb_enabled() const;
  bool new_mmio_enabled() const;
  bool old_mmio_enabled() const;
  u32 vram_read(u32 addr, int bytes) const;
  void vram_write(u32 addr, int bytes, u32 data);
  u32 vram_mask() const { return m_vram_bytes - 1; }
  uint8_t mem_r(offs_t offset) override;
  void mem_w(offs_t offset, uint8_t data) override;
  u32 legacy_read(u32 address, int dsize) override;
  void legacy_write(u32 address, int dsize, u32 data) override;
  void refresh_direct_lfb();
  bool direct_framebuffer_active() const override { return m_direct_size != 0; }

  // --- the MMIO window (S3VirgeControl.cpp) ---------------------------------
  u32 mmio_read(u32 offset, int dsize);
  void mmio_write(u32 offset, int dsize, u32 data);
  u32 mmio_read_dword(u32 offset);
  void mmio_write_dword(u32 offset, u32 data);
  u32 &M(u32 offset) { return r.mmio[(offset & 0x7ffc) >> 2]; }
  u32 M(u32 offset) const { return r.mmio[(offset & 0x7ffc) >> 2]; }
  u8 serial_port_read() const;
  void serial_port_write(u8 data);
  void ddc_attach_monitor();
  void card_tick() override;
  void update_int_line();
  void command_dma_run();

  // --- display (S3VirgeDisplay.cpp) ----------------------------------------
  bool native_crtc_active() const override;
  bool streams_active() const;
  void determine_screen_dimensions(unsigned *height, unsigned *width) override;
  uint64_t direct_view_hash() const override;
  uint64_t hw_cursor_signature() const override;
  unsigned native_bits_per_pixel() const;
  unsigned native_width() const;
  unsigned native_height() const;
  u32 native_start() const;
  u32 native_pitch() const;
  void render_native(bitmap_rgb32 &bitmap);
  void draw_hw_cursor(bitmap_rgb32 &bitmap);
  void draw_overlay(bitmap_rgb32 &bitmap);
  void update_clock();

  // --- the S3d engine: 2D (S3VirgeEngine.cpp) --------------------------------
  void engine_reset();
  void engine_register_written(u32 offset, u32 data);
  void s2d_start(u32 cmd);
  void s2d_host_data(u32 data, int bytes);
  void s2d_host_line(const u8 *line);
  void s2d_bitblt_screen();
  void s2d_rect();
  void s2d_line();
  void s2d_poly();
  void s2d_pixel(s32 x, s32 y, u32 src, bool have_src);
  u32 s2d_pattern(s32 x, s32 y) const;
  // --- the S3d engine: 3D (S3Virge3D.cpp) ------------------------------------
public:
  struct S3dCtx;
  struct S3dRgba {
    int r, g, b, a;
  };

protected:
  void s3d_start(u32 cmd);
  void s3d_setup(S3dCtx &c) const;
  S3dRgba s3d_texel(const S3dCtx &c, int lv, s32 iu, s32 iv) const;
  S3dRgba s3d_sample_level(const S3dCtx &c, int lv, s32 u, s32 v,
                           bool bilinear) const;
  S3dRgba s3d_sample(const S3dCtx &c, s32 u, s32 v, s32 d) const;
  void s3d_pixel(const S3dCtx &c, u32 dest, const s32 *attr);
  void s3d_span(const S3dCtx &c, s32 y, s32 x, s32 xe, int step, s32 *attr,
                const s32 *dx);
  void s3d_triangle();
  void s3d_line();

  /// Written to the state file verbatim.
  struct {
    /// The MMIO window's registers from 0x8000 to 0xffff, by (offset &
    /// 0x7fff) / 4: the streams processor, system control, the S3d
    /// engine's pattern RAM and register sets, the serial port. What a read
    /// returns and what the engine and display run from.
    u32 mmio[0x2000];

    u8 bank;     ///< the 64 KB bank of the 0xa0000 window
    u8 port_3c3; ///< video subsystem enable
    u8 cursor_fg[4], cursor_bg[4];
    u8 cursor_fg_pos, cursor_bg_pos;
    u8 serial_port;
    u32 subsys_status; ///< interrupt status, bits 7..0
    u32 subsys_enable; ///< interrupt enables, bits 7..0
    u32 vblank_seen;
    double dclk_hz;

    /// The 2D engine mid-command: a BitBLT waiting for host data.
    struct {
      bool active;    ///< waiting for host data
      u32 cmd;        ///< the command register it started with
      s32 dx, dy;     ///< the next destination pixel
      s32 x0;         ///< where each line starts
      s32 w, h;       ///< pixels a line, lines left
      u32 line_bytes; ///< host bytes of one line's pixels
      u32 line_fill;  ///< host bytes of the current line so far
      u32 align;      ///< each line starts on this boundary of the stream
      u32 stream_pos; ///< host bytes taken since the command started
      u32 skip;       ///< bytes still to drop: first-dword offset, padding
      s32 poly_left, poly_right; ///< a polygon's edges, 12.20
      u8 line[8192 + 8];
    } e;
  } r;

  static constexpr u8 CR_LOCK_1_IDX = virge::CR_LOCK_1;
  static constexpr u8 CR_LOCK_2_IDX = virge::CR_LOCK_2;

  virge_draw m_draw = {}; ///< the running 2D command, decoded
  const virge_chip_config m_chip;
  u32 m_vram_bytes = 0;
  I2CBus m_ddc;
  u64 m_direct_base = 0;
  u64 m_direct_size = 0;
  std::mutex m_int_lock;
  bool m_int_asserted = false;
  /// ALPHABOX_TRACE_VIRGE (see init()): every access (switched by the
  /// render thread when a trace file comes and goes), the engine's
  /// commands, the display mode.
  std::atomic<bool> m_trace{false};
  bool m_trace_cmd = false;
  bool m_trace_mode = false;
  std::string m_trace_file;
  int m_trace_poll = 0;
  std::string m_last_mode;
  std::set<std::string> m_unimplemented_seen;
  void unimplemented(const std::string &what);
};

#endif // !defined(INCLUDED_S3VIRGE_H)
