/* Alphabox Alpha Emulator
 * Copyright (C) 2020 Tomáš Glozar
 * Copyright (C) 2026 Artur Goulão
 * Website: https://github.com/lenticularis39/axpbox
 *          https://github.com/artur/alphabox
 *
 * Forked from: ES40 emulator
 * Copyright (C) 2007-2008 by the ES40 Emulator Project
 * Copyright (C) 2007 by Camiel Vanderhoeven
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

/* CVGACard -- what every PCI VGA card on the CVGA core shares.
 *
 * CVGA (VGA.hpp) is the MAME-derived VGA itself: CRTC, sequencer, graphics
 * and attribute controllers, planar/linear memory, rendering. What it does
 * not provide is the part that turns a VGA core into a device in this
 * emulator: the render thread that drives the SDL GUI, the refresh and
 * dirty-gating loop, palette upload, screen sizing, the option ROM, the
 * register address maps and state-file plumbing. That used to live inside
 * S3Trio64, so a second card would have had to copy it; it lives here now.
 *
 * A card derives from this and supplies its chip: the extended register
 * maps, its memory aperture decode, any extra I/O ports, and a handful of
 * small hooks below where the shared loop needs a chip fact (the hardware
 * cursor position, an extra display-enable bit, overflow timing bits).
 */

#if !defined(INCLUDED_VGACARD_H)
#define INCLUDED_VGACARD_H

#include "VGA.hpp"
#include "address_map.hpp"
#include "gui/vga.hpp"
#include "mame_shims.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

class CVGACard : public CVGA, public mame_machine_provider {
public:
  CVGACard(CConfigurator *cfg, class CSystem *c, int pcibus, int pcidev);
  virtual ~CVGACard();

  // --- device lifecycle (shared) -----------------------------------------
  virtual void start_threads() override;
  virtual void stop_threads() override;
  virtual void check_state() override;
  void run();

  virtual int SaveState(FILE *f) override;
  virtual int RestoreState(FILE *f) override;
  /// The card's own state beyond the VGA core (extended registers, a
  /// drawing engine, cached decodes): written after the core's block and
  /// read back before post_restore(). The base card has none.
  virtual int save_card_state(FILE *f) {
    (void)f;
    return 0;
  }
  virtual int restore_card_state(FILE *f) {
    (void)f;
    return 0;
  }
  /// Recompute what is derived from the registers (timing, the linear
  /// window) once every register is back.
  virtual void post_restore() {}
  /// True while the card's framebuffer is offered to the CPUs for direct
  /// access (CSystem::set_direct_memory): writes then bypass the card, so
  /// the renderer cannot rely on its dirty flag and hashes what the screen
  /// shows instead (direct_view_hash).
  virtual bool direct_framebuffer_active() const { return false; }
  /// While the CPUs write VRAM directly, a hash of every byte the screen is
  /// drawn from, so that a refresh redraws only when one of them changed.
  /// The whole of VRAM unless the card narrows it to what its current mode
  /// reads (a cursor image in off-screen memory included).
  virtual uint64_t direct_view_hash() const;
  /// A fast hash of VRAM bytes [start, start + bytes), clamped to VRAM, for
  /// direct_view_hash. Not cryptographic: a collision only delays a redraw
  /// to the refresh the dirty gate forces every few frames anyway.
  uint64_t hash_vram(u32 start, u32 bytes, uint64_t seed = 0) const;

  // --- legacy (fixed-address) ranges -------------------------------------
  /// Dispatches the standard VGA ranges (see LegacyRange) and hands every
  /// other id to card_legacy_read/write.
  virtual u32 ReadMem_Legacy(int index, u32 address, int dsize) override;
  virtual void WriteMem_Legacy(int index, u32 address, int dsize,
                               u32 data) override;

  // --- CVGA contract (shared) --------------------------------------------
  virtual u8 get_actl_palette_idx(u8 index) override;
  virtual void redraw_area(unsigned x0, unsigned y0, unsigned width,
                           unsigned height) override;

  address_map &space(int spacenum) override {
    switch (spacenum) {
    case CRTC_REG:
      return m_crtc_map;
    case GC_REG:
      return m_gc_map;
    case SEQ_REG:
      return m_seq_map;
    case ATC_REG:
      return m_atc_map;
    default:
      FAILURE_1(NotImplemented, "Unknown register space %d", spacenum);
    }
  }

protected:
  /// Range ids the base registers and dispatches. Every other id (6, and
  /// LEGACY_CARD_FIRST upward) is the card's.
  enum LegacyRange {
    LEGACY_IO_3B4 = 1,
    LEGACY_IO_3C0 = 2,
    LEGACY_IO_3BA = 3,
    LEGACY_MEM_VGA = 4,
    LEGACY_MEM_ROM = 5,
    LEGACY_IO_BIOS_MSG = 7,
    LEGACY_IO_3D4 = 8,
    LEGACY_IO_3DA = 9,
    LEGACY_CARD_FIRST = 10
  };

  // --- chip hooks ----------------------------------------------------------

  /// Short name for messages ("S3", "Cirrus") and the tag printed by
  /// start/stop_threads (" s3", " cirrus").
  virtual const char *card_name() const = 0;
  virtual const char *thread_tag() const = 0;

  /// State-file magics. They belong to the card, not the base: S3 state
  /// files written before this class existed must still restore.
  virtual u32 state_magic1() const = 0;
  virtual u32 state_magic2() const = 0;

  /// Register maps. init_maps() installs the standard VGA registers first
  /// (vga_*_map), then calls these: a card adds its extended registers and
  /// may replace a standard one.
  virtual void crtc_map(address_map &map) = 0;
  virtual void sequencer_map(address_map &map) = 0;
  virtual void gc_map(address_map &map) = 0;
  virtual void attribute_map(address_map &map) = 0;

  /// Folded into the refresh dirty-gate: the hardware cursor is not flagged
  /// by vga_mem_updated, so a card with one reports its mode/position here.
  virtual uint64_t hw_cursor_signature() const { return 0; }

  /// Extra display gating beyond ATC video enable and SR1 screen-off, e.g.
  /// the S3's VGA subsystem enable register at 0x3c3.
  virtual bool display_enabled() const { return true; }

  /// Chip overflow bits that extend the CRTC display size (S3 CR5D/CR5E,
  /// Cirrus CR1A/CR1B). Called with the standard-VGA values.
  virtual void apply_extended_timing(int &h, int &v) {}

  /// Palette/overscan write protect (S3 CR33 bit 6).
  virtual bool atc_palette_locked() const { return false; }

  /// True while the card's own CRTC drives the display in place of the VGA
  /// one (an accelerator's extended mode): the VGA gates -- ATC video
  /// enable, SR1 screen-off, CR17 sync -- and the VGA mode choice do not
  /// apply, the card sizes the screen (determine_screen_dimensions) and
  /// draws it (screen_update).
  virtual bool native_crtc_active() const { return false; }

  /// The card's own legacy ranges. Unclaimed reads return 0 and writes are
  /// ignored, as an undecoded range would.
  virtual u32 card_legacy_read(int index, u32 address, int dsize) { return 0; }
  virtual void card_legacy_write(int index, u32 address, int dsize, u32 data) {}

  /// I/O ports. io_read/io_write split a multi-byte access into bytes
  /// (little-endian); io_read_b/io_write_b implement the standard VGA
  /// ports. A card overrides the byte handlers to claim its own ports or
  /// gate standard ones, and falls back to these for the rest.
  virtual u32 io_read(u32 address, int dsize);
  virtual void io_write(u32 address, int dsize, u32 data);
  virtual u8 io_read_b(u32 address);
  virtual void io_write_b(u32 address, u8 data);

  /// Miscellaneous Output (0x3c2) write; a card may gate bits first.
  virtual void write_b_3c2(u8 value);
  u8 read_b_3c2();
  u8 read_b_3ca() { return 0; }

  /// The 0xa0000 window, one byte at a time through mem_r/mem_w.
  virtual u32 legacy_read(u32 address, int dsize);
  virtual void legacy_write(u32 address, int dsize, u32 data);

  // --- shared machinery ----------------------------------------------------
  void init_maps();
  void vga_crtc_map(address_map &map);
  void vga_sequencer_map(address_map &map);
  void vga_gc_map(address_map &map);
  void vga_attribute_map(address_map &map);

  /// Register the standard VGA ports (0x3b4, 0x3ba, 0x3c0-0x3cf, 0x3d4,
  /// 0x3da), the VGA BIOS message port (0x500) and the 0xa0000 window.
  void add_vga_legacy_ranges();
  void update();
  /// Called from the card's own thread about every 10 ms, in a machine that
  /// is running (not paused for a firmware reset). The base card has
  /// nothing to do here; one whose chip raises interrupts of its own does.
  virtual void card_tick() {}
  virtual void determine_screen_dimensions(unsigned *piHeight,
                                           unsigned *piWidth);
  virtual void palette_update() override;

  /// Load the card's option ROM (its x86 VGA BIOS, executed by SRM) from
  /// the "rom" config value and map it at 0xc0000.
  void load_option_rom(const char *default_name);
  u32 rom_read(u32 address, int dsize);

  // --- generic VGA field helpers used by the shared render path -----------
  inline bool seq_dotperchar() const {
    return (vga.sequencer.data[1] & 0x01) != 0;
  }
  inline bool x_dotclockdiv2() const {
    return (vga.sequencer.data[1] & 0x08) != 0;
  }
  inline u8 atc_palette(u8 idx) const { return vga.attribute.data[idx & 0x0f]; }
  /// ATC index bit 5, the palette address source: 0 = video off while the
  /// CPU owns palette RAM, 1 = the ATC drives the display.
  inline bool atc_video_enabled() const { return BIT(vga.attribute.index, 5); }

  // --- shared state --------------------------------------------------------
  address_map m_crtc_map{256};
  address_map m_seq_map{256};
  address_map m_gc_map{256};
  address_map m_atc_map{64};

  /// CR11 vertical-retrace interrupt line (not wired).
  nop_callback m_vsync_cb;

  /// Written to the state file verbatim, so its layout is the file format.
  /// memory/memsize are unused -- the VRAM is vga.memory -- and are kept only
  /// so S3 state files written before this class existed still restore;
  /// SaveState writes them as zero so no heap address reaches a file.
  struct SVGACard_state {
    bool vga_mem_updated;
    unsigned x_tilesize;
    unsigned y_tilesize;
    u8 last_bpp;
    u8 *memory;
    u32 memsize;
  } state;

  /// Computed video timing (MAME screen().configure() parameters).
  struct {
    int pixel_clock_hz = 0;
    int xtal_hz = 0;
    int divisor = 1;
    double dclk_freq_mhz = 0.0;
    double vrefresh_hz;
    uint64_t refresh_interval_ms;
  } timing;

  /// Per card, not file-static: two cards in one machine must not share a
  /// BIOS image or a last-drawn size.
  /// Zero-initialized on purpose: it used to be a file-static (implicitly
  /// zeroed), and reads past a short image must keep returning 0.
  u8 option_rom[65536] = {};
  unsigned rom_max = 0;
  unsigned old_iWidth = 0;
  unsigned old_iHeight = 0;

  /// VGA BIOS debug output (port 0x500), printed a line at a time.
  char bios_message[200] = {};
  size_t bios_message_size = 0;

  std::chrono::steady_clock::time_point m_last_refresh_time;
  uint64_t m_last_cursor_sig = 0;
  uint64_t m_last_direct_hash = 0;
  int m_frames_since_render = 0;
  /// Set by RestoreState: the render thread, once its GUI exists, pushes the
  /// restored text font and draws the first frame from the restored VRAM.
  bool m_restored = false;

  std::unique_ptr<std::thread> myThread;
  std::atomic_bool myThreadDead{false};
  bool StopThread = false;

  /// The GUI is initialized once, not on every thread (re)start: the serial
  /// BREAK menu stops and restarts the device threads around every
  /// interaction.
  bool gui_initialized = false;

  /// Firmware reset pauses the render thread instead of stopping it, since
  /// it owns the SDL window: stop_threads() raises PauseThread and waits for
  /// PauseAck.
  std::atomic<bool> PauseThread{false};
  std::atomic<bool> PauseAck{false};
};

#endif // !defined(INCLUDED_VGACARD_H)
