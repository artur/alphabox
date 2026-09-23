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

/* ATI Mach64 CT/VT family.
 *
 * Standard VGA -- register decode, planar memory, rendering, the render
 * thread, option ROM and I/O routing -- comes from CVGACard/CVGA. This
 * class adds what is Mach64, one concern per translation unit:
 *
 *   Mach64.cpp         construction, init(), PCI header, I/O and aperture
 *                      routing, state file
 *   Mach64Control.cpp  the register block: CRTC, cursor, clock, memory
 *                      banks, DAC, EEPROM, configuration, status
 *   Mach64Memory.cpp   the 0xa0000 window (VGA or banked), the memory
 *                      aperture, the register pages
 *   Mach64Engine.cpp   the GUI drawing engine: rectangles, lines, blits,
 *                      host data, patterns, colour compare
 *   Mach64Display.cpp  extended-mode timing and rendering, the hardware
 *                      cursor, the 8-bit DAC
 *   Mach64DDC.cpp      the monitor on the DDC lines (an EDID EEPROM)
 *
 * The CT (1994) is the first Mach64 with the DAC and clock synthesizer on
 * the chip; the VT parts keep its register file and add a video overlay
 * (stored here, not drawn). A mach64_chip_config supplies a part's
 * identity; the ES40 SRM console's own PCI table names only the CT.
 *
 * Register semantics are modelled on 86Box's vid_ati_mach64.c and
 * vid_ati_mach64_accel.c (GPL-2; Sarah Walker, Miran Grca, Connor Hyde),
 * against which the BIOS images this device runs are known to boot. The
 * engine is executed synchronously -- a command completes inside the write
 * that starts it -- but it reports itself busy for as long as the part
 * would have taken (see engine_charge).
 */

#if !defined(INCLUDED_MACH64_H)
#define INCLUDED_MACH64_H

#include <chrono>
#include <mutex>

#include "Eeprom93cx6.hpp"
#include "Mach64Regs.hpp"
#include "VGACard.hpp"
#include "i2c_spd.hpp"

/// The card's own microsecond clock, one origin for every register that
/// uses it and for the engine's timing. An inline function, so the two
/// translation units share the origin.
inline long long mach64_clock_us() {
  using clock = std::chrono::steady_clock;
  static const auto t0 = clock::now();
  return std::chrono::duration_cast<std::chrono::microseconds>(clock::now() -
                                                               t0)
      .count();
}

/**
 * \brief Per-chip parameters. A variant is a table entry rather than a
 * class.
 */
struct mach64_chip_config {
  const char *name;   ///< config "chip" value ("ct", "vt2")
  const char *part;   ///< human-readable part, for startup messages
  u16 pci_device_id;  ///< PCI config space 0x02
  u32 config_chip_id; ///< CONFIG_CHIP_ID: type in bits 15..0, revision 31..24
  u8 revision;        ///< PCI revision id
  u32 vram_bytes;     ///< default framebuffer memory
  const char *default_rom; ///< option ROM file when "rom" is not set
  bool gt;            ///< a 3D RAGE (II/II+): the trapezoid engine
  bool pro;           ///< a 3D RAGE PRO: the triangle setup engine
  bool aux_regs;      ///< BAR2: the 4 KB auxiliary register aperture
};

/// The chips this device can be (see Mach64.cpp); nullptr when unknown.
const mach64_chip_config *mach64_chip_by_name(const char *name);

class CMach64 : public CVGACard {
public:
  CMach64(CConfigurator *cfg, class CSystem *c, int pcibus, int pcidev,
          const mach64_chip_config &chip);
  virtual ~CMach64();

  virtual void init() override;

  virtual u32 ReadMem_Bar(int func, int bar, u32 address, int dsize) override;
  virtual void WriteMem_Bar(int func, int bar, u32 address, int dsize,
                            u32 data) override;

  uint32_t screen_update(bitmap_rgb32 &bitmap,
                         const rectangle &cliprect) override;

protected:
  // --- CVGACard hooks (Mach64.cpp) ---------------------------------------
  const char *card_name() const override { return "Mach64"; }
  const char *thread_tag() const override { return " mach64"; }
  u32 state_magic1() const override { return 0x4D414348; } // 'MACH'
  u32 state_magic2() const override { return 0x48434D41; }
  u8 io_read_b(u32 address) override;
  u8 io_read_b_traced(u32 address);
  void io_write_b(u32 address, u8 data) override;
  u32 card_legacy_read(int index, u32 address, int dsize) override;
  void card_legacy_write(int index, u32 address, int dsize, u32 data) override;
  void crtc_map(address_map &map) override {}
  void sequencer_map(address_map &map) override {}
  void gc_map(address_map &map) override {}
  void attribute_map(address_map &map) override {}
  int save_card_state(FILE *f) override;
  int restore_card_state(FILE *f) override;
  void post_restore() override;
  void recompute_params() override { state.vga_mem_updated = 1; }

  /// Legacy range ids beyond the base's: the sparse I/O groups, then the
  /// ATI extended VGA index/data pair.
  enum {
    LEGACY_IO_SPARSE = LEGACY_CARD_FIRST,
    LEGACY_IO_ATI_EXT = LEGACY_CARD_FIRST + mach64::SPARSE_IO_GROUPS
  };

  // --- register block (Mach64Control.cpp) ---------------------------------
  /// `offset` is within the 2 KB block (bit 10 = block 0).
  u32 reg_read(u32 offset, int bytes);
  void reg_write(u32 offset, int bytes, u32 data);
  u8 reg_read8(u32 offset);
  void reg_write8(u32 offset, u8 data);
  /// Side effects of a block-0 register once its bytes are stored.
  void reg_written(u32 reg);
  void card_tick() override;
  /// Drive INTA from the interrupt the CRTC has latched and the enable
  /// beside it. Called wherever either can change: the tick that latches a
  /// vertical blank, the read that latches one, and the write that
  /// acknowledges or masks it. Call it with m_int_lock held.
  void update_int_line();
  u8 crtc_int_cntl_read();
  u32 config_cntl_read();
  void eeprom_clock();

  // --- the monitor's DDC channel (Mach64DDC.cpp) ----------------------------
  /// DAC_CNTL byte 3 carries the two general-purpose I/O lines the DDC
  /// bus hangs on; the monitor answers with its EDID.
  void ddc_attach_monitor();
  void ddc_drive();
  void i2c_engine_command(u8 cmd);

  // --- the Rage Pro's setup engine (Mach64Setup.cpp) ----------------------
  void setup_written(u32 idx);
  void setup_triangle();
  void i2c_line(bool scl, bool sda);
  u8 dac_gio_read(u8 byte3) const;
  void pll_write(int lane, u8 data);
  u8 pll_read(int lane) const;
  void update_banks();

  // --- memory (Mach64Memory.cpp) ---------------------------------------------
  u32 legacy_read(u32 address, int dsize) override;
  void legacy_write(u32 address, int dsize, u32 data) override;
  uint8_t mem_r(offs_t offset) override;
  void mem_w(offs_t offset, uint8_t data) override;
  /// VGA aperture mode: the register block sits at the top of the 0xa0000
  /// window (0xbf800).
  bool vga_aperture_enabled() const {
    return (r.config_cntl & mach64::CFG_MEM_VGA_AP_EN) != 0;
  }
  /// The two banked 32 KB apertures replace the VGA's memory at 0xa0000
  /// only when memory is addressed linearly, not in the VGA's planes
  /// (RRG-G02700, MEM_VGA_WP_SEL: "Apertures exist only in accelerator
  /// modes, and only if CFG_MEM_VGA_AP_EN is set"). Linear means either the
  /// Mach64's own CRTC drives the screen or CRTC_VGA_LINEAR is set -- the
  /// second is how the Rage II+'s BIOS and its Windows miniport size the
  /// memory, probing through the banks with the display still in VGA mode.
  bool banked_window_active() const {
    return vga_aperture_enabled() &&
           (r.crtc_gen_cntl &
            (mach64::CRTC_EXT_DISP_EN | mach64::CRTC_VGA_LINEAR)) != 0;
  }
  u32 window_offset(u32 offset, bool write) const;
  u32 aperture_read(u32 offset, int dsize);
  void aperture_write(u32 offset, int dsize, u32 data);
  u32 vram_read(u32 addr, int width) const;
  void vram_write(u32 addr, int width, u32 data);
  u32 vram_mask() const { return m_vram_bytes - 1; }

  // --- GUI engine (Mach64Engine.cpp) ------------------------------------
  void engine_write8(u32 reg, u8 data);
  void engine_write16(u32 reg, u16 data);
  void engine_write32(u32 reg, u32 data);
  void engine_set_gui_engine();
  void engine_load_context();
  void engine_start_rect();
  void engine_start_line();
  void engine_run(u32 host_data, int count);
  void engine_run_rect(u32 host_data, int count);
  void engine_run_line(u32 host_data, int count);
  bool engine_colour_compare(u32 src, u32 dst) const;
  u32 engine_mix(int mix, u32 src, u32 dst) const;
  u32 engine_source(int sel, u32 host, int src_x, int src_y, int dst_x,
                    int dst_y) const;

  // --- the 3D RAGE (Mach64Engine3D.cpp) ------------------------------------
  bool is_gt() const { return m_chip.gt; }
  /// A part with a 3D register file (GT or Rage Pro): registers without a
  /// field of their own are kept in regs_t::gt and read back.
  bool has_3d_regs() const { return m_chip.gt || m_chip.pro; }
  /// A GT register by its canonical address.
  u32 &gt_reg(u32 reg) { return r.gt[(mach64::gt_canonical(reg) & 0x3fc) >> 2]; }
  u32 gt_reg(u32 reg) const {
    return r.gt[(mach64::gt_canonical(reg) & 0x3fc) >> 2];
  }
  /// DST_BRES_LNTH as a GT takes it: the leading edge's length, the
  /// trailing edge's start and the choice between a line, a trapezoid and
  /// nothing, all in one write.
  void gt_bres_lnth_written();
  void engine_start_trap();

  // --- display (Mach64Display.cpp) ---------------------------------------
  bool native_crtc_active() const override;
  void determine_screen_dimensions(unsigned *height, unsigned *width) override;
  bool display_enabled() const override;
  void palette_update() override;
  uint64_t hw_cursor_signature() const override;
  void render_native(bitmap_rgb32 &bitmap);
  void draw_hw_cursor(bitmap_rgb32 &bitmap);
  unsigned native_width() const;
  unsigned native_height() const;
  unsigned native_pitch_bytes() const;
  u8 native_bpp_code() const;
  u32 vblank_frame() const;

public:
  /// The register file, written to the state file verbatim. (Public for
  /// the register-lookup helpers in Mach64Control.cpp.)
  struct regs_t {
    // block 0: CRTC and control
    u32 crtc_h_total_disp, crtc_h_sync_strt_wid;
    u32 crtc_v_total_disp, crtc_v_sync_strt_wid;
    u32 crtc_vline, crtc_off_pitch;
    u8 crtc_int_cntl;
    u32 crtc_gen_cntl;
    u32 dsp_config, dsp_on_off;
    u32 ovr_clr, ovr_wid_left_right, ovr_wid_top_bottom;
    u32 vga_dsp_config, vga_dsp_on_off;
    u32 cur_clr0, cur_clr1, cur_offset, cur_horz_vert_posn, cur_horz_vert_off;
    u32 gp_io;
    u32 scratch_reg0, scratch_reg1;
    u32 clock_cntl;
    u32 bus_cntl;
    u32 mem_cntl, mem_vga_wp_sel, mem_vga_rp_sel;
    u32 dac_cntl;
    u32 gen_test_cntl;
    u32 config_cntl, config_chip_id, config_stat0, config_stat1;
    // block 0: GUI engine
    u32 dst_off_pitch, dst_y_x, dst_height_width;
    u32 dst_bres_lnth, dst_bres_err, dst_bres_inc, dst_bres_dec, dst_cntl;
    u32 src_off_pitch, src_y_x, src_height1_width1, src_y_x_start;
    u32 src_height2_width2, src_cntl;
    u32 host_cntl;
    u32 pat_reg0, pat_reg1, pat_cntl;
    u32 sc_left_right, sc_top_bottom;
    u32 dp_bkgd_clr, dp_frgd_clr, write_mask, chain_mask;
    u32 dp_pix_width, dp_mix, dp_src, dp_set_gui_engine;
    u32 clr_cmp_clr, clr_cmp_mask, clr_cmp_cntl;
    u32 context_mask, context_load_cntl, gui_traj_cntl;
    // block 1: overlay and scaler (stored, not drawn)
    u32 block1[0x100];
    // the rest of the chip
    u8 ext_index;    ///< 0x1ce
    u8 ext_regs[64]; ///< 0x1cf
    u8 pll_addr;
    u8 pll_regs[16];
    u8 port_3c3;
    u32 bank_r[2], bank_w[2]; ///< byte offsets of the two 32 KB windows
    u32 vblank_seen; ///< frame at which CRTC_VBLANK_INT was last raised
    /// The 3D RAGE's block-0 registers that have no field above -- the
    /// trailing edge, Z, the texture map and the interpolators -- by dword,
    /// at their canonical address (mach64::gt_canonical). Unused on a CT or
    /// VT, which have none of them.
    u32 gt[256];
  };

  /// The engine's working state for the command in flight; a host-data
  /// command spans the aperture writes that feed it.
  struct accel_t {
    enum { OP_RECT, OP_LINE } op;
    bool busy;
    int dst_x, dst_y, dst_x_start, dst_y_start;
    int src_x, src_y, src_x_start, src_y_start;
    int xinc, yinc;
    int x_count, y_count, xx_count;
    int src_x_count, src_y_count;
    int src_width1, src_height1, src_width2, src_height2;
    u32 src_offset, src_pitch, dst_offset, dst_pitch;
    int mix_bg, mix_fg;
    int source_bg, source_fg, source_mix;
    bool source_host;
    int dst_width, dst_height;
    u8 pattern[8][8];
    u8 pattern_clr4x2[2][4];
    u8 pattern_clr8x1[8];
    int sc_left, sc_right, sc_top, sc_bottom;
    int dst_pix_width, src_pix_width, host_pix_width;
    int dst_size, src_size, host_size;
    u32 dp_bkgd_clr, dp_frgd_clr, write_mask;
    u32 clr_cmp_clr, clr_cmp_mask;
    int clr_cmp_fn;
    bool clr_cmp_src;
    int err;
    bool poly_draw;
  };

protected:
  // --- state ---------------------------------------------------------------
  const mach64_chip_config m_chip;
  u32 m_vram_bytes = 0;
  regs_t r;
  accel_t accel;

  /// When the drawing engine will have finished what it was given.
  ///
  /// It draws instantly -- a command is complete by the time the register
  /// write that started it returns -- but a chip that answered "idle" to
  /// every question would be a chip no driver could pace itself against.
  /// So the work is charged at the rate the part draws it, and the engine
  /// reports busy until that time has passed. The pixels are already on
  /// the screen; what is being modelled is only when the card admits to
  /// being ready for more.
  long long m_engine_busy_until_us = 0;
  /// A Mach64 CT draws about one pixel per engine clock at 60 MHz, and
  /// takes a few clocks to set a command up.
  static constexpr long long kEngineNsPerPixel = 17;
  static constexpr long long kEngineSetupNs = 300;
  void engine_charge(uint64_t pixels);
  bool engine_busy() const { return mach64_clock_us() < m_engine_busy_until_us; }

  /// INTA as this card is currently driving it, so the line is only moved
  /// when it changes.
  bool m_int_asserted = false;
  /// The latch, the enable and the line they drive are touched by the
  /// card's own thread and by a processor reading or acknowledging the
  /// register. Interleaved, the two can decide in one order and move the
  /// line in the other, and a level interrupt left disagreeing with the
  /// latch stays that way: nothing asks again while the answer has not
  /// changed. One lock over deciding and driving keeps them in step.
  std::mutex m_int_lock;

  /// The 93C66 the BIOS keeps the card's settings in (256 x 16).
  CEeprom93cx6 m_eeprom;

  /// The DDC bus to the monitor, bit-banged through DAC_CNTL; the monitor
  /// is a 24C02 holding its EDID.
  I2CBus m_ddc;

  /// The little-endian half of the memory aperture, offered to the CPUs as
  /// plain memory. Through it the card does nothing a store would not --
  /// the bytes land in VRAM -- so a guest drawing into the framebuffer need
  /// not trap on every access. Only the VRAM part is offered: the register
  /// page at the top of the half, the all-ones reads beyond the installed
  /// memory and the byte-swapping big-endian half still come here.
  u64 m_direct_base = 0, m_direct_size = 0;
  void refresh_direct_aperture();
  bool direct_framebuffer_active() const override {
    return m_direct_size != 0;
  }
  uint64_t direct_view_hash() const override;

  /// ALPHABOX_TRACE_MACH64: print every register and configuration access
  /// (bring-up aid; the framebuffer itself is not traced). With the value
  /// "new", only the first read and the first write of each register --
  /// what a driver touches, without the millions of lines a boot makes.
  bool m_trace = false;
  bool m_trace_new = false;
  /// ALPHABOX_TRACE_MACH64=trap: every register a trapezoid is drawn from,
  /// for the first 64 of them -- the triangle setup the driver computed.
  bool m_trace_trap = false;
  int m_traps_traced = 0;
  u8 m_seen[0x1000] = {}; ///< [write][2 KB offset / 4] of trace "new"
  /// Accesses per register since the last report: a driver spinning on
  /// one shows up as the top line every million accesses.
  u32 m_hits[0x442] = {}; ///< registers, VGA ports 0x3c0..0x3ff, ROM, config
  u32 m_hits_total = 0;
  void trace_first(u32 offset, bool write, u32 data);
  void trace_hit(u32 key);
  const char *m_trace_path = "?"; ///< which way the traced access came
  u32 config_read_custom(int func, u32 address, int dsize, u32 data) override;
  void config_write_custom(int func, u32 address, int dsize, u32 old_data,
                           u32 new_data, u32 data) override;
};

#endif // !defined(INCLUDED_MACH64_H)
