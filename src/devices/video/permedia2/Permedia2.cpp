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
 * Permedia 2: construction, power-on state, the PCI header and its
 * indirect window, the SVGA's extended registers, region routing, the
 * state file.
 **/

#include "Permedia2.hpp"
#include "System.hpp"

using namespace permedia2;

CPermedia2::CPermedia2(CConfigurator *cfg, CSystem *c, int pcibus, int pcidev)
    : CVGACard(cfg, c, pcibus, pcidev) {
  memset(&r, 0, sizeof(r));
}

/**
 * Stops the render thread while this object is still whole: the thread
 * calls this card's hooks (see CVGACard::~CVGACard).
 **/
CPermedia2::~CPermedia2() {
  stop_threads();
  delete[] vga.memory;
  vga.memory = nullptr;
}

void CPermedia2::init() {
  // "memory" is the SGRAM fitted, in MB: 2, 4, 6 or 8, one to four 2 MB
  // banks (MemConfig's NumberBanks). The boards shipped with 4 or 8.
  const u64 mb = myCfg->get_num_value("memory", false, 8);
  if (mb != 2 && mb != 4 && mb != 6 && mb != 8)
    FAILURE_1(Configuration,
              "permedia2: memory must be 2, 4, 6 or 8 (MB), not %llu",
              (unsigned long long)mb);
  m_vram_bytes = u32(mb) << 20;

  // The BIOS first: the subsystem IDs are in its last four bytes.
  load_option_rom("SYN80700.PAN");

  // PCI header (2.13, 2.14). With the SVGA at its fixed addresses the
  // class is a VGA-compatible display controller. Region 0 is 128 KB of
  // registers, regions 1 and 2 are the two 8 MB memory apertures, none
  // prefetchable; the expansion ROM is 64 KB; interrupt pin INTA. The
  // subsystem IDs come from ROM bytes 0xfffc-0xffff, as ChipConfig's
  // SubSystemFromROM says the board is strapped to (a 32 KB image sits in
  // a part decoded twice over the 64 KB). 0xf8/0xfc are the indirect
  // window onto the regions.
  const u32 rom_top = rom_max >= 4 ? rom_max - 4 : 0;
  const u32 subsystem = u32(option_rom[rom_top]) |
                        (u32(option_rom[rom_top + 1]) << 8) |
                        (u32(option_rom[rom_top + 2]) << 16) |
                        (u32(option_rom[rom_top + 3]) << 24);
  u32 cfg_data[64] = {};
  u32 cfg_mask[64] = {};
  cfg_data[0x00 >> 2] = (u32(PCI_DEVICE_PERMEDIA2) << 16) | PCI_VENDOR_TI;
  cfg_data[0x04 >> 2] = 0x02800000; // status: medium DEVSEL, fast back-to-back
  cfg_data[0x08 >> 2] = 0x03000000 | PCI_REVISION;
  cfg_data[0x2c >> 2] = subsystem;
  cfg_data[0x3c >> 2] = 0x000001ff; // interrupt pin INTA
  cfg_mask[0x04 >> 2] = 0x00000027; // I/O, memory, bus master, palette snoop
  cfg_mask[0x0c >> 2] = 0x0000ffff;
  cfg_mask[0x10 >> 2] = ~(REGION0_BYTES - 1);
  cfg_mask[0x14 >> 2] = ~(APERTURE_BYTES - 1);
  cfg_mask[0x18 >> 2] = ~(APERTURE_BYTES - 1);
  cfg_mask[0x30 >> 2] = ~(ROM_BYTES - 1) | PCI_ROM_ADDRESS_ENABLE;
  cfg_mask[0x3c >> 2] = 0x000000ff;
  cfg_mask[CFG_INDIRECT_ADDR >> 2] = 0xf07fffff;
  add_function(0, cfg_data, cfg_mask);
  ResetPCI();

  memset((void *)&state, 0, sizeof(state));
  memset(&vga, 0, sizeof(vga));
  memset(&svga, 0, sizeof(svga));

  vga.svga_intf.vram_size = m_vram_bytes;
  vga.memory = new u8[m_vram_bytes];
  memset(vga.memory, 0, m_vram_bytes);

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

  // Permedia 2 power-on state (the manual's reset values).
  memset(&r, 0, sizeof(r));
  r.ctl[IN_FIFO_SPACE >> 2] = 0x20;
  r.ctl[CHIP_CONFIG >> 2] = CHIP_CONFIG_RESET;
  r.ctl[BOOT_ADDRESS >> 2] = 0x31;
  r.ctl[MEM_CONFIG >> 2] = MEM_CONFIG_RESET;
  r.ctl[FIFO_CONTROL >> 2] = 0x1010;
  r.ctl[VS_CONFIGURATION >> 2] = 0x1f0;
  r.ctl[VS_SERIAL_BUS_CONTROL >> 2] = 0x0c;
  r.ctl[DISPLAY_DATA >> 2] = DD_DATA_OUT | DD_CLK_OUT;
  r.vga_control = VGACTL_RESET;
  r.rd_indexed[RDI_PIXEL_CLOCK_A1 + 0] = 0x1c; // A: 25.06 MHz
  r.rd_indexed[RDI_PIXEL_CLOCK_A1 + 1] = 0x02;
  r.rd_indexed[RDI_PIXEL_CLOCK_A1 + 2] = 0x0b;
  r.rd_indexed[RDI_PIXEL_CLOCK_A1 + 3] = 0x10; // B: 28.64 MHz
  r.rd_indexed[RDI_PIXEL_CLOCK_A1 + 4] = 0x02;
  r.rd_indexed[RDI_PIXEL_CLOCK_A1 + 5] = 0x0b;
  r.rd_indexed[RDI_MEMORY_CLOCK_1 + 0] = 0x1c; // 50.11 MHz
  r.rd_indexed[RDI_MEMORY_CLOCK_1 + 1] = 0x02;
  r.rd_indexed[RDI_MEMORY_CLOCK_1 + 2] = 0x02;
  ddc_attach_monitor();

  state.last_bpp = 8;
  state.x_tilesize = X_TILESIZE;
  state.y_tilesize = Y_TILESIZE;
  state.vga_mem_updated = 1;

  timing.divisor = 1;
  timing.vrefresh_hz = 60.0;
  timing.refresh_interval_ms = 16;
  m_last_refresh_time = std::chrono::steady_clock::now();

  m_trace = getenv("ALPHABOX_TRACE_PERMEDIA2") != nullptr;
  printf("%s: 3Dlabs Permedia 2, %u KB, subsystem %04x:%04x\n", devid_string,
         m_vram_bytes / 1024, subsystem & 0xffff, subsystem >> 16);
}

/**
 * The indirect data register (0xfc) is a window, not storage: a read
 * fetches the dword the indirect address names.
 **/
u32 CPermedia2::config_read_custom(int func, u32 address, int dsize, u32 data) {
  if (address == CFG_INDIRECT_DATA && dsize == 32) {
    const u32 a = config_read(0, CFG_INDIRECT_ADDR, 32);
    return region_read(int(a >> 28), a & 0x7ffffc, 32);
  }
  if (m_trace)
    printf("%s: config read  %02x/%d = %08x\n", devid_string, address, dsize,
           data);
  return data;
}

void CPermedia2::config_write_custom(int func, u32 address, int dsize,
                                     u32 old_data, u32 new_data, u32 data) {
  if (address == CFG_INDIRECT_DATA && dsize == 32) {
    const u32 a = config_read(0, CFG_INDIRECT_ADDR, 32);
    region_write(int(a >> 28), a & 0x7ffffc, 32, data);
    return;
  }
  // COMMAND's memory enable and BAR1 decide where, and whether, the
  // first aperture is.
  if (address <= 0x05 || (address >= 0x14 && address <= 0x17))
    refresh_direct_aperture();
}

u32 CPermedia2::region_read(int region, u32 offset, int dsize) {
  const int bytes = dsize / 8;
  switch (region) {
  case 0: {
    // The second 64 KB are the first byte-swapped, for big-endian hosts.
    const bool swapped = (offset & REGION0_HALF) != 0;
    u32 off = offset & (REGION0_HALF - 1);
    if (!swapped)
      return control_read(off, bytes);
    const u32 v = control_read(off & ~3u, 4);
    const u32 s =
        (v << 24) | ((v & 0xff00) << 8) | ((v >> 8) & 0xff00) | (v >> 24);
    return (s >> (8 * (off & 3))) &
           (bytes == 4 ? 0xffffffffu : (1u << (8 * bytes)) - 1);
  }
  case 1:
  case 2:
    return aperture_read(region, offset & (APERTURE_BYTES - 1), dsize);
  case 7:
    return rom_read(offset & (rom_max - 1), dsize);
  }
  return 0;
}

void CPermedia2::region_write(int region, u32 offset, int dsize, u32 data) {
  const int bytes = dsize / 8;
  switch (region) {
  case 0: {
    const bool swapped = (offset & REGION0_HALF) != 0;
    const u32 off = offset & (REGION0_HALF - 1);
    if (!swapped) {
      control_write(off, bytes, data);
      return;
    }
    if (bytes != 4) // byte-swapped partial writes: not used by any driver
      return;
    control_write(off, 4,
                  (data << 24) | ((data & 0xff00) << 8) |
                      ((data >> 8) & 0xff00) | (data >> 24));
    return;
  }
  case 1:
  case 2:
    aperture_write(region, offset & (APERTURE_BYTES - 1), dsize, data);
    return;
  }
}

u32 CPermedia2::ReadMem_Bar(int func, int bar, u32 address, int dsize) {
  u32 v;
  switch (bar) {
  case 0:
  case 1:
  case 2:
    v = region_read(bar, address, dsize);
    break;
  case 6:
    v = rom_read(address & (rom_max - 1), dsize);
    break;
  default:
    return 0;
  }
  if (m_trace)
    printf("%s: bar%d read  %05x/%d = %0*x\n", devid_string, bar, address,
           dsize / 8, dsize / 4, v);
  return v;
}

void CPermedia2::WriteMem_Bar(int func, int bar, u32 address, int dsize,
                              u32 data) {
  if (m_trace && bar == 0)
    printf("%s: r0 write %05x/%d = %0*x\n", devid_string, address, dsize / 8,
           dsize / 4, data);
  if (bar <= 2)
    region_write(bar, address, dsize, data);
}

/**
 * The SVGA's ports. The DAC's four (0x3c6-0x3c9) are the RAMDAC's
 * direct registers 0-3, or 4-15 with VGAControl's DacAddr bits set; they
 * answer only while VGAControl lets the host reach the DAC.
 **/
static int dac_port_reg(u32 port) {
  switch (port) {
  case 0x3c8:
    return 0;
  case 0x3c9:
    return 1;
  case 0x3c6:
    return 2;
  case 0x3c7:
    return 3;
  }
  return -1;
}

u8 CPermedia2::io_read_b(u32 address) {
  const int rs = dac_port_reg(address);
  if (rs >= 0) {
    if (!(r.vga_control & VGACTL_HOST_DAC))
      return 0xff;
    const int hi = (r.vga_control >> VGACTL_DAC_ADDR_SHIFT) & 3;
    if (hi)
      return ramdac_read(rs | (hi << 2));
  }
  u8 v = CVGACard::io_read_b(address);
  if (m_trace && address != 0x3da)
    printf("%s: port read  %03x = %02x\n", devid_string, address, v);
  return v;
}

void CPermedia2::io_write_b(u32 address, u8 data) {
  if (m_trace)
    printf("%s: port write %03x = %02x\n", devid_string, address, data);
  const int rs = dac_port_reg(address);
  if (rs >= 0) {
    if (!(r.vga_control & VGACTL_HOST_DAC))
      return;
    const int hi = (r.vga_control >> VGACTL_DAC_ADDR_SHIFT) & 3;
    if (hi) {
      ramdac_write(rs | (hi << 2), data);
      return;
    }
    CVGACard::io_write_b(address, data);
    if (rs == RD_PALETTE_WRITE_ADDRESS)
      r.rd_index = data;
    return;
  }
  CVGACard::io_write_b(address, data);
}

/**
 * SR5, VGAControl: which of the SVGA and the graphics processor has the
 * display, the host's way to memory and the DAC, the DAC's upper address
 * bits.
 **/
void CPermedia2::sequencer_map(address_map &map) {
  map(SEQ_VGA_CONTROL, SEQ_VGA_CONTROL)
      .lrw8(NAME([this](offs_t) { return u8(r.vga_control & 0x7f); }),
            NAME([this](offs_t, u8 data) {
              if ((r.vga_control ^ data) & VGACTL_VGA_DISPLAY)
                state.vga_mem_updated = 1;
              r.vga_control = data & 0x7f;
            }));
}

/**
 * GR9, Mode640: the banks that stand in for the planes when the SVGA's
 * memory is addressed as 640-wide 256-colour pixels.
 **/
void CPermedia2::gc_map(address_map &map) {
  map(GC_MODE640, GC_MODE640)
      .lrw8(NAME([this](offs_t) { return u8(r.mode640 & ~M640_START_BIT16); }),
            NAME([this](offs_t, u8 data) {
              r.mode640 = data;
              state.vga_mem_updated = 1;
            }));
}

/**
 * State file: the register file. The memory is the VGA core's.
 **/
static constexpr u32 kPermedia2Magic = 0x32444D50; // 'PMD2'

int CPermedia2::save_card_state(FILE *f) {
  fwrite(&kPermedia2Magic, sizeof(u32), 1, f);
  long sz = sizeof(r);
  fwrite(&sz, sizeof(long), 1, f);
  fwrite(&r, sizeof(r), 1, f);
  fwrite(&kPermedia2Magic, sizeof(u32), 1, f);
  return 0;
}

int CPermedia2::restore_card_state(FILE *f) {
  u32 m;
  long sz;
  if (fread(&m, sizeof(u32), 1, f) != 1 || m != kPermedia2Magic) {
    printf("%s: Permedia 2 MAGIC does not match!\n", devid_string);
    return -1;
  }
  if (fread(&sz, sizeof(long), 1, f) != 1 || sz != (long)sizeof(r) ||
      fread(&r, sizeof(r), 1, f) != 1) {
    printf("%s: Permedia 2 register block does not match!\n", devid_string);
    return -1;
  }
  if (fread(&m, sizeof(u32), 1, f) != 1 || m != kPermedia2Magic) {
    printf("%s: Permedia 2 end MAGIC does not match!\n", devid_string);
    return -1;
  }
  return 0;
}

void CPermedia2::post_restore() {
  m_int_asserted = false;
  update_int_line();
  refresh_direct_aperture();
}
