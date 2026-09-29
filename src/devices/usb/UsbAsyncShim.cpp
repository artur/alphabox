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

#include "UsbAsyncShim.hpp"
#include "StdAfx.hpp"
#include <chrono>
#include <cstring>

CUsbAsyncShim::CUsbAsyncShim(std::unique_ptr<CUsbDevice> inner, int latency_us)
    : m_inner(std::move(inner)), m_latency_us(latency_us) {
  m_thread = std::thread([this]() { run(); });
}

CUsbAsyncShim::~CUsbAsyncShim() {
  {
    std::lock_guard<std::mutex> lk(m_mx);
    m_stop = true;
  }
  m_cv.notify_all();
  m_thread.join();
}

const std::vector<u8> &CUsbAsyncShim::device_descriptor() const {
  static const std::vector<u8> none;
  return none;
}

const std::vector<u8> &CUsbAsyncShim::configuration_descriptor() const {
  return device_descriptor();
}

void CUsbAsyncShim::reset() {
  {
    std::lock_guard<std::mutex> lk(m_mx);
    for (Slot &s : m_slot)
      if (s.state == Slot::DONE)
        s.state = Slot::IDLE; // a queued one finishes, and is dropped then
  }
  std::lock_guard<std::mutex> lk(m_inner_mx);
  m_inner->reset();
  CUsbDevice::reset();
}

// The worker: one queued transfer at a time, each after the latency.
void CUsbAsyncShim::run() {
  std::unique_lock<std::mutex> lk(m_mx);
  for (;;) {
    int i = -1;
    m_cv.wait(lk, [&]() {
      if (m_stop)
        return true;
      for (i = 0; i < 32; ++i)
        if (m_slot[i].state == Slot::QUEUED)
          return true;
      return false;
    });
    if (m_stop)
      return;
    Slot &s = m_slot[i];
    const int pid = s.pid, ep = i & 15;
    std::vector<u8> buf = s.buf;
    int len = s.len;
    lk.unlock();
    std::this_thread::sleep_for(std::chrono::microseconds(m_latency_us));
    Result r;
    {
      std::lock_guard<std::mutex> ilk(m_inner_mx);
      r = m_inner->transfer(pid, ep, buf.data(), len);
    }
    lk.lock();
    if (s.state != Slot::QUEUED)
      continue; // reset meanwhile
    if (r == USB_NAK) {
      s.state = Slot::IDLE; // nothing yet: the next frame asks again
      continue;
    }
    s.result = r;
    s.len = len;
    s.buf = std::move(buf);
    s.state = Slot::DONE;
    if (on_complete)
      on_complete();
  }
}

CUsbDevice::Result CUsbAsyncShim::transfer(int pid, int ep, u8 *buf, int &len) {
  if (ep == 0) {
    std::lock_guard<std::mutex> lk(m_inner_mx);
    const Result r = m_inner->transfer(pid, ep, buf, len);
    set_address(m_inner->address());
    return r;
  }
  const int i = (ep & 15) | (pid == PID_IN ? 16 : 0);
  std::lock_guard<std::mutex> lk(m_mx);
  Slot &s = m_slot[i];
  switch (s.state) {
  case Slot::DONE:
    s.state = Slot::IDLE;
    if (pid == PID_IN) {
      len = std::min(len, s.len);
      memcpy(buf, s.buf.data(), len);
    } else {
      len = std::min(len, s.len);
    }
    return s.result;
  case Slot::QUEUED:
    len = 0;
    return USB_NAK;
  default:
    s.state = Slot::QUEUED;
    s.pid = pid;
    s.len = len;
    s.buf.assign(buf, buf + (pid == PID_IN ? 0 : len));
    s.buf.resize(len);
    m_cv.notify_all();
    len = 0;
    return USB_NAK;
  }
}
