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

#if !defined(INCLUDED_USBTABLET_H)
#define INCLUDED_USBTABLET_H

#include "UsbDevice.hpp"
#include <atomic>
#include <mutex>

/**
 * \brief A USB HID pointing device with absolute coordinates.
 *
 * The guest's cursor goes where the host's is, with no relative motion to
 * accelerate and no need to capture the host mouse: the report carries the
 * position as 0..32767 on each axis, three buttons and a wheel. It is the
 * report layout every HID stack with absolute-pointer support understands
 * (Windows 2000's mouhid among them), full speed, one interrupt IN endpoint
 * polled every 10 ms.
 *
 * Documentation consulted: Device Class Definition for HID 1.11; HID Usage
 * Tables 1.12 (Generic Desktop page: X, Y, Wheel; Button page).
 **/
class CUsbTablet : public CUsbDevice {
public:
  CUsbTablet();
  const char *name() const override { return "tablet"; }
  /// On an EHCI port it runs at high speed: USB 2.0, polled every 1 ms.
  bool can_high_speed() const override { return true; }
  void reset() override;

  /// The host pointer, as fractions of the guest screen (0..1, clamped),
  /// and the buttons (bit 0 left, 1 right, 2 middle). Thread-safe.
  void set_position(double x, double y, unsigned buttons);
  void set_buttons(unsigned buttons);
  void add_wheel(int dz);

protected:
  const std::vector<u8> &device_descriptor() const override;
  const std::vector<u8> &configuration_descriptor() const override;
  std::vector<u8> string_descriptor(int index) const override;
  bool other_descriptor(const u8 *setup, std::vector<u8> &out) override;
  bool class_request(const u8 *setup, const std::vector<u8> &data,
                     std::vector<u8> &out) override;
  Result data_in(int ep, u8 *buf, int &len) override;

private:
  std::vector<u8> report();
  std::mutex m_mx; // the pointer state, written by the GUI thread
  int m_x = 16384, m_y = 16384;
  unsigned m_buttons = 0;
  int m_wheel = 0;
  bool m_changed = true;
  int m_idle = 0;     // SET_IDLE duration, 4 ms units; 0 = only on change
  int m_protocol = 1; // report protocol
};

/// The tablet on the USB, if one is configured; the GUI feeds it.
extern std::atomic<CUsbTablet *> theUsbTablet;

#endif
