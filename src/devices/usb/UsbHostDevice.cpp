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

#include "StdAfx.hpp"

#if defined(HAVE_LIBUSB)

#include "UsbHostDevice.hpp"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <libusb.h>
#include <thread>

static const bool g_usbtrace = getenv("ALPHABOX_USBTRACE") != nullptr;

// One libusb context for the process, and the thread that runs its events
// (every completion callback runs there). Opened with the first passed-
// through device and closed with the last.
static std::mutex g_ctx_mx;
static libusb_context *g_ctx = nullptr;
static int g_ctx_refs = 0;
static std::thread g_events;
static std::atomic_bool g_events_stop{false};

static libusb_context *ctx_acquire() {
  std::lock_guard<std::mutex> lk(g_ctx_mx);
  if (g_ctx_refs++ == 0) {
    const int r = libusb_init(&g_ctx);
    if (r != 0) {
      g_ctx_refs = 0;
      FAILURE_1(Runtime, "libusb_init failed: %s", libusb_error_name(r));
    }
    g_events_stop = false;
    g_events = std::thread([]() {
      while (!g_events_stop) {
        timeval tv = {0, 100000};
        libusb_handle_events_timeout_completed(g_ctx, &tv, nullptr);
      }
    });
  }
  return g_ctx;
}

static void ctx_release() {
  std::lock_guard<std::mutex> lk(g_ctx_mx);
  if (--g_ctx_refs == 0) {
    g_events_stop = true;
    libusb_interrupt_event_handler(g_ctx);
    g_events.join();
    libusb_exit(g_ctx);
    g_ctx = nullptr;
  }
}

/// What a submitted transfer carries back to its completion: whose it is,
/// which slot and which generation of it, and the buffer.
struct HostXfer {
  CUsbHostDevice *dev;
  int slot;
  unsigned gen;
  std::vector<u8> buf;
};

CUsbHostDevice::CUsbHostDevice(const char *spec) {
  unsigned vid = 0, pid = 0;
  if (sscanf(spec, "%x:%x", &vid, &pid) != 2)
    FAILURE_1(Configuration,
              "USB host device \"%s\": expected vvvv:pppp (hex IDs)", spec);
  char nm[32];
  snprintf(nm, sizeof(nm), "host %04x:%04x", vid, pid);
  m_name = nm;
  ctx_acquire();
  // The first device with those IDs; the error, when opening it fails, is
  // what libusb says (libusb_open_device_with_vid_pid keeps it to itself).
  const char *why = "not found";
  libusb_device **list = nullptr;
  const ssize_t n = libusb_get_device_list(g_ctx, &list);
  for (ssize_t i = 0; i < n && !m_h; ++i) {
    libusb_device_descriptor dd;
    if (libusb_get_device_descriptor(list[i], &dd) != 0 || dd.idVendor != vid ||
        dd.idProduct != pid)
      continue;
    const int r = libusb_open(list[i], &m_h);
    if (r != 0) {
      why = libusb_error_name(r);
      m_h = nullptr;
    }
  }
  if (list)
    libusb_free_device_list(list, 1);
  if (!m_h) {
    ctx_release();
    // On macOS "not found" can also mean the host will not hand the device
    // to a user program: some classes need an entitlement, or an unlocked
    // Mac (ioreg shows UsbUserClientEntitlementRequired on the device).
    FAILURE_3(Configuration,
              "USB host device %04x:%04x: %s (or the host keeps it from user "
              "programs)",
              vid, pid, why);
  }
  // Take interfaces from host drivers where the host allows it (Linux; on
  // macOS only as root). Where it does not, the claim fails later, per
  // interface, with a warning.
  libusb_set_auto_detach_kernel_driver(m_h, 1);
  const int speed = libusb_get_device_speed(libusb_get_device(m_h));
  m_low_speed = speed == LIBUSB_SPEED_LOW;
  m_high_speed = speed >= LIBUSB_SPEED_HIGH;
  std::fill(std::begin(m_ep_type), std::end(m_ep_type), -1);
  const char *how = m_high_speed ? "high or super" : "full";
  printf("%%USB-I-HOST: %s opened (%s speed).\n", m_name.c_str(),
         m_low_speed ? "low" : how);
}

CUsbHostDevice::~CUsbHostDevice() {
  {
    std::unique_lock<std::mutex> lk(m_mx);
    for (int s = 0; s <= kControl; ++s)
      if (m_slot[s].xfer)
        libusb_cancel_transfer(m_slot[s].xfer);
    for (int s = 0; s < 32; ++s)
      iso_stop(s);
    m_idle.wait(lk, [this]() { return m_in_flight == 0; });
  }
  for (int i : m_claimed)
    libusb_release_interface(m_h, i);
  libusb_close(m_h);
  ctx_release();
}

const std::vector<u8> &CUsbHostDevice::device_descriptor() const {
  static const std::vector<u8> none;
  return none;
}

const std::vector<u8> &CUsbHostDevice::configuration_descriptor() const {
  return device_descriptor();
}

void CUsbHostDevice::completed(libusb_transfer *t) {
  HostXfer *x = (HostXfer *)t->user_data;
  CUsbHostDevice *d = x->dev;
  {
    std::lock_guard<std::mutex> lk(d->m_mx);
    Slot &s = d->m_slot[x->slot];
    if (s.gen == x->gen && s.xfer == t) {
      s.xfer = nullptr;
      s.done = true;
      s.status = t->status;
      const bool control = x->slot == kControl;
      const u8 *data =
          control ? libusb_control_transfer_get_data(t) : t->buffer;
      s.data.assign(data, data + t->actual_length);
      if (g_usbtrace)
        printf("USBT %s slot %d done status %d len %d\n", d->m_name.c_str(),
               x->slot, t->status, t->actual_length);
    }
    // The controller's wake-up takes only its own small lock, never one a
    // thread calling into this device holds; and it must run before the
    // count drops, after which the destructor may free the device.
    if (d->on_complete)
      d->on_complete();
    --d->m_in_flight;
  }
  d->m_idle.notify_all();
  delete x;
  libusb_free_transfer(t);
}

/// What an isochronous transfer carries back to its completion.
struct IsoXfer {
  CUsbHostDevice *dev;
  int slot;
  unsigned gen;
  std::vector<u8> buf;
};

// An isochronous transfer ended. IN: its packets go to the stream's ready
// queue (a packet that failed is left out -- the guest's read then finds
// nothing, as with a packet that never came), and the transfer goes out
// again while the stream is on. OUT: nothing to hand back.
void CUsbHostDevice::iso_completed(libusb_transfer *t) {
  IsoXfer *x = (IsoXfer *)t->user_data;
  CUsbHostDevice *d = x->dev;
  bool again = false;
  {
    std::lock_guard<std::mutex> lk(d->m_mx);
    IsoStream &s = d->m_iso[x->slot];
    const bool in = (t->endpoint & 0x80) != 0;
    const bool live = s.gen == x->gen;
    if (live && in && t->status == LIBUSB_TRANSFER_COMPLETED) {
      for (int i = 0; i < t->num_iso_packets; ++i) {
        const libusb_iso_packet_descriptor &p = t->iso_packet_desc[i];
        if (p.status != LIBUSB_TRANSFER_COMPLETED)
          continue;
        const u8 *b = libusb_get_iso_packet_buffer_simple(t, (unsigned)i);
        s.ready.emplace_back(b, b + p.actual_length);
        if (s.ready.size() > kIsoReady)
          s.ready.pop_front(); // the guest fell behind: the oldest goes
      }
    }
    again = live && in && s.on &&
            std::chrono::steady_clock::now() - s.last_ask <
                std::chrono::milliseconds(100) &&
            libusb_submit_transfer(t) == 0;
    if (!again) {
      auto &v = s.xfers;
      v.erase(std::remove(v.begin(), v.end(), t), v.end());
      if (live && in && v.empty())
        s.on = false;
      if (d->on_complete)
        d->on_complete();
      --d->m_in_flight;
    }
  }
  if (!again) {
    d->m_idle.notify_all();
    delete x;
    libusb_free_transfer(t);
  }
}

// Called with m_mx held: one isochronous transfer of `packets` packets of
// `len` bytes (IN), or of one packet holding `out` (OUT).
bool CUsbHostDevice::iso_submit(int slot, u8 ep_addr, const u8 *out, int len,
                                int packets) {
  libusb_transfer *t = libusb_alloc_transfer(packets);
  if (!t)
    return false;
  IsoXfer *x = new IsoXfer{this, slot, m_iso[slot].gen, {}};
  if (ep_addr & 0x80)
    x->buf.assign((size_t)len * packets, 0);
  else
    x->buf.assign(out, out + len);
  libusb_fill_iso_transfer(t, m_h, ep_addr, x->buf.data(), (int)x->buf.size(),
                           packets, iso_completed, x, 0);
  libusb_set_iso_packet_lengths(t, (unsigned)len);
  const int r = libusb_submit_transfer(t);
  if (r != 0) {
    if (g_usbtrace)
      printf("USBT %s iso slot %d submit failed: %s\n", m_name.c_str(), slot,
             libusb_error_name(r));
    delete x;
    libusb_free_transfer(t);
    return false;
  }
  m_iso[slot].xfers.push_back(t);
  ++m_in_flight;
  return true;
}

// Called with m_mx held: the stream ends; transfers in flight are
// cancelled, and their completions dropped.
void CUsbHostDevice::iso_stop(int slot) {
  IsoStream &s = m_iso[slot];
  ++s.gen;
  s.on = false;
  s.packet = 0;
  s.ready.clear();
  for (libusb_transfer *t : s.xfers)
    libusb_cancel_transfer(t);
}

bool CUsbHostDevice::iso_endpoint(int pid, int ep) const {
  if (pid == PID_SETUP || ep < 1 || ep > 15)
    return false;
  return m_ep_type[ep | (pid == PID_IN ? 16 : 0)] ==
         LIBUSB_TRANSFER_TYPE_ISOCHRONOUS;
}

int CUsbHostDevice::iso_transfer(int pid, int ep, u8 *buf, int len) {
  if (!iso_endpoint(pid, ep))
    return ISO_NO_ENDPOINT;
  const bool in = pid == PID_IN;
  const int slot = ep | (in ? 16 : 0);
  const u8 addr = (u8)(ep | (in ? 0x80 : 0));
  std::lock_guard<std::mutex> lk(m_mx);
  IsoStream &s = m_iso[slot];
  if (!in) {
    if ((int)s.xfers.size() >= kIsoOutMax ||
        !iso_submit(slot, addr, buf, len, 1))
      return ISO_OVERRUN; // the packet cannot go out in time
    return len;
  }
  s.last_ask = std::chrono::steady_clock::now();
  if (!s.on) { // the first read starts the stream
    if (!s.packet)
      s.packet = libusb_get_max_iso_packet_size(libusb_get_device(m_h), addr);
    if (s.packet <= 0) {
      s.packet = 0;
      return ISO_NO_ENDPOINT;
    }
    s.on = true;
    for (int i = 0; i < kIsoInXfers; ++i)
      if (!iso_submit(slot, addr, nullptr, s.packet, kIsoPackets))
        break;
    if (s.xfers.empty())
      s.on = false;
  }
  if (s.ready.empty())
    return ISO_OVERRUN; // nothing arrived in time
  const std::vector<u8> d = std::move(s.ready.front());
  s.ready.pop_front();
  memcpy(buf, d.data(), std::min((int)d.size(), len));
  return (int)d.size();
}

// Called with m_mx held. `out` is the OUT data (for control: the 8-byte
// setup followed by the data stage); an IN transfer asks for `len` bytes.
bool CUsbHostDevice::submit(int slot, u8 ep_addr, int type, const u8 *out,
                            int len) {
  libusb_transfer *t = libusb_alloc_transfer(0);
  if (!t)
    return false;
  HostXfer *x = new HostXfer{this, slot, m_slot[slot].gen, {}};
  const bool in = (ep_addr & 0x80) != 0;
  if (type == LIBUSB_TRANSFER_TYPE_CONTROL) {
    const int wlength = out[6] | (out[7] << 8);
    x->buf.assign(out, out + 8 + (in ? 0 : wlength));
    x->buf.resize(8 + wlength);
    libusb_fill_control_transfer(t, m_h, x->buf.data(), completed, x, 5000);
  } else {
    if (in)
      x->buf.assign(len, 0);
    else
      x->buf.assign(out, out + len);
    if (type == LIBUSB_TRANSFER_TYPE_INTERRUPT)
      libusb_fill_interrupt_transfer(t, m_h, ep_addr, x->buf.data(), len,
                                     completed, x, 0);
    else
      libusb_fill_bulk_transfer(t, m_h, ep_addr, x->buf.data(), len, completed,
                                x, 0);
  }
  const int r = libusb_submit_transfer(t);
  if (r != 0) {
    if (g_usbtrace)
      printf("USBT %s slot %d submit failed: %s\n", m_name.c_str(), slot,
             libusb_error_name(r));
    delete x;
    libusb_free_transfer(t);
    return false;
  }
  m_slot[slot].xfer = t;
  m_slot[slot].done = false;
  ++m_in_flight;
  return true;
}

// Called with m_mx held: forget the slot's transfer, whether in flight or
// finished and not yet taken.
void CUsbHostDevice::cancel(int slot) {
  Slot &s = m_slot[slot];
  if (s.xfer)
    libusb_cancel_transfer(s.xfer);
  s.xfer = nullptr;
  s.done = false;
  ++s.gen;
}

// Called with m_mx held, on a finished slot: its data (from `skip`) and how
// the transfer ended.
CUsbDevice::Result CUsbHostDevice::take(int slot, u8 *buf, int &len,
                                        size_t skip) {
  Slot &s = m_slot[slot];
  if (s.status != LIBUSB_TRANSFER_COMPLETED) {
    len = 0;
    return USB_STALL;
  }
  const size_t n =
      skip < s.data.size() ? std::min((size_t)len, s.data.size() - skip) : 0;
  if (buf && n)
    memcpy(buf, s.data.data() + skip, n);
  len = (int)n;
  return USB_ACK;
}

void CUsbHostDevice::reset() {
  {
    std::lock_guard<std::mutex> lk(m_mx);
    for (int s = 0; s <= kControl; ++s)
      cancel(s);
    for (int s = 0; s < 32; ++s)
      iso_stop(s);
  }
  CUsbDevice::reset();
  m_stage = C_IDLE;
  m_pending_address = -1;
}

// The active configuration's endpoints (of the interfaces' current
// alternate settings, 0 until SET_INTERFACE): their transfer types.
void CUsbHostDevice::learn_endpoints() {
  std::fill(std::begin(m_ep_type), std::end(m_ep_type), -1);
  libusb_config_descriptor *c = nullptr;
  if (libusb_get_active_config_descriptor(libusb_get_device(m_h), &c) != 0)
    return;
  for (int i = 0; i < c->bNumInterfaces; ++i) {
    const libusb_interface &itf = c->interface[i];
    if (itf.num_altsetting < 1)
      continue;
    const libusb_interface_descriptor &a = itf.altsetting[0];
    for (int e = 0; e < a.bNumEndpoints; ++e) {
      const u8 addr = a.endpoint[e].bEndpointAddress;
      m_ep_type[(addr & 0x0f) | ((addr & 0x80) ? 16 : 0)] =
          a.endpoint[e].bmAttributes & 3;
    }
  }
  libusb_free_config_descriptor(c);
}

bool CUsbHostDevice::set_configuration(int value) {
  for (int i : m_claimed)
    libusb_release_interface(m_h, i);
  m_claimed.clear();
  {
    std::lock_guard<std::mutex> lk(m_mx);
    for (int s = 0; s < 32; ++s)
      iso_stop(s);
  }
  // The host device's configuration is changed only when the guest asks
  // for a different one: setting the one it has would reset its endpoints
  // under host drivers holding other interfaces, and "unconfigured" (0)
  // would take the device from them -- the guest's unconfigured device is
  // this one with its interfaces released.
  int cur = -1;
  libusb_get_configuration(m_h, &cur);
  if (value && cur != value) {
    const int r = libusb_set_configuration(m_h, value);
    if (r != 0) {
      printf("%%USB-W-HOSTCONFIG: %s: configuration %d: %s\n", m_name.c_str(),
             value, libusb_error_name(r));
      return false;
    }
  }
  m_configuration = value;
  if (!value)
    return true;
  libusb_config_descriptor *c = nullptr;
  if (libusb_get_active_config_descriptor(libusb_get_device(m_h), &c) == 0) {
    for (int i = 0; i < c->bNumInterfaces; ++i) {
      const int r = libusb_claim_interface(m_h, i);
      if (r == 0)
        m_claimed.push_back(i);
      else
        printf("%%USB-W-HOSTCLAIM: %s: interface %d: %s (a host driver holds "
               "it)\n",
               m_name.c_str(), i, libusb_error_name(r));
    }
    libusb_free_config_descriptor(c);
  }
  learn_endpoints();
  return true;
}

bool CUsbHostDevice::set_interface(int iface, int alt) {
  {
    std::lock_guard<std::mutex> lk(m_mx);
    for (int s = 0; s < 32; ++s)
      iso_stop(s);
  }
  const int r = libusb_set_interface_alt_setting(m_h, iface, alt);
  if (r != 0)
    return false;
  // The endpoints of the new alternate setting, in place of the old one's
  // (an isochronous endpoint exists only in the settings that have it).
  libusb_config_descriptor *c = nullptr;
  if (libusb_get_active_config_descriptor(libusb_get_device(m_h), &c) == 0) {
    if (iface < c->bNumInterfaces && alt < c->interface[iface].num_altsetting) {
      for (int k = 0; k < c->interface[iface].num_altsetting; ++k) {
        const libusb_interface_descriptor &o =
            c->interface[iface].altsetting[k];
        for (int e = 0; e < o.bNumEndpoints; ++e) {
          const u8 addr = o.endpoint[e].bEndpointAddress;
          m_ep_type[(addr & 0x0f) | ((addr & 0x80) ? 16 : 0)] = -1;
        }
      }
      const libusb_interface_descriptor &a =
          c->interface[iface].altsetting[alt];
      for (int e = 0; e < a.bNumEndpoints; ++e) {
        const u8 addr = a.endpoint[e].bEndpointAddress;
        m_ep_type[(addr & 0x0f) | ((addr & 0x80) ? 16 : 0)] =
            a.endpoint[e].bmAttributes & 3;
      }
    }
    libusb_free_config_descriptor(c);
  }
  return true;
}

// The requests carried out here rather than forwarded. True if the request
// is one of them; `ok` says whether it succeeded.
bool CUsbHostDevice::local_request(bool &ok) {
  const int type = m_setup[0], request = m_setup[1];
  const int value = m_setup[2] | (m_setup[3] << 8);
  const int index = m_setup[4] | (m_setup[5] << 8);
  ok = true;
  if (type == 0x00 && request == 0x05) { // SET_ADDRESS
    m_pending_address = value & 0x7f;
    return true;
  }
  if (type == 0x00 && request == 0x09) { // SET_CONFIGURATION
    {
      std::lock_guard<std::mutex> lk(m_mx);
      for (int s = 0; s < kControl; ++s)
        cancel(s);
    }
    // Not under m_mx: a synchronous libusb call waits on the event thread,
    // whose completions take m_mx.
    ok = set_configuration(value & 0xff);
    return true;
  }
  if (type == 0x01 && request == 0x0b) { // SET_INTERFACE
    ok = set_interface(index & 0xff, value & 0xff);
    return true;
  }
  if (type == 0x02 && request == 0x01 && value == 0) { // CLEAR_FEATURE(HALT)
    const u8 ep = index & 0xff;
    {
      std::lock_guard<std::mutex> lk(m_mx);
      cancel((ep & 0x0f) | ((ep & 0x80) ? 16 : 0));
    }
    ok = libusb_clear_halt(m_h, ep) == 0;
    return true;
  }
  return false;
}

// Descriptors as a full-speed device would give them: bulk and interrupt
// packets of at most 64 bytes, a 64-byte endpoint 0, and interrupt
// intervals in frames (a high-speed bInterval n means 2^(n-1) microframes).
void CUsbHostDevice::to_full_speed(std::vector<u8> &d) const {
  if (!m_high_speed || m_hs) // a full-speed device, or on a high-speed port
    return;
  for (size_t p = 0; p + 2 <= d.size() && d[p] >= 2; p += d[p]) {
    const u8 len = d[p], type = d[p + 1];
    if (p + len > d.size())
      break;
    if (type == 1 && len >= 8 && d[p + 7] > 64) // device: endpoint 0
      d[p + 7] = 64;
    if (type == 5 && len >= 7) { // endpoint
      const int kind = d[p + 3] & 3;
      if (kind == 2 || kind == 3) {
        const int mps = d[p + 4] | ((d[p + 5] & 7) << 8);
        if (mps > 64) {
          d[p + 4] = 64;
          d[p + 5] = 0;
        }
      }
      if (kind == 3) {
        const int n = std::max(1, std::min(16, (int)d[p + 6]));
        d[p + 6] = (u8)std::max(1, std::min(255, (1 << (n - 1)) / 8));
      }
    }
  }
}

// Endpoint 0, in the three stages of a control transfer (see CUsbDevice::
// control for the emulated devices' version; this one waits on hardware).
CUsbDevice::Result CUsbHostDevice::control(int pid, u8 *buf, int &len) {
  if (pid == PID_SETUP) {
    if (len != 8) {
      len = 0;
      return USB_STALL;
    }
    {
      std::lock_guard<std::mutex> lk(m_mx);
      cancel(kControl); // a new SETUP aborts an unfinished request
    }
    memcpy(m_setup, buf, 8);
    m_out.clear();
    m_pos = 0;
    const bool in = (m_setup[0] & 0x80) != 0;
    const int wlength = m_setup[6] | (m_setup[7] << 8);
    if (g_usbtrace)
      printf("USBT %s setup %02x %02x %02x%02x %02x%02x %02x%02x\n",
             m_name.c_str(), m_setup[0], m_setup[1], m_setup[3], m_setup[2],
             m_setup[5], m_setup[4], m_setup[7], m_setup[6]);
    m_local = !in && local_request(m_local_ok);
    if (m_local) {
      m_stage = C_STATUS_IN;
    } else if (in) {
      std::lock_guard<std::mutex> lk(m_mx);
      m_local_ok = submit(kControl, 0x80, LIBUSB_TRANSFER_TYPE_CONTROL, m_setup,
                          wlength);
      m_stage = C_DATA_IN;
    } else if (wlength) {
      m_stage = C_DATA_OUT;
    } else {
      std::lock_guard<std::mutex> lk(m_mx);
      m_local_ok =
          submit(kControl, 0x00, LIBUSB_TRANSFER_TYPE_CONTROL, m_setup, 0);
      m_stage = C_STATUS_IN;
    }
    len = 8;
    return USB_ACK;
  }

  std::lock_guard<std::mutex> lk(m_mx);
  Slot &s = m_slot[kControl];
  if (pid == PID_IN) {
    switch (m_stage) {
    case C_DATA_IN: {
      if (!m_local_ok) {
        len = 0;
        return USB_STALL;
      }
      if (!s.done) {
        len = 0;
        return USB_NAK;
      }
      // GET_DESCRIPTOR of the device or a configuration: as a full-speed
      // device would describe itself.
      if (m_pos == 0 && m_setup[0] == 0x80 && m_setup[1] == 0x06 &&
          (m_setup[3] == 1 || m_setup[3] == 2 || m_setup[3] == 7))
        to_full_speed(s.data);
      const Result r = take(kControl, buf, len, m_pos);
      m_pos += len;
      return r;
    }
    case C_DATA_OUT: // the status stage of a request with OUT data
      if (!s.xfer && !s.done) {
        std::vector<u8> all(m_setup, m_setup + 8);
        all.insert(all.end(), m_out.begin(), m_out.end());
        if (!submit(kControl, 0x00, LIBUSB_TRANSFER_TYPE_CONTROL, all.data(),
                    0)) {
          len = 0;
          m_stage = C_IDLE;
          return USB_STALL;
        }
      }
      if (!s.done) {
        len = 0;
        return USB_NAK;
      }
      m_stage = C_IDLE;
      s.done = false;
      len = 0;
      return take(kControl, nullptr, len, 0);
    case C_STATUS_IN:
      if (m_local) {
        m_stage = C_IDLE;
        len = 0;
        if (m_pending_address >= 0) {
          set_address(m_pending_address);
          m_pending_address = -1;
        }
        return m_local_ok ? USB_ACK : USB_STALL;
      }
      if (!m_local_ok) {
        m_stage = C_IDLE;
        len = 0;
        return USB_STALL;
      }
      if (!s.done) {
        len = 0;
        return USB_NAK;
      }
      m_stage = C_IDLE;
      s.done = false;
      len = 0;
      return take(kControl, nullptr, len, 0);
    default:
      len = 0;
      return USB_STALL;
    }
  }
  // OUT: a data stage, or the status stage of an IN request.
  if (m_stage == C_DATA_OUT) {
    m_out.insert(m_out.end(), buf, buf + len);
    return USB_ACK;
  }
  if (m_stage == C_DATA_IN && s.xfer)
    return USB_NAK; // the device has not answered yet
  m_stage = C_IDLE;
  s.done = false;
  return USB_ACK;
}

CUsbDevice::Result CUsbHostDevice::transfer(int pid, int ep, u8 *buf,
                                            int &len) {
  if (ep == 0)
    return control(pid, buf, len);
  if (pid == PID_SETUP || ep > 15) {
    len = 0;
    return USB_STALL;
  }
  const bool in = pid == PID_IN;
  const int slot = ep | (in ? 16 : 0);
  std::lock_guard<std::mutex> lk(m_mx);
  Slot &s = m_slot[slot];
  if (s.done) { // the transfer this TD started has finished
    s.done = false;
    if (!in) {
      if (s.status != LIBUSB_TRANSFER_COMPLETED) {
        len = 0;
        return USB_STALL;
      }
      // What the device took (s.data holds the bytes sent): a retry of a
      // TD run may name more than the transfer it started carried.
      len = std::min(len, (int)s.data.size());
      return USB_ACK;
    }
    return take(slot, buf, len, 0);
  }
  if (s.xfer) {
    len = 0;
    return USB_NAK;
  }
  const int type = m_ep_type[slot];
  if (type != LIBUSB_TRANSFER_TYPE_BULK &&
      type != LIBUSB_TRANSFER_TYPE_INTERRUPT) {
    len = 0;
    return USB_STALL; // no such endpoint, or isochronous
  }
  if (!submit(slot, (u8)(ep | (in ? 0x80 : 0)), type, buf, len)) {
    len = 0;
    return USB_STALL;
  }
  len = 0;
  return USB_NAK;
}

#endif // HAVE_LIBUSB
