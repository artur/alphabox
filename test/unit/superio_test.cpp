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

/* Unit test of the ALi M1543C's Super I/O configuration ports and ISA Plug
 * and Play ports (src/devices/isa/SuperIo.cpp), driven through their ports
 * as a console or OS does; the I/O ranges they ask the bridge for are
 * recorded. Built with every lane as `superio_test`; exit status 0 when
 * every check passes.
 */

#include "SuperIo.hpp"

#include <cstdio>
#include <cstring>
#include <string>

static int checks = 0, failures = 0;

#define CHECK_EQ(got, want)                                                    \
  do {                                                                         \
    const unsigned g_ = (unsigned)(got), w_ = (unsigned)(want);                \
    checks++;                                                                  \
    if (g_ != w_) {                                                            \
      failures++;                                                              \
      printf("FAIL %s:%d: %s = 0x%x, want 0x%x\n", __FILE__, __LINE__, #got,   \
             g_, w_);                                                          \
    }                                                                          \
  } while (0)

static void check_str(int line, const std::string &got, const char *want) {
  checks++;
  if (got == want)
    return;
  failures++;
  printf("FAIL %s:%d: got [%s], want [%s]\n", __FILE__, line, got.c_str(),
         want);
}

/// A Super I/O and a PnP port set, with the ranges they map recorded.
struct Rig {
  SSuperIo_state sst;
  SIsaPnp_state pst;
  std::string maps; // "lpt@378 rd@20b ..."
  CSuperIo sio;
  CIsaPnp pnp;
  Rig()
      : sio(sst,
            [this](int ldn, u16 base) {
              char b[32];
              snprintf(b, sizeof(b), "%sldn%d@%x", maps.empty() ? "" : " ", ldn,
                       base);
              maps += b;
            }),
        pnp(pst, [this](u16 port) {
          char b[32];
          snprintf(b, sizeof(b), "%srd@%x", maps.empty() ? "" : " ", port);
          maps += b;
        }) {
    sio.reset();
    pnp.reset();
  }
  void cfg(u8 index, u8 value) {
    sio.write(0, index);
    sio.write(1, value);
  }
  u8 cfg_read(u8 index) {
    sio.write(0, index);
    return sio.read(1);
  }
  void unlock() {
    sio.write(0, 0x51);
    sio.write(0, 0x23);
  }
  /// The PnP initiation key on ADDRESS, after the two zero writes that
  /// reset the LFSR (ISA PnP 1.0a, 4.3).
  void key() {
    static const u8 k[32] = {0x6A, 0xB5, 0xDA, 0xED, 0xF6, 0xFB, 0x7D, 0xBE,
                             0xDF, 0x6F, 0x37, 0x1B, 0x0D, 0x86, 0xC3, 0x61,
                             0xB0, 0x58, 0x2C, 0x16, 0x8B, 0x45, 0xA2, 0xD1,
                             0xE8, 0x74, 0x3A, 0x9D, 0xCE, 0xE7, 0x73, 0x39};
    pnp.addr_write(0);
    pnp.addr_write(0);
    for (u8 b : k)
      pnp.addr_write(b);
  }
  void pnp_reg(u8 reg, u8 value) {
    pnp.addr_write(reg);
    pnp.data_write(value);
  }
};

/// The configuration ports read 0xff until unlocked with 51h 23h; the chip
/// ID registers 20h-21h read 43h 15h and ignore writes; BBh locks again.
static void test_config_mode() {
  Rig r;
  CHECK_EQ(r.sio.read(1), 0xff);
  r.unlock();
  CHECK_EQ(r.cfg_read(0x20), 0x43);
  CHECK_EQ(r.cfg_read(0x21), 0x15);
  r.cfg(0x20, 0x00);
  CHECK_EQ(r.cfg_read(0x20), 0x43);
  r.cfg(0x07, 3); // LDN 3, the parallel port
  CHECK_EQ(r.cfg_read(0x07), 3);
  CHECK_EQ(r.cfg_read(0x60), 0x03);
  CHECK_EQ(r.cfg_read(0x61), 0x78);
  r.sio.write(0, 0xbb);
  CHECK_EQ(r.sio.read(1), 0xff);
}

/// Activating LDN 3 maps the parallel port's four registers at its base;
/// moving the base moves them.
static void test_lpt_map() {
  Rig r;
  r.unlock();
  r.cfg(0x07, 3);
  r.cfg(0x30, 1);
  r.cfg(0x60, 0x03);
  r.cfg(0x61, 0xbc);
  check_str(__LINE__, r.maps, "ldn3@378 ldn3@378 ldn3@3bc");
}

/// The initiation key moves the PnP logic out of Wait for Key; register
/// 00h then places the READ_DATA port at (value << 2) | 3.
static void test_pnp_read_port() {
  Rig r;
  r.pnp_reg(0x00, 0x82); // ignored before the key
  check_str(__LINE__, r.maps, "");
  r.key();
  CHECK_EQ(r.pst.state, CIsaPnp::PNP_SLEEP);
  r.pnp_reg(0x00, 0x82);
  check_str(__LINE__, r.maps, "rd@20b");
  CHECK_EQ(r.pnp.data_read(), 0xff); // no cards
  r.pnp_reg(0x02, 0x02);             // Wait for Key
  CHECK_EQ(r.pst.state, CIsaPnp::PNP_WAIT_FOR_KEY);
}

/// The READ_DATA port is relocatable within 0x203-0x3FF only (ISA PnP 1.0a,
/// section 3.1, Table 1): a value below 0x80 would put it on 0x003-0x1FF,
/// over the DMA controller, the PICs (0x23 is the PIC backdoor) and the
/// timer. It is ignored.
static void test_pnp_read_port_range() {
  Rig r;
  r.key();
  r.pnp_reg(0x00, 0x08); // 0x23
  r.pnp_reg(0x00, 0x10); // 0x43, the timer's control word
  check_str(__LINE__, r.maps, "");
  CHECK_EQ(r.pst.rd_port, 0);
  r.pnp_reg(0x00, 0x80); // 0x203, the lowest
  r.pnp_reg(0x00, 0xff); // 0x3ff, the highest
  check_str(__LINE__, r.maps, "rd@203 rd@3ff");
  r.pnp_reg(0x00, 0x7f); // 0x1ff: ignored, the port stays at 0x3ff
  CHECK_EQ(r.pst.rd_port, 0x3ff);
}

/// A restore brings back the registers but not the ranges the guest's
/// writes mapped: restored() maps them again. A READ_DATA port outside the
/// range in an old file is not mapped.
static void test_restore_maps() {
  Rig a;
  a.unlock();
  a.cfg(0x07, 3);
  a.cfg(0x30, 1); // LPT on at 0x378
  a.key();
  a.pnp_reg(0x00, 0x82); // READ_DATA at 0x20b

  Rig b;
  b.sst = a.sst;
  b.pst = a.pst;
  b.sio.restored();
  b.pnp.restored();
  check_str(__LINE__, b.maps, "ldn3@378 rd@20b");

  Rig c; // LPT off, a READ_DATA port an older build let through
  c.pst.rd_port = 0x23;
  c.sio.restored();
  c.pnp.restored();
  check_str(__LINE__, c.maps, "");
}

int main() {
  test_config_mode();
  test_lpt_map();
  test_pnp_read_port();
  test_pnp_read_port_range();
  test_restore_maps();
  printf("superio_test: %d checks, %d failed\n", checks, failures);
  return failures ? 1 : 0;
}
