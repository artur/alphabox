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
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

/**
 * \brief A test harness: an emulated device made to answer like real
 * hardware behind libusb.
 *
 * Transfers to its data endpoints finish on a worker thread after a fixed
 * latency, the TD NAKed until then, and the controller is woken when one
 * does (on_complete) -- the timing a passed-through device has, with a
 * device whose data can be checked. It measures and tests the controller's
 * side of passthrough without a host device to pass through
 * (ALPHABOX_USB_ASYNC_US=<latency> wraps USB storage in one). Endpoint 0
 * stays synchronous.
 **/
class CUsbAsyncShim : public CUsbDevice {
public:
  CUsbAsyncShim(std::unique_ptr<CUsbDevice> inner, int latency_us);
  ~CUsbAsyncShim() override;
  const char *name() const override { return m_inner->name(); }
  bool low_speed() const override { return m_inner->low_speed(); }
  Result transfer(int pid, int ep, u8 *buf, int &len) override;
  void reset() override;

protected:
  const std::vector<u8> &device_descriptor() const override;
  const std::vector<u8> &configuration_descriptor() const override;

private:
  void run();
  struct Slot {
    enum State { IDLE, QUEUED, DONE } state = IDLE;
    int pid = 0;
    int len = 0;
    std::vector<u8> buf;
    Result result = USB_ACK;
  };
  std::unique_ptr<CUsbDevice> m_inner;
  std::mutex m_inner_mx; // the wrapped device: one caller at a time
  const int m_latency_us;
  std::mutex m_mx;
  std::condition_variable m_cv;
  Slot m_slot[32]; // [ep] OUT, [16 + ep] IN
  bool m_stop = false;
  std::thread m_thread;
};

#endif
