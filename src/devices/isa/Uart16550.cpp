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
static const u8 IER_RDA = 0x01, IER_THRE = 0x02;
static const u8 IIR_NONE = 0x01, IIR_THRE = 0x02, IIR_RDA = 0x04,
                IIR_FIFO = 0xc0;
static const u8 FCR_ENABLE = 0x01, FCR_RX_RESET = 0x02;
static const u8 MCR_DTR = 0x01, MCR_RTS = 0x02, MCR_OUT1 = 0x04,
                MCR_OUT2 = 0x08, MCR_LOOP = 0x10;
static const u8 LSR_DR = 0x01, LSR_THRE = 0x20, LSR_TEMT = 0x40;
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

  case REG_LSR:
    s.bLSR = LSR_THRE | LSR_TEMT | (rx_empty_locked() ? 0 : LSR_DR);
    return s.bLSR;

  case REG_MSR:
    return s.bMSR;

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
    if (s.bMCR & MCR_LOOP)
      // Loopback: the byte comes back into the receive FIFO instead of
      // going out (Windows 2000's UART detection writes a sentinel here and
      // reads it back through LSR.DR).
      push_rx(value);
    else
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
    // Only bits 3:0 exist; 7:4 read back zero [PC16550D datasheet].
    // Linux's 8250 probe takes a writable bit 6 (UUE) for an XScale UART.
    // Enabling the THRE interrupt while the (instantly drained) holding
    // register is empty arms one THRE interrupt.
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
    s.bMCR = value;
    if (value & MCR_LOOP) {
      // Loopback: the modem outputs drive the modem inputs, DTR->DSR,
      // RTS->CTS, OUT1->RI, OUT2->DCD (Windows 2000's SERIAL.SYS toggles
      // OUT1 and watches RI follow it).
      u8 status = 0;
      if (value & MCR_DTR)
        status |= MSR_DSR;
      if (value & MCR_RTS)
        status |= MSR_CTS;
      if (value & MCR_OUT1)
        status |= MSR_RI;
      if (value & MCR_OUT2)
        status |= MSR_DCD;
      s.bMSR = status | (s.bMSR & 0x0f);
    } else {
      // Loopback released: the inputs read as the idle terminal again.
      s.bMSR = (s.bMSR & 0x0f) | MSR_CTS | MSR_DSR;
    }
    break;

  default:
    s.bSPR = value;
  }
}

/// Bytes from the line. Returns how many fit in the FIFO.
int CUart16550::receive(const u8 *data, int len) {
  std::lock_guard<std::mutex> g(lock);
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
  if ((s.bIER & IER_RDA) && !rx_empty_locked())
    iir = IIR_RDA;
  else if ((s.bIER & IER_THRE) && s.thre_pending)
    iir = IIR_THRE;
  const bool pending = !(iir & IIR_NONE);
  if (s.bFCR & FCR_ENABLE)
    iir |= IIR_FIFO;
  s.bIIR = iir;
  irq(pending);
}
