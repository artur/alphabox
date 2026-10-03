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

#if !defined(INCLUDED_USBKEYBOARD_H)
#define INCLUDED_USBKEYBOARD_H

#include "UsbDevice.hpp"
#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>

/**
 * \brief A USB HID keyboard.
 *
 * The keyboard a machine without an 8042 has -- the EV7 AlphaServers, whose
 * OpenVMS reads its keyboard as KBD0 through the USB HID class driver (and
 * DECwindows waits for one before it starts its server). It is a boot
 * keyboard, interface subclass 1 protocol 1, whose report is the boot
 * protocol's in either protocol: the modifier byte, a reserved byte and up
 * to six keys pressed, as HID usages; the LEDs come back as an output report
 * and are kept. Full speed, one interrupt IN endpoint polled every 10 ms.
 *
 * Keys come from the GUI as the emulator's key codes (BX_KEY_*, gui.hpp),
 * the ones the PS/2 keyboard turns into scan codes. Each change of the
 * keys held is a report of its own, queued until the host polls for it: a
 * press and its release that come between two polls (a scripted key) are
 * two reports, not none.
 *
 * Documentation consulted: Device Class Definition for HID 1.11 (appendix
 * B.1, the boot keyboard's descriptor and report); HID Usage Tables 1.12
 * (chapter 10, the Keyboard/Keypad page).
 **/
class CUsbKeyboard : public CUsbDevice {
public:
  CUsbKeyboard() = default;
  const char *name() const override { return "keyboard"; }
  void reset() override;
  bool save(CUsbSaved &s) const override;
  bool load(CUsbSaved &s) override;

  /// A key pressed or released: a BX_KEY_* code, with BX_KEY_RELEASED for
  /// the release. Thread-safe.
  void key(u32 key_event);

  /// The LEDs the host set (bit 0 Num Lock, 1 Caps Lock, 2 Scroll Lock).
  unsigned leds() const;

protected:
  const std::vector<u8> &device_descriptor() const override;
  const std::vector<u8> &configuration_descriptor() const override;
  std::vector<u8> string_descriptor(int index) const override;
  bool other_descriptor(const u8 *setup, std::vector<u8> &out) override;
  bool class_request(const u8 *setup, const std::vector<u8> &data,
                     std::vector<u8> &out) override;
  Result data_in(int ep, u8 *buf, int &len) override;

private:
  std::vector<u8> report() const; // with m_mx held
  void changed();                 // with m_mx held: queue the new state
  mutable std::mutex m_mx;        // the key state, written by the GUI thread
  u8 m_modifiers = 0;
  u8 m_keys[6] = {};
  std::deque<std::vector<u8>> m_pending; // reports not yet polled
  bool m_changed = false;
  u8 m_leds = 0;
  int m_idle = 125;   // SET_IDLE duration, 4 ms units (HID 7.2.4: 500 ms)
  int m_protocol = 1; // report protocol
  std::chrono::steady_clock::time_point m_last_report; // for the idle rate
};

/// The keyboard on the USB, if one is configured; the GUI feeds it.
extern std::atomic<CUsbKeyboard *> theUsbKeyboard;

#endif
