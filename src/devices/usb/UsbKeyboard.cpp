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

#include "UsbKeyboard.hpp"
#include "StdAfx.hpp"
#include "gui.hpp"
#include <algorithm>
#include <cstring>

std::atomic<CUsbKeyboard *> theUsbKeyboard{nullptr};

// The boot keyboard's report descriptor (HID 1.11 appendix B.1): eight
// modifier bits, a reserved byte, five LED bits out (and three of padding),
// six key usages 0..101.
static const std::vector<u8> kReportDescriptor = {
    0x05, 0x01, // Usage Page (Generic Desktop)
    0x09, 0x06, // Usage (Keyboard)
    0xa1, 0x01, // Collection (Application)
    0x05, 0x07, //   Usage Page (Keyboard/Keypad)
    0x19, 0xe0, //   Usage Minimum (Left Control)
    0x29, 0xe7, //   Usage Maximum (Right GUI)
    0x15, 0x00, //   Logical Minimum (0)
    0x25, 0x01, //   Logical Maximum (1)
    0x75, 0x01, //   Report Size (1)
    0x95, 0x08, //   Report Count (8)
    0x81, 0x02, //   Input (Data, Variable, Absolute): the modifiers
    0x95, 0x01, //   Report Count (1)
    0x75, 0x08, //   Report Size (8)
    0x81, 0x01, //   Input (Constant): reserved
    0x95, 0x05, //   Report Count (5)
    0x75, 0x01, //   Report Size (1)
    0x05, 0x08, //   Usage Page (LEDs)
    0x19, 0x01, //   Usage Minimum (Num Lock)
    0x29, 0x05, //   Usage Maximum (Kana)
    0x91, 0x02, //   Output (Data, Variable, Absolute): the LEDs
    0x95, 0x01, //   Report Count (1)
    0x75, 0x03, //   Report Size (3)
    0x91, 0x01, //   Output (Constant): padding
    0x95, 0x06, //   Report Count (6)
    0x75, 0x08, //   Report Size (8)
    0x15, 0x00, //   Logical Minimum (0)
    0x25, 0x65, //   Logical Maximum (101)
    0x05, 0x07, //   Usage Page (Keyboard/Keypad)
    0x19, 0x00, //   Usage Minimum (0)
    0x29, 0x65, //   Usage Maximum (101)
    0x81, 0x00, //   Input (Data, Array): the keys
    0xc0,       // End Collection
};

static const std::vector<u8> kDeviceDescriptor = {
    18,   1,       // bLength, DEVICE
    0x10, 0x01,    // USB 1.1
    0,    0,    0, // class in the interface
    8,             // endpoint 0 max packet
    0x09, 0x12,    // idVendor 0x1209 (pid.codes)
    0x02, 0xa1,    // idProduct 0xa102
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
    // Interface 0: HID, boot subclass, keyboard protocol
    9, 4, 0, 0, 1, 3, 1, 1, 0,
    // HID 1.11, country 0, one report descriptor
    9, 0x21, 0x11, 0x01, 0, 1, 0x22, (u8)(kReportDescriptor.size() & 0xff),
    (u8)(kReportDescriptor.size() >> 8),
    // Endpoint 1 IN, interrupt, 8 bytes, every 10 ms
    7, 5, 0x81, 3, 8, 0, 10};

/// The HID usage (Keyboard/Keypad page) of a BX_KEY_* code, or 0. The
/// modifiers are usages 0xe0-0xe7.
static u8 hid_usage(u32 bx) {
  static const struct {
    u32 bx;
    u8 usage;
  } table[] = {
      {BX_KEY_CTRL_L, 0xe0},        {BX_KEY_SHIFT_L, 0xe1},
      {BX_KEY_ALT_L, 0xe2},         {BX_KEY_WIN_L, 0xe3},
      {BX_KEY_CTRL_R, 0xe4},        {BX_KEY_SHIFT_R, 0xe5},
      {BX_KEY_ALT_R, 0xe6},         {BX_KEY_WIN_R, 0xe7},
      {BX_KEY_ENTER, 0x28},         {BX_KEY_ESC, 0x29},
      {BX_KEY_BACKSPACE, 0x2a},     {BX_KEY_TAB, 0x2b},
      {BX_KEY_SPACE, 0x2c},         {BX_KEY_MINUS, 0x2d},
      {BX_KEY_EQUALS, 0x2e},        {BX_KEY_LEFT_BRACKET, 0x2f},
      {BX_KEY_RIGHT_BRACKET, 0x30}, {BX_KEY_BACKSLASH, 0x31},
      {BX_KEY_SEMICOLON, 0x33},     {BX_KEY_SINGLE_QUOTE, 0x34},
      {BX_KEY_GRAVE, 0x35},         {BX_KEY_COMMA, 0x36},
      {BX_KEY_PERIOD, 0x37},        {BX_KEY_SLASH, 0x38},
      {BX_KEY_CAPS_LOCK, 0x39},     {BX_KEY_PRINT, 0x46},
      {BX_KEY_SCRL_LOCK, 0x47},     {BX_KEY_PAUSE, 0x48},
      {BX_KEY_INSERT, 0x49},        {BX_KEY_HOME, 0x4a},
      {BX_KEY_PAGE_UP, 0x4b},       {BX_KEY_DELETE, 0x4c},
      {BX_KEY_END, 0x4d},           {BX_KEY_PAGE_DOWN, 0x4e},
      {BX_KEY_RIGHT, 0x4f},         {BX_KEY_LEFT, 0x50},
      {BX_KEY_DOWN, 0x51},          {BX_KEY_UP, 0x52},
      {BX_KEY_NUM_LOCK, 0x53},      {BX_KEY_KP_DIVIDE, 0x54},
      {BX_KEY_KP_MULTIPLY, 0x55},   {BX_KEY_KP_SUBTRACT, 0x56},
      {BX_KEY_KP_ADD, 0x57},        {BX_KEY_KP_ENTER, 0x58},
      {BX_KEY_KP_END, 0x59},        {BX_KEY_KP_DOWN, 0x5a},
      {BX_KEY_KP_PAGE_DOWN, 0x5b},  {BX_KEY_KP_LEFT, 0x5c},
      {BX_KEY_KP_5, 0x5d},          {BX_KEY_KP_RIGHT, 0x5e},
      {BX_KEY_KP_HOME, 0x5f},       {BX_KEY_KP_UP, 0x60},
      {BX_KEY_KP_PAGE_UP, 0x61},    {BX_KEY_KP_INSERT, 0x62},
      {BX_KEY_KP_DELETE, 0x63},     {BX_KEY_LEFT_BACKSLASH, 0x64},
      {BX_KEY_MENU, 0x65},          {BX_KEY_ALT_SYSREQ, 0x46},
      {BX_KEY_CTRL_BREAK, 0x48},    {BX_KEY_INT_BACK, 0x89},
  };
  if (bx >= BX_KEY_A && bx <= BX_KEY_Z)
    return (u8)(0x04 + (bx - BX_KEY_A));
  if (bx >= BX_KEY_1 && bx <= BX_KEY_9)
    return (u8)(0x1e + (bx - BX_KEY_1));
  if (bx == BX_KEY_0)
    return 0x27;
  if (bx >= BX_KEY_F1 && bx <= BX_KEY_F12)
    return (u8)(0x3a + (bx - BX_KEY_F1));
  for (const auto &e : table)
    if (e.bx == bx)
      return e.usage;
  return 0;
}

void CUsbKeyboard::reset() {
  CUsbDevice::reset();
  std::lock_guard<std::mutex> lk(m_mx);
  m_idle = 125;
  m_protocol = 1;
  m_leds = 0;
  m_pending.clear();
  m_changed = true;
}

// The HID state and the keys held; the keys are released at a restore (the
// host's keys are not where they were).
bool CUsbKeyboard::save(CUsbSaved &s) const {
  CUsbDevice::save(s);
  std::lock_guard<std::mutex> lk(m_mx);
  s.put((u32)m_idle);
  s.put((u32)m_protocol);
  s.put(m_leds);
  return true;
}

bool CUsbKeyboard::load(CUsbSaved &s) {
  if (!CUsbDevice::load(s))
    return false;
  std::lock_guard<std::mutex> lk(m_mx);
  m_idle = (int)s.get();
  m_protocol = (int)s.get();
  m_leds = (u8)s.get();
  m_modifiers = 0;
  memset(m_keys, 0, sizeof(m_keys));
  m_pending.clear();
  m_changed = true;
  return s.ok;
}

const std::vector<u8> &CUsbKeyboard::device_descriptor() const {
  return kDeviceDescriptor;
}

const std::vector<u8> &CUsbKeyboard::configuration_descriptor() const {
  return kConfigurationDescriptor;
}

std::vector<u8> CUsbKeyboard::string_descriptor(int index) const {
  switch (index) {
  case 1:
    return utf16_string("Alphabox");
  case 2:
    return utf16_string("Alphabox USB Keyboard");
  case 3:
    return utf16_string("1");
  default:
    return {};
  }
}

// GET_DESCRIPTOR to the interface: the HID descriptor or the report
// descriptor.
bool CUsbKeyboard::other_descriptor(const u8 *setup, std::vector<u8> &out) {
  if ((setup[0] & 0x1f) == 0)
    return false; // a full-speed device: no other-speed views
  const int type = setup[3];
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

bool CUsbKeyboard::class_request(const u8 *setup, const std::vector<u8> &data,
                                 std::vector<u8> &out) {
  std::lock_guard<std::mutex> lk(m_mx);
  switch (setup[1]) {
  case 0x01: // GET_REPORT: the output report (the LEDs) or the input one
    if (setup[3] == 2) {
      out = {m_leds};
      return true;
    }
    out = report();
    return true;
  case 0x02: // GET_IDLE
    out = {(u8)m_idle};
    return true;
  case 0x03: // GET_PROTOCOL
    out = {(u8)m_protocol};
    return true;
  case 0x09: // SET_REPORT: the LEDs
    if (!data.empty())
      m_leds = data[0] & 0x1f;
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

// Called with m_mx held. The boot report, which the report protocol's
// descriptor describes too.
std::vector<u8> CUsbKeyboard::report() const {
  std::vector<u8> r = {m_modifiers, 0};
  r.insert(r.end(), m_keys, m_keys + 6);
  return r;
}

// A change of the keys held: the new state is queued as a report of its own
// (at most 64 waiting; beyond that a change is only seen in the latest
// state, as when the host does not poll).
void CUsbKeyboard::changed() {
  if (m_pending.size() < 64)
    m_pending.push_back(report());
  m_changed = true;
}

// The interrupt endpoint: the next change queued, or the current state when
// the idle duration has passed since the last report (0: only on change);
// else NAK.
CUsbDevice::Result CUsbKeyboard::data_in(int ep, u8 *buf, int &len) {
  if (ep != 1 || !m_configuration) {
    len = 0;
    return USB_STALL;
  }
  std::lock_guard<std::mutex> lk(m_mx);
  const auto now = std::chrono::steady_clock::now();
  const bool idle_due =
      m_idle && now - m_last_report >= std::chrono::milliseconds(4 * m_idle);
  if (!m_changed && !idle_due) {
    len = 0;
    return USB_NAK;
  }
  std::vector<u8> r;
  if (!m_pending.empty()) {
    r = m_pending.front();
    m_pending.pop_front();
  } else {
    r = report();
  }
  m_changed = !m_pending.empty();
  m_last_report = now;
  len = std::min(len, (int)r.size());
  memcpy(buf, r.data(), len);
  return USB_ACK;
}

void CUsbKeyboard::key(u32 key_event) {
  const bool release = (key_event & BX_KEY_RELEASED) != 0;
  const u8 usage = hid_usage(key_event & ~BX_KEY_RELEASED);
  if (!usage)
    return;
  std::lock_guard<std::mutex> lk(m_mx);
  if (usage >= 0xe0) {
    const u8 bit = (u8)(1u << (usage - 0xe0));
    const u8 was = m_modifiers;
    m_modifiers = release ? (u8)(m_modifiers & ~bit) : (u8)(m_modifiers | bit);
    if (m_modifiers != was)
      changed();
    return;
  }
  u8 *held = std::find(m_keys, m_keys + 6, usage);
  if (release) {
    if (held == m_keys + 6)
      return;
    // Close the gap: the array lists the keys in the order pressed.
    std::copy(held + 1, m_keys + 6, held);
    m_keys[5] = 0;
    changed();
  } else if (held == m_keys + 6) {
    u8 *free_slot = std::find(m_keys, m_keys + 6, (u8)0);
    if (free_slot == m_keys + 6)
      return; // a seventh key: dropped (no phantom-state report)
    *free_slot = usage;
    changed();
  }
}

unsigned CUsbKeyboard::leds() const {
  std::lock_guard<std::mutex> lk(m_mx);
  return m_leds;
}
