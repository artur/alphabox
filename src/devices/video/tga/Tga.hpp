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

/* CTga -- the DECchip 21030 "TGA" PCI graphics accelerator, as on the
 * Digital ZLXp-E1 (8 planes, 2 MB of VRAM, a Bt485 RAMDAC).
 *
 * The TGA is not a VGA: it has no VGA registers, no legacy windows and no
 * option ROM the console runs. Digital's consoles (SRM, AlphaBIOS) and
 * operating systems drive it with their own code. Its one PCI memory
 * range is 128 MB of identical copies of a "core space" -- an alternate
 * ROM window, the registers and the frame buffer -- and every drawing
 * operation is started by a write to the frame buffer (or a command
 * register) that the chip interprets according to the mode register:
 * a stipple mask, a fill length, half of a copy, a line segment.
 *
 * So this card does not sit on CVGA: it owns its VRAM, draws the screen
 * itself from the video timing registers and the RAMDAC, and drives the
 * GUI with a render thread of its own, as the VGA cards' CVGACard does.
 *
 * Split by concern:
 *   Tga.cpp        PCI header, address decode, registers, lifecycle, state
 *   TgaEngine.cpp  the graphics modes (simple, stipple, fill, copy, DMA,
 *                  lines) and the pixel write path
 *   TgaDisplay.cpp the screen: timing decode, rendering, the cursors
 *   TgaRamdac.cpp  the Bt485 behind the palette and DAC registers
 *
 * References: the DECchip 21030 PCI Graphics Accelerator Reference Manual
 * (Digital EC-N0683-72, 1994) is the specification this follows; where it
 * is silent or wrong, the Linux tgafb driver (GPL-2.0) and NetBSD's tga(4)
 * (BSD) record what real cards do, and the comments say which. The Bt485
 * follows its Brooktree data sheet as NetBSD's bt485(4) drives it. No code
 * is taken from either driver.
 */

#if !defined(INCLUDED_TGA_H)
#define INCLUDED_TGA_H

#include "PCIDevice.hpp"
#include "TgaRegs.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

/// A ZLXp-E model: what the alternate ROM space's option ID reports and how
/// the frame buffer is built.
struct tga_model_config {
  const char *name; ///< config value: "e1"
  const char *part; ///< for messages
  u32 option_type;  ///< alternate-ROM Dword <15:12>: 0 8-plane, 1 24-plane
  bool deep;        ///< 32-bpp frame buffer
  u32 vram_bytes;
};
const tga_model_config *tga_model_by_name(const char *name);

class CTga : public CPCIDevice {
public:
  CTga(CConfigurator *cfg, class CSystem *c, int pcibus, int pcidev,
       const tga_model_config &model);
  virtual ~CTga();

  virtual void init() override;
  virtual void start_threads() override;
  virtual void stop_threads() override;
  virtual void check_state() override;
  virtual void ResetPCI() override;

  virtual int SaveState(FILE *f) override;
  virtual int RestoreState(FILE *f) override;

  virtual u32 ReadMem_Bar(int func, int bar, u32 address, int dsize) override;
  virtual void WriteMem_Bar(int func, int bar, u32 address, int dsize,
                            u32 data) override;
  virtual u32 config_read_custom(int func, u32 address, int dsize,
                                 u32 data) override;
  virtual void config_write_custom(int func, u32 address, int dsize,
                                   u32 old_data, u32 new_data,
                                   u32 data) override;

private:
  // --- address decode (Tga.cpp) ------------------------------------------
  u32 core_bytes() const;
  u32 space_read(u32 address);
  void space_write(u32 address, u32 data, u32 bytemask);
  u32 altrom_read(u32 offset);
  u32 reg_read(unsigned reg);
  void reg_write(unsigned reg, u32 data);

  // --- the engine (TgaEngine.cpp) ----------------------------------------
  /// A write into frame buffer space, or one handed on by GCTR: what it
  /// does is the mode's.
  void fb_write(u32 fbaddr, u32 data, u32 bytemask, bool via_gctr);
  void op_simple(u32 a, u32 data, u32 bytemask);
  void op_stipple(u32 a, u32 mask, bool opaque);
  void op_block_stipple(u32 a, u32 mask);
  void op_fill(u32 a, u32 data, int kind);
  void op_copy(u32 a, u32 mask);
  void op_dma_read(u32 a, u32 data);
  void op_dma_write(u32 a, u32 data);
  void line_setup(unsigned octant, u32 slope);
  void line_draw(u32 mask, bool from_gctr_or_slope);
  void copy64_load(u32 a);
  void copy64_store(u32 a);
  u64 byte_shift(u64 in, int quad_bytes);
  void pixel_write(u32 byteaddr, unsigned width, u32 src, bool rop_on);
  void op_done();

  /// The destination step: bytes from one pixel to the next, and the
  /// width of the memory a pixel occupies (1 or 4 bytes).
  unsigned dst_step() const;
  unsigned dst_width() const;
  u32 dst_byte_offset() const;
  bool deep() const { return (r[tga::GDER] & tga::GDER_DEEP) != 0; }

  u8 vram8(u32 a) const { return m_vram[a & m_vram_mask]; }
  u32 vram32(u32 a) const;
  void vram32_set(u32 a, u32 v);

  // --- the display (TgaDisplay.cpp) --------------------------------------
  void run();
  void update_screen();
  bool screen_geometry(unsigned &w, unsigned &h, unsigned &stride_px,
                       u32 &start) const;
  void render(unsigned w, unsigned h, unsigned stride_px, u32 start);
  void draw_bt485_cursor(unsigned w, unsigned h);
  void draw_tga_cursor(unsigned w, unsigned h);
  void frame_tick();
  void update_irq();

  // --- the RAMDAC (TgaRamdac.cpp) ----------------------------------------
  void ramdac_reset();
  void ramdac_write(unsigned rs, u8 v);
  u8 ramdac_read(unsigned rs);
  u32 ramdac_color(u8 index) const;
  void epdr_write(u32 data);
  u32 epdr_read();

  // --- tracing -----------------------------------------------------------
  void trace(const char *fmt, ...);
  void unimplemented(const std::string &what);

  const tga_model_config &m_model;
  std::vector<u8> m_vram;
  u32 m_vram_mask = 0;

  /// The option ROM behind the expansion ROM BAR and the alternate ROM
  /// space. Without a "rom" image it reads as an erased EEPROM (0xff).
  std::vector<u8> m_rom;

  /// Everything the state file carries.
  u32 r[tga::NUM_REGS];
  struct engine_state {
    u64 copybuf[8];  ///< the copy buffer, 8 quadwords
    u64 residue;     ///< the byte shifter's residue register
    u32 cbr_low;     ///< latched even copy-buffer register write
    u32 cbr_fill;    ///< next copy-buffer entry a PIO write fills
    bool copy_drain; ///< copy direction flag: true = destination next
    bool addr_new;   ///< GADR written since the last operation
    bool bres_new;   ///< GB3R (or a slope) written since the last line
    bool gpxr_persistent;
    u32 cur_addr;    ///< the Bresenham engine's working address
    int32_t cur_err; ///< working error term
    u32 cur_len;     ///< working length (0 = 16)
    u32 gpxr;        ///< the pixel mask now in force
    u32 icsbits;     ///< ICS1562 shift register, as shifted in
    u32 icscount;
  } e;
  struct bt485_state {
    u8 palette[256][3];
    u8 coc[4][3]; ///< overscan, cursor colours 1..3
    u8 cursor[1024];
    u8 cmd[4]; ///< command registers 0..3
    u8 pixmask;
    u8 wr_addr, rd_addr;
    u8 wr_sub, rd_sub;
    u8 coc_wr, coc_rd, coc_wsub, coc_rsub;
    u8 latch[3];
    u16 cur_x, cur_y;
  } dac;

  // --- display / thread --------------------------------------------------
  std::vector<u32> m_frame;
  unsigned m_last_w = 0, m_last_h = 0;
  std::atomic<u64> m_generation{1}; ///< bumped on every change to the screen
  u64 m_drawn_generation = 0;
  std::chrono::steady_clock::time_point m_last_frame;
  int m_frames_since_render = 0;

  std::unique_ptr<std::thread> myThread;
  std::atomic_bool myThreadDead{false};
  std::atomic_bool StopThread{false};
  std::atomic<bool> PauseThread{false};
  std::atomic<bool> PauseAck{false};
  bool gui_initialized = false;
  /// The GUI has one window. A TGA in a machine with a VGA card, or a
  /// second TGA, leaves it to that card: it draws its screen only into
  /// ALPHABOX_TGA_DUMP files, if asked.
  bool m_owns_gui = false;
  const char *m_dump_prefix = nullptr;
  unsigned m_dump_seq = 0;
  std::chrono::steady_clock::time_point m_last_dump;
  void dump_frame(unsigned w, unsigned h);

  std::mutex m_irq_lock;
  bool m_irq_asserted = false;

  /// ALPHABOX_TRACE_TGA: 1 traces every access; file:<path> only while
  /// <path> exists (polled by the render thread).
  std::atomic<bool> m_trace{false};
  std::string m_trace_file;
  /// ALPHABOX_TRACE_TGA=first: each graphics mode, raster op and register
  /// the first time it is used.
  bool m_trace_first = false;
  void first_use(const char *fmt, ...);
  std::set<std::string> m_unimplemented_seen;
};

/// The first TGA, if any: the ALi's graphics-console setting and the
/// one-GUI-card rule look for it.
extern CTga *theTGA;

#endif // !defined(INCLUDED_TGA_H)
