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
 * Mach64: construction, power-on state, PCI header, I/O port and aperture
 * routing, the state file.
 **/

#include "Mach64.hpp"
#include "System.hpp"

using namespace mach64;

/* The parts. CONFIG_CHIP_ID carries the type in its low half and the
 * revision in the top byte, as 86Box reports them. VRAM is what the boards
 * shipped with; the BIOS sizes it by probing the aperture. */
static const mach64_chip_config mach64_chips[] = {
    {"ct", "Mach64 CT", PCI_DEVICE_CT, 0x00004354, 0x40, 2u << 20,
     "mach64ct.bin", false, false, false},
    {"vt2", "264VT2", PCI_DEVICE_VT2, 0x40005654, 0x40, 4u << 20,
     "mach64vt2.bin", false, false, false},
    {"vt3", "264VT3", PCI_DEVICE_VT3, 0x9a005655, 0x40, 4u << 20,
     "mach64vt2.bin", false, false, false},
    // The 3D Rage II+: the VT's register file and the GT's 3D engine. Its
    // ASIC ID, 9Ah, is also its PCI revision (RRG-G02700 ch. 4, 7), and the
    // revision Windows 2000's DISPLAY.INF names for II+ parts; the inbox
    // driver it binds is atirage.sys/atirage.dll, which has a Direct3D HAL.
    // It also has the auxiliary register aperture at BAR2: atirage takes
    // its register base from the card's third PCI resource and, without
    // one, gets the ROM's and polls the vertical blank there for ever.
    {"rage2p", "3D Rage II+ (264GT-B)", PCI_DEVICE_GTB, 0x9a004755, 0x9a,
     4u << 20, "rageii-pci.bin", true, false, true},
    // The 3D Rage Pro (GB): the triangle setup engine in place of the GT's
    // trapezoids. ASIC ID 5Ch (UMC A4, RRG-G03300's revision table) as
    // CONFIG_CHIP_ID[31:24] and the PCI revision; Windows 2000 binds it to
    // atimpab.sys/atidrab.dll, whose Direct3D HAL ATI offers from the Rage
    // Pro on (their chip tables: family 8). The auxiliary register
    // aperture is documented for it (RRG-G03300 2-15).
    {"ragepro", "3D Rage Pro (GB)", PCI_DEVICE_GB, 0x5c004742, 0x5c, 8u << 20,
     "rage2pr-bga-40212-103-mx27c512.bin", false, true, true},
};

const mach64_chip_config *mach64_chip_by_name(const char *name) {
  for (const auto &c : mach64_chips)
    if (strcmp(c.name, name) == 0)
      return &c;
  return nullptr;
}

/* Sparse I/O: group n answers at 0x02EC + n * 0x400 for the block-0
 * register below (-1: nothing there). The four ports of a group are the
 * register's four bytes. */
static const int sparse_io_reg[SPARSE_IO_GROUPS] = {
    CRTC_H_TOTAL_DISP, // 02EC
    CRTC_H_SYNC_STRT_WID,
    CRTC_V_TOTAL_DISP,
    CRTC_V_SYNC_STRT_WID,
    CRTC_VLINE_CRNT_VLINE,
    CRTC_OFF_PITCH,
    CRTC_INT_CNTL,
    CRTC_GEN_CNTL, // 1EEC
    OVR_CLR,       // 22EC
    OVR_WID_LEFT_RIGHT,
    OVR_WID_TOP_BOTTOM,
    CUR_CLR0, // 2EEC
    CUR_CLR1,
    CUR_OFFSET,
    CUR_HORZ_VERT_POSN,
    CUR_HORZ_VERT_OFF,
    SCRATCH_REG0, // 42EC
    SCRATCH_REG1,
    CLOCK_CNTL, // 4AEC
    -1,         // 4EEC
    MEM_CNTL,   // 52EC
    MEM_VGA_WP_SEL,
    MEM_VGA_RP_SEL,
    DAC_REGS, // 5EEC
    DAC_CNTL,
    GEN_TEST_CNTL, // 66EC
    CONFIG_CNTL,   // 6AEC
    CONFIG_CHIP_ID,
    CONFIG_STAT0, // 72EC
    -1,           // 76EC
    -1,           // 7AEC
    -1,           // 7EEC
};

CMach64::CMach64(CConfigurator *cfg, CSystem *c, int pcibus, int pcidev,
                 const mach64_chip_config &chip)
    : CVGACard(cfg, c, pcibus, pcidev), m_chip(chip) {
  memset(&r, 0, sizeof(r));
  memset(&accel, 0, sizeof(accel));
}

/**
 * Stops the render thread while this object is still whole: the thread
 * calls this card's hooks (see CVGACard::~CVGACard).
 **/
CMach64::~CMach64() {
  stop_threads();
  delete[] vga.memory;
  vga.memory = nullptr;
}

void CMach64::init() {
  // "memory" picks the framebuffer size in MB (1, 2, 4, 8); the chip's
  // own default otherwise.
  const u64 mb = myCfg->get_num_value("memory", false, m_chip.vram_bytes >> 20);
  if (mb != 1 && mb != 2 && mb != 4 && mb != 8)
    FAILURE_1(Configuration,
              "mach64: memory must be 1, 2, 4 or 8 (MB), not %llu",
              (unsigned long long)mb);
  m_vram_bytes = u32(mb) << 20;

  // PCI header: a VGA-compatible display controller with the 16 MB
  // memory aperture (BAR0, prefetchable), the 256-byte block I/O register
  // window (BAR1), on the parts that have it the 4 KB register aperture
  // (BAR2), and a 64 KB expansion ROM. The console finds the BIOS
  // at 0xc0000 like the other cards', but ATI's Windows miniport reads the
  // BIOS's data tables through the ROM BAR, so this card exposes one. 0x40
  // is ATI's I/O configuration register: bits 1..0 pick the sparse I/O
  // base (0: 0x2EC), bit 2 enables the block I/O BAR, as the CT powers up.
  u32 cfg_data[64] = {};
  u32 cfg_mask[64] = {};
  cfg_data[0x00 >> 2] = (u32(m_chip.pci_device_id) << 16) | PCI_VENDOR_ATI;
  cfg_data[0x04 >> 2] = 0x02000000; // status: medium DEVSEL
  cfg_data[0x08 >> 2] = 0x03000000 | m_chip.revision;
  cfg_data[0x10 >> 2] = 0x00000008; // prefetchable 32-bit memory
  cfg_data[0x14 >> 2] = 0x00000001; // I/O
  if (m_chip.aux_regs) {
    cfg_data[0x18 >> 2] = 0x00000000; // 32-bit memory, not prefetchable
    cfg_mask[0x18 >> 2] = ~(AUX_APERTURE_BYTES - 1);
  }
  cfg_data[0x3c >> 2] = 0x000000ff;
  cfg_data[0x40 >> 2] = 0x00000004;
  cfg_mask[0x04 >> 2] = 0x0000ffff;
  cfg_mask[0x0c >> 2] = 0x0000ffff;
  cfg_mask[0x10 >> 2] = ~(APERTURE_BYTES - 1);
  cfg_mask[0x14 >> 2] = 0xffffff00;
  cfg_mask[0x30 >> 2] =
      ~(u32(sizeof(option_rom)) - 1) | PCI_ROM_ADDRESS_ENABLE;
  cfg_mask[0x3c >> 2] = 0x000000ff;
  cfg_mask[0x40 >> 2] = 0x00000007;
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

  // The sparse I/O groups and the extended VGA index/data pair.
  for (int n = 0; n < SPARSE_IO_GROUPS; n++)
    add_legacy_io(LEGACY_IO_SPARSE + n, SPARSE_IO_BASE + (u32(n) << 10), 4);
  add_legacy_io(LEGACY_IO_ATI_EXT, ATI_EXT_INDEX, 2);

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

  // Mach64 power-on state.
  memset(&r, 0, sizeof(r));
  r.config_chip_id = m_chip.config_chip_id;
  r.config_stat0 = 4;
  r.dac_cntl = DAC_TYPE_INTERNAL;
  r.dst_cntl = DST_X_DIR | DST_Y_DIR;
  r.port_3c3 = 0x01;
  r.gui_traj_cntl = DST_X_DIR | DST_Y_DIR;
  switch (m_vram_bytes >> 20) {
  case 1:
    r.mem_cntl = MEM_1M;
    break;
  case 2:
    r.mem_cntl = MEM_2M;
    break;
  case 4:
    r.mem_cntl = MEM_4M;
    break;
  default:
    r.mem_cntl = MEM_8M;
    break;
  }
  update_banks();
  m_eeprom.init(8); // 93C66: 256 words
  for (auto &w : m_eeprom.data)
    w = 0xffff; // blank, as a card whose BIOS has not written it yet
  ddc_attach_monitor();

  load_option_rom(m_chip.default_rom);

  state.last_bpp = 8;
  state.x_tilesize = X_TILESIZE;
  state.y_tilesize = Y_TILESIZE;
  state.vga_mem_updated = 1;

  timing.divisor = 1;
  timing.vrefresh_hz = 60.0;
  timing.refresh_interval_ms = 16;
  m_last_refresh_time = std::chrono::steady_clock::now();

  if (const char *t = getenv("ALPHABOX_TRACE_MACH64")) {
    m_trace_new = strcmp(t, "new") == 0 || strcmp(t, "trap") == 0;
    m_trace_trap = strcmp(t, "trap") == 0;
    m_trace = !m_trace_new;
  }
  printf("%s: ATI %s, %u KB\n", devid_string, m_chip.part, m_vram_bytes / 1024);
}

u32 CMach64::config_read_custom(int func, u32 address, int dsize, u32 data) {
  if (m_trace_new)
    trace_hit(0x441);
  if (m_trace)
    printf("%s: config read  %02x/%d = %08x\n", devid_string, address, dsize,
           data);
  return data;
}

void CMach64::config_write_custom(int func, u32 address, int dsize,
                                  u32 old_data, u32 new_data, u32 data) {
  if (m_trace)
    printf("%s: config write %02x/%d = %08x (now %08x)\n", devid_string,
           address, dsize, data, new_data);
  // COMMAND's memory enable and BAR0 decide where, and whether, the
  // aperture is.
  if (address <= 0x05 || (address >= 0x10 && address <= 0x13))
    refresh_direct_aperture();
}

/**
 * Offer the aperture's VRAM to the CPUs as plain memory while it is
 * decoded (COMMAND memory enable, BAR0 placed) and the card sits on the
 * hose -- a bridge forwards its ranges elsewhere -- and withdraw it the
 * moment either changes, which also flushes every CPU's page cache. The
 * offer stops short of the register page at the top of the half: with 8
 * MB installed, the last 4 KB of VRAM are behind the registers there, as
 * on the chip. ALPHABOX_LFB_DIRECT=0 keeps every access trapping, as the
 * S3's does.
 **/
void CMach64::refresh_direct_aperture() {
  static const bool enabled = [] {
    const char *e = getenv("ALPHABOX_LFB_DIRECT");
    return !(e && e[0] == '0');
  }();
  const u32 cmd = config_read(0, 0x04, 16);
  const u32 bar0 = config_read(0, 0x10, 32) & 0xfffffff0u;
  u64 base = 0, size = 0;
  if (enabled && !myBridge && (cmd & 0x0002) && bar0 != 0 && vga.memory) {
    base = bus_address(false, bar0);
    size = m_vram_bytes;
    if (size > APERTURE_HALF - APERTURE_REG_PAGE)
      size = APERTURE_HALF - APERTURE_REG_PAGE;
  }
  if (base == m_direct_base && size == m_direct_size)
    return;
  m_direct_base = base;
  m_direct_size = size;
  printf("%s: aperture %s for direct access (%llx + %llx)\n", devid_string,
         size ? "offered" : "withdrawn", (unsigned long long)base,
         (unsigned long long)size);
  cSystem->set_direct_memory(base, size, size ? vga.memory : nullptr);
}

/**
 * VGA ports the base does not claim: 0x3c3 (video subsystem enable) and
 * the two undefined ports next to it, which the BIOS touches and which are
 * dead on this card.
 **/
u8 CMach64::io_read_b(u32 address) {
  if (m_trace_new && address >= 0x3c0 && address <= 0x3ff)
    trace_hit(0x400 + (address - 0x3c0));
  if (m_trace && address != 0x3da) {
    const u8 v = io_read_b_traced(address);
    printf("%s: port read  %03x = %02x\n", devid_string, address, v);
    return v;
  }
  return io_read_b_traced(address);
}

u8 CMach64::io_read_b_traced(u32 address) {
  switch (address) {
  case 0x3c3:
    return r.port_3c3;
  case 0x3cb:
  case 0x3cd:
    return 0;
  }
  return CVGACard::io_read_b(address);
}

void CMach64::io_write_b(u32 address, u8 data) {
  if (m_trace)
    printf("%s: port write %03x = %02x\n", devid_string, address, data);
  switch (address) {
  case 0x3c3:
    r.port_3c3 = data;
    return;
  case 0x3cb:
  case 0x3cd:
    return;
  }
  CVGACard::io_write_b(address, data);
}

/**
 * The card's own I/O ranges: a sparse I/O group is one register's four
 * bytes; 0x1ce/0x1cf index the extended VGA registers.
 **/
u32 CMach64::card_legacy_read(int index, u32 address, int dsize) {
  m_trace_path = "sparse";
  if (index >= LEGACY_IO_SPARSE &&
      index < LEGACY_IO_SPARSE + SPARSE_IO_GROUPS) {
    const int reg = sparse_io_reg[index - LEGACY_IO_SPARSE];
    if (reg < 0)
      return 0;
    return reg_read(REG_BLOCK0 | u32(reg) | (address & 3), dsize / 8);
  }
  if (index == LEGACY_IO_ATI_EXT) {
    u32 data = 0;
    for (int i = 0; i < dsize / 8; i++) {
      const u32 port = ATI_EXT_INDEX + address + i;
      const u8 b = (port == ATI_EXT_INDEX) ? r.ext_index
                                           : r.ext_regs[r.ext_index & 0x3f];
      data |= u32(b) << (8 * i);
    }
    return data;
  }
  return 0;
}

void CMach64::card_legacy_write(int index, u32 address, int dsize, u32 data) {
  m_trace_path = "sparse";
  if (index >= LEGACY_IO_SPARSE &&
      index < LEGACY_IO_SPARSE + SPARSE_IO_GROUPS) {
    const int reg = sparse_io_reg[index - LEGACY_IO_SPARSE];
    if (reg < 0)
      return;
    reg_write(REG_BLOCK0 | u32(reg) | (address & 3), dsize / 8, data);
    return;
  }
  if (index == LEGACY_IO_ATI_EXT) {
    for (int i = 0; i < dsize / 8; i++) {
      const u32 port = ATI_EXT_INDEX + address + i;
      const u8 b = u8(data >> (8 * i));
      if (port == ATI_EXT_INDEX)
        r.ext_index = b;
      else
        r.ext_regs[r.ext_index & 0x3f] = b;
    }
  }
}

/**
 * BAR0 is the memory aperture; BAR1 the block I/O window onto the first
 * 256 bytes of block 0; BAR2 the auxiliary register aperture, block 1 at
 * 0 and block 0 at 0x400, the upper 2 KB reserved (RRG-G03300 2-15);
 * BAR6 the option ROM.
 **/
static const char *bar_path(int bar) {
  return bar == 0 ? "aperture" : bar == 2 ? "auxregs" : "blockio";
}

u32 CMach64::ReadMem_Bar(int func, int bar, u32 address, int dsize) {
  m_trace_path = bar_path(bar);
  switch (bar) {
  case 0:
    return aperture_read(address, dsize);
  case 1:
    return reg_read(REG_BLOCK0 | (address & 0xff), dsize / 8);
  case 2:
    if (address >= REG_BLOCK_BYTES)
      return 0;
    return reg_read(address, dsize / 8);
  case 6: {
    const u32 v = rom_read(address, dsize);
    if (m_trace)
      printf("%s: rom read  %05x/%d = %0*x\n", devid_string, address, dsize / 8,
             dsize / 4, v);
    if (m_trace_new)
      trace_hit(0x440);
    return v;
  }
  }
  return 0;
}

void CMach64::WriteMem_Bar(int func, int bar, u32 address, int dsize,
                           u32 data) {
  m_trace_path = bar_path(bar);
  switch (bar) {
  case 0:
    aperture_write(address, dsize, data);
    return;
  case 1:
    reg_write(REG_BLOCK0 | (address & 0xff), dsize / 8, data);
    return;
  case 2:
    if (address < REG_BLOCK_BYTES)
      reg_write(address, dsize / 8, data);
    return;
  }
}

/**
 * State file: the register file, then the EEPROM. The engine's working
 * state is not saved: a command completes within the write that starts
 * it, and a host-data transfer resumes from its registers.
 **/
static constexpr u32 kMach64Magic = 0x36344D41; // 'AM64'

int CMach64::save_card_state(FILE *f) {
  fwrite(&kMach64Magic, sizeof(u32), 1, f);
  long sz = sizeof(r);
  fwrite(&sz, sizeof(long), 1, f);
  fwrite(&r, sizeof(r), 1, f);
  sz = sizeof(m_eeprom.data);
  fwrite(&sz, sizeof(long), 1, f);
  fwrite(m_eeprom.data, sizeof(m_eeprom.data), 1, f);
  fwrite(&kMach64Magic, sizeof(u32), 1, f);
  return 0;
}

int CMach64::restore_card_state(FILE *f) {
  u32 m;
  long sz;
  if (fread(&m, sizeof(u32), 1, f) != 1 || m != kMach64Magic) {
    printf("%s: Mach64 MAGIC does not match!\n", devid_string);
    return -1;
  }
  if (fread(&sz, sizeof(long), 1, f) != 1 || sz != (long)sizeof(r) ||
      fread(&r, sizeof(r), 1, f) != 1) {
    printf("%s: Mach64 register block does not match!\n", devid_string);
    return -1;
  }
  if (fread(&sz, sizeof(long), 1, f) != 1 ||
      sz != (long)sizeof(m_eeprom.data) ||
      fread(m_eeprom.data, sizeof(m_eeprom.data), 1, f) != 1) {
    printf("%s: Mach64 EEPROM block does not match!\n", devid_string);
    return -1;
  }
  if (fread(&m, sizeof(u32), 1, f) != 1 || m != kMach64Magic) {
    printf("%s: Mach64 end MAGIC does not match!\n", devid_string);
    return -1;
  }
  return 0;
}

void CMach64::post_restore() {
  update_banks();
  accel.busy = false;
  refresh_direct_aperture();
}
