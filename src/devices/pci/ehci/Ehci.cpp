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

#include "Ehci.hpp"
#include "SCSIBus.hpp"
#include "StdAfx.hpp"
#include "System.hpp"
#include "UsbAsyncShim.hpp"
#include "UsbAudio.hpp"
#include "UsbHostDevice.hpp"
#include "UsbStorage.hpp"
#include "UsbTablet.hpp"
#include <algorithm>
#include <chrono>
#include <cstring>

// ALPHABOX_USBTRACE=1: register writes and schedule events.
static const bool g_trace = getenv("ALPHABOX_USBTRACE") != nullptr;
static const auto g_trace_t0 = std::chrono::steady_clock::now();
// Microseconds since start, for the trace.
static double trace_us() {
  return std::chrono::duration<double, std::micro>(
             std::chrono::steady_clock::now() - g_trace_t0)
      .count();
}

// PCI configuration space: the EHCI function of a NEC uPD720101 -- with
// its companions function 2 on INTC, alone function 0 on INTA.
static void ehci_config_space(u32 *data, u32 *mask, bool companions) {
  data[0x00 >> 2] = 0x00e01033; // CFID: NEC, uPD720101 EHCI function
  data[0x04 >> 2] = 0x02100000; // CFCS: DEVSEL medium, capabilities list
  data[0x08 >> 2] = 0x0c032004; // CFRV: serial bus / USB / EHCI, rev. 4
  if (companions)
    data[0x0c >> 2] = 0x00800000; // header type: multi-function
  data[0x10 >> 2] = 0x00000000; // BAR0: registers, 256 bytes of memory
  data[0x2c >> 2] = 0x00e01033; // CSID: subsystem = the part itself
  data[0x34 >> 2] = 0x00000040; // CCAP: power management at 0x40
  data[0x3c >> 2] = companions ? 0x000003ff : 0x000001ff; // CFIT: no line yet
  data[0x40 >> 2] = 0x7e020001; // PMC: power management 1.1, no PME
  data[0x44 >> 2] = 0x00000000; // PMCSR: D0
  data[0x60 >> 2] = 0x00002020; // SBRN: USB 2.0; FLADJ: 60000-bit frame

  mask[0x04 >> 2] = 0x00000157; // CFCS: command
  mask[0x0c >> 2] = 0x0000ffff; // CFLT: latency timer + cache line size
  mask[0x10 >> 2] = 0xffffff00; // BAR0
  mask[0x3c >> 2] = 0x000000ff; // CFIT: interrupt line
  mask[0x44 >> 2] = 0x00000003; // PMCSR: power state
  mask[0x60 >> 2] = 0x00003f00; // FLADJ
}

// PCI configuration space: an OHCI companion function of a NEC uPD720101,
// function f on INTA + f.
static void ohci_config_space(u32 *data, u32 *mask, int f) {
  data[0x00 >> 2] = 0x00351033; // CFID: NEC, uPD720101 OHCI function
  data[0x04 >> 2] = 0x02100000; // CFCS: DEVSEL medium, capabilities list
  data[0x08 >> 2] = 0x0c031043; // CFRV: serial bus / USB / OHCI, rev. 43
  data[0x0c >> 2] = 0x00800000; // header type: multi-function
  data[0x10 >> 2] = 0x00000000; // BAR0: registers, 4 KB of memory
  data[0x2c >> 2] = 0x00351033; // CSID: subsystem = the part itself
  data[0x34 >> 2] = 0x00000040; // CCAP: power management at 0x40
  data[0x3c >> 2] = 0x2a0100ff | ((u32)(f + 1) << 8); // CFIT: no line yet
  data[0x40 >> 2] = 0x7e020001; // PMC: power management 1.1, no PME
  data[0x44 >> 2] = 0x00000000; // PMCSR: D0

  mask[0x04 >> 2] = 0x00000157; // CFCS: command
  mask[0x0c >> 2] = 0x0000ffff; // CFLT: latency timer + cache line size
  mask[0x10 >> 2] = 0xfffff000; // BAR0
  mask[0x3c >> 2] = 0x000000ff; // CFIT: interrupt line
  mask[0x44 >> 2] = 0x00000003; // PMCSR: power state
}

// An OHCI companion: the card's function f, its memory accesses and its
// interrupt pin the card's.
struct CEhci::CCompanion : COhciHost {
  CEhci &card;
  const int func;
  COhci ohci;
  CCompanion(CEhci &c, int f)
      : card(c), func(f), ohci(*this, kPortsPerCompanion, false, "ohci") {}
  void ohci_dma_read(u32 a, void *d, size_t size, size_t count) override {
    card.dma_read(a, d, size, count);
  }
  void ohci_dma_write(u32 a, void *s, size_t size, size_t count) override {
    card.dma_write(a, s, size, count);
  }
  void ohci_irq(bool level) override { card.do_pci_interrupt(func, level); }
};

// Capability registers.
static const u32 kCapLength = 0x20;
static const u32 kHciVersion = 0x0100;
static const u32 kHccParams = 0x00000012; // programmable list; IST 1 frame

// HCSPARAMS: the ports, port power control, and the companions: N_CC of
// them with N_PCC ports each, routed in order (PRR 0).
u32 CEhci::hcs_params() const {
  u32 v = kPorts | (1u << 4);
  if (m_with_companions)
    v |= ((u32)kPortsPerCompanion << 8) | ((u32)kCompanions << 12);
  return v;
}

CEhci::CEhci(CConfigurator *cfg, CSystem *c, int pcibus, int pcidev)
    : CPCIDevice(cfg, c, pcibus, pcidev), CDiskController(kPorts + 1, 1) {
  m_with_companions = myCfg->get_bool_value("companions", true);
  m_func = m_with_companions ? kCompanions : 0;
  u32 cfg_data[64] = {}, cfg_mask[64] = {};
  ehci_config_space(cfg_data, cfg_mask, m_with_companions);
  add_function(m_func, cfg_data, cfg_mask);
  if (m_with_companions)
    for (int f = 0; f < kCompanions; ++f) {
      u32 od[64] = {}, om[64] = {};
      ohci_config_space(od, om, f);
      add_function(f, od, om);
      m_comp[f] = std::make_unique<CCompanion>(*this, f);
      for (int i = 0; i < kPortsPerCompanion; ++i)
        m_comp[f]->ohci.bind_port(i, &m_port[f * kPortsPerCompanion + i]);
    }
  m_xfer.resize(0x5000);
  ResetPCI();
  for (int p = 0; p < kPorts; ++p) {
    char key[8];
    snprintf(key, sizeof(key), "port%d", p + 1);
    const char *what = myCfg->get_text_value(key, "");
    if (!strcmp(what, "tablet")) {
      // A full-speed tablet, as a real one is, where a companion can take
      // it; without companions it runs at high speed so that it works.
      auto t = std::make_unique<CUsbTablet>(!m_with_companions);
      theUsbTablet.store(t.get());
      attach(p, std::move(t));
      printf("%s: USB tablet on port %d.\n", devid_string, p + 1);
    } else if (!strcmp(what, "audio") && m_with_companions) {
      // Full speed: the EHCI leaves it for the port's companion.
      attach(p, usb_async_wrap(std::make_unique<CUsbAudio>(), devid_string,
                               p + 1));
      printf("%s: USB audio speaker on port %d.\n", devid_string, p + 1);
    } else if (!strncmp(what, "host:", 5)) {
#if defined(HAVE_LIBUSB)
      attach(p, std::make_unique<CUsbHostDevice>(what + 5));
      printf("%s: USB host device %s on port %d.\n", devid_string, what + 5,
             p + 1);
#else
      FAILURE_1(Configuration,
                "%s: USB host passthrough needs a build with libusb", key);
#endif
    } else if (*what) {
      FAILURE_2(Configuration,
                "%s: unknown USB device \"%s\" (EHCI ports take tablet, "
                "audio (with companions) or host:vvvv:pppp; disks are "
                "disk<port>.0)",
                key, what);
    }
  }
  if (m_with_companions)
    printf("%s: EHCI USB 2.0 controller (function %d), %d ports, and %d OHCI "
           "companions (functions 0-%d).\n",
           devid_string, m_func, kPorts, kCompanions, kCompanions - 1);
  else
    printf("%s: EHCI USB 2.0 controller, %d ports, no companions.\n",
           devid_string, kPorts);
  theUsbFaultTarget.store(this); // tests address the USB 2.0 card first
}

CEhci::~CEhci() {
  stop_threads();
  for (auto &port : m_port)
    if (port.dev && theUsbTablet.load() == port.dev.get())
      theUsbTablet.store(nullptr);
  CUsbFaultTarget *me = this;
  theUsbFaultTarget.compare_exchange_strong(me, nullptr);
}

// Test faults (CUsbPortFaults, docs/headless.md): from the GUI thread.
bool CEhci::inject_fault(const char *op, int port, int ep, int arg) {
  std::lock_guard<std::mutex> lk(m_mx);
  if (port < 1 || port > kPorts || !m_port[port - 1].dev)
    return false;
  const int p = port - 1;
  // A port the companion has: its business.
  int local;
  if (COhci *c = companion_of(p, &local))
    if (m_port[p].ohci_owns)
      return c->inject_fault(op, local + 1, ep, arg);
  CUsbPortFaults &f = m_port[p].faults;
  if (!strcmp(op, "detach") || !strcmp(op, "attach")) {
    f.unplugged = !strcmp(op, "detach");
    if (f.unplugged)
      m_port[p].dev->reset(); // what is left of it when it comes back
    port_refresh(p);
  } else if (!strcmp(op, "stall")) {
    f.stall_ep = ep;
    f.stall_left = arg > 0 ? arg : 1;
  } else if (!strcmp(op, "nak")) {
    f.nak_ep = ep;
    f.nak_until_ms = (u64)std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count() +
                     (u64)arg;
  } else if (!strcmp(op, "phase")) {
    return m_port[p].dev->inject_phase_error();
  } else {
    return false;
  }
  kick();
  return true;
}

void CEhci::ResetPCI() {
  CPCIDevice::ResetPCI();
  // The start-up self-test runs while the firmware resets the PCI bus; it
  // owns the controller until it is done (and resets it itself then).
  if (m_selftest)
    return;
  std::lock_guard<std::mutex> lk(m_mx);
  for (int p = 0; p < kPorts; ++p) {
    if (!m_port[p].dev)
      continue;
    // Not under a transfer the companion may be running.
    COhci *c = companion_of(p);
    std::unique_lock<std::mutex> clk;
    if (c)
      clk = std::unique_lock<std::mutex>(c->mutex());
    m_port[p].dev->reset();
  }
  for (auto &c : m_comp)
    if (c)
      c->ohci.reset();
  reset_controller();
}

COhci *CEhci::companion_of(int p, int *local) {
  if (!m_comp[p / kPortsPerCompanion])
    return nullptr;
  if (local)
    *local = p % kPortsPerCompanion;
  return &m_comp[p / kPortsPerCompanion]->ohci;
}

void CEhci::route(int p, bool to_companion) {
  u32 &r = state.portsc[p];
  r = to_companion ? (r | PS_OWNER) : (r & ~PS_OWNER);
  int local;
  if (COhci *c = companion_of(p, &local))
    c->port_routed(local, to_companion);
  port_refresh(p);
}

void CEhci::register_disk(class CDisk *dsk, int bus, int dev) {
  if (bus < 1 || bus > kPorts || dev != 0)
    FAILURE(Configuration, "EHCI disks are named disk<port>.0, port 1 to 4");
  if (m_port[bus - 1].dev)
    FAILURE_1(Configuration, "EHCI port %d already has a device", bus);
  CDiskController::register_disk(dsk, bus, dev);
  std::unique_ptr<CUsbDevice> d =
      std::make_unique<CUsbStorage>(new CSCSIBus(myCfg, cSystem), dsk);
  attach(bus - 1, usb_async_wrap(std::move(d), devid_string, bus));
  printf("%s: USB storage on port %d.\n", devid_string, bus);
}

void CEhci::attach(int p, std::unique_ptr<CUsbDevice> dev) {
  // Whichever controller has the port: wake both.
  dev->on_complete = [this, p]() {
    kick();
    if (COhci *c = companion_of(p))
      c->kick();
  };
  m_port[p].dev = std::move(dev);
}

void CEhci::kick() {
  {
    std::lock_guard<std::mutex> lk(m_kick_mx);
    m_kicked = true;
  }
  m_kick_cv.notify_one();
}

void CEhci::start_threads() {
  for (auto &c : m_comp)
    if (c)
      c->ohci.start_threads();
  if (!myThread) {
    printf(" ehci");
    StopThread = false;
    myThread = std::make_unique<std::thread>([this]() { run(); });
    if (getenv("ALPHABOX_EHCI_SELFTEST") && !m_selftest_thread) {
      m_selftest = true;
      m_selftest_thread = std::make_unique<std::thread>([this]() {
        try {
          selftest();
        } catch (CException &e) {
          printf("%%EHCI-E-SELFTEST: %s\n", e.displayText().c_str());
        }
        m_selftest = false;
      });
    }
  }
}

void CEhci::dma_read(u32 a, void *d, size_t size, size_t count) {
  if (m_selftest)
    memcpy(d, cSystem->PtrToMem(a), size * count);
  else
    do_pci_read(a, d, size, count);
}

void CEhci::dma_write(u32 a, const void *s, size_t size, size_t count) {
  if (m_selftest)
    memcpy(cSystem->PtrToMem(a), s, size * count);
  else
    do_pci_write(a, (void *)s, size, count);
}

void CEhci::stop_threads() {
  StopThread = true;
  kick();
  if (m_selftest_thread) {
    m_selftest_thread->join();
    m_selftest_thread = nullptr;
  }
  if (myThread) {
    printf(" ehci");
    myThread->join();
    myThread = nullptr;
  }
  for (auto &c : m_comp)
    if (c)
      c->ohci.stop_threads();
}

void CEhci::check_state() {
  if (myThreadDead.load())
    FAILURE(Thread, "EHCI thread has died");
  for (auto &c : m_comp)
    if (c && c->ohci.thread_dead())
      FAILURE(Thread, "EHCI card's OHCI thread has died");
}

// Power-on and HCRESET state: halted, schedules off, ports unpowered and
// owned by the companions (absent ones, when the card has none) until
// CONFIGFLAG is set.
void CEhci::reset_controller() {
  state.usbcmd = 0x00080000; // interrupt threshold 8 microframes
  state.usbsts = STS_HALTED;
  state.usbintr = 0;
  state.frindex = 0;
  state.periodic_base = 0;
  state.async_addr = 0;
  state.configflag = 0;
  state.doorbell = false;
  for (int p = 0; p < kPorts; ++p) {
    state.portsc[p] = 0;
    route(p, true);
  }
  update_irq();
}

void CEhci::update_irq() {
  const bool level = (state.usbsts & state.usbintr & 0x3f) != 0;
  if (g_trace && level != m_irq_traced) {
    printf("EHCIT %12.1f irq %d sts=%04x\n", trace_us(), (int)level,
           state.usbsts & 0x3f);
    m_irq_traced = level;
  }
  do_pci_interrupt(m_func, level);
}

void CEhci::status(u32 bits) {
  state.usbsts |= bits;
  update_irq();
}

// A port's connection as the controller sees it: present when a device is
// attached, the port powered, and the port this controller's (not handed to
// a companion). A change sets CSC and Port Change Detect.
void CEhci::port_refresh(int p) {
  u32 &r = state.portsc[p];
  const bool present = m_port[p].dev && !m_port[p].faults.unplugged &&
                       (r & PS_PP) && !(r & PS_OWNER);
  const bool was = (r & PS_CCS) != 0;
  r &= ~(PS_CCS | (3u << 10));
  if (present) {
    r |= PS_CCS;
    if (!(r & PS_PED)) // line state: K for low speed (hand it off), else J
      r |= m_port[p].dev->low_speed() ? (1u << 10) : (2u << 10);
  } else {
    r &= ~(PS_PED | PS_SUSP);
  }
  if (was != present) {
    r |= PS_CSC;
    status(STS_PCD);
  }
}

// PORTSC writes (EHCI 1.0 2.3.9).
void CEhci::port_write(int p, u32 data) {
  u32 &r = state.portsc[p];
  r &= ~(data & (PS_CSC | PS_PEDC | PS_OCC)); // write 1 to clear
  if (!(data & PS_PED))                       // only the controller enables
    r &= ~PS_PED;
  // PORT_OWNER: the port goes to the companion, or comes back (4.2.2).
  if ((data ^ r) & PS_OWNER)
    route(p, (data & PS_OWNER) != 0);
  r = (r & ~(PS_PP | (7u << 20))) | (data & (PS_PP | (7u << 20)));
  if (!(r & PS_PP))
    r &= ~(PS_PED | PS_SUSP | PS_PR);
  // Suspend and resume.
  if ((data & PS_SUSP) && (r & PS_PED))
    r |= PS_SUSP;
  if (data & PS_FPR)
    r |= PS_FPR;
  else if (r & PS_FPR)
    r &= ~(PS_FPR | PS_SUSP); // resume signalling ended
  // Reset: PR 1 starts it (the port disables), PR 0 ends it. The device
  // resets; a high-speed one enables the port, anything else is left for a
  // companion controller: the port stays disabled, and the driver hands it
  // over with PORT_OWNER (on a card without companions it goes nowhere).
  if (data & PS_PR) {
    if (!(r & PS_PR)) {
      r |= PS_PR;
      r &= ~(PS_PED | PS_SUSP);
    }
  } else if (r & PS_PR) {
    r &= ~PS_PR;
    port_refresh(p);
    if ((r & PS_CCS) && m_port[p].dev) {
      m_port[p].dev->reset();
      const bool hs = m_port[p].dev->can_high_speed();
      m_port[p].dev->set_high_speed(hs);
      if (hs)
        r |= PS_PED;
    }
  }
  port_refresh(p);
}

u32 CEhci::reg_read(u32 off) {
  switch (off) {
  case 0x00:
    return kCapLength | (kHciVersion << 16);
  case 0x04:
    return hcs_params();
  case 0x08:
    return kHccParams;
  case 0x0c:
    return 0; // HCSP-PORTROUTE
  case 0x20:
    return state.usbcmd;
  case 0x24:
    if (g_trace)
      printf("EHCIT %12.1f read usbsts=%04x\n", trace_us(),
             state.usbsts & 0xffff);
    return state.usbsts;
  case 0x28:
    return state.usbintr;
  case 0x2c:
    return state.frindex;
  case 0x30:
    return 0; // CTRLDSSEGMENT: 32-bit structures only
  case 0x34:
    return state.periodic_base;
  case 0x38:
    return state.async_addr;
  case 0x60:
    return state.configflag;
  default:
    if (off >= 0x64 && off < 0x64 + 4 * kPorts)
      return state.portsc[(off - 0x64) / 4];
    return 0;
  }
}

void CEhci::reg_write(u32 off, u32 data) {
  if (g_trace)
    printf("EHCI W %02x=%08x\n", off, data);
  switch (off) {
  case 0x20: { // USBCMD
    if (data & CMD_HCRESET) {
      reset_controller();
      return;
    }
    const u32 was = state.usbcmd;
    state.usbcmd = data & 0x00ff0b7d; // ITC, park bits, schedules, FLS, RS
    if (data & CMD_IAAD)
      state.doorbell = true;
    if (state.usbcmd & CMD_RS)
      state.usbsts &= ~STS_HALTED;
    else
      state.usbsts |= STS_HALTED;
    state.usbsts = (state.usbsts & ~(STS_PSS | STS_ASS)) |
                   ((state.usbcmd & CMD_PSE) ? STS_PSS : 0) |
                   ((state.usbcmd & CMD_ASE) ? STS_ASS : 0);
    if (((state.usbcmd & ~was) & (CMD_ASE | CMD_RS)) || state.doorbell)
      kick();
    break;
  }
  case 0x24: // USBSTS: write 1 to clear the interrupt causes
    if (g_trace)
      printf("EHCIT %12.1f clear usbsts %04x\n", trace_us(), data & 0x3f);
    state.usbsts &= ~(data & 0x3f);
    break;
  case 0x28:
    state.usbintr = data & 0x3f;
    break;
  case 0x2c: // FRINDEX: writable while halted
    if (state.usbsts & STS_HALTED)
      state.frindex = data & 0x3fff;
    break;
  case 0x34:
    state.periodic_base = data & ~0xfffu;
    break;
  case 0x38:
    state.async_addr = data & ~0x1fu;
    break;
  case 0x60: { // CONFIGFLAG: 1 routes every port to this controller, 0
               // every port to the companions
    const u32 cf = data & 1;
    if (cf != state.configflag)
      for (int p = 0; p < kPorts; ++p)
        route(p, !cf);
    state.configflag = cf;
    break;
  }
  default:
    if (off >= 0x64 && off < 0x64 + 4 * kPorts)
      port_write((off - 0x64) / 4, data);
  }
  update_irq();
}

u32 CEhci::ReadMem_Bar(int func, int bar, u32 address, int dsize) {
  if (bar != 0)
    return 0;
  if (func != m_func)
    return func < kCompanions && m_comp[func]
               ? (u32)m_comp[func]->ohci.usb_hci_read(address, dsize)
               : 0;
  std::lock_guard<std::mutex> lk(m_mx);
  const u32 v = reg_read(address & 0xfc);
  const int shift = 8 * (address & 3);
  switch (dsize) {
  case 8:
    return (v >> shift) & 0xff;
  case 16:
    return (v >> shift) & 0xffff;
  default:
    return v;
  }
}

void CEhci::WriteMem_Bar(int func, int bar, u32 address, int dsize, u32 data) {
  if (bar != 0)
    return;
  if (func != m_func) {
    if (func < kCompanions && m_comp[func])
      m_comp[func]->ohci.usb_hci_write(address, dsize, data);
    return;
  }
  std::lock_guard<std::mutex> lk(m_mx);
  if (dsize == 32) {
    reg_write(address & 0xfc, data);
    return;
  }
  // A narrower write: merge into the register (write-1-to-clear bits of
  // the other bytes written as 0, so they are left alone).
  const int shift = 8 * (address & 3);
  const u32 m = (dsize == 8 ? 0xffu : 0xffffu) << shift;
  u32 v = reg_read(address & 0xfc);
  if ((address & 0xfc) == 0x24)
    v = 0;
  else if ((address & 0xfc) >= 0x64)
    v &= ~(PS_CSC | PS_PEDC | PS_OCC);
  reg_write(address & 0xfc, (v & ~m) | ((data << shift) & m));
}

// The thread: microframes while the asynchronous schedule has work, frames
// (1 ms) when it has none, and at once when a device or the driver wakes it.
void CEhci::run() {
  using clk = std::chrono::steady_clock;
  try {
    auto next_frame = clk::now() + std::chrono::milliseconds(1);
    // A real EHCI walks the asynchronous ring continuously, and a driver
    // queues qTDs with plain memory writes -- nothing tells the card. So for
    // a while after the ring last had work, it is walked every microframe
    // even when idle, which is when the next transfer usually arrives; after
    // that, once a frame (and at once on a wake-up).
    auto last_work = clk::now() - std::chrono::seconds(1);
    bool busy = false;
    while (!StopThread) {
      if (busy) {
        // A microframe. Not a timed wait: on macOS one that short sleeps
        // for about a millisecond (timer coalescing), which is what a
        // transfer then waited -- nada's EHCI driver read 16 MB in ~850 ms
        // against 79 ms with a wake-up per transfer. Yielding costs host
        // CPU only while the ring is busy.
        const auto wake =
            std::min(clk::now() + std::chrono::microseconds(125), next_frame);
        for (;;) {
          {
            std::lock_guard<std::mutex> klk(m_kick_mx);
            if (m_kicked || StopThread) {
              m_kicked = false;
              break;
            }
          }
          if (clk::now() >= wake)
            break;
          std::this_thread::yield();
        }
      } else {
        std::unique_lock<std::mutex> klk(m_kick_mx);
        m_kick_cv.wait_until(klk, next_frame, [this]() { return m_kicked; });
        m_kicked = false;
      }
      if (StopThread)
        break;
      std::lock_guard<std::mutex> lk(m_mx);
      const auto now = clk::now();
      if (!(state.usbcmd & CMD_RS)) {
        next_frame = now + std::chrono::milliseconds(1);
        busy = false;
        continue;
      }
      if (now >= next_frame) {
        int frames =
            1 + (int)std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - next_frame)
                    .count();
        next_frame += std::chrono::milliseconds(frames);
        if (frames > 100) { // the host did not run us: do not catch up
          frames = 1;
          next_frame = now + std::chrono::milliseconds(1);
        }
        // FRINDEX counts microframes; the frame list rolls over at its size.
        // Frames the thread overslept are run one by one, up to a limit: an
        // isochronous stream moves data in each, and a real controller
        // would not have skipped them. Beyond the limit they are counted.
        const int fls = (state.usbcmd >> 2) & 3;
        const u32 list_bit = 1u << (13 - fls); // 1024: bit 13; 512: 12; ...
        const int run_now = std::min(frames, kCatchUpFrames);
        for (int i = 0; i < frames; ++i) {
          const u32 before = state.frindex;
          state.frindex = (state.frindex + 8) & 0x3fff;
          if ((before ^ state.frindex) & list_bit)
            status(STS_FLR);
          if (i >= frames - run_now)
            frame();
        }
      }
      // Data moved in this pass (an emulated device finishes every qTD in
      // the pass that finds it, so Reclamation alone would miss it).
      if (async_pass())
        last_work = now;
      const bool was_busy = busy;
      busy = (state.usbcmd & CMD_ASE) &&
             now - last_work < std::chrono::milliseconds(50);
      if (g_trace && busy != was_busy)
        printf("EHCIT %12.1f busy %d\n", trace_us(), (int)busy);
    }
  } catch (CException &e) {
    printf("Exception in EHCI thread: %s.\n", e.displayText().c_str());
    myThreadDead.store(true);
  }
}

void CEhci::frame() {
  if (state.usbcmd & CMD_PSE)
    periodic_frame();
}

// The periodic list's entry for this frame: isochronous iTDs run their
// frame's transactions, interrupt QHs are serviced (one qTD each); siTDs and
// FSTNs are passed over (no full-speed device is ever behind a high-speed
// hub here: the companions serve them).
void CEhci::periodic_frame() {
  const int fls = (state.usbcmd >> 2) & 3;
  const u32 entries = 1024u >> fls;
  const u32 idx = (state.frindex >> 3) & (entries - 1);
  u32 link;
  dma_read(state.periodic_base + 4 * idx, &link, sizeof(u32), 1);
  u32 seen[64];
  int nseen = 0;
  for (int guard = 0; !(link & 1) && guard < 128; ++guard) {
    const u32 addr = link & ~0x1fu;
    const int typ = (link >> 1) & 3;
    u32 next;
    dma_read(addr, &next, sizeof(u32), 1); // every type links at dword 0
    if (typ == 1) { // QH; an interrupt tree shares nodes: service once
      if (std::find(seen, seen + nseen, addr) != seen + nseen)
        break; // the rest of this path was walked already
      if (nseen < 64)
        seen[nseen++] = addr;
      u32 caps;
      dma_read(addr + 8, &caps, sizeof(u32), 1);
      if (caps & 0xff) // S-mask: polled in some microframe of this frame
        service_qh(addr, true);
    } else if (typ == 0) {
      service_itd(addr);
    }
    link = next;
  }
}

// The asynchronous ring, from ASYNCLISTADDR round to it again, repeated
// while it moves data. Reclamation says whether any queue head had an active
// qTD on the last round (the thread keeps polling while it does). The
// async-advance doorbell is answered after a full round. True if a qTD
// moved data. (One that is only waiting -- NAKed -- does not count: its
// device wakes the thread when it answers, and a pipe a passed-through
// device keeps pending would otherwise keep the thread spinning.)
bool CEhci::async_pass() {
  if (!(state.usbcmd & CMD_ASE) || !state.async_addr) {
    state.usbsts &= ~STS_RECL;
    if (state.doorbell) {
      state.doorbell = false;
      state.usbcmd &= ~CMD_IAAD;
      status(STS_IAA);
    }
    return false;
  }
  bool active = false, worked = false;
  for (int round = 0; round < 16; ++round) {
    int moved = 0;
    active = false;
    u32 qh = state.async_addr;
    for (int guard = 0; guard < 128; ++guard) {
      u32 w[3];
      dma_read(qh, w, sizeof(u32), 3);
      const int n = service_qh(qh, false);
      if (n > 0)
        moved += n;
      if (n != 0)
        active = true; // -1: an active qTD that did not move (NAK)
      const u32 next = w[0] & ~0x1fu;
      if ((w[0] & 1) || !next || next == state.async_addr)
        break;
      qh = next;
    }
    if (!moved)
      break;
    worked = true;
  }
  if (active)
    state.usbsts |= STS_RECL;
  else
    state.usbsts &= ~STS_RECL;
  if (state.doorbell) {
    state.doorbell = false;
    state.usbcmd &= ~CMD_IAAD;
    status(STS_IAA);
  }
  return worked;
}

CUsbDevice *CEhci::device_at(int address) {
  for (int p = 0; p < kPorts; ++p)
    if (m_port[p].dev && (state.portsc[p] & PS_PED) &&
        !m_port[p].faults.unplugged && m_port[p].dev->address() == address)
      return m_port[p].dev.get();
  return nullptr;
}

// One queue head (EHCI 1.0 4.10): run the qTD in its overlay, and when that
// is done fetch the next (the alternate next after a short packet). Returns
// the number of qTDs finished, or -1 when an active qTD waited (NAK).
int CEhci::service_qh(u32 qh_addr, bool periodic) {
  u32 q[12];
  dma_read(qh_addr, q, sizeof(u32), 12);
  const u32 epchar = q[1];
  const int devaddr = epchar & 0x7f;
  const int ep = (epchar >> 8) & 0xf;
  const bool dtc = (epchar >> 14) & 1;
  const int mps = std::max(1, (int)((epchar >> 16) & 0x7ff));
  const u32 ACTIVE = 0x80, HALTED = 0x40, XACTERR = 0x08;
  int done = 0;
  bool waited = false;
  auto write_overlay = [&]() {
    dma_write(qh_addr + 12, &q[3], sizeof(u32), 9);
  };
  for (int guard = 0; guard < (periodic ? 1 : 16); ++guard) {
    u32 token = q[6];
    if (token & HALTED)
      break;
    if (!(token & ACTIVE)) {
      // Advance: after a short packet (bytes left over) to the alternate
      // next qTD if there is one, else to the next.
      u32 next;
      if (((token >> 16) & 0x7fff) && !(q[5] & 1))
        next = q[5] & ~0x1fu;
      else if (!(q[4] & 1))
        next = q[4] & ~0x1fu;
      else
        break; // nothing queued
      u32 t[8];
      dma_read(next, t, sizeof(u32), 8);
      if (!(t[2] & ACTIVE))
        break; // not handed over yet
      if (g_trace)
        printf("EHCIT %12.1f pickup qh=%08x td=%08x pid=%d len=%d\n",
               trace_us(), qh_addr, next, (int)((t[2] >> 8) & 3),
               (int)((t[2] >> 16) & 0x7fff));
      q[3] = next;
      q[4] = t[0];
      q[5] = t[1];
      // Data toggle from the qTD, or kept in the queue head.
      q[6] = dtc ? t[2] : (t[2] & 0x7fffffff) | (q[6] & 0x80000000);
      for (int i = 0; i < 5; ++i)
        q[7 + i] = t[3 + i];
      write_overlay();
      token = q[6];
    }
    const int pidcode = (token >> 8) & 3;
    const int total = (token >> 16) & 0x7fff;
    int cpage = (token >> 12) & 7;
    int offset = q[7] & 0xfff;
    // The qTD's buffer from the current position: pages q[7..11].
    auto span = [&](u8 *data, int n, bool to_guest) {
      int k = offset, pg = cpage;
      while (n > 0 && pg < 5) {
        const int chunk = std::min(n, 0x1000 - (k & 0xfff));
        const u32 pa = (q[7 + pg] & ~0xfffu) + (k & 0xfff);
        if (to_guest)
          dma_write(pa, data, 1, chunk);
        else
          dma_read(pa, data, 1, chunk);
        data += chunk;
        n -= chunk;
        k += chunk;
        if ((k & 0xfff) == 0) {
          ++pg;
          k = 0;
        }
      }
    };
    auto retire = [&](u32 tok) {
      if (g_trace)
        printf("EHCIT %12.1f retire qh=%08x td=%08x token=%08x\n", trace_us(),
               qh_addr, q[3], tok);
      q[6] = tok;
      write_overlay();
      dma_write(q[3] + 8, &tok, sizeof(u32), 1);   // the qTD's token
      dma_write(q[3] + 12, &q[7], sizeof(u32), 1); // and its offset
    };
    CUsbDevice *dev = device_at(devaddr);
    if (!dev || pidcode == 3) {
      // Nobody answered: three strikes and halted (CERR spent at once).
      retire((token & ~(ACTIVE | (3u << 10))) | HALTED | XACTERR);
      status(STS_ERR);
      ++done;
      break;
    }
    const int pid = pidcode == 2   ? CUsbDevice::PID_SETUP
                    : pidcode == 1 ? CUsbDevice::PID_IN
                                   : CUsbDevice::PID_OUT;
    const bool in = pid == CUsbDevice::PID_IN;
    u8 *buf = m_xfer.data();
    const int want = std::min(total, (int)m_xfer.size());
    if (!in && want)
      span(buf, want, false);
    int n = want;
    CUsbDevice::Result r;
    CUsbPortFaults *f = faults_for(dev);
    if (f && f->intercept(dev, pid, ep, buf, n, r))
      n = 0; // a test fault answered it
    else
      r = dev->transfer(pid, ep, buf, n);
    if (r == CUsbDevice::USB_NAK) {
      waited = true;
      break; // the next round, or the device's wake-up, retries
    }
    if (r == CUsbDevice::USB_STALL) {
      retire((token & ~ACTIVE) | HALTED);
      status(STS_ERR);
      ++done;
      break;
    }
    if (in && n)
      span(buf, n, true);
    // Toggle: one per max-size packet (a zero-length one counts).
    const int packets = std::max(1, (n + mps - 1) / mps);
    if (packets & 1)
      token ^= 0x80000000;
    // Position: the offset in buffer pointer 0, the page in the token.
    const int k = offset + n;
    cpage = std::min(4, cpage + (k >> 12));
    offset = k & 0xfff;
    q[7] = (q[7] & ~0xfffu) | offset;
    const int left = total - n;
    token = (token & ~((0x7fffu << 16) | (7u << 12))) | ((u32)left << 16) |
            ((u32)cpage << 12);
    const bool short_packet = in && n < want;
    if (left == 0 || short_packet) {
      token &= ~ACTIVE;
      retire(token);
      if ((token & 0x8000) || short_packet) // IOC, or short
        status(STS_INT);
      ++done;
    } else {
      q[6] = token; // more to move (an OUT the device took in part)
      write_overlay();
    }
    if (periodic)
      break;
  }
  return done ? done : (waited ? -1 : 0);
}

// A high-speed isochronous transfer descriptor (EHCI 1.0 3.3, 4.7). Its
// eight transaction words are the eight microframes of the frame whose list
// entry reaches it: each active one moves up to Mult packets of the
// endpoint's maximum size (buffer page PG, offset, continuing into the next
// page), and is written back inactive with its status -- for IN with the
// byte count received. A device that does not answer is a Transaction
// Error; an IN packet longer than the host allowed, Babble; data the device
// could not supply or take in time (ISO_OVERRUN: a device behind libusb), a
// Data Buffer Error, which is what a controller reports for a transaction
// it could not move. IOC raises USBINT; an error raises USBERRINT. There is
// no handshake, no toggle and no halt: the next transaction runs whatever
// became of this one. The whole frame's transactions run at once (the
// thread's time is a frame), in microframe order.
void CEhci::service_itd(u32 itd_addr) {
  u32 d[16];
  dma_read(itd_addr, d, sizeof(u32), 16);
  const u32 ACTIVE = 1u << 31, BUFERR = 1u << 30, BABBLE = 1u << 29,
            XACTERR = 1u << 28, IOC = 1u << 15;
  const int devaddr = d[9] & 0x7f, ep = (d[9] >> 8) & 0xf;
  const bool in = (d[10] >> 11) & 1;
  const int mps = std::max(1, (int)(d[10] & 0x7ff));
  const int mult = std::max(1, (int)(d[11] & 3));
  bool ioc = false, err = false;
  for (int uf = 0; uf < 8; ++uf) {
    u32 t = d[1 + uf];
    if (!(t & ACTIVE))
      continue;
    const int len = (t >> 16) & 0xfff; // up to 3072
    const int pg = (t >> 12) & 7;
    const u32 offset = t & 0xfff;
    // The transaction's buffer: page PG from the offset, then page PG+1.
    auto span = [&](u8 *data, int n, bool to_guest) {
      int at = 0;
      for (int page = pg, k = (int)offset; at < n && page < 7; ++page, k = 0) {
        const int chunk = std::min(n - at, 0x1000 - k);
        const u32 pa = (d[9 + page] & ~0xfffu) + (u32)k;
        if (to_guest)
          dma_write(pa, data + at, 1, chunk);
        else
          dma_read(pa, data + at, 1, chunk);
        at += chunk;
      }
      return at == n;
    };
    u32 errs = 0;
    int got = len;
    u8 *buf = m_xfer.data();
    CUsbDevice *dev = device_at(devaddr);
    if (!dev) {
      errs = XACTERR;
      got = 0;
    } else if (!in) {
      if (!span(buf, len, false))
        errs = BUFERR;
      // The microframe's data, up to Mult packets of the endpoint's size,
      // is one isochronous transfer for the device (CUsbDevice::
      // iso_transfer): what it takes in a microframe.
      const int n = std::min(len, mult * mps);
      const int r = dev->iso_transfer(CUsbDevice::PID_OUT, ep, buf, n);
      if (r == CUsbDevice::ISO_NO_ENDPOINT)
        errs = XACTERR;
      else if (r < 0)
        errs = BUFERR; // underrun: the data did not go out in time
    } else {
      // Up to Mult packets, as much as the transaction has room for.
      const int room = std::min(len, mult * mps);
      const int r = dev->iso_transfer(CUsbDevice::PID_IN, ep, buf, room);
      got = 0;
      if (r == CUsbDevice::ISO_NO_ENDPOINT) {
        errs = XACTERR;
      } else if (r < 0) { // overrun: the data was not there in time
        errs = BUFERR;
      } else if (r > room) { // more than the host allowed
        errs = BABBLE;
        got = room;
      } else {
        got = r;
      }
      if (got && !span(buf, got, true))
        errs |= BUFERR;
    }
    if (g_trace)
      printf("EHCIT %12.1f itd %08x uframe %d %s ep %d len %d -> %d%s\n",
             trace_us(), itd_addr, uf, in ? "in" : "out", ep, len, got,
             errs ? " error" : "");
    // Written back: inactive, the status, and for IN the length received.
    t &= ~(ACTIVE | BUFERR | BABBLE | XACTERR);
    t |= errs;
    if (in)
      t = (t & ~(0xfffu << 16)) | ((u32)got << 16);
    d[1 + uf] = t;
    dma_write(itd_addr + 4 * (1 + uf), &t, sizeof(u32), 1);
    ioc |= (t & IOC) != 0;
    err |= errs != 0;
  }
  if (err)
    status(STS_ERR);
  if (ioc)
    status(STS_INT);
}

static u32 ehci_magic1 = 0xE4C11001;
static u32 ehci_magic2 = 0x1001E4C1;
static u32 ehci_magic_comp = 0xE4C10CC2; // the companions' block
static u32 ehci_magic_run = 0xE4C1D0C5;  // runtime and devices' block

int CEhci::SaveState(FILE *f) {
  long ss = sizeof(state);
  int res;
  if ((res = CPCIDevice::SaveState(f)))
    return res;
  fwrite(&ehci_magic1, sizeof(u32), 1, f);
  fwrite(&ss, sizeof(long), 1, f);
  fwrite(&state, sizeof(state), 1, f);
  fwrite(&ehci_magic2, sizeof(u32), 1, f);
  // The companions' registers follow (a card without them saves as before).
  if (m_with_companions) {
    fwrite(&ehci_magic_comp, sizeof(u32), 1, f);
    for (auto &c : m_comp)
      fwrite(&c->ohci.state, sizeof(c->ohci.state), 1, f);
  }
  // The companions' runtime state and the devices, in a block of its own
  // (a state saved before it existed restores as before).
  CUsbSaved s;
  for (auto &c : m_comp)
    if (c)
      c->ohci.save_runtime(s);
  usb_save_ports(s, m_port, kPorts);
  usb_save_block(f, ehci_magic_run, s);
  printf("%s: %d bytes saved.\n", devid_string, (int)(ss + s.b.size()));
  return 0;
}

int CEhci::RestoreState(FILE *f) {
  long ss;
  u32 m1, m2;
  int res;
  if ((res = CPCIDevice::RestoreState(f)))
    return res;
  if (fread(&m1, sizeof(u32), 1, f) != 1 || m1 != ehci_magic1 ||
      fread(&ss, sizeof(long), 1, f) != 1 || ss != sizeof(state) ||
      fread(&state, sizeof(state), 1, f) != 1 ||
      fread(&m2, sizeof(u32), 1, f) != 1 || m2 != ehci_magic2) {
    printf("%s: saved state does not match.\n", devid_string);
    return -1;
  }
  if (m_with_companions) {
    u32 m3;
    bool ok = fread(&m3, sizeof(u32), 1, f) == 1 && m3 == ehci_magic_comp;
    for (auto &c : m_comp)
      ok = ok && fread(&c->ohci.state, sizeof(c->ohci.state), 1, f) == 1;
    if (!ok) {
      printf("%s: saved state has no companions' block (saved with "
             "companions = false, or before they existed).\n",
             devid_string);
      return -1;
    }
  }
  // The routing the saved registers say.
  for (int p = 0; p < kPorts; ++p) {
    int local;
    if (COhci *c = companion_of(p, &local))
      c->port_routed(local, (state.portsc[p] & PS_OWNER) != 0);
  }
  // The companions' frame numbers and done queues, and the devices, as the
  // guest left them. A device whose state was not saved (one behind
  // libusb, or a state file from before) is reset and shown to the guest as
  // reconnected, on whichever controller has its port.
  CUsbSaved s;
  bool reconnect[kPorts];
  for (int p = 0; p < kPorts; ++p)
    reconnect[p] = m_port[p].dev != nullptr;
  if (usb_load_block(f, ehci_magic_run, s)) {
    for (auto &c : m_comp)
      if (c)
        c->ohci.load_runtime(s);
    usb_load_ports(s, m_port, kPorts, reconnect);
  }
  for (int p = 0; p < kPorts; ++p) {
    if (!reconnect[p])
      continue;
    printf("%s: the device on port %d is shown as reconnected.\n", devid_string,
           p + 1);
    m_port[p].dev->reset();
    state.portsc[p] &= ~(PS_PED | PS_CCS);
    port_refresh(p);
    int local;
    if (COhci *c = companion_of(p, &local))
      c->reconnect(local);
  }
  for (auto &c : m_comp)
    if (c)
      c->ohci.refresh_irq();
  update_irq();
  printf("%s: %d bytes restored.\n", devid_string, (int)(ss + s.b.size()));
  return 0;
}

// ALPHABOX_EHCI_SELFTEST=1: the controller driven from inside the emulator
// by a minimal EHCI driver, before any firmware runs -- the evidence that
// its schedule works without a guest driver for it. Needs a USB disk on
// port 1 whose sector 0 ends in 55 AA. The structures live in the top 16 MB
// of guest memory, addressed physically (dma_read/dma_write). Prints
// %EHCI-I-SELFTEST lines, and PASS or FAIL.
void CEhci::selftest() {
  const u32 base =
      (u32)((1ull << cSystem->get_memory_bits()) - (16u << 20)) & ~0xfffu;
  const u32 QH0 = base, QH1 = base + 0x100, QH2 = base + 0x200;
  const u32 TD = base + 0x1000, DATA = base + 0x10000;
  auto w32 = [&](u32 a, u32 v) { memcpy(cSystem->PtrToMem(a), &v, 4); };
  auto r32 = [&](u32 a) {
    u32 v;
    memcpy(&v, cSystem->PtrToMem(a), 4);
    return v;
  };
  auto reg = [&](u32 off, u32 v) {
    std::lock_guard<std::mutex> lk(m_mx);
    reg_write(off, v);
  };
  auto rreg = [&](u32 off) {
    std::lock_guard<std::mutex> lk(m_mx);
    return reg_read(off);
  };
  auto say = [](const char *what, bool ok) {
    printf("%%EHCI-I-SELFTEST: %-40s %s\n", what, ok ? "ok" : "FAILED");
    return ok;
  };
  auto sleep_ms = [](int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
  };
  bool pass = true;

  // Controller and port.
  reg(0x20, CMD_HCRESET);
  pass &= say("reset: halted", (rreg(0x24) & STS_HALTED) != 0);
  reg(0x60, 1); // CONFIGFLAG
  reg(0x64, PS_PP);
  pass &= say("port 1: device connected", (rreg(0x64) & PS_CCS) != 0);
  reg(0x64, PS_PP | PS_PR | PS_CSC);
  sleep_ms(10);
  reg(0x64, PS_PP);
  pass &= say("port 1: enabled after reset (high speed)",
              (rreg(0x64) & PS_PED) != 0);

  // The asynchronous ring: control QH (head), bulk OUT ep2, bulk IN ep1.
  auto make_qh = [&](u32 qh, u32 next, int addr, int ep, int mps, bool dtc,
                     bool head) {
    std::lock_guard<std::mutex> lk(m_mx);
    for (int i = 0; i < 12; ++i)
      w32(qh + 4 * i, 0);
    w32(qh + 0, next | (1u << 1));
    w32(qh + 4, addr | (ep << 8) | (2u << 12) | (dtc ? 1u << 14 : 0) |
                    (head ? 1u << 15 : 0) | ((u32)mps << 16));
    w32(qh + 8, 1u << 30); // Mult 1
    w32(qh + 16, 1);       // no next qTD yet
    w32(qh + 20, 1);
  };
  make_qh(QH0, QH1, 0, 0, 64, true, true);
  make_qh(QH1, QH2, 0, 2, 512, false, false);
  make_qh(QH2, QH0, 0, 1, 512, false, false);
  reg(0x38, QH0);
  reg(0x20, CMD_RS | CMD_ASE | 0x00080000);
  pass &= say("running", !(rreg(0x24) & STS_HALTED));

  // A chain of qTDs handed to a queue head, then waited for.
  struct Step {
    int pid; // 0 OUT, 1 IN, 2 SETUP
    u32 buf;
    int len;
    int dt;
  };
  auto run_tds = [&](u32 qh, const std::vector<Step> &steps) {
    {
      std::lock_guard<std::mutex> lk(m_mx);
      for (size_t i = 0; i < steps.size(); ++i) {
        const u32 td = TD + 0x40 * (u32)i;
        const bool last = i + 1 == steps.size();
        w32(td + 0, last ? 1 : td + 0x40);
        w32(td + 4, 1);
        w32(td + 8, 0x80u | ((u32)steps[i].pid << 8) | (3u << 10) |
                        (last ? 0x8000u : 0) | ((u32)steps[i].len << 16) |
                        ((u32)steps[i].dt << 31));
        for (int p = 0; p < 5; ++p)
          w32(td + 12 + 4 * p, (steps[i].buf & ~0xfffu) + 0x1000u * p +
                                   (p ? 0 : (steps[i].buf & 0xfff)));
      }
      // The overlay is idle: point it at the chain.
      w32(qh + 16, TD);
      w32(qh + 20, 1);
      w32(qh + 24, 0);
    }
    kick();
    const u32 last_td = TD + 0x40 * (u32)(steps.size() - 1);
    for (int t = 0; t < 2000; ++t) {
      if (!(r32(last_td + 8) & 0x80) || (r32(qh + 24) & 0x40))
        break;
      sleep_ms(1);
    }
    const u32 tok = r32(last_td + 8);
    return !(tok & 0xc0) ? 0 : (int)(tok & 0xff) | 0x100;
  };
  auto setup = [&](u32 at, u8 type, u8 req, u16 val, u16 idx, u16 len) {
    const u8 s[8] = {type,           req,           (u8)val,
                     (u8)(val >> 8), (u8)idx,       (u8)(idx >> 8),
                     (u8)len,        (u8)(len >> 8)};
    memcpy(cSystem->PtrToMem(at), s, 8);
  };

  // GET_DESCRIPTOR(device).
  setup(DATA, 0x80, 6, 0x0100, 0, 18);
  int e =
      run_tds(QH0, {{2, DATA, 8, 0}, {1, DATA + 0x100, 18, 1}, {0, 0, 0, 1}});
  const u8 *dd = (const u8 *)cSystem->PtrToMem(DATA + 0x100);
  pass &= say("GET_DESCRIPTOR(device)", e == 0 && dd[0] == 18 && dd[1] == 1);
  pass &= say("  says USB 2.0", dd[2] == 0x00 && dd[3] == 0x02);
  // SET_ADDRESS(1), then the control queue head talks to address 1.
  setup(DATA, 0x00, 5, 1, 0, 0);
  e = run_tds(QH0, {{2, DATA, 8, 0}, {1, 0, 0, 1}});
  pass &= say("SET_ADDRESS(1)", e == 0);
  make_qh(QH0, QH1, 1, 0, 64, true, true);
  make_qh(QH1, QH2, 1, 2, 512, false, false);
  make_qh(QH2, QH0, 1, 1, 512, false, false);
  // SET_CONFIGURATION(1).
  setup(DATA, 0x00, 9, 1, 0, 0);
  e = run_tds(QH0, {{2, DATA, 8, 0}, {1, 0, 0, 1}});
  pass &= say("SET_CONFIGURATION(1)", e == 0);

  // Bulk-Only READ(10) of sector 0: CBW out, 512 bytes in, CSW in.
  u8 cbw[31] = {'U', 'S', 'B',  'C', 1, 0, 0, 0, 0, 2, 0, 0, 0x80,
                0,   10,  0x28, 0,   0, 0, 0, 0, 0, 0, 1, 0};
  memcpy(cSystem->PtrToMem(DATA), cbw, sizeof(cbw));
  e = run_tds(QH1, {{0, DATA, 31, 0}});
  pass &= say("bulk OUT: CBW", e == 0);
  memset(cSystem->PtrToMem(DATA + 0x1000), 0, 512);
  e = run_tds(QH2, {{1, DATA + 0x1000, 512, 0}});
  const u8 *sec = (const u8 *)cSystem->PtrToMem(DATA + 0x1000);
  pass &= say("bulk IN: sector 0 (55 AA)",
              e == 0 && sec[510] == 0x55 && sec[511] == 0xaa);
  e = run_tds(QH2, {{1, DATA + 0x2000, 13, 0}});
  const u8 *csw = (const u8 *)cSystem->PtrToMem(DATA + 0x2000);
  pass &= say("bulk IN: CSW, status good",
              e == 0 && !memcmp(csw, "USBS", 4) && csw[4] == 1 && csw[12] == 0);
  pass &= say("USBSTS: USBINT raised", (rreg(0x24) & STS_INT) != 0);

  // A phase error and the host's reset recovery (Bulk-Only Transport 1.0,
  // 5.3.4 and 6.6): the command ends in a CSW with status 2; the host sends
  // Bulk-Only Mass Storage Reset, then CLEAR_FEATURE(ENDPOINT_HALT) to the
  // bulk IN and OUT endpoints, and the device must take the next CBW. Then
  // a reset in the middle of a command's data stage, which must leave the
  // device ready for a new CBW all the same.
  auto read10 = [&](u8 tag, bool data, bool status) {
    cbw[4] = tag;
    memcpy(cSystem->PtrToMem(DATA), cbw, sizeof(cbw));
    int r = run_tds(QH1, {{0, DATA, 31, 0}});
    memset(cSystem->PtrToMem(DATA + 0x1000), 0, 512);
    if (data)
      r |= run_tds(QH2, {{1, DATA + 0x1000, 512, 0}});
    memset(cSystem->PtrToMem(DATA + 0x2000), 0xff, 13);
    if (status)
      r |= run_tds(QH2, {{1, DATA + 0x2000, 13, 0}});
    return r;
  };
  auto csw_is = [&](u8 tag, u8 st) {
    return !memcmp(csw, "USBS", 4) && csw[4] == tag && csw[12] == st;
  };
  auto reset_recovery = [&]() {
    setup(DATA + 0x3000, 0x21, 0xff, 0, 0, 0); // Bulk-Only Mass Storage Reset
    int r = run_tds(QH0, {{2, DATA + 0x3000, 8, 0}, {1, 0, 0, 1}});
    setup(DATA + 0x3000, 0x02, 0x01, 0, 0x81, 0); // CLEAR_FEATURE(HALT) IN
    r |= run_tds(QH0, {{2, DATA + 0x3000, 8, 0}, {1, 0, 0, 1}});
    setup(DATA + 0x3000, 0x02, 0x01, 0, 0x02, 0); // and OUT
    r |= run_tds(QH0, {{2, DATA + 0x3000, 8, 0}, {1, 0, 0, 1}});
    return r;
  };
  bool phased = false;
  {
    std::lock_guard<std::mutex> lk(m_mx);
    phased = m_port[0].dev->inject_phase_error();
  }
  if (phased) {
    e = read10(2, true, true);
    pass &= say("phase error: CSW status 2", e == 0 && csw_is(2, 2));
    pass &=
        say("  reset recovery: reset, clear both halts", reset_recovery() == 0);
    e = read10(3, true, true);
    pass &= say("  next CBW: data and a good CSW, its tag",
                e == 0 && sec[510] == 0x55 && sec[511] == 0xaa && csw_is(3, 0));
    e = read10(4, false, false); // the CBW, and no more
    pass &= say("reset in a data stage", e == 0 && reset_recovery() == 0);
    e = read10(5, true, true);
    pass &= say("  next CBW: data and a good CSW, its tag",
                e == 0 && sec[510] == 0x55 && sec[511] == 0xaa && csw_is(5, 0));
  }

  reg(0x20, 0x00080000); // stop, and leave the controller to the guest
  reg(0x20, CMD_HCRESET);
  if (m_with_companions)
    pass &= selftest_companions(base);
  pass &= selftest_iso(base);
  printf("%%EHCI-I-SELFTEST: %s\n", pass ? "PASS" : "FAIL");
}

namespace {
/// The self-test's full-speed device: a USB 1.1 one with nothing but
/// endpoint 0, which an EHCI port leaves for its companion.
class CFullSpeedProbe : public CUsbDevice {
public:
  const char *name() const override { return "probe"; }

protected:
  const std::vector<u8> &device_descriptor() const override {
    static const std::vector<u8> d = {18,   1,    0x10, 0x01, 0, 0, 0, 64, 0x34,
                                      0x12, 0x78, 0x56, 0x00, 1, 0, 0, 0,  1};
    return d;
  }
  const std::vector<u8> &configuration_descriptor() const override {
    static const std::vector<u8> d = {9, 2, 18, 0, 1, 1,    0, 0x80, 50,
                                      9, 4, 0,  0, 0, 0xff, 0, 0,    0};
    return d;
  }
};
/// The self-test's isochronous device: high speed, an isochronous OUT
/// endpoint 2 and IN endpoint 1 of 1024 bytes and three transactions a
/// microframe, which loop back -- each packet sent OUT comes back IN as a
/// packet of the same length, in order, and an IN with nothing queued gets
/// a zero-length packet. Only the self-test plugs it in.
class CIsoLoopback : public CUsbDevice {
public:
  const char *name() const override { return "iso loopback"; }
  bool can_high_speed() const override { return true; }
  int iso_transfer(int pid, int ep, u8 *buf, int len) override {
    if (pid == PID_OUT && ep == 2) {
      if (m_queue.size() < 64)
        m_queue.emplace_back(buf, buf + len);
      return len;
    }
    if (pid == PID_IN && ep == 1) {
      if (m_queue.empty())
        return 0;
      const std::vector<u8> p = std::move(m_queue.front());
      m_queue.erase(m_queue.begin());
      memcpy(buf, p.data(), std::min((int)p.size(), len));
      return (int)p.size(); // above len: babble
    }
    return ISO_NO_ENDPOINT;
  }
  bool iso_endpoint(int pid, int ep) const override {
    return (pid == PID_OUT && ep == 2) || (pid == PID_IN && ep == 1);
  }

protected:
  const std::vector<u8> &device_descriptor() const override {
    static const std::vector<u8> d = {18,   1,  0x00, 0x02, 0xff, 0,
                                      0,    64, 0x34, 0x12, 0x79, 0x56,
                                      0x00, 1,  0,    0,    0,    1};
    return d;
  }
  const std::vector<u8> &configuration_descriptor() const override {
    // wMaxPacketSize 0x1400: 1024 bytes, two more transactions (Mult 3).
    static const std::vector<u8> d = {
        9, 2, 32, 0, 1,    1, 0,    0x80, 50, 9, 4, 0,    0, 2,    0xff, 0,
        0, 0, 7,  5, 0x81, 1, 0x00, 0x14, 1,  7, 5, 0x02, 1, 0x00, 0x14, 1};
    return d;
  }

private:
  std::vector<std::vector<u8>> m_queue;
};
} // namespace

// The self-test's isochronous part (EHCI 1.0 3.3, 4.7): high-speed iTDs
// in the periodic list, against the loopback device on a free port. An
// OUT iTD sends eight transactions of different lengths (up to three
// packets each, across buffer pages); IN iTDs from two frames later read
// them back into 3072-byte transactions, the last with IOC; another iTD
// addresses a device that is not there. Checks the data, the lengths
// written back, the status of every transaction, and USBINT. With
// ALPHABOX_USB_ASYNC_US the loopback answers through the async shim, as a
// device behind libusb would.
bool CEhci::selftest_iso(u32 base) {
  const u32 LIST = base + 0x4000;
  const u32 OUTBUF = base + 0x20000, INBUF = base + 0x30000;
  auto w32 = [&](u32 a, u32 v) { memcpy(cSystem->PtrToMem(a), &v, 4); };
  auto r32 = [&](u32 a) {
    u32 v;
    memcpy(&v, cSystem->PtrToMem(a), 4);
    return v;
  };
  auto reg = [&](u32 off, u32 v) {
    std::lock_guard<std::mutex> lk(m_mx);
    reg_write(off, v);
  };
  auto rreg = [&](u32 off) {
    std::lock_guard<std::mutex> lk(m_mx);
    return reg_read(off);
  };
  auto say = [](const char *what, bool ok) {
    printf("%%EHCI-I-SELFTEST: %-40s %s\n", what, ok ? "ok" : "FAILED");
    return ok;
  };
  int q = -1;
  for (int p = 0; p < kPorts && q < 0; ++p)
    if (!m_port[p].dev)
      q = p;
  if (q < 0) {
    printf("%%EHCI-I-SELFTEST: no free port for the isochronous loopback: "
           "iTDs not checked\n");
    return true;
  }
  {
    std::lock_guard<std::mutex> lk(m_mx);
    COhci *c = companion_of(q);
    std::unique_lock<std::mutex> clk;
    if (c)
      clk = std::unique_lock<std::mutex>(c->mutex());
    m_port[q].dev =
        usb_async_wrap(std::make_unique<CIsoLoopback>(), devid_string, q + 1);
  }
  bool pass = true;
  const u32 psc = 0x64 + 4 * q;
  reg(0x20, CMD_HCRESET);
  reg(0x60, 1); // CONFIGFLAG: the ports are the EHCI's
  reg(psc, PS_PP);
  reg(psc, PS_PP | PS_PR | PS_CSC);
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  reg(psc, PS_PP);
  char what[64];
  snprintf(what, sizeof(what), "port %d: iso loopback, high speed", q + 1);
  pass &= say(what, (rreg(psc) & PS_PED) != 0);

  // The frame list: empty but for the OUT iTD at frame 3, sixteen IN iTDs
  // at frames 5 to 20, and one for a missing device at frame 22.
  constexpr int kIn = 16;
  const u32 ITD_OUT = base + 0x5000, ITD_NONE = base + 0x5040;
  auto itd_in = [&](int k) { return base + 0x5100 + 0x40u * (u32)k; };
  auto inbuf = [&](int k) { return INBUF + 0x8000u * (u32)k; };
  for (u32 i = 0; i < 1024; ++i)
    w32(LIST + 4 * i, 1);
  w32(LIST + 4 * 3, ITD_OUT);
  for (int k = 0; k < kIn; ++k)
    w32(LIST + 4 * (5 + k), itd_in(k));
  w32(LIST + 4 * 22, ITD_NONE);
  // The endpoint words: address 0 (the loopback is not addressed), max
  // packet 1024, Mult 3.
  auto make_itd = [&](u32 itd, int ep, bool in, u32 buf, int dev) {
    for (int i = 0; i < 16; ++i)
      w32(itd + 4 * i, 0);
    w32(itd, 1);
    for (int p = 0; p < 7; ++p)
      w32(itd + 36 + 4 * p, (buf & ~0xfffu) + 0x1000u * p);
    w32(itd + 36, (buf & ~0xfffu) | ((u32)ep << 8) | (u32)dev);
    w32(itd + 40, r32(itd + 40) | (in ? 1u << 11 : 0) | 1024);
    w32(itd + 44, r32(itd + 44) | 3);
  };
  auto tx = [&](u32 itd, int uf, int len, u32 off, bool ioc) {
    w32(itd + 4 + 4 * uf, (1u << 31) | ((u32)len << 16) | (ioc ? 1u << 15 : 0) |
                              ((off >> 12) << 12) | (off & 0xfff));
  };
  int lens[8];
  std::vector<u8> sent;
  u32 at = 0x123; // start mid-page, so transactions cross pages
  make_itd(ITD_OUT, 2, false, OUTBUF, 0);
  u8 *ob = (u8 *)cSystem->PtrToMem(OUTBUF);
  for (int i = 0; i < 8; ++i) {
    lens[i] = i == 7 ? 0 : 100 + 400 * i; // up to 2500 (three packets), and
                                          // a zero-length one
    for (int k = 0; k < lens[i]; ++k)
      sent.push_back(ob[at + k] = (u8)(i * 37 + k));
    tx(ITD_OUT, i, lens[i], at, false);
    at += lens[i];
  }
  for (int k = 0; k < kIn; ++k) {
    make_itd(itd_in(k), 1, true, inbuf(k), 0);
    memset(cSystem->PtrToMem(inbuf(k)), 0xee, 8 * 3072);
    for (int i = 0; i < 8; ++i)
      tx(itd_in(k), i, 3072, 3072u * i, k == kIn - 1 && i == 7);
  }
  make_itd(ITD_NONE, 1, true, INBUF, 9); // address 9: nobody
  tx(ITD_NONE, 0, 64, 0, false);

  reg(0x24, 0x3f);
  reg(0x2c, 0); // FRINDEX, while halted
  reg(0x34, LIST);
  reg(0x20, CMD_RS | CMD_PSE | 0x00080000);
  for (int t = 0; t < 300 && (r32(ITD_NONE + 4) & (1u << 31)); ++t)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  const u32 sts = rreg(0x24);
  reg(0x20, 0x00080000);

  bool out_ok = true;
  for (int i = 0; i < 8; ++i) {
    const u32 to = r32(ITD_OUT + 4 + 4 * i);
    out_ok &= (to & 0xf0000000) == 0 && (int)((to >> 16) & 0xfff) == lens[i];
  }
  pass &= say("iTD OUT: 8 transactions done, no error", out_ok);
  // The IN transactions in order: what they received (the length written
  // back), and how each ended. Straight from the device, the first IN iTD
  // gets the eight packets back as they were sent, and the rest empty
  // packets; through the async shim (ALPHABOX_USB_ASYNC_US) a transaction
  // may find nothing arrived yet -- a Data Buffer Error, never anything
  // else -- but the bytes received, in order, are still the bytes sent.
  const bool shim = getenv("ALPHABOX_USB_ASYNC_US") != nullptr;
  std::vector<u8> got;
  int errors = 0, buffer_errors = 0;
  bool first_ok = true;
  for (int k = 0; k < kIn; ++k)
    for (int i = 0; i < 8; ++i) {
      const u32 ti = r32(itd_in(k) + 4 + 4 * i);
      const int n = (ti >> 16) & 0xfff;
      if (ti & 0xb0000000)
        ++errors; // active, babble or transaction error
      if (ti & (1u << 30))
        ++buffer_errors;
      const u8 *d = (const u8 *)cSystem->PtrToMem(inbuf(k) + 3072u * i);
      got.insert(got.end(), d, d + n);
      if (k == 0)
        first_ok &= !(ti & 0xf0000000) && n == lens[i];
    }
  if (!shim) {
    pass &= say("iTD IN: lengths back, no error",
                first_ok && !errors && !buffer_errors);
  } else {
    char line[80];
    snprintf(line, sizeof(line),
             "iTD IN via the shim: %d of %d not in time (DBE)", buffer_errors,
             8 * kIn);
    pass &= say(line, !errors);
  }
  pass &= say("iTD IN: the data sent, in order", got == sent);
  pass &= say("iTD IN: IOC raised USBINT", (sts & STS_INT) != 0);
  const u32 tn = r32(ITD_NONE + 4);
  pass &= say("iTD to no device: transaction error",
              !(tn & (1u << 31)) && (tn & (1u << 28)) && !((tn >> 16) & 0xfff));
  pass &= say("  and USBERRINT", (sts & STS_ERR) != 0);

  reg(0x20, CMD_HCRESET);
  {
    std::lock_guard<std::mutex> lk(m_mx);
    COhci *c = companion_of(q);
    std::unique_lock<std::mutex> clk;
    if (c)
      clk = std::unique_lock<std::mutex>(c->mutex());
    m_port[q].dev.reset();
  }
  for (auto &c : m_comp)
    if (c)
      c->ohci.reset();
  return pass;
}

// The self-test's second part, on a card with companions: port routing
// (EHCI 1.0 4.2). CONFIGFLAG 0 gives every port to the companions and 1
// takes them back; a full-speed device on a free port stays disabled after
// the EHCI's port reset, PORT_OWNER hands it to its companion, which then
// enumerates it (GET_DESCRIPTOR over its control list), and clearing
// PORT_OWNER takes it back.
bool CEhci::selftest_companions(u32 base) {
  const u32 HCCA = base + 0x3000, ED = base + 0x3100, TD = base + 0x3200;
  const u32 DATA = base + 0x10000;
  auto w32 = [&](u32 a, u32 v) { memcpy(cSystem->PtrToMem(a), &v, 4); };
  auto r32 = [&](u32 a) {
    u32 v;
    memcpy(&v, cSystem->PtrToMem(a), 4);
    return v;
  };
  auto reg = [&](u32 off, u32 v) {
    std::lock_guard<std::mutex> lk(m_mx);
    reg_write(off, v);
  };
  auto rreg = [&](u32 off) {
    std::lock_guard<std::mutex> lk(m_mx);
    return reg_read(off);
  };
  auto oreg = [&](int c, u32 off, u32 v) {
    m_comp[c]->ohci.usb_hci_write(off, 32, v);
  };
  auto roreg = [&](int c, u32 off) {
    return (u32)m_comp[c]->ohci.usb_hci_read(off, 32);
  };
  auto say = [](const char *what, bool ok) {
    printf("%%EHCI-I-SELFTEST: %-40s %s\n", what, ok ? "ok" : "FAILED");
    return ok;
  };
  auto sleep_ms = [](int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
  };
  bool pass = true;

  // After the reset CONFIGFLAG is 0: every port is a companion's.
  bool all = true;
  for (int p = 0; p < kPorts; ++p)
    all &= (rreg(0x64 + 4 * p) & (PS_OWNER | PS_CCS)) == PS_OWNER;
  pass &= say("CONFIGFLAG 0: every port a companion's", all);
  oreg(0, 0x50, 1u << 16); // companion 0: SetGlobalPower
  pass &= say("companion 0 port 1: the disk connected", roreg(0, 0x54) & 1);
  reg(0x60, 1);
  pass &= say("CONFIGFLAG 1: taken from the companion",
              (roreg(0, 0x54) & 0x10001) == 0x10000);

  // A full-speed device: the tablet if the card has one, else a probe
  // plugged into a free port for the test.
  int q = -1;
  bool probe = false;
  for (int p = 0; p < kPorts && q < 0; ++p)
    if (m_port[p].dev && !m_port[p].dev->can_high_speed() &&
        !m_port[p].dev->low_speed())
      q = p;
  for (int p = kPorts - 1; p >= 0 && q < 0; --p)
    if (!m_port[p].dev) {
      q = p;
      probe = true;
    }
  if (q < 0) {
    printf("%%EHCI-I-SELFTEST: no free port for a full-speed device: "
           "its routing not checked\n");
    reg(0x20, CMD_HCRESET);
    for (auto &c : m_comp)
      c->ohci.reset();
    return pass;
  }
  const int cq = q / kPortsPerCompanion, lq = q % kPortsPerCompanion;
  const u32 psc = 0x64 + 4 * q, rhps = 0x54 + 4 * lq;
  if (probe) {
    std::lock_guard<std::mutex> lk(m_mx);
    std::lock_guard<std::mutex> clk(m_comp[cq]->ohci.mutex());
    m_port[q].dev = std::make_unique<CFullSpeedProbe>();
  }
  char what[64];
  reg(psc, PS_PP);
  snprintf(what, sizeof(what), "port %d: full-speed %s connected", q + 1,
           m_port[q].dev->name());
  pass &= say(what, (rreg(psc) & PS_CCS) != 0);
  reg(psc, PS_PP | PS_PR | PS_CSC);
  sleep_ms(10);
  reg(psc, PS_PP);
  pass &= say("  left disabled after reset, line J",
              (rreg(psc) & (PS_CCS | PS_PED | (3u << 10))) ==
                  (PS_CCS | (2u << 10)));
  reg(psc, PS_PP | PS_OWNER | PS_CSC);
  pass &= say("  PORT_OWNER: gone from the EHCI", !(rreg(psc) & PS_CCS));
  oreg(cq, 0x50, 1u << 16);
  snprintf(what, sizeof(what), "  companion %d port %d: connected, full speed",
           cq, lq + 1);
  pass &= say(what, (roreg(cq, rhps) & 0x201) == 0x001);
  oreg(cq, rhps, 1u << 4); // SetPortReset
  pass &= say("  companion: enabled after its reset", roreg(cq, rhps) & 2);

  // GET_DESCRIPTOR(device) over the companion's control list: SETUP, IN,
  // status OUT, and the empty tail TD.
  const u32 TAIL = TD + 0x60;
  static const u8 setup[8] = {0x80, 6, 0, 1, 0, 0, 18, 0};
  memcpy(cSystem->PtrToMem(DATA), setup, 8);
  memset(cSystem->PtrToMem(DATA + 0x100), 0, 18);
  const u32 not_accessed = 0xfu << 28, no_int = 7u << 21;
  const u32 td[3][4] = {
      {not_accessed | (2u << 24) | no_int, DATA, TD + 0x20, DATA + 7},
      {not_accessed | (3u << 24) | no_int | (2u << 19) | (1u << 18),
       DATA + 0x100, TD + 0x40, DATA + 0x100 + 17},
      {not_accessed | (3u << 24) | (1u << 19), 0, TAIL, 0}};
  for (int i = 0; i < 3; ++i)
    for (int k = 0; k < 4; ++k)
      w32(TD + 0x20 * i + 4 * k, td[i][k]);
  for (int k = 0; k < 4; ++k)
    w32(TAIL + 4 * k, 0);
  w32(ED + 0, 64u << 16); // address 0, endpoint 0, direction from the TD
  w32(ED + 4, TAIL);
  w32(ED + 8, TD);
  w32(ED + 12, 0);
  oreg(cq, 0x18, HCCA);
  oreg(cq, 0x20, ED);
  oreg(cq, 0x04, 0x90); // UsbOperational, control list enabled
  oreg(cq, 0x08, 2);    // ControlListFilled
  for (int t = 0; t < 2000; ++t) {
    if ((r32(ED + 8) & ~0xfu) == TAIL || (r32(ED + 8) & 1))
      break;
    sleep_ms(1);
  }
  bool ok = (r32(ED + 8) & ~0xfu) == TAIL;
  for (int i = 0; i < 3; ++i)
    ok &= (r32(TD + 0x20 * i) >> 28) == 0;
  const u8 *dd = (const u8 *)cSystem->PtrToMem(DATA + 0x100);
  pass &= say("  companion: GET_DESCRIPTOR(device)",
              ok && dd[0] == 18 && dd[1] == 1 &&
                  (!probe || (dd[8] == 0x34 && dd[9] == 0x12)));
  oreg(cq, 0x04, 0); // UsbReset

  // PORT_OWNER cleared: back to the EHCI.
  reg(psc, PS_PP);
  pass &= say("  PORT_OWNER 0: back to the EHCI",
              (rreg(psc) & PS_CCS) && !(roreg(cq, rhps) & 1));

  reg(0x20, CMD_HCRESET);
  {
    std::lock_guard<std::mutex> lk(m_mx);
    std::lock_guard<std::mutex> clk(m_comp[cq]->ohci.mutex());
    if (probe)
      m_port[q].dev.reset();
    else
      m_port[q].dev->reset();
  }
  for (auto &c : m_comp)
    c->ohci.reset();
  return pass;
}
