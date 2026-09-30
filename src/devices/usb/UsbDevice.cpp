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

#include "UsbDevice.hpp"
#include "StdAfx.hpp"
#include <chrono>
#include <cstring>

// ALPHABOX_USBTRACE=1 also logs each control request a device serves.
static const bool g_usbtrace = getenv("ALPHABOX_USBTRACE") != nullptr;

void CUsbDevice::reset() {
  m_address = 0;
  m_pending_address = -1;
  m_configuration = 0;
  m_stage = ST_IDLE;
  m_ctl.clear();
  m_ctl_pos = 0;
}

std::vector<u8> CUsbDevice::utf16_string(const char *s) {
  std::vector<u8> d;
  d.push_back(0);
  d.push_back(3); // STRING
  for (; *s; ++s) {
    d.push_back((u8)*s);
    d.push_back(0);
  }
  d[0] = (u8)d.size();
  return d;
}

std::vector<u8> CUsbDevice::string_descriptor(int index) const {
  (void)index;
  return {};
}

CUsbDevice::Result CUsbDevice::transfer(int pid, int ep, u8 *buf, int &len) {
  if (ep == 0)
    return control(pid, buf, len);
  if (pid == PID_IN)
    return data_in(ep, buf, len);
  if (pid == PID_OUT)
    return data_out(ep, buf, len);
  len = 0;
  return USB_STALL;
}

// Endpoint 0. A SETUP starts a transfer (and aborts any unfinished one, as
// the specification requires). A request with a device-to-host data stage is
// answered in full at SETUP time and handed out over the IN transfers that
// follow; one with a host-to-device data stage collects the OUT data and runs
// when the status stage (a zero-length IN) arrives; one without a data stage
// runs at SETUP time. SET_ADDRESS takes effect after its status stage.
CUsbDevice::Result CUsbDevice::control(int pid, u8 *buf, int &len) {
  if (pid == PID_SETUP) {
    if (len != 8) {
      len = 0;
      return USB_STALL;
    }
    memcpy(m_setup, buf, 8);
    m_ctl.clear();
    m_ctl_pos = 0;
    m_ctl_stall = false;
    const bool in = (m_setup[0] & 0x80) != 0;
    const int wlength = m_setup[6] | (m_setup[7] << 8);
    if (in) {
      std::vector<u8> out;
      std::vector<u8> none;
      const bool ok = ((m_setup[0] & 0x60) == 0)
                          ? standard_request(m_setup, none, out)
                          : class_request(m_setup, none, out);
      if (!ok)
        m_ctl_stall = true;
      if (out.size() > (size_t)wlength)
        out.resize(wlength);
      m_ctl = out;
      m_stage = ST_DATA_IN;
    } else if (wlength) {
      m_stage = ST_DATA_OUT;
    } else {
      std::vector<u8> out;
      std::vector<u8> none;
      const bool ok = ((m_setup[0] & 0x60) == 0)
                          ? standard_request(m_setup, none, out)
                          : class_request(m_setup, none, out);
      m_ctl_stall = !ok;
      m_stage = ST_STATUS;
    }
    if (g_usbtrace)
      printf("USBT %s setup %02x %02x %02x%02x %02x%02x %02x%02x%s\n", name(),
             m_setup[0], m_setup[1], m_setup[3], m_setup[2], m_setup[5],
             m_setup[4], m_setup[7], m_setup[6],
             m_ctl_stall ? " -> STALL" : "");
    len = 8;
    return USB_ACK;
  }
  if (m_ctl_stall) {
    len = 0;
    return USB_STALL;
  }
  if (pid == PID_IN) {
    if (m_stage == ST_DATA_IN) {
      const size_t n = std::min((size_t)len, m_ctl.size() - m_ctl_pos);
      memcpy(buf, m_ctl.data() + m_ctl_pos, n);
      m_ctl_pos += n;
      len = (int)n;
      return USB_ACK;
    }
    if (m_stage == ST_DATA_OUT) { // status stage of a host-to-device request
      std::vector<u8> out;
      const bool ok = ((m_setup[0] & 0x60) == 0)
                          ? standard_request(m_setup, m_ctl, out)
                          : class_request(m_setup, m_ctl, out);
      m_stage = ST_IDLE;
      len = 0;
      return ok ? USB_ACK : USB_STALL;
    }
    // Status stage of a request without data.
    m_stage = ST_IDLE;
    if (m_pending_address >= 0) {
      m_address = m_pending_address;
      m_pending_address = -1;
    }
    len = 0;
    return USB_ACK;
  }
  // OUT: data stage of a host-to-device request, or the zero-length status
  // stage of a device-to-host one.
  if (m_stage == ST_DATA_OUT) {
    m_ctl.insert(m_ctl.end(), buf, buf + len);
    return USB_ACK;
  }
  m_stage = ST_IDLE;
  return USB_ACK;
}

bool CUsbDevice::standard_request(const u8 *setup, const std::vector<u8> &data,
                                  std::vector<u8> &out) {
  (void)data;
  const int request = setup[1];
  const int value = setup[2] | (setup[3] << 8);
  switch (request) {
  case 0x00: // GET_STATUS: self-powered 0, remote wakeup 0; not halted
    out = {0, 0};
    return true;
  case 0x01: // CLEAR_FEATURE
  case 0x03: // SET_FEATURE (endpoint halt, remote wakeup): accepted
    return true;
  case 0x05: // SET_ADDRESS, after the status stage
    m_pending_address = value & 0x7f;
    return true;
  case 0x06: { // GET_DESCRIPTOR
    const int type = value >> 8, index = value & 0xff;
    if ((setup[0] & 0x1f) != 0) // interface or endpoint: a class descriptor
      return other_descriptor(setup, out);
    if (type == 1) {
      out = device_descriptor();
      return true;
    }
    if (type == 2) {
      out = configuration_descriptor();
      return true;
    }
    if (type == 3) {
      if (index == 0) {
        out = {4, 3, 0x09, 0x04}; // US English
        return true;
      }
      out = string_descriptor(index);
      return !out.empty();
    }
    return other_descriptor(setup, out);
  }
  case 0x08: // GET_CONFIGURATION
    out = {(u8)m_configuration};
    return true;
  case 0x09: // SET_CONFIGURATION
    m_configuration = value & 0xff;
    configured();
    return true;
  case 0x0a: // GET_INTERFACE
    out = {(u8)get_interface(setup[4])};
    return true;
  case 0x0b: // SET_INTERFACE
    if (!set_interface(setup[4], value & 0xff))
      return false;
    configured();
    return true;
  default:
    return false;
  }
}

std::atomic<CUsbFaultTarget *> theUsbFaultTarget{nullptr};

bool CUsbPortFaults::intercept(CUsbDevice *dev, int pid, int ep, const u8 *buf,
                               int len, CUsbDevice::Result &r) {
  // CLEAR_FEATURE(ENDPOINT_HALT) on its way to the device lifts the halt.
  if (pid == CUsbDevice::PID_SETUP && ep == 0 && len == 8 && buf[0] == 0x02 &&
      buf[1] == 0x01 && buf[2] == 0 && buf[3] == 0)
    halted &= ~(1u << bit(buf[4]));
  if (ep == 0 && pid == CUsbDevice::PID_SETUP)
    return false;
  const int addr = ep | (pid == CUsbDevice::PID_IN ? 0x80 : 0);
  if (addr == nak_ep) {
    const u64 now = (u64)std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count();
    if (now < nak_until_ms) {
      r = CUsbDevice::USB_NAK;
      return true;
    }
    nak_ep = -1;
  }
  if (addr == stall_ep && stall_left > 0 && !(halted & (1u << bit(addr)))) {
    halted |= 1u << bit(addr);
    --stall_left;
    dev->endpoint_halted(addr);
  }
  if (halted & (1u << bit(addr))) {
    r = CUsbDevice::USB_STALL;
    return true;
  }
  return false;
}
