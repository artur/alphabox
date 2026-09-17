/* AXPbox Alpha Emulator
 * Copyright (C) 2020 Tomáš Glozar
 * Copyright (C) 2026 Artur Goulão
 * Website: https://github.com/lenticularis39/axpbox
 *          https://github.com/artur/axpbox
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

/**
 * \file
 * The device plumbing shared by every PCI VGA card on the CVGA core (see
 * VGACard.hpp). Lifted out of S3Trio64.cpp unchanged in behaviour.
 **/

#include "VGACard.hpp"
#include "StdAfx.hpp"
#include "System.hpp"
#include "gui/gui.hpp"

CVGACard::CVGACard(CConfigurator *cfg, CSystem *c, int pcibus, int pcidev)
    : CVGA(cfg, c, pcibus, pcidev) {}

/**
 * Destructor. Only a backstop: by the time it runs the derived card is
 * already destroyed, while the render thread calls that card's hooks. A
 * card must therefore stop the thread in its own destructor; the second
 * call here is then a no-op.
 **/
CVGACard::~CVGACard() { stop_threads(); }

void CVGACard::init_maps() {
  crtc_map(m_crtc_map);
  sequencer_map(m_seq_map);
  gc_map(m_gc_map);
  attribute_map(m_atc_map);
}

void CVGACard::add_vga_legacy_ranges() {
  add_legacy_io(LEGACY_IO_3B4, 0x3b4, 2);
  add_legacy_io(LEGACY_IO_3BA, 0x3ba, 2);
  add_legacy_io(LEGACY_IO_3C0, 0x3c0, 16);
  add_legacy_io(LEGACY_IO_3D4, 0x3d4, 2);
  add_legacy_io(LEGACY_IO_3DA, 0x3da, 1);

  /* The VGA BIOS we use sends text messages to port 0x500.
     We listen for these messages at port 500. */
  add_legacy_io(LEGACY_IO_BIOS_MSG, 0x500, 1);
  bios_message_size = 0;
  bios_message[0] = '\0';

  // Legacy video address space: A0000 -> bffff
  add_legacy_mem(LEGACY_MEM_VGA, 0xa0000, 128 * 1024);
}

/**
 * Read from one of the Legacy (fixed-address) memory ranges.
 **/
u32 CVGACard::ReadMem_Legacy(int index, u32 address, int dsize) {
  switch (index) {
  case LEGACY_IO_3B4:
    return io_read(address + 0x3b4, dsize);
  case LEGACY_IO_3C0:
    return io_read(address + 0x3c0, dsize);
  case LEGACY_IO_3BA:
    return io_read(address + 0x3ba, dsize);
  case LEGACY_MEM_VGA:
    return legacy_read(address, dsize);
  case LEGACY_MEM_ROM:
    return rom_read(address, dsize);
  case LEGACY_IO_BIOS_MSG:
    return 0;
  case LEGACY_IO_3D4:
    return io_read(address + 0x3d4, dsize);
  case LEGACY_IO_3DA:
    return io_read(address + 0x3da, dsize);
  default:
    return card_legacy_read(index, address, dsize);
  }
}

/**
 * Write to one of the Legacy (fixed-address) memory ranges.
 **/
void CVGACard::WriteMem_Legacy(int index, u32 address, int dsize, u32 data) {
  switch (index) {
  case LEGACY_IO_3B4:
    io_write(address + 0x3b4, dsize, data);
    return;
  case LEGACY_IO_3C0:
    io_write(address + 0x3c0, dsize, data);
    return;
  case LEGACY_IO_3BA:
    io_write(address + 0x3ba, dsize, data);
    return;
  case LEGACY_MEM_VGA:
    legacy_write(address, dsize, data);
    return;
  case LEGACY_MEM_ROM:
    return;
  case LEGACY_IO_BIOS_MSG: {
    const char c = (char)(data & 0xff);
    // A full buffer is flushed as a line rather than overrun.
    if (bios_message_size >= sizeof(bios_message) - 1 || c == '\n' ||
        c == '\r') {
      if (bios_message_size > 0) {
        bios_message[bios_message_size] = '\0';
        printf("%s: %s\n", thread_tag() + 1, bios_message);
      }
      bios_message_size = 0;
      if (c == '\n' || c == '\r')
        return;
    }
    bios_message[bios_message_size++] = c;
    return;
  }
  case LEGACY_IO_3D4:
    io_write(address + 0x3d4, dsize, data);
    return;
  case LEGACY_IO_3DA:
    io_write(address + 0x3da, dsize, data);
    return;
  default:
    card_legacy_write(index, address, dsize, data);
    return;
  }
}

/**
 * Read from I/O ports, one byte at a time.
 **/
u32 CVGACard::io_read(u32 address, int dsize) {
  switch (dsize) {
  case 8:
    return io_read_b(address);
  case 16:
    return (u32)io_read_b(address) | ((u32)io_read_b(address + 1) << 8);
  case 32:
    return (u32)io_read_b(address) | ((u32)io_read_b(address + 1) << 8) |
           ((u32)io_read_b(address + 2) << 16) |
           ((u32)io_read_b(address + 3) << 24);
  default:
    FAILURE(InvalidArgument, "Unsupported dsize");
  }
}

/**
 * Write to I/O ports, one byte at a time.
 **/
void CVGACard::io_write(u32 address, int dsize, u32 data) {
  switch (dsize) {
  case 8:
    io_write_b(address, (u8)data);
    break;

  case 16:
    io_write_b(address, (u8)data);
    io_write_b(address + 1, (u8)(data >> 8));
    break;

  case 32:
    printf("%s Weird Size io write: %" PRIx32 ", %d, %" PRIx32 "   \n",
           card_name(), address, dsize, data);
    io_write_b(address, (u8)data);
    io_write_b(address + 1, (u8)(data >> 8));
    io_write_b(address + 2, (u8)(data >> 16));
    io_write_b(address + 3, (u8)(data >> 24));
    break;

  default:
    FAILURE(InvalidArgument, "Weird IO size");
  }
}

/**
 * Read one byte from a standard VGA I/O port.
 **/
u8 CVGACard::io_read_b(u32 address) {
  switch (address) {
  case 0x3c0:
    return atc_address_r(0);

  case 0x3c1:
    return atc_data_r(0);

  case 0x3c2:
    return read_b_3c2();

  case 0x3c4:
    return sequencer_address_r(0);

  case 0x3c5:
    return sequencer_data_r(0);

  case 0x3c6:
    return ramdac_mask_r(0);

  case 0x3c7:
    return ramdac_state_r(0);

  case 0x3c8:
    return ramdac_write_index_r(0);

  case 0x3c9:
    return ramdac_data_r(0);

  case 0x3ca:
    return read_b_3ca();

  case 0x3cc:
    return miscellaneous_output_r(0);

  case 0x3ce:
    return gc_address_r(0);

  case 0x3cf:
    return gc_data_r(0);

  case 0x3b4:
  case 0x3d4:
    return crtc_address_r(0);

  case 0x3b5:
  case 0x3d5:
    return crtc_data_r(0);

  case 0x3ba:
  case 0x3da: {
    // Input Status Register 1 — wall-clock vblank (no CRT timing engine)
    using clock = std::chrono::steady_clock;
    static auto t0 = clock::now();
    auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - t0)
            .count();

    const int frame_ms = 1000 / 70; // ~70Hz
    const int vblank_ms = 1;

    u8 data = 0;
    if ((ms % frame_ms) < vblank_ms)
      data |= 0x08 | 0x01;

    vga.attribute.state = 0; // ATC flip-flop reset
    return data;
  }

  case 0x3bb: /* Feature Control (mono) readback; mirror 3CA behavior */
  case 0x3db: /* Feature Control (color) readback; same treatment */
    return read_b_3ca();

  case 0x3b6:
  case 0x3b7:
  case 0x3b8:
  case 0x3b9:
  case 0x3d6:
  case 0x3d7:
  case 0x3d8:
  case 0x3d9:
    return 0xFF; // open bus

  default:
    printf("%s: Unhandled io port %x read\n", card_name(), address);
    return 0;
  }
}

/**
 * Write one byte to a standard VGA I/O port.
 **/
void CVGACard::io_write_b(u32 address, u8 data) {
  switch (address) {
  case 0x3c0: {
    bool was_index_phase = (vga.attribute.state == 0);
    // Snapshot previous video-enabled state BEFORE the MAME canonical write
    bool prev_ve = atc_video_enabled();
    atc_address_data_w(0, data);
    if (was_index_phase) {
      // Detect video enable/disable transitions from MAME canonical source
      bool new_ve = atc_video_enabled();
      if (!new_ve && prev_ve) {
        bx_gui->lock();
        bx_gui->clear_screen();
        bx_gui->unlock();
      } else if (new_ve && !prev_ve) {
        redraw_area(0, 0, old_iWidth, old_iHeight);
      }
    }
    break;
  }

  case 0x3c2:
    write_b_3c2(data);
    m_ioas = bool(BIT(data, 0));
    break;

  case 0x3c4:
    sequencer_address_w(0, data);
    break;

  case 0x3c5:
    sequencer_data_w(0, data);
    break;

  case 0x3c6:
    ramdac_mask_w(0, data);
    break;

  case 0x3c7:
    ramdac_read_index_w(0, data);
    break;

  case 0x3c8:
    ramdac_write_index_w(0, data);
    break;

  case 0x3c9:
    ramdac_data_w(0, data);
    break;

  case 0x3ce:
    gc_address_w(0, data);
    break;

  case 0x3cf:
    gc_data_w(0, data);
    break;

  case 0x3ba:
  case 0x3da:
    feature_control_w(0, data);
    break;

  case 0x3b4:
  case 0x3d4:
    vga.crtc.index = data & 0x7f;
    break;

  case 0x3b5:
  case 0x3d5:
    crtc_data_w(0, data);
    break;

  case 0x3bb:
    break;

  case 0x3b6:
  case 0x3b7:
  case 0x3b8:
  case 0x3b9:
  case 0x3d6:
  case 0x3d7:
  case 0x3d8:
  case 0x3d9:
    // Dead ports — 32-bit writes to the CRTC pair (3D4/3D5) spill here.
    // Real hardware silently ignores them.
    break;

  default:
    FAILURE_1(NotImplemented, "Unhandled port %x write", address);
  }
}

/**
 * Write to the VGA Miscellaneous Output Register (0x3c2)
 *
 * \code
 * +-+-+-+-+---+-+-+
 * |7|6|5| |3 2|1|0|
 * +-+-+-+-+---+-+-+
 *  ^ ^ ^    ^  ^ ^
 *  | | |    |  | +- 0: I/OAS -- Input/Output Address Select: Selects the CRT
 *  | | |    |  |       controller addresses.
 *  | | |    |  |         0: Compatibility with monochrome adapter
 *  | | |    |  |            (0x3b4,0x3b5,0x03ba)
 *  | | |    |  |         1: Compatibility with color graphics adapter (CGA)
 *  | | |    |  |            (0x3d4,0x3d5,0x03da)
 *  | | |    |  +--- 1: RAM Enable: Controls access from the system:
 *  | | |    |            0: Disables access to the display buffer
 *  | | |    |            1: Enables access to the display buffer
 *  | | |    +--- 2..3: Clock Select: Controls the selection of the dot clocks
 *  | | |               used in driving the display timing:
 *  | | |                 00: Select 25 Mhz clock (320/640 pixel wide modes)
 *  | | |                 01: Select 28 Mhz clock (360/720 pixel wide modes)
 *  | | |                 10: Undefined (possible external clock)
 *  | | |                 11: Undefined (possible external clock)
 *  | | +----------- 5: Odd/Even Page Select: Selects the upper/lower 64K page
 *  | |                 of memory when the system is in an even/odd mode.
 *  | |                   0: Selects the low page.
 *  | |                   1: Selects the high page.
 *  | +------------- 6: Horizontal Sync Polarity
 *  |                     0: Positive sync pulse.
 *  |                     1: Negative sync pulse.
 *  +--------------- 7: Vertical Sync Polarity
 *                        0: Positive sync pulse.
 *                        1: Negative sync pulse.
 * \endcode
 **/
void CVGACard::write_b_3c2(u8 value) { vga.miscellaneous_output = value; }

/**
 * Read from the VGA Input Status register (0x3c2)
 *
 * \code
 * +-----+-+-------+
 * |     |4|       |
 * +-----+-+-------+
 *        ^
 *        +--------- 4: Switch Sense:
 *                      Returns the status of the four sense switches as
 *                      selected by the Clock Select field of the
 *                      Miscellaneous Output Register (see write_b_3c2)
 * \endcode
 **/
u8 CVGACard::read_b_3c2() {
  u8 res = 0x60; // is VGA (bits 5-6 set)

  // Sense bit readback: select which of 4 sense switches based on clock select
  // MAME: const u8 sense_bit = (3 - (vga.miscellaneous_output >> 2)) & 3;
  //        if(BIT(m_input_sense->read(), sense_bit)) res |= 0x10;
  const u8 sense_bit = (3 - ((vga.miscellaneous_output >> 2) & 3)) & 3;
  if (BIT(0x0F, sense_bit)) // all sense pins active
    res |= 0x10;

  res |= vga.crtc.irq_latch << 7;
  return res;
}

/**
 * Read from the 0xa0000 window, one byte at a time.
 **/
u32 CVGACard::legacy_read(u32 address, int dsize) {
  u32 data = 0;
  switch (dsize) {
  case 32:
    data |= (u32)mem_r(address + 3) << 24;
    data |= (u32)mem_r(address + 2) << 16;
    [[fallthrough]];
  case 16:
    data |= (u32)mem_r(address + 1) << 8;
    [[fallthrough]];
  case 8:
    data |= (u32)mem_r(address + 0);
    break;
  default:
    FAILURE(InvalidArgument, "Unsupported dsize");
  }

  return data;
}

/**
 * Write to the 0xa0000 window, one byte at a time.
 **/
void CVGACard::legacy_write(u32 address, int dsize, u32 data) {
  switch (dsize) {
  case 8:
    mem_w(address, (u8)data);
    break;

  case 16:
    mem_w(address, (u8)data);
    mem_w(address + 1, (u8)(data >> 8));
    break;

  case 32:
    mem_w(address, (u8)data);
    mem_w(address + 1, (u8)(data >> 8));
    mem_w(address + 2, (u8)(data >> 16));
    mem_w(address + 3, (u8)(data >> 24));
    break;

  default:
    FAILURE(InvalidArgument, "Unsupported dsize");
  }
}

/**
 * Thread entry point.
 *
 * The thread first initializes the GUI, and then starts looping the
 * following actions until interrupted (by StopThread being set to true)
 *   - Handle any GUI events (mouse moves, keypresses)
 *   - Update the GUI to match the screen buffer
 *   - Flush the updated GUI content to the screen
 *   .
 **/
void CVGACard::run() {
  try {
    // Initialize the GUI once (and let it know our tilesize). The serial
    // BREAK menu stops and restarts the device threads around every
    // interaction, so this runs again on "continue" -- and a second
    // SDL_Init/window creation is not what the GUI expects.
    if (!gui_initialized) {
      bx_gui->init(state.x_tilesize, state.y_tilesize);
      gui_initialized = true;
    }
    bool was_paused = false;
    PauseAck.store(false, std::memory_order_release);
    for (;;) {
      // Terminate thread if StopThread is set to true
      if (StopThread)
        return;
      // Handle GUI events (50 times per second)
      bx_gui->lock();
      bx_gui->handle_events();
      bx_gui->unlock();
      std::this_thread::sleep_for(std::chrono::milliseconds(10));

      // During firmware reset: keep pumping events (window stays alive),
      // but do NOT touch emulated VGA state.
      if (PauseThread.load(std::memory_order_acquire)) {
        if (!was_paused) {
          bx_gui->lock();
          bx_gui->clear_screen();
          bx_gui->unlock();
          was_paused = true;
        }
        PauseAck.store(true, std::memory_order_release);
        continue;
      }
      PauseAck.store(false, std::memory_order_release);
      was_paused = false;

      // Update the screen (50 times per second)
      bx_gui->lock();
      update();
      bx_gui->flush();
      bx_gui->unlock();
    }
  }

  catch (CException &e) {
    printf("Exception in %s thread: %s.\n", card_name(),
           e.displayText().c_str());
    myThreadDead.store(true);

    // Let the thread die...
  }
}

/**
 * Create and start thread.
 **/
void CVGACard::start_threads() {
  // Resume after reset if the thread already exists
  PauseThread.store(false, std::memory_order_release);

  if (!myThread) {
    printf("%s", thread_tag());
    StopThread = false;
    myThread = std::make_unique<std::thread>([this]() { this->run(); });
  }
}

/**
 * Stop and destroy thread.
 **/
void CVGACard::stop_threads() {
  // During firmware reset, do NOT kill the render thread (it owns the SDL
  // window). Just pause it so the window stays alive.
  if (cSystem && cSystem->IsResetInProgress()) {
    PauseThread.store(true, std::memory_order_release);

    // Wait briefly until the thread acknowledges the pause
    if (myThread) {
      for (int spin = 0; spin < 600; spin++) // up to ~600ms
      {
        if (PauseAck.load(std::memory_order_acquire))
          break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      // Make it visible in the log whether we paused or stopped
      printf("%s(pause)", thread_tag());
    }
    return;
  }

  // Normal shutdown: actually stop the thread.
  StopThread = true;
  if (myThread) {
    printf("%s", thread_tag());
    myThread->join();
    myThread = nullptr;
  }
}

/**
 * Check if threads are still running.
 **/
void CVGACard::check_state() {
  if (myThreadDead.load())
    FAILURE_1(Thread, "%s thread has died", card_name());
}

/**
 * Save state to a Virtual Machine State file.
 **/
int CVGACard::SaveState(FILE *f) {
  long ss = sizeof(state);
  u32 magic1 = state_magic1();
  u32 magic2 = state_magic2();
  int res;

  if ((res = CPCIDevice::SaveState(f)))
    return res;

  // state.memory and state.memsize are vestigial: nothing assigns or reads
  // them, as the real VRAM is vga.memory (allocated in init, and not part of
  // the savefile). Write them as zero so state files are deterministic
  // instead of carrying stray heap bytes.
  SVGACard_state saved = state;
  saved.memory = nullptr;
  saved.memsize = 0;

  fwrite(&magic1, sizeof(u32), 1, f);
  fwrite(&ss, sizeof(long), 1, f);
  fwrite(&saved, sizeof(saved), 1, f);
  fwrite(&magic2, sizeof(u32), 1, f);
  printf("%s: %d bytes saved.\n", devid_string, (int)ss);
  return 0;
}

/**
 * Restore state from a Virtual Machine State file.
 **/
int CVGACard::RestoreState(FILE *f) {
  long ss;
  u32 m1;
  u32 m2;
  int res;
  size_t r;

  if ((res = CPCIDevice::RestoreState(f)))
    return res;

  r = fread(&m1, sizeof(u32), 1, f);
  if (r != 1) {
    printf("%s: unexpected end of file!\n", devid_string);
    return -1;
  }

  if (m1 != state_magic1()) {
    printf("%s: MAGIC 1 does not match!\n", devid_string);
    return -1;
  }

  r = fread(&ss, sizeof(long), 1, f);
  if (r != 1) {
    printf("%s: unexpected end of file!\n", devid_string);
    return -1;
  }

  if (ss != sizeof(state)) {
    printf("%s: STRUCT SIZE does not match!\n", devid_string);
    return -1;
  }

  r = fread(&state, sizeof(state), 1, f);
  if (r != 1) {
    printf("%s: unexpected end of file!\n", devid_string);
    return -1;
  }

  // Never let a pointer out of the file reach this process, even though
  // nothing reads these two today (see SaveState).
  state.memory = nullptr;
  state.memsize = 0;

  r = fread(&m2, sizeof(u32), 1, f);
  if (r != 1) {
    printf("%s: unexpected end of file!\n", devid_string);
    return -1;
  }

  if (m2 != state_magic2()) {
    printf("%s: MAGIC 2 does not match!\n", devid_string);
    return -1;
  }

  printf("%s: %d bytes restored.\n", devid_string, (int)ss);
  return 0;
}

/**
 * Load the option ROM named by the "rom" config value.
 **/
void CVGACard::load_option_rom(const char *default_name) {
  const char *name = myCfg->get_text_value("rom", default_name);
  FILE *rom = fopen(name, "rb");
  if (!rom) {
    FAILURE_2(FileNotFound, "%s rom file %s not found", card_name(), name);
  }

  rom_max = (unsigned)fread(option_rom, 1, sizeof(option_rom), rom);
  fclose(rom);

  // Option ROM address space: C0000
  add_legacy_mem(LEGACY_MEM_ROM, 0xc0000, rom_max);
}

/**
 * Read from the option ROM.
 **/
u32 CVGACard::rom_read(u32 address, int dsize) {
  u32 data = 0x00;
  u8 *x = (u8 *)option_rom;
  if (address <= rom_max) {
    x += address;
    switch (dsize) {
    case 8:
      data = (u32)endian_8((*((u8 *)x)) & 0xff);
      break;
    case 16:
      data = (u32)endian_16((*((u16 *)x)) & 0xffff);
      break;
    case 32:
      data = (u32)endian_32((*((u32 *)x)) & 0xffffffff);
      break;
    }
  }

  return data;
}

u8 CVGACard::get_actl_palette_idx(u8 index) { return atc_palette(index); }

void CVGACard::redraw_area(unsigned x0, unsigned y0, unsigned width,
                           unsigned height) {
  if ((width == 0) || (height == 0))
    return;

  state.vga_mem_updated = 1;
}

void CVGACard::update() {
  unsigned iWidth = 0, iHeight = 0;

  /* no screen update necessary
     Gate on the card's own enable, ATC video enable and SR1 "Screen Off" */
  if (!display_enabled() || !atc_video_enabled())
    return;

  const bool screen_off = (vga.sequencer.data[1] & 0x20) != 0; // SR1 bit5
  if (screen_off)
    return;

  auto now = std::chrono::steady_clock::now();
  auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - m_last_refresh_time)
                        .count();

  if (elapsed_ms < (long long)timing.refresh_interval_ms)
    return;
  m_last_refresh_time = now;

  const uint8_t cur_mode = pc_vga_choosevideomode();

  if (cur_mode == SCREEN_OFF) {
    state.vga_mem_updated = 0;
    return;
  }

  // Dirty-gate: re-rasterize + re-upload only when something visible changed.
  // vga_mem_updated covers VRAM + CRTC text-cursor + palette + mode writes; a
  // hardware cursor is not flagged, so the card folds it into a signature.
  // Force a refresh every few frames so the cursor / blinking text still
  // animate on an otherwise static screen; tick_frame() keeps the blink
  // counter advancing on the skip path so blink timing stays correct.
  const int kBlinkRefreshFrames =
      8; // >= 2x the ~1.9 Hz VGA blink toggle at a 60 Hz refresh
  const uint64_t cursor_sig = hw_cursor_signature();
  if (!state.vga_mem_updated && cursor_sig == m_last_cursor_sig &&
      ++m_frames_since_render < kBlinkRefreshFrames) {
    screen().tick_frame(); // keep cursor/text-blink timing alive while skipping
                           // the render
    return;
  }
  m_frames_since_render = 0;
  m_last_cursor_sig = cursor_sig;

  vga.crtc.start_addr =
      vga.crtc.start_addr_latch; // FIXME: Figure out proper handling, but makes
                                 // BSD happy again....
  vga.attribute.pel_shift = vga.attribute.pel_shift_latch;

  determine_screen_dimensions(&iHeight, &iWidth);

  if (iWidth == 0 || iHeight == 0)
    return;

  // Update screen shim's visible area
  screen().set_visible_area(iWidth, iHeight);

  // Ensure bitmap is large enough
  m_render_bitmap.allocate(iWidth, iHeight);

  // Render via MAME's screen_update pipeline
  rectangle clip = m_render_bitmap.cliprect();
  screen_update(m_render_bitmap, clip);

  // Tick the frame counter (for cursor blink)
  screen().tick_frame();

  // MAME always produces ARGB32 — tell SDL we're in 32bpp mode.
  if (state.last_bpp != 32 || iWidth != old_iWidth || iHeight != old_iHeight) {
    bx_gui->dimension_update(iWidth, iHeight, 0, 0, 32);
    old_iWidth = iWidth;
    old_iHeight = iHeight;
    state.last_bpp = 32;
  }

  bx_gui->graphics_frame_update(m_render_bitmap.raw(), iWidth, iHeight);

  state.vga_mem_updated = 0;
}

void CVGACard::determine_screen_dimensions(unsigned *piHeight,
                                           unsigned *piWidth) {
  int ai[0x20];
  int i;
  int h;
  int v;
  for (i = 0; i < 0x20; i++)
    ai[i] = m_crtc_map.read_byte(i);

  h = (ai[1] + 1) * (seq_dotperchar() ? 8 : 9) / timing.divisor;
  v = (ai[18] | ((ai[7] & 0x02) << 7) | ((ai[7] & 0x40) << 3)) + 1;
  apply_extended_timing(h, v);
  v *= (get_interlace_mode() + 1); // interlaced mode

  if (vga.gc.shift256) {
    // was shift_reg == 2 mode 13h / 256-color byte mode
    // chain_four vs modeX
    *piWidth = h;
    *piHeight = v;
  } else if (vga.gc.shift_reg) {
    // was shift_reg == 1 CGA 4-color interleave
    if (x_dotclockdiv2())
      h <<= 1;
    *piWidth = h;
    *piHeight = v;
  } else {
    // was shift_reg == 0 standard VGA planar / EGA
    *piWidth = 640;
    *piHeight = 480;
    if (m_crtc_map.read_byte(0x06) == 0xBF) {
      if (m_crtc_map.read_byte(0x17) == 0xA3 &&
          m_crtc_map.read_byte(0x14) == 0x40 &&
          m_crtc_map.read_byte(0x09) == 0x41) {
        *piWidth = 320;
        *piHeight = 240;
      } else {
        if (x_dotclockdiv2())
          h <<= 1;
        *piWidth = h;
        *piHeight = v;
      }
    } else if ((h >= 640) && (v >= 480)) {
      *piWidth = h;
      *piHeight = v;
    }
  }
}

void CVGACard::palette_update() {
  CVGA::palette_update();

  for (int i = 0; i < 256; i++) {
    // pal6bit: expand 6-bit color to 8-bit
    u8 r = (vga.dac.color[3 * (i & vga.dac.mask) + 0] & 0x3f);
    u8 g = (vga.dac.color[3 * (i & vga.dac.mask) + 1] & 0x3f);
    u8 b = (vga.dac.color[3 * (i & vga.dac.mask) + 2] & 0x3f);
    // Expand 6-bit to 8-bit: (val << 2) | (val >> 4)
    r = (r << 2) | (r >> 4);
    g = (g << 2) | (g >> 4);
    b = (b << 2) | (b >> 4);
    bx_gui->palette_change((unsigned)i, (unsigned)r, (unsigned)g, (unsigned)b);
  }
}
