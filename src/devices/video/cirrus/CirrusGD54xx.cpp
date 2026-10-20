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

/**
 * \file
 * GD54xx family: construction, power-on state, PCI aperture and I/O port
 * routing.
 **/

#include "CirrusGD54xx.hpp"
#include "System.hpp"

#include <cstring>

using namespace cirrus;

CCirrusGD54xx::CCirrusGD54xx(CConfigurator *cfg, CSystem *c, int pcibus,
                             int pcidev, const cirrus_chip_config &chip)
    : CVGACard(cfg, c, pcibus, pcidev), m_chip(chip) {}

/**
 * Stops the render thread while this object is still whole: the thread
 * calls this card's hooks (see CVGACard::~CVGACard).
 **/
CCirrusGD54xx::~CCirrusGD54xx() {
  stop_threads();
  delete[] vga.memory;
  vga.memory = nullptr;
}

void CCirrusGD54xx::init() {
  // PCI header: a VGA-compatible display controller with one prefetchable
  // memory BAR (the linear aperture), no I/O BARs, no interrupt pin, and
  // no expansion ROM BAR -- the BIOS is found at 0xc0000.
  u32 cfg_data[64] = {};
  u32 cfg_mask[64] = {};
  cfg_data[0x00 >> 2] = (u32(m_chip.pci_device_id) << 16) | PCI_VENDOR_CIRRUS;
  cfg_data[0x04 >> 2] = 0x02000000; // status: medium DEVSEL
  cfg_data[0x08 >> 2] = 0x03000000 | m_chip.revision;
  cfg_data[0x10 >> 2] = 0x00000008; // prefetchable 32-bit memory
  cfg_data[0x3c >> 2] = 0x000000ff;
  cfg_mask[0x04 >> 2] = 0x0000ffff;
  cfg_mask[0x0c >> 2] = 0x0000ffff;
  cfg_mask[0x10 >> 2] = ~(m_chip.linear_bytes - 1);
  cfg_mask[0x3c >> 2] = 0x000000ff;
  add_function(0, cfg_data, cfg_mask);
  ResetPCI();

  memset((void *)&state, 0, sizeof(state));
  memset(&vga, 0, sizeof(vga));
  memset(&svga, 0, sizeof(svga));

  vga.svga_intf.vram_size = m_chip.vram_bytes;
  vga.memory = new u8[m_chip.vram_bytes];
  memset(vga.memory, 0, m_chip.vram_bytes);

  add_vga_legacy_ranges();
  init_maps();

  // Standard VGA power-on state.
  vga.gc.bit_mask = 0xff;
  vga.gc.memory_map_sel = 3; // colour text
  vga.sequencer.data[0] = 0x03;
  vga.sequencer.data[4] = 0x06;
  vga.sequencer.char_sel.base[0] = 0x20000;
  vga.sequencer.char_sel.base[1] = 0x20000;
  vga.crtc.line_compare = 1023;
  vga.crtc.vert_disp_end = 399;
  vga.dac.mask = 0xff;
  vga.dac.dirty = 1;

  // Cirrus power-on state.
  sequencer_reset();
  graphics_reset();
  crtc_reset();
  update_banks();
  define_video_mode();
  m_blitter.set_vram(vga.memory, m_chip.vram_bytes);
  m_blitter.reset();

  load_option_rom(m_chip.default_rom);

  state.last_bpp = 8;
  state.x_tilesize = X_TILESIZE;
  state.y_tilesize = Y_TILESIZE;
  state.vga_mem_updated = 1;

  timing.divisor = 1;
  timing.vrefresh_hz = 60.0;
  timing.refresh_interval_ms = 16;
  m_last_refresh_time = std::chrono::steady_clock::now();

  printf("%s: Cirrus Logic %s, %u KB\n", devid_string, m_chip.part,
         m_chip.vram_bytes / 1024);
}

/**
 * The card's own state for a snapshot, written after the VGA core's block:
 * the extended graphics controller registers (bank select, the BitBLT
 * register file, the extended write-mode colours), the hidden DAC, the
 * extended palette and the cursor/port bits kept outside the core. The bank
 * windows are derived from GR09 to GR0B, and an operation in progress is
 * dropped (the blitter is not in the file), so neither is stored.
 *
 * Files written before this block existed carry no 'CL54' record at this
 * place: they go straight to the core's end magic. restore_card_state
 * recognises that and leaves the power-on values in place, as such a file
 * always did.
 **/
namespace {
constexpr u32 kCirrusMagic = 0x434C3534; // 'CL54'
constexpr u32 kCirrusVersion = 1;
struct CirrusCardState {
  u32 version;
  u32 dac_arm;
  u8 gr[256];
  u8 ext_palette[16 * 3];
  u8 hidden_dac;
  u8 cursor_x_low;
  u8 cursor_y_low;
  u8 port_3c3;
};
} // namespace

int CCirrusGD54xx::save_card_state(FILE *f) {
  CirrusCardState x;
  memset(&x, 0, sizeof(x));
  x.version = kCirrusVersion;
  x.dac_arm = (u32)m_dac_arm;
  memcpy(x.gr, m_gr, sizeof(x.gr));
  memcpy(x.ext_palette, m_ext_palette, sizeof(x.ext_palette));
  x.hidden_dac = m_hidden_dac;
  x.cursor_x_low = m_cursor_x_low;
  x.cursor_y_low = m_cursor_y_low;
  x.port_3c3 = m_port_3c3;
  const long n = sizeof(x);
  fwrite(&kCirrusMagic, sizeof(u32), 1, f);
  fwrite(&n, sizeof(long), 1, f);
  if (fwrite(&x, 1, sizeof(x), f) != sizeof(x))
    return -1;
  fwrite(&kCirrusMagic, sizeof(u32), 1, f);
  return 0;
}

int CCirrusGD54xx::restore_card_state(FILE *f) {
  m_card_block_restored = false;
  // Nothing in the blitter survives a snapshot.
  m_blitter.reset();

  u32 m;
  const long pos = ftell(f);
  if (fread(&m, sizeof(u32), 1, f) != 1) {
    printf("%s: unexpected end of file!\n", devid_string);
    return -1;
  }
  if (m != kCirrusMagic) {
    // An older file: no card block, the core's end magic follows.
    if (fseek(f, pos, SEEK_SET)) {
      printf("%s: cannot reposition the state file!\n", devid_string);
      return -1;
    }
    return 0;
  }
  long n;
  CirrusCardState x;
  if (fread(&n, sizeof(long), 1, f) != 1 || n != (long)sizeof(x)) {
    printf("%s: Cirrus STRUCT SIZE does not match!\n", devid_string);
    return -1;
  }
  if (fread(&x, 1, sizeof(x), f) != sizeof(x) || fread(&m, 4, 1, f) != 1 ||
      m != kCirrusMagic) {
    printf("%s: Cirrus end MAGIC does not match!\n", devid_string);
    return -1;
  }
  if (x.version != kCirrusVersion) {
    printf("%s: Cirrus state version %u is not supported!\n", devid_string,
           x.version);
    return -1;
  }
  m_dac_arm = (int)x.dac_arm;
  memcpy(m_gr, x.gr, sizeof(m_gr));
  memcpy(m_ext_palette, x.ext_palette, sizeof(m_ext_palette));
  m_hidden_dac = x.hidden_dac;
  m_cursor_x_low = x.cursor_x_low;
  m_cursor_y_low = x.cursor_y_low;
  m_port_3c3 = x.port_3c3;
  m_card_block_restored = true;
  return 0;
}

void CCirrusGD54xx::post_restore() {
  if (!m_card_block_restored)
    return;
  update_banks();      // from GR09..GR0B
  define_video_mode(); // SR07 and the hidden DAC
}

void CCirrusGD54xx::recompute_params() {
  define_video_mode();
  state.vga_mem_updated = 1;
}

/**
 * Line length in bytes. Packed-pixel modes use eight bytes per CR13 unit
 * (CR1B supplies its bit 8); planar modes keep the VGA rule.
 **/
uint16_t CCirrusGD54xx::offset() {
  if (vga.sequencer.data[SEQ_EXT_MODE] & SEQ_EXT_MODE_SVGA)
    return vga.crtc.offset << 3;
  return CVGA::offset();
}

u8 CCirrusGD54xx::io_read_b(u32 address) {
  switch (address) {
  case 0x3c3:
    return m_port_3c3;

  case 0x3c6:
    return dac_mask_read();

  case 0x3c7:
  case 0x3c8:
    m_dac_arm = 0;
    break;

  case 0x3c9: {
    m_dac_arm = 0;
    u8 data;
    if (dac_data_read(data))
      return data;
    break;
  }
  }
  return CVGACard::io_read_b(address);
}

void CCirrusGD54xx::io_write_b(u32 address, u8 data) {
  switch (address) {
  case 0x3c3:
    m_port_3c3 = data;
    return;

  case 0x3c6:
    dac_mask_write(data);
    return;

  case 0x3c7:
  case 0x3c8:
    m_dac_arm = 0;
    break;

  case 0x3c9:
    m_dac_arm = 0;
    if (dac_data_write(data))
      return;
    break;
  }
  CVGACard::io_write_b(address, data);
}

/**
 * PCI BAR0: the linear aperture, little-endian multi-byte access.
 **/
u32 CCirrusGD54xx::ReadMem_Bar(int func, int bar, u32 address, int dsize) {
  if (bar != 0)
    return 0;

  u32 data = 0;
  for (int i = 0; i < dsize / 8; i++)
    data |= u32(mem_linear_r(address + i)) << (8 * i);
  return data;
}

void CCirrusGD54xx::WriteMem_Bar(int func, int bar, u32 address, int dsize,
                                 u32 data) {
  if (bar != 0)
    return;

  for (int i = 0; i < dsize / 8; i++)
    mem_linear_w(address + i, u8(data >> (8 * i)));
}
