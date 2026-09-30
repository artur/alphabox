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
 *
 * Although this is not required, the author would appreciate being notified of,
 * and receiving any modifications you may make to the source code that might
 * serve the general public.
 */

#include "AliM1543C_usb.hpp"
#include "AliM1543C.hpp"
#include "SCSIBus.hpp"
#include "StdAfx.hpp"
#include "System.hpp"
#include "UsbAsyncShim.hpp"
#include "UsbAudio.hpp"
#include "UsbHostDevice.hpp"
#include "UsbStorage.hpp"
#include "UsbTablet.hpp"
#include <chrono>
#include <cstring>

u32 usb_cfg_data[64] = {
    /*00*/ 0x523710b9, // CFID: vendor + device
    /*04*/ 0x02800000, // CFCS: command + status
    /*08*/ 0x0c031003, // CFRV: class + revision
    /*0c*/ 0x00000000, // CFLT: latency timer + cache line size
    /*10*/ 0x00000000, // BAR0:
    /*14*/ 0x00000000, // BAR1:
    /*18*/ 0x00000000, // BAR2:
    /*1c*/ 0x00000000, // BAR3:
    /*20*/ 0x00000000, // BAR4:
    /*24*/ 0x00000000, // BAR5:
    /*28*/ 0x00000000, // CCIC: CardBus
    /*2c*/ 0x00000000, // CSID: subsystem + vendor
    /*30*/ 0x00000000, // BAR6: expansion rom base
    /*34*/ 0x00000000, // CCAP: capabilities pointer
    /*38*/ 0x00000000,
    /*3c*/ 0x500001ff, // CFIT: interrupt configuration
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0};

u32 usb_cfg_mask[64] = {
    /*00*/ 0x00000000, // CFID: vendor + device
    /*04*/ 0x00000157, // CFCS: command + status
    /*08*/ 0x00000000, // CFRV: class + revision
    /*0c*/ 0x0000ffff, // CFLT: latency timer + cache line size
    /*10*/ 0xfffff000, // BAR0
    /*14*/ 0x00000000, // BAR1:
    /*18*/ 0x00000000, // BAR2:
    /*1c*/ 0x00000000, // BAR3:
    /*20*/ 0x00000000, // BAR4:
    /*24*/ 0x00000000, // BAR5:
    /*28*/ 0x00000000, // CCIC: CardBus
    /*2c*/ 0x00000000, // CSID: subsystem + vendor
    /*30*/ 0x00000000, // BAR6: expansion rom base
    /*34*/ 0x00000000, // CCAP: capabilities pointer
    /*38*/ 0x00000000,
    /*3c*/ 0x000000ff, // CFIT: interrupt configuration
    /*40*/ 0x04100000, // TM - test mode register
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0};

/**
 * Constructor.
 **/
CAliM1543C_usb::CAliM1543C_usb(CConfigurator *cfg, CSystem *c, int pcibus,
                               int pcidev)
    : CPCIDevice(cfg, c, pcibus, pcidev), CDiskController(kPorts + 1, 1),
      m_ohci(*this, kPorts, true, "usb") {
  add_function(0, usb_cfg_data, usb_cfg_mask);
  for (int p = 0; p < kPorts; ++p)
    m_ohci.bind_port(p, &m_port[p]);

  ResetPCI();

  // Devices on the root hub's ports: port1..port3 = "tablet".
  for (int p = 0; p < kPorts; ++p) {
    char key[8];
    snprintf(key, sizeof(key), "port%d", p + 1);
    const char *what = myCfg->get_text_value(key, "");
    if (!strcmp(what, "tablet")) {
      auto t = std::make_unique<CUsbTablet>();
      theUsbTablet.store(t.get());
      attach(p, std::move(t));
      printf("%s: USB tablet on port %d.\n", devid_string, p + 1);
    } else if (!strcmp(what, "audio")) {
      attach(p, std::make_unique<CUsbAudio>());
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
      FAILURE_2(Configuration, "%s: unknown USB device \"%s\"", key, what);
    }
  }

  // Tests' usb:... faults come here unless an EHCI card takes them.
  CUsbFaultTarget *none = nullptr;
  theUsbFaultTarget.compare_exchange_strong(none, this);

  printf(
      "%s: $Id: AliM1543C_usb.cpp,v 1.6 2008/03/14 15:30:50 iamcamiel Exp $\n",
      devid_string);
}

CAliM1543C_usb::~CAliM1543C_usb() {
  stop_threads();
  CUsbFaultTarget *me = this;
  theUsbFaultTarget.compare_exchange_strong(me, nullptr);
  for (auto &port : m_port)
    if (port.dev && theUsbTablet.load() == port.dev.get())
      theUsbTablet.store(nullptr);
}

void CAliM1543C_usb::register_disk(class CDisk *dsk, int bus, int dev) {
  if (bus < 1 || bus > kPorts || dev != 0)
    FAILURE(Configuration,
            "USB disks are named disk<port>.0, with port 1 to 3");
  if (m_port[bus - 1].dev)
    FAILURE_1(Configuration, "USB port %d already has a device", bus);
  CDiskController::register_disk(dsk, bus, dev);
  // Each storage device is the initiator on a SCSI bus of its own.
  std::unique_ptr<CUsbDevice> d =
      std::make_unique<CUsbStorage>(new CSCSIBus(myCfg, cSystem), dsk);
  // ALPHABOX_USB_ASYNC_US=<us>: answer like hardware behind libusb (the
  // passthrough path's timing, with data that can be checked).
  if (const char *e = getenv("ALPHABOX_USB_ASYNC_US")) {
    d = std::make_unique<CUsbAsyncShim>(std::move(d), atoi(e));
    printf("%s: USB storage on port %d answers after %d us.\n", devid_string,
           bus, atoi(e));
  }
  attach(bus - 1, std::move(d));
  printf("%s: USB storage on port %d.\n", devid_string, bus);
}

void CAliM1543C_usb::ResetPCI() {
  CPCIDevice::ResetPCI();
  m_ohci.reset();
  std::lock_guard<std::mutex> lk(m_ohci.mutex());
  for (auto &port : m_port)
    if (port.dev)
      port.dev->reset();
}

// The function's interrupt leaves through the bridge's USBIR routing byte
// to an ISA IRQ (IRQ 10 unless the firmware moves it), as a level.
void CAliM1543C_usb::ohci_irq(bool level) {
  const int irq = theAli ? theAli->routed_irq(0x74) : -1;
  if (irq != m_irq && m_irq >= 0 && m_irq_level)
    theAli->pic_set_line(m_irq >> 3, m_irq & 7, false);
  if (irq >= 0 && (level != m_irq_level || irq != m_irq))
    theAli->pic_set_line(irq >> 3, irq & 7, level);
  m_irq = irq;
  m_irq_level = level;
}

bool CAliM1543C_usb::inject_fault(const char *op, int port, int ep, int arg) {
  return m_ohci.inject_fault(op, port, ep, arg);
}

void CAliM1543C_usb::attach(int p, std::unique_ptr<CUsbDevice> dev) {
  dev->on_complete = [this]() { m_ohci.kick(); };
  m_port[p].dev = std::move(dev);
}

void CAliM1543C_usb::start_threads() { m_ohci.start_threads(); }

void CAliM1543C_usb::stop_threads() { m_ohci.stop_threads(); }

void CAliM1543C_usb::check_state() {
  if (m_ohci.thread_dead())
    FAILURE(Thread, "USB thread has died");
}

u32 CAliM1543C_usb::ReadMem_Bar(int func, int bar, u32 address, int dsize) {
  u32 data = 0;
  switch (bar) {
  case 0:
    data = (u32)m_ohci.usb_hci_read(address, dsize);
    break;
  default:
    printf("%%USB-W-READBAR: Bad BAR %d selected.\n", bar);
  }

  return data;
}

void CAliM1543C_usb::WriteMem_Bar(int func, int bar, u32 address, int dsize,
                                  u32 data) {
  switch (bar) {
  case 0:
    m_ohci.usb_hci_write(address, dsize, data);
    break;
  default:
    printf("%%USB-W-WRITEBAR: Bad BAR %d selected.\n", bar);
  }

  return;
}

// ALPHABOX_USBTRACE=1: log each OHCI register write with the per-register read
// counts since the previous write, plus a read summary every 20000 reads
// (driver-polling diagnosis).
static const bool g_usbtrace = getenv("ALPHABOX_USBTRACE") != nullptr;
static const auto g_usbtrace_t0 = std::chrono::steady_clock::now();
static u64 g_usb_reads[0x110 / 4 + 1];
static u64 g_usb_reads_total;

static void usbtrace_line(const char *what, u64 address, u64 data) {
  char buf[640];
  int len = snprintf(buf, sizeof(buf), "USBT %10.1f %s %03x=%08x reads:",
                     std::chrono::duration<double, std::milli>(
                         std::chrono::steady_clock::now() - g_usbtrace_t0)
                         .count(),
                     what, (unsigned)address, (unsigned)data);
  for (int i = 0; i <= 0x110 / 4 && len < (int)sizeof(buf) - 24; i++)
    if (g_usb_reads[i]) {
      len += snprintf(buf + len, sizeof(buf) - len, " %03x:%llu", i * 4,
                      (unsigned long long)g_usb_reads[i]);
      g_usb_reads[i] = 0;
    }
  printf("%s\n", buf);
}

COhci::COhci(COhciHost &host, int ports, bool legacy, const char *name)
    : m_host(host), m_ports(ports), m_legacy(legacy), m_name(name) {
  m_xfer.resize(kMaxRun + 0x2000);
  memset(&state, 0, sizeof(state));
}

COhci::~COhci() { stop_threads(); }

// UsbReset with every register at its power-on value: no schedule runs,
// no HCCA is written, the root hub's ports are off.
void COhci::reset() {
  std::lock_guard<std::mutex> lk(m_mx);
  memset(state.usb_data, 0, sizeof(state.usb_data));
  state.usb_data[0x34 / 4] = 0x2edf;                    // HcFmInterval
  state.usb_data[0x44 / 4] = 0x0628;                    // HcLSThreshold
  state.usb_data[0x48 / 4] = 0x01000000 | (u32)m_ports; // HcRhDescriptorA
  m_frame = 0;
  m_done_head = 0;
  m_done_delay = 7;
  ohci_update_irq();
}

// Test faults (CUsbPortFaults, docs/headless.md): from the GUI thread.
bool COhci::inject_fault(const char *op, int port, int ep, int arg) {
  std::lock_guard<std::mutex> lk(m_mx);
  if (port < 1 || port > m_ports || !port_dev(port - 1))
    return false;
  const int p = port - 1;
  CUsbPortFaults &f = m_port[p]->faults;
  if (!strcmp(op, "detach") || !strcmp(op, "attach")) {
    f.unplugged = !strcmp(op, "detach");
    if (f.unplugged)
      port_dev(p)->reset();
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
    return port_dev(p)->inject_phase_error();
  } else {
    return false;
  }
  kick();
  return true;
}

void COhci::port_routed(int p, bool owns) {
  std::lock_guard<std::mutex> lk(m_mx);
  if (!m_port[p])
    return;
  m_port[p]->ohci_owns = owns;
  port_refresh(p);
}

void COhci::reconnect(int p) {
  std::lock_guard<std::mutex> lk(m_mx);
  if (!port_dev(p))
    return;
  port_reg(p) &= ~(RH_PES | RH_PSS);
  port_change(p, RH_CSC | RH_PESC);
}

void COhci::save_runtime(CUsbSaved &s) const {
  s.put(m_frame);
  s.put(m_done_head);
  s.put((u32)m_done_delay);
}

void COhci::load_runtime(CUsbSaved &s) {
  std::lock_guard<std::mutex> lk(m_mx);
  const u32 frame = s.get(), head = s.get(), delay = s.get();
  if (!s.ok || delay > 7)
    return;
  m_frame = frame & 0xffff;
  m_done_head = head;
  m_done_delay = (int)delay;
}

void COhci::refresh_irq() {
  std::lock_guard<std::mutex> lk(m_mx);
  ohci_update_irq();
}

void COhci::kick() {
  {
    std::lock_guard<std::mutex> lk(m_kick_mx);
    m_kicked = true;
  }
  m_kick_cv.notify_one();
}

void COhci::start_threads() {
  if (!myThread) {
    printf(" %s", m_name);
    StopThread = false;
    myThread = std::make_unique<std::thread>([this]() { this->run(); });
  }
}

void COhci::stop_threads() {
  StopThread = true;
  kick(); // out of its wait
  if (myThread) {
    printf(" %s", m_name);
    myThread->join();
    myThread = nullptr;
  }
}

// The frame clock: 1 ms, paced by wall time; frames missed while the host
// thread was not scheduled are not replayed (the frame number still counts
// them, as a real controller's would).
void COhci::run() {
  try {
    auto next = std::chrono::steady_clock::now();
    while (!StopThread) {
      next += std::chrono::milliseconds(1);
      // Sleep to the next frame -- or less, when a device finishes a
      // transfer first: then the control and bulk lists run again at once,
      // the way a real controller would have seen the device answer.
      for (;;) {
        std::unique_lock<std::mutex> klk(m_kick_mx);
        if (!m_kick_cv.wait_until(klk, next, [this]() { return m_kicked; }))
          break; // the frame is due
        m_kicked = false;
        klk.unlock();
        if (StopThread)
          break;
        std::lock_guard<std::mutex> lk(m_mx);
        if (ohci_operational()) {
          service_async_lists();
          // TDs that want their interrupt at once get it now, not at the
          // frame's end: a driver waiting on each transfer (usbstor waits on
          // three per command) would otherwise wait a frame for each.
          if (m_done_head && m_done_delay == 0)
            write_back_done();
        }
      }
      const auto now = std::chrono::steady_clock::now();
      int elapsed = 1;
      if (now - next > std::chrono::milliseconds(1)) {
        elapsed += (int)std::chrono::duration_cast<std::chrono::milliseconds>(
                       now - next)
                       .count();
        next = now;
      }
      std::lock_guard<std::mutex> lk(m_mx);
      if (!ohci_operational())
        continue;
      // Frames the thread slept through are run now, one by one, up to a
      // limit: an isochronous stream moves a packet in each, and a real
      // controller would not have skipped them. Beyond the limit they are
      // counted but not run.
      const int run_now = std::min(elapsed, kCatchUpFrames);
      for (int i = 0; i < elapsed; ++i) {
        const u32 before = m_frame;
        m_frame = (m_frame + 1) & 0xffff;
        if ((before ^ m_frame) & 0x8000)
          state.usb_data[0x0c / 4] |= OHCI_INT_FNO;
        if (i >= elapsed - run_now)
          frame();
      }
    }
  } catch (CException &e) {
    printf("Exception in %s thread: %s.\n", m_name, e.displayText().c_str());
    myThreadDead.store(true);
  }
}

// One frame (OHCI 1.0a 6.3, 6.4): the frame number into the HCCA, then the
// periodic list for this frame, then the control and bulk lists, then the
// done queue.
void COhci::frame() {
  const u32 hcca = state.usb_data[0x18 / 4];
  if (hcca) {
    u32 fn = m_frame; // HccaFrameNumber, and HccaPad1 cleared
    do_pci_write(hcca + 0x80, &fn, sizeof(u32), 1);
  }
  u32 &control = state.usb_data[4 / 4];
  u32 &cmd = state.usb_data[8 / 4];
  if ((control & OHCI_CTL_PLE) && hcca) {
    u32 head;
    do_pci_read(hcca + 4 * (m_frame & 31), &head, sizeof(u32), 1);
    for (u32 ed = head & ~0xfu, n = 0; ed && n < 256; ++n) {
      u32 e[4];
      do_pci_read(ed, e, sizeof(u32), 4);
      // Isochronous endpoints sit at the list's tail: with
      // IsochronousEnable clear the controller stops at the first one.
      if ((e[0] & (1u << 15)) && !(control & OHCI_CTL_IE))
        break;
      service_ed(ed, true);
      ed = e[3] & ~0xfu;
    }
  }
  (void)cmd;
  service_async_lists();
  // The done queue goes out when its interrupt delay runs out.
  if (m_done_head && m_done_delay != 7) {
    if (m_done_delay == 0)
      write_back_done();
    else
      --m_done_delay;
  }
  ohci_status(OHCI_INT_SF);
}

// The control and bulk lists, each while its filled bit is set. The bit is
// cleared as a list starts and set again if any of its endpoints had a TD
// queued -- run or NAKed -- so an endpoint waiting on its device is retried
// without the driver having to ask (OHCI 1.0a 7.2.2, HcCommandStatus).
void COhci::service_async_lists() {
  const u32 control = state.usb_data[4 / 4];
  u32 &cmd = state.usb_data[8 / 4];
  if ((control & OHCI_CTL_CLE) && (cmd & OHCI_CMD_CLF)) {
    cmd &= ~OHCI_CMD_CLF;
    if (service_list(state.usb_data[0x20 / 4]))
      cmd |= OHCI_CMD_CLF;
    state.usb_data[0x24 / 4] = 0; // HcControlCurrentED: at the list's end
  }
  if ((control & OHCI_CTL_BLE) && (cmd & OHCI_CMD_BLF)) {
    cmd &= ~OHCI_CMD_BLF;
    if (service_list(state.usb_data[0x28 / 4]))
      cmd |= OHCI_CMD_BLF;
    state.usb_data[0x2c / 4] = 0;
  }
}

// A whole control or bulk list, from its head. True if any endpoint on it
// had a TD queued.
bool COhci::service_list(u32 head) {
  bool found = false;
  for (u32 ed = head & ~0xfu, n = 0; ed && n < 256; ++n) {
    u32 e[4];
    do_pci_read(ed, e, sizeof(u32), 4);
    bool f = false;
    service_ed(ed, false, &f);
    found |= f;
    ed = e[3] & ~0xfu;
  }
  return found;
}

CUsbDevice *COhci::device_at(int address) {
  for (int p = 0; p < m_ports; ++p)
    if (port_dev(p) && (port_reg(p) & RH_PES) && !m_port[p]->faults.unplugged &&
        port_dev(p)->address() == address)
      return port_dev(p);
  return nullptr;
}

// The TDs queued on one endpoint (OHCI 1.0a 4.2, 4.3.1). A periodic endpoint
// gets one TD a visit; control and bulk run until a TD NAKs or the queue is
// empty. Returns how many TDs made progress; *found says whether the
// endpoint had any queued at all.
//
// On a bulk endpoint, consecutive TDs are handed to the device as ONE
// transfer while each but the last is a whole number of max-size packets:
// exactly the stream a real device would see, since the controller moves
// packets and the device only ever notices a short one. When the data ends
// early (a short packet), the TD it ends in is retired short and the rest
// stay queued -- as on the bus. For a device behind libusb that is one
// round trip for the whole run instead of one per TD.
int COhci::service_ed(u32 ed_addr, bool periodic, bool *found) {
  u32 ed[4];
  do_pci_read(ed_addr, ed, sizeof(u32), 4);
  const u32 flags = ed[0];
  if (found)
    *found = false;
  if (flags & (1u << 14)) // sKip
    return 0;
  const int ep = (flags >> 7) & 0xf;
  const int mps = std::max(1, (int)((flags >> 16) & 0x7ff));
  const int ed_dir = (flags >> 11) & 3;
  struct Td {
    u32 addr;
    u32 w[4];
    int len;
  };
  auto td_len = [](const u32 w[4]) {
    const u32 cbp = w[1], be = w[3];
    int len = 0;
    if (cbp)
      len = (int)((be & 0xfff) - (cbp & 0xfff) + 1 +
                  (((cbp ^ be) & ~0xfffu) ? 0x1000 : 0));
    return (len < 0 || len > 0x2000) ? 0 : len;
  };
  auto td_pid = [&](const u32 w[4]) {
    // Direction: the endpoint's, or the TD's when the endpoint says so.
    const int dir = (ed_dir == 1 || ed_dir == 2) ? ed_dir : (w[0] >> 19) & 3;
    return dir == 0   ? CUsbDevice::PID_SETUP
           : dir == 1 ? CUsbDevice::PID_OUT
                      : CUsbDevice::PID_IN;
  };
  // A TD's buffer, CBP..BE, possibly across one 4 KB page boundary.
  auto copy = [&](const Td &t, u8 *data, bool to_guest, int n) {
    const u32 cbp = t.w[1], be = t.w[3];
    const int first = std::min(n, (int)(0x1000 - (cbp & 0xfff)));
    if (to_guest) {
      do_pci_write(cbp, data, 1, first);
      if (n > first)
        do_pci_write(be & ~0xfffu, data + first, 1, n - first);
    } else {
      do_pci_read(cbp, data, 1, first);
      if (n > first)
        do_pci_read(be & ~0xfffu, data + first, 1, n - first);
    }
  };
  // Retire one TD that moved n bytes with condition cc: data toggle, CBP,
  // an underrun for a short IN without bufferRounding, the done queue, and
  // the endpoint's new head (halted on an error). Returns the final cc.
  auto finish = [&](Td &t, int n, int cc, bool in) {
    // Data toggle: one per max-size packet moved (at least one).
    const int packets = std::max(1, (n + mps - 1) / mps);
    int toggle = (t.w[0] & (1u << 25)) ? (t.w[0] >> 24) & 1 : (ed[2] >> 1) & 1;
    if (cc == 0)
      toggle ^= packets & 1;
    t.w[0] = (t.w[0] & ~(3u << 24)) | (2u << 24) | ((u32)toggle << 24);
    if (cc == 0 && in && n < t.len && !(t.w[0] & (1u << 18)))
      cc = 9; // DataUnderrun
    // CBP: the first byte not transferred, or 0 when all were.
    const u32 cbp = t.w[1], be = t.w[3];
    if (n >= t.len)
      t.w[1] = 0;
    else if (((cbp & 0xfff) + n) >= 0x1000) // continued on BE's page
      t.w[1] = (be & ~0xfffu) + ((cbp & 0xfff) + n - 0x1000);
    else
      t.w[1] = cbp + n;
    const u32 next_td = t.w[2] & ~0xfu;
    retire_td(t.addr, t.w, cc);
    ed[2] = next_td | ((u32)toggle << 1) | (cc ? 1u : 0u);
    do_pci_write(ed_addr + 8, &ed[2], sizeof(u32), 1);
    return cc;
  };

  int done = 0;
  Td run[32];
  for (int guard = 0; guard < 64; ++guard) {
    const u32 head = ed[2] & ~0xfu, tail = ed[1] & ~0xfu;
    if ((ed[2] & 1) || head == tail) // Halted, or nothing queued
      break;
    if (found)
      *found = true;
    Td &first = run[0];
    first.addr = head;
    do_pci_read(head, first.w, sizeof(u32), 4);
    first.len = td_len(first.w);
    if (flags & (1u << 15)) {
      if (periodic)
        return service_iso_ed(ed_addr, ed);
      // An isochronous endpoint on the control or bulk list is the
      // driver's mistake: its TDs are retired untouched.
      retire_td(head, first.w, 0xf); // NotAccessed
      ed[2] = (first.w[2] & ~0xfu) | (ed[2] & 3);
      ++done;
      continue;
    }
    const int pid = td_pid(first.w);
    const bool in = pid == CUsbDevice::PID_IN;
    // The run: this TD, and on a bulk endpoint the ones after it while the
    // stream stays unbroken (whole packets) and the buffer holds them.
    int count = 1, total = first.len;
    if (!periodic && ep != 0 && pid != CUsbDevice::PID_SETUP) {
      while (count < 32 && run[count - 1].len > 0 &&
             run[count - 1].len % mps == 0) {
        const u32 next = run[count - 1].w[2] & ~0xfu;
        if (!next || next == tail)
          break;
        Td &t = run[count];
        t.addr = next;
        do_pci_read(next, t.w, sizeof(u32), 4);
        t.len = td_len(t.w);
        if (td_pid(t.w) != pid || total + t.len > kMaxRun)
          break;
        total += t.len;
        ++count;
      }
    }
    u8 *buf = m_xfer.data();
    if (!in)
      for (int i = 0, at = 0; i < count; at += run[i].len, ++i)
        if (run[i].len)
          copy(run[i], buf + at, false, run[i].len);
    CUsbDevice *dev = device_at(flags & 0x7f);
    if (!dev) {
      finish(first, 0, 5, in); // DeviceNotResponding
      ++done;
      break;
    }
    int n = total;
    CUsbDevice::Result r;
    CUsbPortFaults *f = faults_for(dev);
    if (f && f->intercept(dev, pid, ep, buf, n, r))
      n = 0; // a test fault answered it
    else
      r = dev->transfer(pid, ep, buf, n);
    if (r == CUsbDevice::USB_NAK)
      break; // retried when the device answers, or next frame
    if (r == CUsbDevice::USB_STALL) {
      finish(first, 0, 4, in); // Stall
      ++done;
      break;
    }
    // Hand the bytes out to the run's TDs in order. The TD the data ends
    // in is short; if it ended exactly between TDs yet short of the run,
    // the device sent a zero-length packet, which lands in the next TD.
    int at = 0, cc = 0;
    for (int i = 0; i < count; ++i) {
      const int k = std::min(run[i].len, n - at);
      if (in && k)
        copy(run[i], buf + at, true, k);
      at += k;
      cc = finish(run[i], k, 0, in);
      ++done;
      if (cc || k < run[i].len)
        break;
      if (at == n && n < total) {
        // IN: a zero-length packet ended it, and it lands in the next TD.
        // OUT: the device took less; the rest stays queued for next time.
        if (in && i + 1 < count) {
          cc = finish(run[i + 1], 0, 0, in);
          ++done;
        }
        break;
      }
    }
    if (cc || periodic || n < total)
      break;
  }
  return done;
}

// An isochronous endpoint's TDs (OHCI 1.0a 4.3.2, 6.4.4.3). Each TD covers
// FrameCount + 1 consecutive frames from StartingFrame, one packet a frame,
// the packets' buffers given by eight 13-bit offsets into the TD's (at most)
// two pages. In each frame the controller works out which packet of the head
// TD is due -- R = frame number - StartingFrame -- moves it, and replaces
// its offset with the packet status word: condition code and, for IN, the
// size received. After the last packet the TD is retired to the done queue.
// A TD whose frames have all gone by without it (the driver queued it late)
// is retired with DataOverrun; one whose first frame is still to come waits.
// An isochronous pipe has no handshake and no data toggle, and an error in
// one packet does not halt the endpoint.
int COhci::service_iso_ed(u32 ed_addr, u32 ed[4]) {
  const u32 flags = ed[0];
  const int ep = (flags >> 7) & 0xf;
  const bool in = ((flags >> 11) & 3) == 2;
  const int mps = (int)((flags >> 16) & 0x7ff);
  int done = 0;
  for (int guard = 0; guard < 16; ++guard) {
    const u32 head = ed[2] & ~0xfu, tail = ed[1] & ~0xfu;
    if ((ed[2] & 1) || head == tail)
      break;
    u32 td[8];
    do_pci_read(head, td, sizeof(u32), 8);
    const u32 next_td = td[2] & ~0xfu;
    const int fc = (td[0] >> 24) & 7;
    const int r = (int16_t)(u16)(m_frame - (td[0] & 0xffff));
    if (r < 0) // its first frame is still to come
      break;
    if (r > fc) { // all its frames are past: too late
      if (g_usbtrace)
        printf("USBT iso TD %08x late: frames %04x+%d, now %04x\n", head,
               td[0] & 0xffff, fc + 1, m_frame);
      retire_iso_td(head, td, 8); // DataOverrun
      ed[2] = next_td | (ed[2] & 2);
      do_pci_write(ed_addr + 8, &ed[2], sizeof(u32), 1);
      ++done;
      continue;
    }
    // Packet r: from its offset to the next one's, or to BufferEnd.
    const u32 bp0 = td[1] & ~0xfffu, be = td[3];
    auto offset = [&](int i) {
      return (td[4 + i / 2] >> (16 * (i & 1))) & 0xffff;
    };
    const u32 be13 =
        (be & 0xfff) | (((be ^ bp0) & ~0xfffu) ? 0x1000u : 0u); // BE's offset
    const u32 start = offset(r) & 0x1fff;
    const u32 end = r < fc ? (offset(r + 1) & 0x1fff) : be13 + 1;
    int len = end > start ? (int)(end - start) : 0;
    if (len > 1023)
      len = 1023;
    // Offset o as an address: page 0 or BufferEnd's page, by bit 12.
    auto copy = [&](u8 *data, int n, bool to_guest) {
      for (int at = 0; at < n;) {
        const u32 o = start + at;
        const u32 addr = ((o & 0x1000) ? (be & ~0xfffu) : bp0) | (o & 0xfff);
        const int k = std::min(n - at, (int)(0x1000 - (o & 0xfff)));
        if (to_guest)
          do_pci_write(addr, data + at, 1, k);
        else
          do_pci_read(addr, data + at, 1, k);
        at += k;
      }
    };
    u8 *buf = m_xfer.data();
    int cc = 0, size = 0;
    CUsbDevice *dev = device_at(flags & 0x7f);
    if (!dev) {
      cc = 5; // DeviceNotResponding
    } else if (in) {
      const int want = mps ? std::min(len, mps) : len;
      const int n = dev->iso_transfer(CUsbDevice::PID_IN, ep, buf, want);
      if (n < 0) {
        cc = 5;
      } else {
        size = std::min(n, len);
        if (size)
          copy(buf, size, true);
        cc = n > len ? 8 : n < len ? 9 : 0; // DataOverrun, DataUnderrun
      }
    } else {
      if (len)
        copy(buf, len, false);
      const int n = dev->iso_transfer(CUsbDevice::PID_OUT, ep, buf, len);
      cc = n < 0 ? 5 : 0;
    }
    // The packet status word replaces the offset.
    const u32 psw = ((u32)cc << 12) | (in ? (u32)size & 0x7ff : 0u);
    const int w = 4 + r / 2, sh = 16 * (r & 1);
    td[w] = (td[w] & ~(0xffffu << sh)) | (psw << sh);
    if (r == fc) {
      retire_iso_td(head, td, 0);
      ed[2] = next_td | (ed[2] & 2);
      do_pci_write(ed_addr + 8, &ed[2], sizeof(u32), 1);
    } else {
      do_pci_write(head + 4 * w, &td[w], sizeof(u32), 1);
    }
    ++done;
    break; // one packet a frame
  }
  return done;
}

// An isochronous TD onto the done queue: the whole TD goes back, with its
// packet status words; its FrameCount stays where a general TD's error
// count would be cleared.
void COhci::retire_iso_td(u32 td_addr, u32 td[8], int cc) {
  td[0] = (td[0] & 0x0fffffff) | ((u32)cc << 28);
  td[2] = m_done_head;
  do_pci_write(td_addr, td, sizeof(u32), 8);
  m_done_head = td_addr;
  const int di = (td[0] >> 21) & 7;
  if (di < m_done_delay)
    m_done_delay = di;
}

// A TD onto the done queue, with its condition code; the queue's interrupt
// delay is the smallest any of its TDs asks for.
void COhci::retire_td(u32 td_addr, u32 td[4], int cc) {
  td[0] = (td[0] & 0x0fffffff & ~(3u << 26)) | ((u32)cc << 28);
  td[2] = m_done_head;
  do_pci_write(td_addr, td, sizeof(u32), 4);
  m_done_head = td_addr;
  const int di = (td[0] >> 21) & 7;
  if (di < m_done_delay)
    m_done_delay = di;
}

// HccaDoneHead, when the previous one has been consumed (WDH clear); bit 0
// says other interrupt status is pending too.
void COhci::write_back_done() {
  if (state.usb_data[0x0c / 4] & OHCI_INT_WDH)
    return;
  const u32 hcca = state.usb_data[0x18 / 4];
  if (!hcca)
    return;
  const u32 other = state.usb_data[0x0c / 4] & state.usb_data[0x10 / 4] &
                    ~OHCI_INT_WDH & 0x7f;
  u32 dh = m_done_head | (other ? 1u : 0u);
  do_pci_write(hcca + 0x84, &dh, sizeof(u32), 1);
  m_done_head = 0;
  m_done_delay = 7;
  ohci_status(OHCI_INT_WDH);
}

void COhci::ohci_status(u32 bits) {
  state.usb_data[0x0c / 4] |= bits;
  ohci_update_irq();
}

// Root hub ports (OHCI 1.0a 7.4.4). CCS follows attachment and power; a
// reset completes at once.
void COhci::port_refresh(int p) {
  u32 &r = port_reg(p);
  const bool powered = (r & RH_PPS) != 0;
  const bool present = port_dev(p) && powered && !m_port[p]->faults.unplugged;
  const bool was = (r & RH_CCS) != 0;
  r &= ~(RH_CCS | RH_LSDA);
  if (present) {
    r |= RH_CCS;
    if (port_dev(p)->low_speed())
      r |= RH_LSDA;
  } else {
    r &= ~(RH_PES | RH_PSS);
  }
  if (was != present)
    port_change(p, RH_CSC);
}

void COhci::port_change(int p, u32 bits) {
  port_reg(p) |= bits;
  ohci_status(OHCI_INT_RHSC);
}

void COhci::port_write(int p, u32 data) {
  u32 &r = port_reg(p);
  // Change bits: write 1 to clear.
  r &= ~(data & (RH_CSC | RH_PESC | RH_PSSC | (1u << 19) | RH_PRSC));
  if (data & (1u << 0)) // ClearPortEnable
    r &= ~RH_PES;
  if (data & (1u << 1)) { // SetPortEnable
    if (r & RH_CCS)
      r |= RH_PES;
    else
      port_change(p, RH_CSC);
  }
  if (data & (1u << 2)) { // SetPortSuspend
    if (r & RH_CCS)
      r |= RH_PSS;
    else
      port_change(p, RH_CSC);
  }
  if ((data & (1u << 3)) && (r & RH_PSS)) { // ClearSuspendStatus: resumed
    r &= ~RH_PSS;
    port_change(p, RH_PSSC);
  }
  if (data & (1u << 4)) { // SetPortReset
    if (r & RH_CCS) {
      port_dev(p)->reset();
      port_dev(p)->set_high_speed(false); // an OHCI port is full speed
      r |= RH_PES;
      port_change(p, RH_PRSC);
    } else {
      port_change(p, RH_CSC);
    }
  }
  if (data & (1u << 8)) // SetPortPower
    r |= RH_PPS;
  if (data & (1u << 9)) // ClearPortPower
    r &= ~(RH_PPS | RH_PES | RH_PSS);
  port_refresh(p);
}

u64 COhci::usb_hci_read(u64 address, int dsize) {
  std::lock_guard<std::mutex> lk(m_mx);
  u64 data = 0;
  if (g_usbtrace && address < 0x110) {
    g_usb_reads[address / 4]++;
    if (++g_usb_reads_total % 20000 == 0)
      usbtrace_line("R", address, state.usb_data[address / 4]);
  }
  if (dsize != 32)
    printf("%%USB-W-HCIREAD: Non dword read, returning 32 bits anyway.\n");
  switch (address) {
  case 0: // HcRevision: 1.0, with legacy support where the part has it
    data = m_legacy ? 0x00000110 : 0x00000010;
    break;

  case 0x14: // HcInterruptDisable reads back the enable mask
    data = state.usb_data[0x10 / 4];
    break;

  case 0x30: // HcDoneHead
    data = m_done_head;
    break;

  case 0x38: // HcFrameRemaining: the frame is always about to begin
    data = state.usb_data[0x34 / 4] & 0x3fff;
    break;

  case 0x3c: // HcFmNumber
    data = m_frame;
    break;

  case 4:     // HcControl
  case 8:     // HcCommandStatus
  case 0x0c:  // HcInterruptStatus
  case 0x10:  // HcInterruptEnable
  case 0x18:  // HcHCCA
  case 0x1c:  // HcPeriodCurrentED
  case 0x20:  // HcControlHeadED
  case 0x24:  // HcControlCurrentED
  case 0x28:  // HcBulkHeadED
  case 0x2c:  // HcBulkCurrentED
  case 0x34:  // HcFmInterval
  case 0x40:  // HcPeriodicStart
  case 0x44:  // HcLSThreshold
  case 0x48:  // HcRhDescriptorA
  case 0x4c:  // HcRhDescriptorB
  case 0x50:  // HcRhStatus
  case 0x54:  // HcRhPortStatus1
  case 0x58:  // HcRhPortStatus2
  case 0x5c:  // HcRhPortStatus3
    data = state.usb_data[address / 4];
    break;

  case 0x100: // HceControlRegister
  case 0x104: // HceInputRegister
  case 0x108: // HceOutputRegister
  case 0x10c: // HceStatusRegister
    data = m_legacy ? state.usb_data[address / 4] : 0;
    break;

  default:
    printf("%%USB-W-HCIREAD: Reading from unknown address %x.  Ignoring.\n",
           (int)address);
  }
  return data;
}

// HcControl HCFS == UsbOperational (OHCI 1.0a 7.1.2).
bool COhci::ohci_operational() const {
  return ((state.usb_data[4 / 4] >> 6) & 3) == 2;
}

// A level: an enabled status bit with MasterInterruptEnable set. Where it
// goes is the host function's business.
void COhci::ohci_update_irq() {
  const u32 enable = state.usb_data[0x10 / 4];
  const bool level = (enable & OHCI_INT_MIE) &&
                     (state.usb_data[0x0c / 4] & enable & 0x4000007f);
  m_host.ohci_irq(level);
}

void COhci::usb_hci_write(u64 address, int dsize, u64 data) {
  std::lock_guard<std::mutex> lk(m_mx);
  if (g_usbtrace)
    usbtrace_line("W", address, data);
  if (dsize != 32)
    printf("%%USB-W-HCIWRITE: Non dword write, writing 32 bits anyway.\n");
  switch (address) {
  case 4: { // HcControl
    const u32 was = (state.usb_data[4 / 4] >> 6) & 3;
    state.usb_data[address / 4] = (u32)data & 0x7ff;
    const u32 now = (state.usb_data[4 / 4] >> 6) & 3;
    if (now == 0 && was != 0) { // UsbReset: the bus resets, ports disable
      for (int p = 0; p < m_ports; ++p)
        port_reg(p) &= ~(RH_PES | RH_PSS);
    }
    break;
  }

  case 8: // HcCommandStatus: write 1 to set
    if (data & OHCI_CMD_HCR) {
      // Software reset: registers to their defaults (IR and RWC survive),
      // functional state UsbSuspend, and HCR clears when the reset is done --
      // immediately here. The root hub is not reset.
      const u32 keep = state.usb_data[4 / 4] & 0x300;
      for (u32 off = 0x08; off <= 0x44; off += 4)
        state.usb_data[off / 4] = 0;
      state.usb_data[0x34 / 4] = 0x2edf;
      state.usb_data[0x44 / 4] = 0x0628;
      state.usb_data[4 / 4] = keep | 0xc0;
      m_frame = 0;
      m_done_head = 0;
      m_done_delay = 7;
    }
    if (data & OHCI_CMD_OCR) {
      // Ownership change: no SMM firmware owns the controller, so the handoff
      // completes at once -- InterruptRouting clears and OC is reported.
      state.usb_data[4 / 4] &= ~(u32)0x100;
      state.usb_data[0x0c / 4] |= OHCI_INT_OC;
    }
    state.usb_data[8 / 4] |= (u32)data & 0x06; // CLF/BLF; HCR/OCR self-clear
    // A real controller walks the control and bulk lists for the rest of
    // the frame, so a TD the driver has just queued runs within it, not at
    // the next frame: wake the frame thread (not here -- a device may do
    // disk I/O, and this is a processor's thread).
    if (data & 0x06)
      kick();
    break;

  case 0x0c: // HcInterruptStatus: write 1 to clear
    state.usb_data[address / 4] &= ~(u32)data;
    // A consumed done head lets the next one go out.
    if ((data & OHCI_INT_WDH) && m_done_head && m_done_delay == 0)
      write_back_done();
    break;

  case 0x10: // HcInterruptEnable: write 1 to set
    state.usb_data[0x10 / 4] |= (u32)data & 0xc000007f;
    break;

  case 0x14: // HcInterruptDisable: write 1 to clear the enable bit
    state.usb_data[0x10 / 4] &= ~((u32)data & 0xc000007f);
    break;

  case 0x18: // HcHCCA: the controller needs a 256-byte-aligned block
    state.usb_data[address / 4] = (u32)data & 0xffffff00;
    break;

  case 0x1c: // HcPeriodCurrentED
  case 0x20: // HcControlHeadED
  case 0x24: // HcControlCurrentED
  case 0x28: // HcBulkHeadED
  case 0x2c: // HcBulkCurrentED
    state.usb_data[address / 4] = (u32)data & ~(u32)0xf;
    break;

  case 0x30: // HcDoneHead, HcFrameRemaining, HcFmNumber: read-only
  case 0x38:
  case 0x3c:
    break;

  case 0x50: // HcRhStatus
    // SetGlobalPower (LPSC)
    if (data & (1u << 16))
      for (int p = 0; p < m_ports; ++p) {
        port_reg(p) |= RH_PPS;
        port_refresh(p);
      }
    if (data & 1u) // ClearGlobalPower (LPS)
      for (int p = 0; p < m_ports; ++p) {
        port_reg(p) &= ~(RH_PPS | RH_PES | RH_PSS);
        port_refresh(p);
      }
    if (data & (1u << 15)) // SetRemoteWakeupEnable
      state.usb_data[address / 4] |= 0x8000;
    if (data & (1u << 31)) // ClearRemoteWakeupEnable
      state.usb_data[address / 4] &= ~(u32)0x8000;
    break;

  case 0x54: // HcRhPortStatus1..3
  case 0x58:
  case 0x5c:
    if ((int)(address - 0x54) / 4 < m_ports)
      port_write((int)(address - 0x54) / 4, (u32)data);
    break;

  case 0x34:  // HcFmInterval
  case 0x40:  // HcPeriodicStart
  case 0x44:  // HcLSThreshold
  case 0x48:  // HcRhDescriptorA
  case 0x4c:  // HcRhDescriptorB
  case 0x100: // HceControlRegister
  case 0x104: // HceInputRegister
  case 0x108: // HceOutputRegister
  case 0x10c: // HceStatusRegister
    state.usb_data[address / 4] = (u32)data;
    break;

  default:
    printf("%%USB-W-HCIWRITE: Writing to unknown address %x.  Ignoring.\n",
           (int)address);
  }
  ohci_update_irq();
}

static u32 usb_magic1 = 0x9000432B;
static u32 usb_magic2 = 0xB2340009;
static u32 usb_magic_run = 0x9000432C; // the runtime and devices' block

/**
 * Save state to a Virtual Machine State file.
 **/
int CAliM1543C_usb::SaveState(FILE *f) {
  COhci::SUSB_state &state = m_ohci.state;
  long ss = sizeof(state);
  int res;

  if ((res = CPCIDevice::SaveState(f)))
    return res;

  fwrite(&usb_magic1, sizeof(u32), 1, f);
  fwrite(&ss, sizeof(long), 1, f);
  fwrite(&state, sizeof(state), 1, f);
  fwrite(&usb_magic2, sizeof(u32), 1, f);
  // The schedule's runtime state and the devices', in a block of its own
  // (a state saved before it existed restores as before).
  CUsbSaved s;
  m_ohci.save_runtime(s);
  usb_save_ports(s, m_port, kPorts);
  usb_save_block(f, usb_magic_run, s);
  printf("%s: %d bytes saved.\n", devid_string, (int)(ss + s.b.size()));
  return 0;
}

/**
 * Restore state from a Virtual Machine State file.
 **/
int CAliM1543C_usb::RestoreState(FILE *f) {
  COhci::SUSB_state &state = m_ohci.state;
  long ss;
  u32 m1;
  u32 m2;
  int res;
  size_t r;

  if ((res = CPCIDevice::RestoreState(f)))
    return res;

  r = fread(&m1, sizeof(u32), 1, f);
  if (r != 1) {
    printf("%s: unexpected end of file!\n", devid_string);
    return -1;
  }

  if (m1 != usb_magic1) {
    printf("%s: MAGIC 1 does not match!\n", devid_string);
    return -1;
  }

  r = fread(&ss, sizeof(long), 1, f);
  if (r != 1) {
    printf("%s: unexpected end of file!\n", devid_string);
    return -1;
  }

  if (ss != sizeof(state)) {
    printf("%s: STRUCT SIZE does not match!\n", devid_string);
    return -1;
  }

  r = fread(&state, sizeof(state), 1, f);
  if (r != 1) {
    printf("%s: unexpected end of file!\n", devid_string);
    return -1;
  }

  r = fread(&m2, sizeof(u32), 1, f);
  if (r != 1) {
    printf("%s: unexpected end of file!\n", devid_string);
    return -1;
  }

  if (m2 != usb_magic2) {
    printf("%s: MAGIC 2 does not match!\n", devid_string);
    return -1;
  }

  // The frame number, the done queue and the devices, as the guest left
  // them. A device whose state was not saved (one behind libusb, or a
  // state file from before) is reset and shown to the guest as reconnected,
  // so its driver enumerates it again.
  CUsbSaved s;
  bool reconnect[kPorts];
  for (int p = 0; p < kPorts; ++p)
    reconnect[p] = m_port[p].dev != nullptr;
  if (usb_load_block(f, usb_magic_run, s)) {
    m_ohci.load_runtime(s);
    usb_load_ports(s, m_port, kPorts, reconnect);
  }
  for (int p = 0; p < kPorts; ++p) {
    if (!reconnect[p])
      continue;
    printf("%s: the device on port %d is shown as reconnected.\n", devid_string,
           p + 1);
    m_port[p].dev->reset();
    m_ohci.reconnect(p);
  }
  m_ohci.refresh_irq();

  printf("%s: %d bytes restored.\n", devid_string, (int)(ss + s.b.size()));
  return 0;
}
