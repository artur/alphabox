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

#if !defined(INCLUDED_USBDEVICE_H)
#define INCLUDED_USBDEVICE_H

#include <cstdint> // datatypes.hpp needs the fixed-width types

#include "datatypes.hpp"
#include <functional>
#include <vector>

/**
 * \brief A device on the emulated USB, as a host controller sees it.
 *
 * The controller hands it one transfer at a time -- a SETUP, OUT or IN to
 * an endpoint, with the TD's whole buffer -- and it answers ACK (with the
 * byte count), NAK (nothing yet: try again next frame) or STALL. Data
 * toggles and packetisation are the controller's business; an emulated
 * device has no wire to corrupt them.
 *
 * Endpoint 0 is handled here: the three stages of a control transfer, and
 * the standard requests (descriptors, address, configuration, features,
 * status). A device supplies its descriptors, its class requests and its
 * other endpoints.
 *
 * Documentation consulted: Universal Serial Bus Specification 1.1,
 * chapter 9 (device framework).
 **/
class CUsbDevice {
public:
  enum Result { USB_ACK, USB_NAK, USB_STALL };
  enum Pid { PID_SETUP, PID_OUT, PID_IN };

  virtual ~CUsbDevice() = default;

  /// One transfer. For SETUP and OUT, buf holds len bytes; for IN, up to
  /// len bytes may be written and len is set to the count. Called with the
  /// controller's lock held. A device that is not emulated here (a host
  /// device passed through) takes over the whole of it, endpoint 0 included.
  virtual Result transfer(int pid, int ep, u8 *buf, int &len);

  /// Bus reset (the port was reset): default address, unconfigured.
  virtual void reset();

  virtual bool low_speed() const { return false; }
  /// Whether the device can run at high speed (USB 2.0); only then does an
  /// EHCI port keep it. The port it is on says which it is running at.
  virtual bool can_high_speed() const { return false; }
  void set_high_speed(bool hs) { m_hs = hs; }
  bool high_speed() const { return m_hs; }

  /// Set by the controller: a device whose transfers finish on another
  /// thread (a host device behind libusb) calls it when one does, so the
  /// controller retries the NAKed TD at once instead of at the next frame.
  /// Called without the device's own locks held.
  std::function<void()> on_complete;
  virtual const char *name() const = 0;
  int address() const { return m_address; }

protected:
  int m_configuration = 0;
  bool m_hs = false; // attached to a high-speed port, running at 480 Mb/s

  /// Standard descriptors. The configuration descriptor is the whole block
  /// (configuration + interfaces + class descriptors + endpoints).
  virtual const std::vector<u8> &device_descriptor() const = 0;
  virtual const std::vector<u8> &configuration_descriptor() const = 0;
  /// String descriptor `index` in UTF-16LE, or empty for none. Index 0 is
  /// the language list, answered here.
  virtual std::vector<u8> string_descriptor(int index) const;
  /// A descriptor the standard does not define (a class one, such as HID's
  /// report descriptor, asked for with GET_DESCRIPTOR). False: STALL.
  virtual bool other_descriptor(const u8 *setup, std::vector<u8> &out) {
    (void)setup;
    (void)out;
    return false;
  }
  /// A class or vendor request. For device-to-host requests, fill `out`;
  /// for host-to-device ones `data` holds the data stage. False: STALL.
  virtual bool class_request(const u8 *setup, const std::vector<u8> &data,
                             std::vector<u8> &out) {
    (void)setup;
    (void)data;
    (void)out;
    return false;
  }
  /// Endpoints other than 0.
  virtual Result data_in(int ep, u8 *buf, int &len) {
    (void)ep;
    (void)buf;
    len = 0;
    return USB_STALL;
  }
  virtual Result data_out(int ep, const u8 *buf, int len) {
    (void)ep;
    (void)buf;
    (void)len;
    return USB_STALL;
  }
  /// SET_CONFIGURATION or SET_INTERFACE took effect.
  virtual void configured() {}

  static std::vector<u8> utf16_string(const char *s);
  /// For a device that serves endpoint 0 itself (and so SET_ADDRESS).
  void set_address(int address) { m_address = address; }

private:
  Result control(int pid, u8 *buf, int &len);
  bool standard_request(const u8 *setup, const std::vector<u8> &data,
                        std::vector<u8> &out);

  int m_address = 0;
  int m_pending_address = -1; // SET_ADDRESS applies after the status stage
  // Endpoint 0: the SETUP being served, the data stage in either direction.
  enum Stage { ST_IDLE, ST_DATA_IN, ST_DATA_OUT, ST_STATUS };
  Stage m_stage = ST_IDLE;
  u8 m_setup[8] = {};
  std::vector<u8> m_ctl; // IN: the response; OUT: data received so far
  size_t m_ctl_pos = 0;
  bool m_ctl_stall = false;
};

#endif
