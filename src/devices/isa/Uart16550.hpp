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
 * Contains code from ES40-Emu:
 * Copyright (C) 2023-2026 gdwnldsKSC and the ES40-Emu contributors
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

/* The 16550 UART's registers, receive FIFO and interrupt output, apart from
 * any host connection: CSerial (Serial.cpp) puts one on the ISA bus and
 * connects it to a telnet client, and test/unit/uart16550_test.cpp drives
 * one directly. The model has no clock: a byte written to THR is on the
 * line at once, so THRE and TEMT always read 1.
 */

#if !defined(INCLUDED_UART16550_H)
#define INCLUDED_UART16550_H

#include <cstdint> // datatypes.hpp needs the fixed-width types

#include "datatypes.hpp"
#include <functional>
#include <mutex>

/// Receive FIFO size. A 16550's is 16 bytes; this one holds more so that a
/// burst of input from the host is never lost (one slot stays free).
#define UART_FIFO_SIZE 1024

/**
 * The port's registers as the state file stores them (CSerial::SaveState
 * writes the struct as it is): any change to the layout makes old state
 * files unreadable and needs a new state-file version.
 **/
struct SSrl_state {
  u8 bTHR; /**< Transmit Hold Register */
  u8 bRDR; /**< Received Data Register */
  u8 bBRB_LSB;
  u8 bBRB_MSB;
  u8 bIER; /**< Interrupt Enable Register */
  u8 bIIR; /**< Interrupt Identification Register */
  u8 bFCR;
  u8 bLCR;           /**< Line Control Register (Data Format Register) */
  u8 bMCR;           /**< Modem Control Register */
  u8 bLSR;           /**< Line Status Register */
  u8 bMSR;           /**< Modem Status Register */
  u8 bSPR;           /**< Scratch Pad Register */
  int serial_cycles; ///< CSerial's receive tick, not a UART register
  char rcvBuffer[UART_FIFO_SIZE];
  int rcvW;
  int rcvR;
  int iNumber;       ///< the port number (serial0, serial1)
  bool thre_pending; /**< THRE interrupt latched (THR emptied, not yet acked
                        by an IIR read) */
};
static_assert(sizeof(SSrl_state) == 1056,
              "SSrl_state is in the state file: its layout must not change");

/**
 * \brief A 16550 UART: the eight registers at offsets 0-7 of its I/O
 * range, over an SSrl_state that its owner keeps (and saves).
 *
 * Thread safe: a processor reading or writing a register and the host side
 * delivering received bytes may run at once. Each recomputes the interrupt
 * identification and drives the line under one lock, so IIR is never seen
 * half-computed (a processor that read "no interrupt pending" with a
 * character waiting would dismiss the interrupt, and the edge-triggered PIC
 * line, held high, would never interrupt again).
 **/
class CUart16550 {
public:
  /// The interrupt output, called with the UART's lock held whenever the
  /// interrupt state is recomputed (the receiver tracks edges).
  using IrqLine = std::function<void(bool active)>;
  /// A byte the guest transmitted, called with the UART's lock held.
  using Transmit = std::function<void(u8 byte)>;

  CUart16550(SSrl_state &state, IrqLine irq, Transmit tx)
      : s(state), irq(std::move(irq)), tx(std::move(tx)) {}

  void reset();
  u8 read(u32 reg);
  void write(u32 reg, u8 value);
  int receive(const u8 *data, int len);
  bool rx_empty();
  void update();

private:
  void update_locked();
  bool rx_empty_locked() const { return s.rcvR == s.rcvW; }
  bool push_rx(u8 byte);

  std::mutex lock;
  SSrl_state &s;
  IrqLine irq;
  Transmit tx;
};

#endif // !defined(INCLUDED_UART16550_H)
