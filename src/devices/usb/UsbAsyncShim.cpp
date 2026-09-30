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
#include <cstring>

std::unique_ptr<CUsbDevice> usb_async_wrap(std::unique_ptr<CUsbDevice> dev,
                                           const char *who, int port) {
  const char *e = getenv("ALPHABOX_USB_ASYNC_US");
  if (!e)
    return dev;
  printf("%s: USB %s on port %d answers after %d us.\n", who, dev->name(), port,
         atoi(e));
  return std::make_unique<CUsbAsyncShim>(std::move(dev), atoi(e));
}

CUsbAsyncShim::CUsbAsyncShim(std::unique_ptr<CUsbDevice> inner, int latency_us)
    : m_inner(std::move(inner)), m_latency(latency_us) {
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
    m_iso_out.clear();
    for (IsoIn &s : m_iso_in) {
      s.on = false;
      s.ready.clear();
    }
  }
  std::lock_guard<std::mutex> lk(m_inner_mx);
  m_inner->reset();
  CUsbDevice::reset();
}

// The worker: each queued bulk or interrupt transfer, isochronous OUT packet
// and IN stream when it falls due, the device called with only its own lock
// held.
void CUsbAsyncShim::run() {
  std::unique_lock<std::mutex> lk(m_mx);
  std::vector<u8> tmp(3072); // an IN packet: at most 3 x 1024 bytes
  while (!m_stop) {
    auto now = clock::now();
    auto next = now + std::chrono::milliseconds(100);
    // Bulk and interrupt transfers.
    for (int i = 0; i < 32; ++i) {
      Slot &s = m_slot[i];
      if (s.state != Slot::QUEUED)
        continue;
      if (s.due > now) {
        next = std::min(next, s.due);
        continue;
      }
      const int pid = s.pid, ep = i & 15;
      std::vector<u8> buf = s.buf;
      int len = s.len;
      lk.unlock();
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
    // Isochronous OUT: each packet to the device when its time comes.
    while (!m_iso_out.empty() && m_iso_out.front().due <= now) {
      IsoPacket p = std::move(m_iso_out.front());
      m_iso_out.pop_front();
      lk.unlock();
      {
        std::lock_guard<std::mutex> ilk(m_inner_mx);
        m_inner->iso_transfer(PID_OUT, p.ep, p.data.data(), (int)p.data.size());
      }
      lk.lock();
    }
    if (!m_iso_out.empty())
      next = std::min(next, m_iso_out.front().due);
    // Isochronous IN: a packet from the device each (micro)frame while the
    // guest reads the endpoint, ready for it the latency later.
    const auto period =
        m_hs ? std::chrono::microseconds(125) : std::chrono::microseconds(1000);
    for (int ep = 0; ep < 16; ++ep) {
      IsoIn &s = m_iso_in[ep];
      if (!s.on)
        continue;
      if (now - s.last_ask > std::chrono::milliseconds(100)) {
        s.on = false; // the guest stopped reading
        s.ready.clear();
        continue;
      }
      if (s.next <= now) {
        lk.unlock();
        int n;
        {
          std::lock_guard<std::mutex> ilk(m_inner_mx);
          n = m_inner->iso_endpoint(PID_IN, ep)
                  ? m_inner->iso_transfer(PID_IN, ep, tmp.data(),
                                          (int)tmp.size())
                  : ISO_NO_ENDPOINT;
        }
        lk.lock();
        if (!s.on)
          continue;
        if (n >= 0) {
          s.ready.push_back(
              {ep,
               std::vector<u8>(tmp.begin(),
                               tmp.begin() + std::min(n, (int)tmp.size())),
               now + m_latency});
          if (s.ready.size() > kIsoQueue)
            s.ready.pop_front(); // the guest fell behind: the oldest goes
        }
        s.next += period;
        if (s.next < now)
          s.next = now + period; // late: no burst to catch up
      }
      next = std::min(next, s.next);
    }
    m_cv.wait_until(lk, next);
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
    s.due = clock::now() + m_latency;
    m_cv.notify_all();
    len = 0;
    return USB_NAK;
  }
}

int CUsbAsyncShim::iso_transfer(int pid, int ep, u8 *buf, int len) {
  if (!iso_endpoint(pid, ep))
    return ISO_NO_ENDPOINT;
  ep &= 15;
  const auto now = clock::now();
  std::lock_guard<std::mutex> lk(m_mx);
  if (pid == PID_OUT) {
    if (m_iso_out.size() >= kIsoQueue)
      return ISO_OVERRUN; // the pipeline is full: this packet is lost
    m_iso_out.push_back({ep, std::vector<u8>(buf, buf + len), now + m_latency});
    m_cv.notify_all();
    return len;
  }
  IsoIn &s = m_iso_in[ep];
  s.last_ask = now;
  if (!s.on) { // the first read starts the stream
    s.on = true;
    s.next = now;
    m_cv.notify_all();
  }
  if (s.ready.empty() || s.ready.front().due > now)
    return ISO_OVERRUN; // nothing arrived in time
  const std::vector<u8> d = std::move(s.ready.front().data);
  s.ready.pop_front();
  memcpy(buf, d.data(), std::min((int)d.size(), len));
  return (int)d.size();
}
