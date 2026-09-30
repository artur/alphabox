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

#if !defined(INCLUDED_USBASYNCSHIM_H)
#define INCLUDED_USBASYNCSHIM_H

#include "UsbDevice.hpp"
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

/**
 * \brief A test harness: an emulated device made to answer like real
 * hardware behind libusb.
 *
 * Transfers to its bulk and interrupt endpoints finish on a worker thread
 * after a fixed latency, the TD NAKed until then, and the controller is
 * woken when one does (on_complete) -- the timing a passed-through device
 * has, with a device whose data can be checked. Its isochronous endpoints
 * move data the way CUsbHostDevice's libusb isochronous transfers do: an
 * OUT packet is taken at once and reaches the device the latency later
 * (a full queue: ISO_OVERRUN); an IN endpoint the guest reads is streamed
 * from the device one packet a (micro)frame on the worker, each packet
 * available to the guest the latency after it was fetched, and a read that
 * finds none ready is ISO_OVERRUN -- the controller's missed-data status,
 * never a NAK, which an isochronous pipe does not have. It measures and
 * tests the controller's side of passthrough without a host device to pass
 * through (ALPHABOX_USB_ASYNC_US=<latency> wraps USB storage and the
 * speaker in one; the EHCI self-test its isochronous loopback). Endpoint 0
 * stays synchronous.
 **/
class CUsbAsyncShim : public CUsbDevice {
public:
  CUsbAsyncShim(std::unique_ptr<CUsbDevice> inner, int latency_us);
  ~CUsbAsyncShim() override;
  const char *name() const override { return m_inner->name(); }
  bool low_speed() const override { return m_inner->low_speed(); }
  bool can_high_speed() const override { return m_inner->can_high_speed(); }
  void set_high_speed(bool hs) override {
    CUsbDevice::set_high_speed(hs);
    std::lock_guard<std::mutex> lk(m_inner_mx);
    m_inner->set_high_speed(hs);
  }
  void endpoint_halted(int ep_addr) override {
    std::lock_guard<std::mutex> lk(m_inner_mx);
    m_inner->endpoint_halted(ep_addr);
  }
  bool inject_phase_error() override {
    std::lock_guard<std::mutex> lk(m_inner_mx);
    return m_inner->inject_phase_error();
  }
  Result transfer(int pid, int ep, u8 *buf, int &len) override;
  int iso_transfer(int pid, int ep, u8 *buf, int len) override;
  bool iso_endpoint(int pid, int ep) const override {
    std::lock_guard<std::mutex> lk(m_inner_mx);
    return m_inner->iso_endpoint(pid, ep);
  }
  void reset() override;
  /// Like the passed-through device it stands in for: transfers in flight
  /// on its worker are not saved; a restore shows it reconnected.
  bool save(CUsbSaved &) const override { return false; }

protected:
  const std::vector<u8> &device_descriptor() const override;
  const std::vector<u8> &configuration_descriptor() const override;

private:
  using clock = std::chrono::steady_clock;
  void run();
  struct Slot {
    enum State { IDLE, QUEUED, DONE } state = IDLE;
    int pid = 0;
    int len = 0;
    std::vector<u8> buf;
    Result result = USB_ACK;
    clock::time_point due;
  };
  struct IsoPacket {
    int ep;
    std::vector<u8> data;
    clock::time_point due;
  };
  /// An IN endpoint's stream: on while the guest keeps reading it.
  struct IsoIn {
    bool on = false;
    std::deque<IsoPacket> ready;
    clock::time_point next, last_ask;
  };
  static constexpr size_t kIsoQueue = 64; // packets queued per direction
  std::unique_ptr<CUsbDevice> m_inner;
  mutable std::mutex m_inner_mx; // the wrapped device: one caller at a time
  const std::chrono::microseconds m_latency;
  std::mutex m_mx;
  std::condition_variable m_cv;
  Slot m_slot[32]; // [ep] OUT, [16 + ep] IN
  std::deque<IsoPacket> m_iso_out;
  IsoIn m_iso_in[16];
  bool m_stop = false;
  std::thread m_thread;
};

/// ALPHABOX_USB_ASYNC_US=<us>: `dev` wrapped in a CUsbAsyncShim with that
/// latency (and a line saying so, for the port of `who`); else `dev`.
std::unique_ptr<CUsbDevice> usb_async_wrap(std::unique_ptr<CUsbDevice> dev,
                                           const char *who, int port);

#endif
