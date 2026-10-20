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

#include "Uart16550.hpp"

// Register offsets.
enum : u32 {
  REG_RBR_THR_DLL = 0,
  REG_IER_DLM = 1,
  REG_IIR_FCR = 2,
  REG_LCR = 3,
  REG_MCR = 4,
  REG_LSR = 5,
  REG_MSR = 6,
  REG_SCR = 7,
};

static const u8 LCR_DLAB = 0x80;
static const u8 IER_RDA = 0x01, IER_THRE = 0x02, IER_RLS = 0x04, IER_MS = 0x08;
static const u8 IIR_NONE = 0x01, IIR_MS = 0x00, IIR_THRE = 0x02, IIR_RDA = 0x04,
                IIR_RLS = 0x06, IIR_FIFO = 0xc0;
static const u8 FCR_ENABLE = 0x01, FCR_RX_RESET = 0x02;
static const u8 MCR_DTR = 0x01, MCR_RTS = 0x02, MCR_OUT1 = 0x04,
                MCR_OUT2 = 0x08, MCR_LOOP = 0x10;
static const u8 LSR_DR = 0x01, LSR_OE = 0x02, LSR_THRE = 0x20, LSR_TEMT = 0x40;
static const u8 MSR_DCTS = 0x01, MSR_DDSR = 0x02, MSR_TERI = 0x04,
                MSR_DDCD = 0x08, MSR_DELTAS = 0x0f;
static const u8 MSR_CTS = 0x10, MSR_DSR = 0x20, MSR_RI = 0x40, MSR_DCD = 0x80;

/// Power-on values: nothing pending, the transmitter empty, and the modem
/// inputs showing CTS and DSR (a terminal that is there and ready).
void CUart16550::reset() {
  std::lock_guard<std::mutex> g(lock);
  s.rcvW = 0;
  s.rcvR = 0;
  s.bTHR = 0x00;
  s.bRDR = 0x00;
  s.bBRB_LSB = 0x00;
  s.bBRB_MSB = 0x00;
  s.bIER = 0x00;
  s.bIIR = IIR_NONE;
  s.bFCR = 0x00;
  s.bLCR = 0x00;
  s.bMCR = 0x00;
  s.bLSR = LSR_THRE | LSR_TEMT;
  s.bMSR = MSR_CTS | MSR_DSR;
  s.bSPR = 0x00;
  s.serial_cycles = 0;
  s.thre_pending = false;
}

u8 CUart16550::read(u32 reg) {
  std::lock_guard<std::mutex> g(lock);
  switch (reg & 7) {
  case REG_RBR_THR_DLL:
    if (s.bLCR & LCR_DLAB)
      return s.bBRB_LSB;
    // An empty FIFO reads the last character again.
    if (!rx_empty_locked()) {
      s.bRDR = s.rcvBuffer[s.rcvR];
      s.rcvR = (s.rcvR + 1) % UART_FIFO_SIZE;
    }
    // Received-data interrupt follows the FIFO -- it may have just emptied.
    update_locked();
    return s.bRDR;

  case REG_IER_DLM:
    return (s.bLCR & LCR_DLAB) ? s.bBRB_MSB : s.bIER;

  case REG_IIR_FCR: {
    const u8 iir = s.bIIR;
    if ((iir & 0x0f) == IIR_THRE) // the cause, apart from the FIFO bits
      s.thre_pending = false;     // reading IIR acknowledges THRE
    update_locked();
    return iir;
  }

  case REG_LCR:
    return s.bLCR;

  case REG_MCR:
    return s.bMCR;

  case REG_LSR: {
    // OE is kept in bLSR from the overrun to this read, which clears it
    // and the receiver-line-status interrupt (PC16550D 8.4, Table IV).
    const u8 lsr = LSR_THRE | LSR_TEMT | (rx_empty_locked() ? 0 : LSR_DR) |
                   (s.bLSR & LSR_OE);
    s.bLSR = lsr & ~LSR_OE;
    update_locked();
    return lsr;
  }

  case REG_MSR: {
    // Reading MSR clears its delta bits, and with them the modem-status
    // interrupt (PC16550D 8.9 and Table IV).
    const u8 msr = s.bMSR;
    s.bMSR &= ~MSR_DELTAS;
    update_locked();
    return msr;
  }

  default:
    return s.bSPR;
  }
}

void CUart16550::write(u32 reg, u8 value) {
  std::lock_guard<std::mutex> g(lock);
  switch (reg & 7) {
  case REG_RBR_THR_DLL:
    if (s.bLCR & LCR_DLAB) {
      s.bBRB_LSB = value;
      break;
    }
    if (s.bMCR & MCR_LOOP) {
      // Loopback: the byte comes back into the receive FIFO instead of
      // going out (Windows 2000's UART detection writes a sentinel here and
      // reads it back through LSR.DR). Into a full FIFO it is an overrun:
      // the byte is lost and LSR.OE set (PC16550D 8.4, bit 1). Only here:
      // host input that does not fit waits on the host side instead.
      if (!push_rx(value))
        s.bLSR |= LSR_OE;
    } else
      tx(value);
    // Transmission is instantaneous: the holding register is empty again,
    // so THRE is re-armed (delivered only if its interrupt is enabled).
    s.thre_pending = true;
    update_locked();
    break;

  case REG_IER_DLM:
    if (s.bLCR & LCR_DLAB) {
      s.bBRB_MSB = value;
      break;
    }
    // Bits 7:4 are always 0 (PC16550D 8.7; ALi M1543C p. 262): Linux's
    // probe takes an IER bit 6 that sticks for an XScale UART. Enabling the
    // THRE interrupt while the (instantly drained) holding register is
    // empty arms one THRE interrupt.
    value &= 0x0f;
    if ((value & IER_THRE) && !(s.bIER & IER_THRE))
      s.thre_pending = true;
    s.bIER = value;
    update_locked();
    break;

  case REG_IIR_FCR:
    // FCR. Bit 0 is what a driver reads back in IIR bits 7:6 to know it has
    // a 16550 with FIFOs: without it, every OS's probe concludes "16450, no
    // FIFO" and takes an interrupt and two register reads per character.
    // Bit 1 empties the receive FIFO; bit 2 has nothing to empty
    // (transmission is immediate).
    s.bFCR = value;
    if (value & FCR_RX_RESET)
      s.rcvR = s.rcvW;
    update_locked();
    break;

  case REG_LCR:
    s.bLCR = value;
    break;

  case REG_MCR:
    // Bits 7:5 are permanently 0 (PC16550D 8.8; ALi M1543C p. 263).
    value &= 0x1f;
    s.bMCR = value;
    if (value & MCR_LOOP) {
      // Loopback: the modem outputs drive the modem inputs, DTR->DSR,
      // RTS->CTS, OUT1->RI, OUT2->DCD (Windows 2000's SERIAL.SYS toggles
      // OUT1 and watches RI follow it).
      u8 inputs = 0;
      if (value & MCR_DTR)
        inputs |= MSR_DSR;
      if (value & MCR_RTS)
        inputs |= MSR_CTS;
      if (value & MCR_OUT1)
        inputs |= MSR_RI;
      if (value & MCR_OUT2)
        inputs |= MSR_DCD;
      set_modem_inputs(inputs);
    } else {
      // Loopback released: the inputs read as the idle terminal again.
      set_modem_inputs(MSR_CTS | MSR_DSR);
    }
    break;

  case REG_LSR:
  case REG_MSR:
    // Status registers: a write is the 16550's factory test, "not
    // recommended" (PC16550D 8.4, p. 17; ALi M1543C p. 260), and changes
    // nothing here. It used to land in the scratch register.
    break;

  default:
    s.bSPR = value;
  }
}

/// Bytes from the line. Returns how many fit in the FIFO; the caller keeps
/// the rest. In loopback the receiver's input is disconnected (PC16550D
/// 8.8, p. 18; ALi M1543C p. 263) and nothing is taken: a key typed during
/// a driver's loopback test waits instead of being read as the test's byte.
int CUart16550::receive(const u8 *data, int len) {
  std::lock_guard<std::mutex> g(lock);
  if (s.bMCR & MCR_LOOP)
    return 0;
  int taken = 0;
  while (taken < len && push_rx(data[taken])) {
    taken++;
    update_locked();
  }
  return taken;
}

bool CUart16550::rx_empty() {
  std::lock_guard<std::mutex> g(lock);
  return rx_empty_locked();
}

/// Recompute the interrupt identification and drive the line.
void CUart16550::update() {
  std::lock_guard<std::mutex> g(lock);
  update_locked();
}

/**
 * The modem inputs (MSR bits 7:4) take new levels. The delta bits record
 * what changed since MSR was last read -- DCTS, DDSR and DDCD any change,
 * TERI the end of a ring (RI going from 1 to 0) -- and raise the
 * modem-status interrupt when it is enabled (PC16550D 8.9, pp. 18-19;
 * ALi M1543C p. 264). In loopback the inputs are MCR's low four bits
 * (PC16550D 8.8), so a driver toggling them sees the changes a real port
 * reports; outside it they only change when loopback ends.
 **/
void CUart16550::set_modem_inputs(u8 inputs) {
  const u8 old = s.bMSR & 0xf0;
  const u8 changed = old ^ inputs;
  u8 deltas = s.bMSR & MSR_DELTAS;
  if (changed & MSR_CTS)
    deltas |= MSR_DCTS;
  if (changed & MSR_DSR)
    deltas |= MSR_DDSR;
  if ((old & MSR_RI) && !(inputs & MSR_RI))
    deltas |= MSR_TERI;
  if (changed & MSR_DCD)
    deltas |= MSR_DDCD;
  s.bMSR = inputs | deltas;
  update_locked();
}

/// Into the receive FIFO; false when it is full.
bool CUart16550::push_rx(u8 byte) {
  const int next = (s.rcvW + 1) % UART_FIFO_SIZE;
  if (next == s.rcvR)
    return false;
  s.rcvBuffer[s.rcvW] = (char)byte;
  s.rcvW = next;
  return true;
}

void CUart16550::update_locked() {
  // Received data is a level (it follows the FIFO); THRE is latched in
  // thre_pending (set when THR empties or its interrupt is enabled, cleared
  // by an IIR read that reports it), because with instant transmission a
  // plain THRE level would interrupt without end. Into a local first: IIR
  // is never seen half-computed.
  u8 iir = IIR_NONE;
  if ((s.bIER & IER_RLS) && (s.bLSR & LSR_OE))
    iir = IIR_RLS;
  else if ((s.bIER & IER_RDA) && !rx_empty_locked())
    iir = IIR_RDA;
  else if ((s.bIER & IER_THRE) && s.thre_pending)
    iir = IIR_THRE;
  else if ((s.bIER & IER_MS) && (s.bMSR & MSR_DELTAS))
    iir = IIR_MS;
  const bool pending = !(iir & IIR_NONE);
  if (s.bFCR & FCR_ENABLE)
    iir |= IIR_FIFO;
  s.bIIR = iir;
  irq(pending);
}
