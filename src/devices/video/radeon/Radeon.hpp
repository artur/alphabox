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
 *   RadeonControl.cpp  the register aperture: MM_INDEX/MM_DATA, memory
 *                      controller, CRTC status, DAC and palette, DDC
 *                      lines, interrupts, configuration mirrors
 *   RadeonTiming.cpp   the PLLs, the pixel clock and the CRTC timing it
 *                      gives, the memory's power-up status
 *   RadeonMemory.cpp   the framebuffer aperture and the 0xa0000 window
 *   RadeonEngine.cpp   the 2D engine (GUI master control, fills, blits,
 *                      host data, lines)
 *   RadeonQueue.cpp    the command FIFO and the engine thread that drains
 *                      it and the CP's streams; busy status, WAIT_UNTIL,
 *                      the destination cache controls, the idle interrupt
 *   RadeonCP.cpp       the command processor: the ring buffer, indirect
 *                      buffers, the PIO queue, the micro-engine RAM and
 *                      the packets they carry
 *   RadeonGart.cpp     the memory controller's view of the bus: the AGP
 *                      window and the card's own PCI GART
 *   RadeonChips.cpp    the part's facts (RadeonChip.hpp)
 *   RadeonDisplay.cpp  the primary CRTC's extended modes, the hardware
 *                      cursor, the 8-bit palette
 *   Radeon3D.cpp, RadeonTcl.cpp, RadeonRaster.cpp
 *                      the 3D engine (CRadeon3D, Radeon3D.hpp): vertex
 *                      fetch and the 3D packets, TCL, rasteriser and
 *                      pixel pipeline
 *   RadeonSelfTest.cpp ALPHABOX_RADEON_SELFTEST: the engines driven
 *                      through the registers and the CP, checked
 *                      against software references
 *   RadeonSelfTestQueue.cpp  its checks of the FIFO, the CP's streams,
 *                      the GART and the clocks
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

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Radeon3D.hpp"
#include "RadeonChip.hpp"
#include "RadeonRegs.hpp"
#include "VGACard.hpp"
#include "i2c_spd.hpp"

/// The card's own nanosecond clock (the CRTC, the PLLs, engine timing).
inline long long radeon_clock_ns() {
  using clock = std::chrono::steady_clock;
  static const auto t0 = clock::now();
  return std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - t0)
      .count();
}

class CRadeon : public CVGACard {
  friend class CRadeon3D;

public:
  CRadeon(CConfigurator *cfg, class CSystem *c, int pcibus, int pcidev);
  virtual ~CRadeon();

  virtual void init() override;
  virtual void stop_threads() override;

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
  void palette_write_data(u32 data);
  u32 palette_read_data();
  void ddc_attach_monitor();
  void ddc_drive();
  u32 gpio_read(u32 reg) const;
  void card_tick() override;
  void update_int_line();

  // --- the PLLs and the CRTC's timing (RadeonTiming.cpp) ------------------
  u32 pll_read(u8 index);
  void pll_write(u8 index, u32 data, u32 byte_mask);
  /// Dividers written with PPLL_CNTL's atomic update enabled wait for
  /// PPLL_REF_DIV's update bit; this applies them once it has completed.
  void pll_settle() const;
  /// The pixel clock the primary CRTC runs at (kHz): the PPLL from its
  /// effective dividers, or the reference.
  u32 pixel_clock_khz() const;
  /// The CRTC's frame: its period (ns), total and displayed lines.
  void crtc_frame_params(long long *frame_ns, u32 *total, u32 *disp) const;
  /// Where the CRTC is now: the line (and whether it is past the displayed
  /// ones) and the frame count, from the card's clock, continuous across
  /// mode changes.
  u32 current_vline(bool *in_vblank, long long *frame = nullptr) const;
  /// MC_STATUS MEM_PWRUP_COMPL_A/B (<1:0>): clear for a while after each
  /// SDRAM mode-register write.
  u32 mc_pwrup_bits() const;

  /// The effective PPLL dividers (what the PLL runs on) and the atomic
  /// update in progress; the CRTC's phase anchor; the timing lock.
  mutable std::mutex m_clk_mx;
  mutable u32 m_ppll_eff_ref = 0, m_ppll_eff_div[4] = {};
  mutable bool m_ppll_update = false;
  mutable long long m_ppll_update_done_ns = 0;
  long long m_pll_test_t0_ns = 0;
  u32 m_pll_test_start = 0;
  long long m_mc_pwrup_ns = 0;
  mutable long long m_crtc_anchor_ns = 0, m_crtc_anchor_frame = 0,
                    m_crtc_frame_ns = 0;
  long long m_last_vline_frame = -1;
  u32 m_last_vline_line = 0;

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
  /// Pixels the 2D engine has written (the busy-time model's input).
  u64 m_eng_pixels = 0;

  // --- the command FIFO and the engine thread (RadeonQueue.cpp) ----------
  /// Registers whose writes go through the RBBM's command FIFO: the
  /// rendering engine's, from 0x1400 on (Rage 128 Pro RRG 3-149's map:
  /// "Rendering Engine (GUI) Registers (FIFOed)").
  static bool is_fifo_reg(u32 reg);
  /// Of those, the status registers a read must not wait behind the FIFO
  /// for.
  static bool is_status_reg(u32 reg);
  /// True on the engine's own thread (or while a write runs inline): its
  /// writes to engine registers act at once instead of being queued.
  static bool in_engine();
  /// Runs the enclosing scope as the engine (with m_exec_mx held).
  struct engine_scope {
    bool saved;
    engine_scope();
    ~engine_scope();
  };
  struct fifo_entry {
    u32 reg, data, mask;
  };
  void queue_write(u32 reg, u32 data, u32 mask);
  void queue_apply(const fifo_entry &e);
  /// A FIFO-ordered register the engine itself acts on (WAIT_UNTIL, the
  /// cache controls); true when handled.
  bool engine_sync_reg(u32 reg, u32 data);
  void engine_thread_start();
  void engine_thread_stop();
  void engine_main();
  bool engine_has_work_locked() const;
  /// Wake the engine (new ring or PIO data) and note that it is busy.
  void engine_kick();
  /// Wait until every command queued before now has been taken, then
  /// (with the engine's lock) read the register.
  u32 read_after_queue(u32 reg);
  /// Wait until the FIFO, the CP's streams and the engine are idle (a
  /// bounded wait: the state file, the self-test, a CPU read of the
  /// framebuffer the engine draws in is the guest's business).
  void engine_drain();
  /// Account a command's time on the engine's timeline.
  void engine_charge(u64 clocks);
  bool engine_busy() const;
  u32 rbbm_status() const;
  /// Latch GUI_IDLE when the engine has gone idle since it was last busy.
  void engine_idle_check();
  u32 cache_ctlstat(u32 reg) const;

  std::mutex m_exec_mx;      ///< held while the engine's state changes
  mutable std::mutex m_q_mx; ///< the queues and the flags below
  std::condition_variable m_q_work, m_q_space;
  std::deque<fifo_entry> m_fifo;
  std::deque<u32> m_csq; ///< the CP's primary PIO queue
  std::unique_ptr<std::thread> m_eng_thread;
  bool m_eng_stop = false;
  bool m_eng_waiting = false;
  bool m_executing = false;
  bool m_eng_held = false; ///< RBBM_SOFT_RESET holds the engine
  u64 m_enq_seq = 0, m_done_seq = 0;
  int m_last_unit = 0; ///< 1 the 2D engine, 2 the 3D engine last
  int m_flush_pending = 0;
  std::atomic<long long> m_busy_until_ns{0};
  std::atomic<long long> m_flush_done_ns{0};
  bool m_was_busy = false;
  /// ALPHABOX_RADEON_SYNC=1: every command runs inside the write that
  /// starts it and the engine always reads idle (the model before the
  /// FIFO existed, kept for A/B runs).
  bool m_sync = false;
  bool m_fifo_warned = false;

  // --- the command processor (RadeonCP.cpp) --------------------------------
  bool cp_reg_write(u32 reg, u32 data);
  bool cp_reg_read(u32 reg, u32 *v);
  u32 cp_read32(u32 mc);
  void cp_write32(u32 mc, u32 data);
  /// The CSQ mode in force (CP_CSQ_CNTL<31:28>, or CP_CSQ_MODE's enables).
  u32 cp_csq_mode() const;
  bool cp_primary_bm() const;
  bool cp_primary_pio() const;
  bool cp_indirect_bm() const;
  /// The micro-engine holds a complete microcode image.
  bool cp_microcode_ok() const { return m_me_loaded; }
  /// The ring has dwords the CP will read (enabled, microcode loaded).
  bool cp_ring_has_work() const;
  /// The ring has unread dwords at all (CP_CMDSTRM busy).
  bool cp_ring_pending() const;
  /// Read and act on the ring up to the end of one packet.
  void cp_ring_step();
  void cp_rptr_writeback();
  void cp_soft_reset();
  void cp_run_buffer(u32 mc, u32 dwords);
  /// A dword fetched from a command buffer: BUF_SWAP applies in host
  /// memory.
  u32 cp_fetch(u32 mc);
  void cp_feed(u32 d);
  void cp_packet3(u8 op, const std::vector<u32> &payload);
  /// A type-3 packet for the 3D engine; false when it is not one.
  bool r3d_packet3(u8 op, const std::vector<u32> &payload);

  // --- the memory controller's view of the bus (RadeonGart.cpp) ------------
  /// A memory-controller address: the framebuffer (`*is_vram`, `*addr` a
  /// VRAM offset), the AGP window or the PCI GART (`*addr` a bus
  /// address). False when nothing decodes it.
  bool cp_translate(u32 mc, bool *is_vram, u32 *addr);
  bool gart_translate(u32 mc, u32 *bus);

  // --- the self-test (RadeonSelfTest.cpp) -----------------------------------
  /// ALPHABOX_RADEON_SELFTEST: true when every check passed.
  bool selftest();
  using selftest_report = std::function<void(const std::string &what, bool ok,
                                             const std::string &detail)>;
  /// The FIFO, CP and GART checks (RadeonSelfTestQueue.cpp).
  void selftest_queue(const selftest_report &report);

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
    int src_sc_right, src_sc_bottom;          ///< the source's, inclusive
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
  const radeon::ChipInfo *m_chip = &radeon::default_chip();
  /// The clocks the card's BIOS gives (its PLL block), else the chip
  /// row's: the PLLs' reference and the engine clock, kHz.
  u32 m_ref_khz = 27000, m_sclk_khz = 180000;
  void read_bios_clocks();
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
  u32 m_me_written[8] = {}; ///< which entries a driver has loaded
  bool m_me_loaded = false;
  bool m_cp_warned_noucode = false, m_cp_warned_ib = false,
       m_cp_warned_pio = false;
  u32 m_rptr_since_wb = 0; ///< dwords read since the last write-back
  bool m_cp_unknown_seen[256] = {};
  int m_cp_bad_reads = 0;

  /// The INTA line as this card last drove it, and the lock deciding it.
  bool m_int_asserted = false;
  std::mutex m_int_lock;
  long long m_last_vblank_frame = -1;

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
