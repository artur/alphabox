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
 * ALPHABOX_OHCI_SELFTEST=1: the ALi's OHCI driven from inside the emulator
 * by a minimal host controller driver, once the console has opened the PCI
 * DMA window -- the register-level evidence for the root hub and done queue
 * behaviour that no guest driver exercises. Needs `port1 = "tablet";` on
 * the ali_usb function. Prints %OHCI-I-SELFTEST lines, then PASS or FAIL.
 *
 * What it checks: that HcRhDescriptorA keeps its read only fields; that a
 * suspended port answers no token; that, after a host controller reset, the
 * done queue of a control TD still waits a millisecond before it goes back
 * (the 29e213e class of bug); that the controller masters no memory while
 * the function's Bus Master Enable is clear; and that the interrupt line
 * follows the bridge's USBIR routing when the routing moves.
 **/
#include "StdAfx.hpp"

#include "AliM1543C.hpp"
#include "AliM1543C_usb.hpp"
#include "System.hpp"

#include <chrono>
#include <thread>

namespace {
using clk = std::chrono::steady_clock;
double us_since(clk::time_point t) {
  return std::chrono::duration<double, std::micro>(clk::now() - t).count();
}
} // namespace

void CAliM1543C_usb::selftest() {
  auto nap_ms = [](int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
  };
  bool pass = true;
  auto say = [&](const char *what, bool ok) {
    printf("%%OHCI-I-SELFTEST: %-52s %s\n", what, ok ? "ok" : "FAILED");
    pass &= ok;
  };
  auto reg = [&](u32 off, u32 v) { m_ohci.usb_hci_write(off, 32, v); };
  auto rreg = [&](u32 off) { return (u32)m_ohci.usb_hci_read(off, 32); };

  /* A PCI address that is main memory: the console programs the DMA
     windows during its boot, so keep looking until one answers. */
  u32 base = 0;
  for (int tries = 0; tries < 600 && !base; tries++) {
    for (u32 b : {0x40000000u, 0x80000000u, 0x00800000u, 0u}) {
      const u32 a = b + 0x01100000;
      const u64 phys = cSystem->PCI_Phys(myPCIBus, a);
      if (cSystem->PtrToMem(phys) && cSystem->PtrToMem(phys + 0xffff)) {
        base = a;
        break;
      }
    }
    if (!base)
      nap_ms(500);
  }
  if (!base) {
    printf("%%OHCI-E-SELFTEST: no DMA window to put structures in.\n");
    return;
  }
  nap_ms(2000);

  const u32 HCCA = base, ED = base + 0x100, TD0 = base + 0x200,
            BUF = base + 0x1000;
  u32 tds = 0;
  auto w32 = [&](u32 a, u32 v) { do_pci_write(a, &v, sizeof(u32), 1); };
  auto r32 = [&](u32 a) {
    u32 v = 0;
    do_pci_read(a, &v, sizeof(u32), 1);
    return v;
  };
  auto td_addr = [&](u32 n) { return TD0 + 0x20 * n; };

  // The root hub's own registers, before anything runs.
  reg(0x48, 0);
  say("HcRhDescriptorA: NDP survives a write of 0",
      (rreg(0x48) & 0xff) == (u32)kPorts);
  reg(0x48, 0xffffffffu);
  const u32 a = rreg(0x48);
  say("HcRhDescriptorA: NDP, DT and reserved bits survive ones",
      (a & 0x000004ffu) == (u32)kPorts && !(a & 0x00ffe000u));
  say("HcRhDescriptorA: PSM, NPS, OCPM, NOCP, POTPGT writable",
      (a & 0xff001b00u) == 0xff001b00u);
  reg(0x48, 0x01000000);

  // Bring the controller up: HCCA, a control ED, the first port's device.
  auto cmd = [&](u32 v) { config_write(0, 4, 16, v); };
  const u32 command = config_read(0, 4, 16);
  cmd(command | 7); // I/O, memory, bus master
  reg(0x08, 1);     // HCR
  for (u32 o = 0; o < 0x100; o += 4)
    w32(HCCA + o, 0);
  reg(0x18, HCCA);
  reg(0x4, 0x80 | 0x10); // UsbOperational, control list enabled
  reg(0x54, 1 << 8);     // port 1: SetPortPower
  reg(0x54, 1 << 4);     // SetPortReset: enabled, the device at address 0
  say("port 1: enabled with the tablet", (rreg(0x54) & 3) == 3);

  // One SETUP TD (GET_DESCRIPTOR) after another, on one control ED.
  static const u8 setup[8] = {0x80, 6, 0, 1, 0, 0, 8, 0};
  do_pci_write(BUF, (void *)setup, 1, 8);
  w32(ED + 0, 8u << 16);   // FA 0, EN 0, MPS 8
  w32(ED + 4, td_addr(0)); // TailP: the dummy
  w32(ED + 8, td_addr(0)); // HeadP
  w32(ED + 12, 0);
  reg(0x20, ED); // HcControlHeadED
  auto queue = [&](int di) {
    const u32 t = td_addr(tds), nxt = td_addr(tds + 1);
    // not accessed (CC 15), DATA0 toggle, delay `di`, SETUP
    w32(t + 0, (0xfu << 28) | (2u << 24) | ((u32)di << 21));
    w32(t + 4, BUF);
    w32(t + 8, nxt);
    w32(t + 12, BUF + 7);
    w32(nxt + 0, 0xf0000000u);
    w32(ED + 4, nxt); // TailP: the new dummy
    reg(0x08, 2);     // ControlListFilled
    return tds++;
  };
  // Run one TD to retirement; returns its condition code, -1 on a timeout,
  // and sets *retired / *written to the microseconds (since queueing) at
  // which the TD's CC changed and the done head went to the HCCA.
  auto run = [&](int di, double *retired, double *written) {
    w32(HCCA + 0x84, 0);
    reg(0x0c, 0x2); // clear WritebackDoneHead
    const u32 t = td_addr(queue(di));
    const auto t0 = clk::now();
    double tr = -1, tw = -1;
    while (us_since(t0) < 20000 && (tr < 0 || (di != 7 && tw < 0))) {
      if (tr < 0 && (r32(t) >> 28) != 0xf)
        tr = us_since(t0);
      if (tw < 0 && r32(HCCA + 0x84))
        tw = us_since(t0);
    }
    if (retired)
      *retired = tr;
    if (written)
      *written = tw;
    return tr < 0 ? -1 : (int)(r32(t) >> 28);
  };

  say("a SETUP TD to the tablet: completes", run(0, nullptr, nullptr) == 0);

  // A suspended port propagates no traffic.
  reg(0x54, 1 << 2); // SetPortSuspend
  say("port 1 suspended: reads PSS", (rreg(0x54) & 4) != 0);
  say("a SETUP TD to a suspended port: DeviceNotResponding",
      run(0, nullptr, nullptr) == 5);
  // That error halted the ED; the driver clears it and resumes the port.
  w32(ED + 8, r32(ED + 8) & ~3u);
  reg(0x54, 1 << 3); // ClearSuspendStatus
  say("port 1 resumed: PSS clear", (rreg(0x54) & 4) == 0);
  say("a SETUP TD after the resume: completes", run(0, nullptr, nullptr) == 0);

  // The done queue of a control TD is held a millisecond, also after a
  // host controller reset that fell in the wait.
  double tr, tw;
  w32(HCCA + 0x84, 0);
  run(7, &tr, &tw); // retired with no interrupt wanted: the hold is armed
  nap_ms(5);        // and its millisecond runs out
  reg(0x08, 1);     // HCR: the done queue and its wait go
  reg(0x18, HCCA);
  reg(0x20, ED);
  reg(0x4, 0x80 | 0x10);
  reg(0x54, 1 << 8);
  // The reset left the root hub alone, so the port is still enabled.
  const int cc = run(0, &tr, &tw);
  printf("%%OHCI-I-SELFTEST: after a reset: TD retired at %.0f us, done queue "
         "written at %.0f us\n",
         tr, tw);
  say("after a host controller reset: TD completes", cc == 0);
  say("... and its done queue waits for the hold (>= 700 us)",
      tr >= 0 && tw >= 0 && tw - tr >= 700);

  // Bus mastering: with the function's Bus Master Enable clear the
  // controller reads and writes no memory.
  auto frame_no = [&] { return r32(HCCA + 0x80); };
  nap_ms(5);
  const u32 f1 = frame_no();
  nap_ms(10);
  say("bus master on: the HCCA frame number advances", frame_no() != f1);
  cmd(config_read(0, 4, 16) & ~4u);
  nap_ms(5);
  const u32 f2 = frame_no();
  nap_ms(15);
  say("bus master off: the HCCA frame number holds", frame_no() == f2);
  cmd(config_read(0, 4, 16) | 4u);
  nap_ms(15);
  say("bus master on again: it advances", frame_no() != f2);

  // The interrupt line follows the routing. An event with the controller
  // not running leaves a level that nothing re-evaluates but the move.
  reg(0x4, 0);            // UsbReset: no frames
  reg(0x0c, 0xffffffffu); // clear the status bits
  reg(0x10, 0x80000040);  // MIE, RootHubStatusChange
  reg(0x54, 1 << 18);     // clear PSSC
  reg(0x54, 1 << 16);     // and CSC
  reg(0x54, 1 << 2);      // SetPortSuspend: a port status change
  reg(0x54, 1 << 3);      // ClearSuspendStatus: PSSC
  nap_ms(3);
  if (theAli) {
    const u32 was = theAli->config_read(0, 0x74, 8);
    const int old_irq = theAli->routed_irq(0x74);
    say("RHSC pending with MIE: the line is up",
        old_irq >= 0 && theAli->irq_requested(old_irq));
    // 0xb routes to IRQ 12; pick another if that is where it already is.
    const u32 to = old_irq == 12 ? 0x3 : 0xb;
    const int new_irq = old_irq == 12 ? 10 : 12;
    theAli->config_write(0, 0x74, 8, (was & ~0xfu) | to);
    nap_ms(3);
    say("USBIR moved: the old IRQ is released",
        !theAli->irq_requested(old_irq));
    say("USBIR moved: the new one is raised", theAli->irq_requested(new_irq));
    theAli->config_write(0, 0x74, 8, was);
    nap_ms(3);
    say("USBIR moved back: the new IRQ is released",
        !theAli->irq_requested(new_irq));
    say("USBIR moved back: the old one is raised",
        theAli->irq_requested(old_irq));
  }
  reg(0x10, 0x80000000);

  printf("%%OHCI-I-SELFTEST: %s\n", pass ? "PASS" : "FAIL");
}
