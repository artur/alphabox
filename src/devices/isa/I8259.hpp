/* Alphabox Alpha Emulator
 * Copyright (C) 2026 Artur Goulão
 * Website: https://github.com/artur/alphabox
 *
 * Forked from: ES40 emulator
 * Copyright (C) 2007-2008 by the ES40 Emulator Project
 * Copyright (C) 2007 by Camiel Vanderhoeven
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

/* The ALi M1543C's two cascaded 8259A interrupt controllers (master at
 * 20h-21h, slave at A0h-A1h on the master's IR2) with their edge/level
 * control registers (4D0h-4D1h), apart from the bridge: CAliM1543C owns the
 * pair and connects the master's INT output to the chipset, and
 * test/unit/i8259_test.cpp drives one directly.
 *
 * Reference: Intel 8259A Programmable Interrupt Controller, 231468-003
 * (Dec 1988); ALi M1543C data sheet v1.10, section 4.2.1.3.
 */

#if !defined(INCLUDED_I8259_H)
#define INCLUDED_I8259_H

#include <cstdint> // datatypes.hpp needs the fixed-width types

#include "datatypes.hpp"
#include <functional>
#include <mutex>

/**
 * Both controllers' registers as the bridge's state file stores them
 * (inside the ALi's state struct, written as it is): the layout must not
 * change. Index 0 is the master, 1 the slave. IRR (request) and ISR
 * (in-service) are kept separate so an edge that arrives while a
 * higher-priority IRQ is in service is still latched and re-fires after
 * EOI.
 **/
struct SPic_state {
  u8 irr[2];             // raw interrupt request register
  u8 imr[2];             // interrupt mask register (1 = masked)
  u8 isr[2];             // in-service register
  u8 last_irr[2];        // line history for edge detection
  u8 irq_base[2];        // ICW2 vector base (was pic_intvec)
  u8 priority_add[2];    // priority rotation offset
  u8 read_reg_select[2]; // OCW3 bit 0: 0=IRR, 1=ISR readback
  u8 poll[2];            // OCW3 poll-mode pending
  u8 special_mask[2];    // OCW3 special mask mode
  u8 init_state[2];      // 0=normal, 1=ICW2, 2=ICW3, 3=ICW4
  u8 auto_eoi[2];        // ICW4 AEOI bit
  u8 rotate_on_aeoi[2];  // OCW2 rotate-on-AEOI
  u8 special_fnm[2];     // ICW4 special fully nested mode
  u8 init4[2];           // ICW1 bit 0: ICW4 needed
  u8 single_mode[2];     // ICW1 bit 1: single PIC, no slave
  u8 elcr[2];            // edge/level control register (ELCR)
  u8 ltim[2];            // ICW1 bit 3: level-trigger global mode
};
static_assert(sizeof(SPic_state) == 34,
              "SPic_state is in the state file: its layout must not change");

/**
 * \brief The cascaded pair of 8259As, over an SPic_state its owner keeps
 * (and saves).
 *
 * Thread safe: every entry takes the pair's lock. The master's INT output
 * is driven with the lock held, so the receiver must not call back in.
 *
 * Three ways in for a device's interrupt line: interrupt() latches an edge
 * each call (in level mode IRR then stays until deassert()); deassert()
 * drops the line; set_line() makes IRR follow a level the device holds.
 **/
class CI8259 {
public:
  /// The master's INT output; called with the lock held, on every update.
  using IntLine = std::function<void(bool high)>;

  CI8259(SPic_state &state, IntLine out) : s(state), int_line(std::move(out)) {}

  /// Power-on: both controllers reset, every input edge-triggered.
  void reset();

  /// The ports: index 0 the master (20h-21h), 1 the slave (A0h-A1h).
  u8 read(int index, u32 address);
  void write(int index, u32 address, u8 data);
  /// The edge/level control registers (4D0h, 4D1h).
  u8 read_elcr(int index);
  void write_elcr(int index, u8 data);
  /// The vector of an interrupt acknowledge cycle.
  u8 read_vector();

  void interrupt(int index, int intno);
  void deassert(int index, int intno);
  void set_line(int index, int intno, bool active);

private:
  void init_reset(int index);
  int get_irq(int index);
  void update_output(int index);
  void intack(int index, int irq);
  void interrupt_locked(int index, int intno);
  void deassert_locked(int index, int intno);
  void set_line_locked(int index, int intno, bool active);

  std::mutex lock;
  SPic_state &s;
  IntLine int_line;
};

#endif // !defined(INCLUDED_I8259_H)
