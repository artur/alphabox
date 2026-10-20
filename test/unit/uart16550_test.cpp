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

/* Unit test of the 16550 register model (src/devices/isa/Uart16550.cpp):
 * register access sequences a driver makes, checked against what a 16550
 * answers. The model has no clock (transmission is instantaneous), so the
 * test needs none: it records the interrupt line and the transmitted bytes
 * through the model's two callbacks. Built with every lane as
 * `uart16550_test`; exit status 0 when every check passes.
 */

#include "Uart16550.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int checks = 0, failures = 0;

#define CHECK_EQ(got, want)                                                    \
  do {                                                                         \
    const unsigned g_ = (unsigned)(got), w_ = (unsigned)(want);                \
    checks++;                                                                  \
    if (g_ != w_) {                                                            \
      failures++;                                                              \
      printf("FAIL %s:%d: %s = 0x%02x, want 0x%02x\n", __FILE__, __LINE__,     \
             #got, g_, w_);                                                    \
    }                                                                          \
  } while (0)

/// A UART with its callbacks recorded.
struct Rig {
  SSrl_state st;
  bool line = false; // the interrupt output now
  int rising = 0;    // edges seen on it
  std::string sent;  // bytes transmitted to the line
  CUart16550 uart;
  Rig()
      : uart(
            st,
            [this](bool on) {
              if (on && !line)
                rising++;
              line = on;
            },
            [this](u8 b) { sent += (char)b; }) {
    memset(&st, 0, sizeof(st));
    uart.reset();
  }
  u8 rd(u32 r) { return uart.read(r); }
  void wr(u32 r, u8 v) { uart.write(r, v); }
  void line_in(const char *text) {
    uart.receive((const u8 *)text, (int)strlen(text));
  }
};

enum {
  RBR = 0,
  THR = 0,
  DLL = 0,
  IER = 1,
  DLM = 1,
  IIR = 2,
  FCR = 2,
  LCR = 3,
  MCR = 4,
  LSR = 5,
  MSR = 6,
  SCR = 7
};

/// Reset values, and the initialisation every console and driver makes:
/// LCR=80, divisor, LCR=03 (8N1), MCR, FCR, IER (the ES40 console's
/// sequence, ALPHABOX_TRACE_SERIAL).
static void test_reset_and_init() {
  Rig r;
  CHECK_EQ(r.rd(IER), 0x00);
  CHECK_EQ(r.rd(IIR), 0x01); // no interrupt pending, FIFOs off
  CHECK_EQ(r.rd(LCR), 0x00);
  CHECK_EQ(r.rd(MCR), 0x00);
  CHECK_EQ(r.rd(LSR), 0x60);        // THRE and TEMT: the transmitter is empty
  CHECK_EQ(r.rd(MSR) & 0xf0, 0x30); // CTS and DSR asserted
  CHECK_EQ(r.line, false);

  r.wr(LCR, 0x80);
  r.wr(DLL, 0x02);
  r.wr(DLM, 0x00);
  r.wr(LCR, 0x07);
  r.wr(MCR, 0x0f);
  r.wr(FCR, 0x07);
  r.wr(IER, 0x00);
  CHECK_EQ(r.rd(LCR), 0x07);
  CHECK_EQ(r.rd(MCR), 0x0f);
  CHECK_EQ(r.rd(IIR), 0xc1); // FIFOs on, nothing pending
  CHECK_EQ(r.sent.size(), 0);
}

/// DLAB selects the divisor latch at offsets 0 and 1, and nothing else.
static void test_divisor_latch() {
  Rig r;
  r.wr(IER, 0x05);
  r.wr(LCR, 0x83);
  r.wr(DLL, 0x0c); // 9600 baud from 1.8432 MHz
  r.wr(DLM, 0x01);
  CHECK_EQ(r.rd(DLL), 0x0c);
  CHECK_EQ(r.rd(DLM), 0x01);
  CHECK_EQ(r.rd(LCR), 0x83);
  CHECK_EQ(r.sent.size(), 0); // a DLL write is not a transmission
  r.wr(LCR, 0x03);
  CHECK_EQ(r.rd(IER), 0x05); // IER untouched by the DLM write
  r.wr(THR, 'x');
  CHECK_EQ(r.sent.size(), 1);
  r.wr(LCR, 0x83);
  CHECK_EQ(r.rd(DLL), 0x0c); // and the divisor untouched by THR
  CHECK_EQ(r.rd(DLM), 0x01);
}

/// Transmission, and the THRE interrupt: raised when it is enabled with
/// the holding register empty and after each THR write, acknowledged by
/// the IIR read that reports it.
static void test_thre_interrupt() {
  Rig r;
  r.wr(LCR, 0x03);
  r.wr(FCR, 0x01);
  r.wr(THR, 'A');
  CHECK_EQ(r.sent == "A", true);
  CHECK_EQ(r.rd(LSR) & 0x60, 0x60);
  CHECK_EQ(r.line, false); // not enabled
  r.wr(IER, 0x02);
  CHECK_EQ(r.line, true);
  CHECK_EQ(r.rising, 1);
  CHECK_EQ(r.rd(IIR), 0xc2); // THRE, FIFOs on
  CHECK_EQ(r.line, false);   // the read acknowledged it
  CHECK_EQ(r.rd(IIR), 0xc1);
  r.wr(THR, 'B');
  CHECK_EQ(r.line, true);
  CHECK_EQ(r.rising, 2);
  CHECK_EQ(r.rd(IIR), 0xc2);
  r.wr(IER, 0x00);
  CHECK_EQ(r.line, false);
  // Re-enabling with the holding register empty interrupts again (Linux's
  // start-up test of THRE re-assertion relies on it).
  r.wr(IER, 0x02);
  CHECK_EQ(r.line, true);
  CHECK_EQ(r.rd(IIR), 0xc2);
  CHECK_EQ(r.sent == "AB", true);
}

/// Received data: LSR.DR, the RDA interrupt while data is waiting, the
/// bytes in order, and the interrupt gone when the FIFO is empty.
static void test_rda_interrupt() {
  Rig r;
  r.wr(LCR, 0x03);
  r.wr(FCR, 0x07);
  r.wr(IER, 0x01);
  CHECK_EQ(r.line, false);
  CHECK_EQ(r.rd(LSR) & 0x01, 0x00);
  r.line_in("ok");
  CHECK_EQ(r.rd(LSR), 0x61);
  CHECK_EQ(r.line, true);
  CHECK_EQ(r.rd(IIR), 0xc4);
  CHECK_EQ(r.line, true); // reading IIR does not acknowledge RDA
  CHECK_EQ(r.rd(RBR), 'o');
  CHECK_EQ(r.line, true);
  CHECK_EQ(r.rd(RBR), 'k');
  CHECK_EQ(r.line, false);
  CHECK_EQ(r.rd(IIR), 0xc1);
  CHECK_EQ(r.rd(LSR), 0x60);
}

/// RDA outranks THRE; THRE stays pending behind it.
static void test_priority() {
  Rig r;
  r.wr(FCR, 0x01);
  r.wr(IER, 0x03); // RDA and THRE
  r.line_in("z");
  CHECK_EQ(r.rd(IIR), 0xc4);
  CHECK_EQ(r.rd(RBR), 'z');
  CHECK_EQ(r.rd(IIR), 0xc2); // THRE, still pending
  CHECK_EQ(r.rd(IIR), 0xc1);
  CHECK_EQ(r.line, false);
}

/// The receive FIFO: fills, refuses what does not fit, drains in order;
/// FCR bit 1 empties it.
static void test_fifo() {
  Rig r;
  r.wr(FCR, 0x01);
  std::vector<u8> in(UART_FIFO_SIZE + 10);
  for (size_t i = 0; i < in.size(); i++)
    in[i] = (u8)(i * 7);
  CHECK_EQ(r.uart.receive(in.data(), (int)in.size()), UART_FIFO_SIZE - 1);
  int bad = 0;
  for (int i = 0; i < UART_FIFO_SIZE - 1; i++)
    if (r.rd(RBR) != in[i])
      bad++;
  CHECK_EQ(bad, 0);
  CHECK_EQ(r.rd(LSR) & 0x01, 0);
  CHECK_EQ(r.uart.rx_empty(), true);
  r.line_in("abc");
  r.wr(FCR, 0x03); // receive FIFO reset
  CHECK_EQ(r.rd(LSR) & 0x01, 0);
  CHECK_EQ(r.uart.rx_empty(), true);
}

/// Loopback: transmitted bytes come back into the receiver and the modem
/// outputs drive the modem inputs (Linux's probe writes MCR=1a and expects
/// MSR bits 7:4 = 9).
static void test_loopback() {
  Rig r;
  r.wr(LCR, 0x03);
  r.wr(MCR, 0x1a); // LOOP, RTS, OUT2
  CHECK_EQ(r.rd(MSR) & 0xf0, 0x90);
  r.wr(MCR, 0x1f);
  CHECK_EQ(r.rd(MSR) & 0xf0, 0xf0);
  r.wr(MCR, 0x10);
  CHECK_EQ(r.rd(MSR) & 0xf0, 0x00);
  r.wr(MCR, 0x14); // OUT1 -> RI
  CHECK_EQ(r.rd(MSR) & 0xf0, 0x40);
  r.wr(THR, 0x55);
  CHECK_EQ(r.sent.size(), 0); // not on the line
  CHECK_EQ(r.rd(LSR) & 0x01, 0x01);
  CHECK_EQ(r.rd(RBR), 0x55);
  // The receiver's input is disconnected: the line's bytes are not taken
  // (the host side keeps them) and do not mix with the looped ones.
  CHECK_EQ(r.uart.receive((const u8 *)"typed", 5), 0);
  r.wr(THR, 0x56);
  CHECK_EQ(r.rd(RBR), 0x56);
  CHECK_EQ(r.rd(LSR) & 0x01, 0x00);
  r.wr(MCR, 0x0b);
  CHECK_EQ(r.rd(MSR) & 0xf0, 0x30); // back to the idle terminal
  CHECK_EQ(r.uart.receive((const u8 *)"k", 1), 1);
  CHECK_EQ(r.rd(RBR), 'k');
  r.wr(THR, 0x66);
  CHECK_EQ(r.sent.size(), 1);
  CHECK_EQ(r.rd(LSR) & 0x01, 0x00);
}

/// IER bits 7:4 and MCR bits 7:5 read 0 (PC16550D 8.7 and 8.8; ALi
/// M1543C pp. 262-263). Linux's 8250 probe writes IER bit 6 (UUE) and
/// takes the port for an XScale UART when the bit sticks.
static void test_reserved_bits() {
  Rig r;
  r.wr(LCR, 0x03);
  r.wr(IER, 0xff);
  CHECK_EQ(r.rd(IER), 0x0f);
  r.wr(IER, 0x40); // Linux: iersave | UART_IER_UUE
  CHECK_EQ(r.rd(IER) & 0x40, 0x00);
  r.wr(MCR, 0xef);
  CHECK_EQ(r.rd(MCR), 0x0f);
  r.wr(MCR, 0xe0);
  CHECK_EQ(r.rd(MCR), 0x00);
  CHECK_EQ(r.rd(MSR) & 0xf0, 0x30); // bit 4 (loop) was not set
}

/// Modem status: the delta bits and the modem-status interrupt, driven in
/// loopback by MCR's low four bits (PC16550D 8.8 and 8.9, Table IV).
static void test_modem_status() {
  Rig r;
  CHECK_EQ(r.rd(MSR), 0x30); // the idle terminal: no change to report
  r.wr(FCR, 0x01);
  r.wr(IER, 0x08); // modem-status interrupt only
  CHECK_EQ(r.line, false);
  r.wr(MCR, 0x10); // loopback, all outputs off: CTS and DSR drop
  CHECK_EQ(r.line, true);
  CHECK_EQ(r.rd(IIR), 0xc0); // modem status, FIFOs on
  CHECK_EQ(r.rd(MSR), 0x03); // DCTS, DDSR
  CHECK_EQ(r.line, false);   // reading MSR cleared it
  CHECK_EQ(r.rd(IIR), 0xc1);
  CHECK_EQ(r.rd(MSR), 0x00);
  r.wr(MCR, 0x14); // OUT1 -> RI rises: not a trailing edge
  CHECK_EQ(r.line, false);
  CHECK_EQ(r.rd(MSR), 0x40);
  r.wr(MCR, 0x10); // RI falls: TERI
  CHECK_EQ(r.line, true);
  CHECK_EQ(r.rd(MSR), 0x04);
  r.wr(MCR, 0x18); // OUT2 -> DCD
  CHECK_EQ(r.rd(MSR), 0x88);
  r.wr(MCR, 0x13); // RTS, DTR on, OUT2 off: CTS, DSR, DCD change together
  CHECK_EQ(r.rd(MSR), 0x3b);
  // Lowest priority: THRE is reported first.
  r.wr(MCR, 0x10);
  r.wr(IER, 0x0a);
  CHECK_EQ(r.rd(IIR), 0xc2);
  CHECK_EQ(r.rd(IIR), 0xc0);
  CHECK_EQ(r.rd(MSR), 0x03);
  // Leaving loopback: the idle terminal's CTS and DSR come back.
  r.wr(MCR, 0x00);
  CHECK_EQ(r.rd(MSR), 0x33);
  CHECK_EQ(r.rd(IIR), 0xc1);
  // Without loopback nothing changes, however MCR is written.
  r.wr(MCR, 0x0f);
  r.wr(MCR, 0x00);
  CHECK_EQ(r.rd(MSR), 0x30);
  CHECK_EQ(r.line, false);
}

/// The scratch register keeps what is written, and only that: writes to
/// the read-only LSR and MSR change neither them nor it.
static void test_scratch() {
  Rig r;
  r.wr(SCR, 0xa5);
  CHECK_EQ(r.rd(SCR), 0xa5);
  r.wr(SCR, 0x5a);
  CHECK_EQ(r.rd(SCR), 0x5a);
  r.wr(LSR, 0x00);
  r.wr(MSR, 0xff);
  CHECK_EQ(r.rd(SCR), 0x5a);
  CHECK_EQ(r.rd(LSR), 0x60);
  CHECK_EQ(r.rd(MSR), 0x30);
}

int main() {
  test_reset_and_init();
  test_divisor_latch();
  test_thre_interrupt();
  test_rda_interrupt();
  test_priority();
  test_fifo();
  test_loopback();
  test_modem_status();
  test_reserved_bits();
  test_scratch();
  printf("uart16550_test: %d checks, %d failed\n", checks, failures);
  return failures ? 1 : 0;
}
