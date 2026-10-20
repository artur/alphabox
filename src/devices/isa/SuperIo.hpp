/* Alphabox Alpha Emulator
 * Copyright (C) 2026 Artur Goulão
 * Website: https://github.com/artur/alphabox
 *
 * Contains code from ES40-Emu:
 * Copyright (C) 2024-2026 gdwnldsKSC and the ES40-Emu contributors
 * Website: https://github.com/ES40-Emu/es40
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

/* Two parts of the ALi M1543C apart from the bridge: the Super I/O's
 * configuration ports (370h index, 371h data) with its logical devices'
 * register banks, and the ISA Plug and Play ports (279h ADDRESS, A79h
 * WRITE_DATA and a READ_DATA port the OS places). CAliM1543C owns one of
 * each and maps the I/O ranges they ask for; test/unit/superio_test.cpp
 * drives them directly.
 */

#if !defined(INCLUDED_SUPERIO_H)
#define INCLUDED_SUPERIO_H

#include <cstdint> // datatypes.hpp needs the fixed-width types

#include "datatypes.hpp"
#include <functional>

/**
 * The Super I/O's registers as the bridge's state file stores them (inside
 * the ALi's state struct, written as it is): the layout must not change.
 **/
struct SSuperIo_state {
  bool config_mode = false;
  u8 unlock_state = 0;
  u8 index = 0;
  u8 ldn = 0;
  u8 chip_regs[256]{};
  u8 ldn_regs[16][256]{};
};
static_assert(
    sizeof(SSuperIo_state) == 4356,
    "SSuperIo_state is in the state file: its layout must not change");

/**
 * \brief The Super I/O configuration interface: unlock with 51h 23h on
 * 370h, select a register by index, read and write it on 371h, leave with
 * BBh. Index 07h selects the logical device (LDN) whose bank answers above
 * 30h.
 **/
class CSuperIo {
public:
  /// Place a logical device's I/O range at `base` (only the parallel port,
  /// LDN 3, has a range that moves with its registers).
  using MapLdn = std::function<void(int ldn, u16 base)>;

  CSuperIo(SSuperIo_state &state, MapLdn map) : s(state), map(std::move(map)) {}

  /// The power-on register values.
  void reset();
  /// The two ports: 0 index (370h), 1 data (371h).
  u8 read(u32 address);
  void write(u32 address, u8 data);
  /// After a restore: map the logical devices' ranges again, as the guest's
  /// writes did when it set them up.
  void restored();

private:
  u8 current_reg() const;
  void apply_ldn(int ldn);

  SSuperIo_state &s;
  MapLdn map;
};

/**
 * The ISA PnP protocol state as the bridge's state file stores it: the
 * layout must not change.
 **/
struct SIsaPnp_state {
  int state = 0; // PNP_WAIT_FOR_KEY at boot
  int key_pos = 0;
  u8 reg = 0;      // last register selected via 0x279
  u16 rd_port = 0; // OS-set RD_DATA port in 0x203-0x3FF
  u8 wake_csn = 0;
};
static_assert(sizeof(SIsaPnp_state) == 16,
              "SIsaPnp_state is in the state file: its layout must not change");

/**
 * \brief ISA Plug-and-Play protocol (ports 0x279 ADDRESS, 0xA79 WRITE_DATA,
 * and an OS-selectable READ_DATA port in 0x203-0x3FF). We expose no PnP
 * cards, so the implementation only tracks enough protocol state to swallow
 * the OS's enumeration cleanly and answer "no cards".
 **/
class CIsaPnp {
public:
  enum {
    PNP_WAIT_FOR_KEY = 0,
    PNP_SLEEP = 1,
    PNP_ISOLATION = 2,
    PNP_CONFIG = 3
  };
  /// Place the READ_DATA port.
  using MapReadPort = std::function<void(u16 port)>;

  CIsaPnp(SIsaPnp_state &state, MapReadPort map)
      : s(state), map(std::move(map)) {}

  void reset();
  void addr_write(u8 data);
  void data_write(u8 data);
  u8 data_read();
  /// After a restore: place the READ_DATA port again, if the OS had.
  void restored();

private:
  static const u8 init_key[32];
  static bool valid_read_port(u16 port) {
    return port >= 0x203 && port <= 0x3ff;
  }

  SIsaPnp_state &s;
  MapReadPort map;
};

#endif // !defined(INCLUDED_SUPERIO_H)
