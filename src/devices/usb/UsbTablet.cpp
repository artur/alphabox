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

#include "UsbTablet.hpp"
#include "StdAfx.hpp"
#include <algorithm>
#include <cstring>

std::atomic<CUsbTablet *> theUsbTablet{nullptr};

// The report: buttons (3 bits + 5 of padding), X and Y (16 bits each,
// 0..32767, absolute), wheel (8 bits, relative).
static const std::vector<u8> kReportDescriptor = {
    0x05, 0x01,       // Usage Page (Generic Desktop)
    0x09, 0x02,       // Usage (Mouse)
    0xa1, 0x01,       // Collection (Application)
    0x09, 0x01,       //   Usage (Pointer)
    0xa1, 0x00,       //   Collection (Physical)
    0x05, 0x09,       //     Usage Page (Button)
    0x19, 0x01,       //     Usage Minimum (1)
    0x29, 0x03,       //     Usage Maximum (3)
    0x15, 0x00,       //     Logical Minimum (0)
    0x25, 0x01,       //     Logical Maximum (1)
    0x95, 0x03,       //     Report Count (3)
    0x75, 0x01,       //     Report Size (1)
    0x81, 0x02,       //     Input (Data, Variable, Absolute)
    0x95, 0x01,       //     Report Count (1)
    0x75, 0x05,       //     Report Size (5)
    0x81, 0x01,       //     Input (Constant)
    0x05, 0x01,       //     Usage Page (Generic Desktop)
    0x09, 0x30,       //     Usage (X)
    0x09, 0x31,       //     Usage (Y)
    0x15, 0x00,       //     Logical Minimum (0)
    0x26, 0xff, 0x7f, //     Logical Maximum (32767)
    0x35, 0x00,       //     Physical Minimum (0)
    0x46, 0xff, 0x7f, //     Physical Maximum (32767)
    0x75, 0x10,       //     Report Size (16)
    0x95, 0x02,       //     Report Count (2)
    0x81, 0x02,       //     Input (Data, Variable, Absolute)
    0x05, 0x01,       //     Usage Page (Generic Desktop)
    0x09, 0x38,       //     Usage (Wheel)
    0x15, 0x81,       //     Logical Minimum (-127)
    0x25, 0x7f,       //     Logical Maximum (127)
    0x35, 0x00,       //     Physical Minimum (0)
    0x45, 0x00,       //     Physical Maximum (0)
    0x75, 0x08,       //     Report Size (8)
    0x95, 0x01,       //     Report Count (1)
    0x81, 0x06,       //     Input (Data, Variable, Relative)
    0xc0,             //   End Collection
    0xc0,             // End Collection
};

static const std::vector<u8> kDeviceDescriptor = {
    18,   1,       // bLength, DEVICE
    0x10, 0x01,    // USB 1.1
    0,    0,    0, // class in the interface
    8,             // endpoint 0 max packet
    0x09, 0x12,    // idVendor 0x1209 (pid.codes)
    0x01, 0xa1,    // idProduct 0xa101
    0x00, 0x01,    // bcdDevice 1.00
    1,    2,    3, // manufacturer, product, serial strings
    1,             // one configuration
};

static const std::vector<u8> kConfigurationDescriptor = {
    // Configuration
    9, 2, 34, 0, // bLength, CONFIGURATION, wTotalLength 34
    1,           // one interface
    1,           // bConfigurationValue
    0,           // no string
    0xa0,        // bus-powered, remote wakeup
    50,          // 100 mA
    // Interface 0: HID, no boot protocol (a tablet has none)
    9, 4, 0, 0, 1, 3, 0, 0, 0,
    // HID 1.01, one report descriptor
    9, 0x21, 0x01, 0x01, 0, 1, 0x22, (u8)(kReportDescriptor.size() & 0xff),
    (u8)(kReportDescriptor.size() >> 8),
    // Endpoint 1 IN, interrupt, 8 bytes, every 10 ms
    7, 5, 0x81, 3, 8, 0, 10};

// At high speed: USB 2.0, and the interrupt endpoint's interval in the
// high-speed unit (2^(bInterval-1) microframes: 4 = 8 microframes = 1 ms).
static std::vector<u8> high_speed_form(std::vector<u8> d, bool config) {
  if (!config) {
    d[2] = 0x00; // bcdUSB 2.00
    d[3] = 0x02;
    return d;
  }
  for (size_t p = 0; p + 2 <= d.size() && d[p] >= 2; p += d[p])
    if (d[p + 1] == 5)
      d[p + 6] = 4;
  return d;
}
static const std::vector<u8> kDeviceDescriptorHS =
    high_speed_form(kDeviceDescriptor, false);
static const std::vector<u8> kConfigurationDescriptorHS =
    high_speed_form(kConfigurationDescriptor, true);

CUsbTablet::CUsbTablet(bool high_speed) : m_can_hs(high_speed) {}

void CUsbTablet::reset() {
  CUsbDevice::reset();
  std::lock_guard<std::mutex> lk(m_mx);
  m_idle = 0;
  m_protocol = 1;
  m_changed = true;
}

const std::vector<u8> &CUsbTablet::device_descriptor() const {
  return m_hs ? kDeviceDescriptorHS : kDeviceDescriptor;
}

const std::vector<u8> &CUsbTablet::configuration_descriptor() const {
  return m_hs ? kConfigurationDescriptorHS : kConfigurationDescriptor;
}

std::vector<u8> CUsbTablet::string_descriptor(int index) const {
  switch (index) {
  case 1:
    return utf16_string("Alphabox");
  case 2:
    return utf16_string("Alphabox USB Tablet");
  case 3:
    return utf16_string("1");
  default:
    return {};
  }
}

// GET_DESCRIPTOR to the interface: the HID descriptor or the report
// descriptor.
bool CUsbTablet::other_descriptor(const u8 *setup, std::vector<u8> &out) {
  const int type = setup[3];
  if ((setup[0] & 0x1f) == 0) { // to the device: the other speed's view
    if (type == 6) {            // DEVICE_QUALIFIER
      out = {10, 6, 0x00, 0x02, 0, 0, 0, 8, 1, 0};
      return true;
    }
    if (type == 7) { // OTHER_SPEED_CONFIGURATION
      out = m_hs ? kConfigurationDescriptor : kConfigurationDescriptorHS;
      out[1] = 7;
      return true;
    }
    return false;
  }
  if (type == 0x22) {
    out = kReportDescriptor;
    return true;
  }
  if (type == 0x21) {
    out.assign(kConfigurationDescriptor.begin() + 18,
               kConfigurationDescriptor.begin() + 27);
    return true;
  }
  return false;
}

bool CUsbTablet::class_request(const u8 *setup, const std::vector<u8> &data,
                               std::vector<u8> &out) {
  (void)data;
  std::lock_guard<std::mutex> lk(m_mx);
  switch (setup[1]) {
  case 0x01: // GET_REPORT
    out = report();
    return true;
  case 0x02: // GET_IDLE
    out = {(u8)m_idle};
    return true;
  case 0x03: // GET_PROTOCOL
    out = {(u8)m_protocol};
    return true;
  case 0x09: // SET_REPORT: nothing to set (no output reports)
    return true;
  case 0x0a: // SET_IDLE
    m_idle = setup[3];
    return true;
  case 0x0b: // SET_PROTOCOL
    m_protocol = setup[2];
    return true;
  default:
    return false;
  }
}

// Called with m_mx held.
std::vector<u8> CUsbTablet::report() {
  const int wheel = std::max(-127, std::min(127, m_wheel));
  m_wheel -= wheel;
  return {(u8)(m_buttons & 7), (u8)(m_x & 0xff), (u8)(m_x >> 8),
          (u8)(m_y & 0xff),    (u8)(m_y >> 8),   (u8)(int8_t)wheel};
}

// The interrupt endpoint: a report when something changed (or, with an idle
// rate set, on every poll), else NAK -- the controller tries again next
// frame, which is how a real device answers a poll with nothing to say.
CUsbDevice::Result CUsbTablet::data_in(int ep, u8 *buf, int &len) {
  if (ep != 1 || !m_configuration) {
    len = 0;
    return USB_STALL;
  }
  std::lock_guard<std::mutex> lk(m_mx);
  if (!m_changed && !m_idle && !m_wheel) {
    len = 0;
    return USB_NAK;
  }
  const std::vector<u8> r = report();
  m_changed = m_wheel != 0;
  len = std::min(len, (int)r.size());
  memcpy(buf, r.data(), len);
  return USB_ACK;
}

void CUsbTablet::set_position(double x, double y, unsigned buttons) {
  const int ix = (int)(std::max(0.0, std::min(1.0, x)) * 32767.0 + 0.5);
  const int iy = (int)(std::max(0.0, std::min(1.0, y)) * 32767.0 + 0.5);
  std::lock_guard<std::mutex> lk(m_mx);
  if (ix != m_x || iy != m_y || buttons != m_buttons) {
    m_x = ix;
    m_y = iy;
    m_buttons = buttons;
    m_changed = true;
  }
}

void CUsbTablet::set_buttons(unsigned buttons) {
  std::lock_guard<std::mutex> lk(m_mx);
  if (buttons != m_buttons) {
    m_buttons = buttons;
    m_changed = true;
  }
}

void CUsbTablet::add_wheel(int dz) {
  std::lock_guard<std::mutex> lk(m_mx);
  m_wheel += dz;
  m_changed = true;
}
