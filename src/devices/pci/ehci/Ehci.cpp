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
#include "UsbHostDevice.hpp"
#include "UsbStorage.hpp"
#include "UsbTablet.hpp"
#include <algorithm>
#include <chrono>
#include <cstring>

// ALPHABOX_USBTRACE=1: register writes and schedule events.
static const bool g_trace = getenv("ALPHABOX_USBTRACE") != nullptr;

// PCI configuration space: the EHCI function of a NEC uPD720101.
static void ehci_config_space(u32 *data, u32 *mask) {
  data[0x00 >> 2] = 0x00e01033; // CFID: NEC, uPD720101 EHCI function
  data[0x04 >> 2] = 0x02100000; // CFCS: DEVSEL medium, capabilities list
  data[0x08 >> 2] = 0x0c032004; // CFRV: serial bus / USB / EHCI, rev. 4
  data[0x10 >> 2] = 0x00000000; // BAR0: registers, 256 bytes of memory
  data[0x2c >> 2] = 0x00e01033; // CSID: subsystem = the part itself
  data[0x34 >> 2] = 0x00000040; // CCAP: power management at 0x40
  data[0x3c >> 2] = 0x000001ff; // CFIT: INTA, no line yet
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

// Capability registers.
static const u32 kCapLength = 0x20;
static const u32 kHciVersion = 0x0100;
static const u32 kHcsParams = CEhci::kPorts | (1u << 4); // PPC, no companions
static const u32 kHccParams = 0x00000012; // programmable list; IST 1 frame

CEhci::CEhci(CConfigurator *cfg, CSystem *c, int pcibus, int pcidev)
    : CPCIDevice(cfg, c, pcibus, pcidev), CDiskController(kPorts + 1, 1) {
  static u32 cfg_data[64], cfg_mask[64];
  ehci_config_space(cfg_data, cfg_mask);
  add_function(0, cfg_data, cfg_mask);
  ResetPCI();
  m_xfer.resize(0x5000);
  reset_controller();
  for (int p = 0; p < kPorts; ++p) {
    char key[8];
    snprintf(key, sizeof(key), "port%d", p + 1);
    const char *what = myCfg->get_text_value(key, "");
    if (!strcmp(what, "tablet")) {
      auto t = std::make_unique<CUsbTablet>();
      theUsbTablet.store(t.get());
      attach(p, std::move(t));
      printf("%s: USB tablet on port %d.\n", devid_string, p + 1);
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
                "%s: unknown USB device \"%s\" (EHCI ports "
                "take tablet or host:vvvv:pppp; disks are disk<port>.0)",
                key, what);
    }
  }
  printf("%s: EHCI USB 2.0 controller, %d ports.\n", devid_string, kPorts);
  theUsbFaultTarget.store(this); // tests address the USB 2.0 card first
}

CEhci::~CEhci() {
  stop_threads();
  for (auto &d : m_dev)
    if (d && theUsbTablet.load() == d.get())
      theUsbTablet.store(nullptr);
  CUsbFaultTarget *me = this;
  theUsbFaultTarget.compare_exchange_strong(me, nullptr);
}

// Test faults (CUsbPortFaults, docs/headless.md): from the GUI thread.
bool CEhci::inject_fault(const char *op, int port, int ep, int arg) {
  std::lock_guard<std::mutex> lk(m_mx);
  if (port < 1 || port > kPorts || !m_dev[port - 1])
    return false;
  const int p = port - 1;
  CUsbPortFaults &f = m_faults[p];
  if (!strcmp(op, "detach") || !strcmp(op, "attach")) {
    f.unplugged = !strcmp(op, "detach");
    if (f.unplugged)
      m_dev[p]->reset(); // what is left of it when it comes back
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
    return m_dev[p]->inject_phase_error();
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
  for (auto &d : m_dev)
    if (d)
      d->reset();
  reset_controller();
}

void CEhci::register_disk(class CDisk *dsk, int bus, int dev) {
  if (bus < 1 || bus > kPorts || dev != 0)
    FAILURE(Configuration, "EHCI disks are named disk<port>.0, port 1 to 4");
  if (m_dev[bus - 1])
    FAILURE_1(Configuration, "EHCI port %d already has a device", bus);
  CDiskController::register_disk(dsk, bus, dev);
  std::unique_ptr<CUsbDevice> d =
      std::make_unique<CUsbStorage>(new CSCSIBus(myCfg, cSystem), dsk);
  if (const char *e = getenv("ALPHABOX_USB_ASYNC_US"))
    d = std::make_unique<CUsbAsyncShim>(std::move(d), atoi(e));
  attach(bus - 1, std::move(d));
  printf("%s: USB storage on port %d.\n", devid_string, bus);
}

void CEhci::attach(int p, std::unique_ptr<CUsbDevice> dev) {
  dev->on_complete = [this]() { kick(); };
  m_dev[p] = std::move(dev);
}

void CEhci::kick() {
  {
    std::lock_guard<std::mutex> lk(m_kick_mx);
    m_kicked = true;
  }
  m_kick_cv.notify_one();
}

void CEhci::start_threads() {
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
}

void CEhci::check_state() {
  if (myThreadDead.load())
    FAILURE(Thread, "EHCI thread has died");
}

// Power-on and HCRESET state: halted, schedules off, ports unpowered and
// owned by the (absent) companion until CONFIGFLAG is set.
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
    state.portsc[p] = PS_OWNER;
    port_refresh(p);
  }
  update_irq();
}

void CEhci::update_irq() {
  do_pci_interrupt(0, (state.usbsts & state.usbintr & 0x3f) != 0);
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
  const bool present =
      m_dev[p] && !m_faults[p].unplugged && (r & PS_PP) && !(r & PS_OWNER);
  const bool was = (r & PS_CCS) != 0;
  r &= ~(PS_CCS | (3u << 10));
  if (present) {
    r |= PS_CCS;
    if (!(r & PS_PED)) // line state: K for low speed (hand it off), else J
      r |= m_dev[p]->low_speed() ? (1u << 10) : (2u << 10);
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
  r = (r & ~(PS_PP | PS_OWNER | (7u << 20))) |
      (data & (PS_PP | PS_OWNER | (7u << 20)));
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
  // companion controller (there is none: it stays disabled).
  if (data & PS_PR) {
    if (!(r & PS_PR)) {
      r |= PS_PR;
      r &= ~(PS_PED | PS_SUSP);
    }
  } else if (r & PS_PR) {
    r &= ~PS_PR;
    port_refresh(p);
    if ((r & PS_CCS) && m_dev[p]) {
      m_dev[p]->reset();
      const bool hs = m_dev[p]->can_high_speed();
      m_dev[p]->set_high_speed(hs);
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
    return kHcsParams;
  case 0x08:
    return kHccParams;
  case 0x0c:
    return 0; // HCSP-PORTROUTE
  case 0x20:
    return state.usbcmd;
  case 0x24:
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
  case 0x60: { // CONFIGFLAG: 1 routes every port to this controller
    const u32 cf = data & 1;
    if (cf != state.configflag)
      for (int p = 0; p < kPorts; ++p) {
        state.portsc[p] =
            cf ? (state.portsc[p] & ~PS_OWNER) : (state.portsc[p] | PS_OWNER);
        port_refresh(p);
      }
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
        const int fls = (state.usbcmd >> 2) & 3;
        const u32 list_bit = 1u << (13 - fls); // 1024: bit 13; 512: 12; ...
        const u32 before = state.frindex;
        state.frindex = (state.frindex + 8 * frames) & 0x3fff;
        if ((before ^ state.frindex) & list_bit)
          status(STS_FLR);
        frame();
      }
      // Data moved in this pass (an emulated device finishes every qTD in
      // the pass that finds it, so Reclamation alone would miss it).
      if (async_pass())
        last_work = now;
      busy = (state.usbcmd & CMD_ASE) &&
             now - last_work < std::chrono::milliseconds(50);
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

// The periodic list's entry for this frame: interrupt QHs are serviced
// (one qTD each); isochronous iTDs/siTDs and FSTNs are passed over.
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
    if (m_dev[p] && (state.portsc[p] & PS_PED) && !m_faults[p].unplugged &&
        m_dev[p]->address() == address)
      return m_dev[p].get();
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

static u32 ehci_magic1 = 0xE4C11001;
static u32 ehci_magic2 = 0x1001E4C1;

int CEhci::SaveState(FILE *f) {
  long ss = sizeof(state);
  int res;
  if ((res = CPCIDevice::SaveState(f)))
    return res;
  fwrite(&ehci_magic1, sizeof(u32), 1, f);
  fwrite(&ss, sizeof(long), 1, f);
  fwrite(&state, sizeof(state), 1, f);
  fwrite(&ehci_magic2, sizeof(u32), 1, f);
  printf("%s: %d bytes saved.\n", devid_string, (int)ss);
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
  // The devices' own state is not saved: reset them and show them to the
  // guest as reconnected.
  for (int p = 0; p < kPorts; ++p) {
    if (!m_dev[p])
      continue;
    m_dev[p]->reset();
    state.portsc[p] &= ~(PS_PED | PS_CCS);
    port_refresh(p);
  }
  printf("%s: %d bytes restored.\n", devid_string, (int)ss);
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

  reg(0x20, 0x00080000); // stop, and leave the controller to the guest
  reg(0x20, CMD_HCRESET);
  printf("%%EHCI-I-SELFTEST: %s\n", pass ? "PASS" : "FAIL");
}
