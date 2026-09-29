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
#include <atomic>
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
  virtual void set_high_speed(bool hs) { m_hs = hs; }
  bool high_speed() const { return m_hs; }

  /// Set by the controller: a device whose transfers finish on another
  /// thread (a host device behind libusb) calls it when one does, so the
  /// controller retries the NAKed TD at once instead of at the next frame.
  /// Called without the device's own locks held.
  std::function<void()> on_complete;
  virtual const char *name() const = 0;
  int address() const { return m_address; }

  /// Test faults (see CUsbPortFaults). The endpoint `ep_addr` (0x81 = IN
  /// 1, 0x02 = OUT 2) was just made to halt: a device with a transfer in
  /// progress there moves on as a real one does after a STALL.
  virtual void endpoint_halted(int ep_addr) { (void)ep_addr; }
  /// usb:phase -- the device's next command ends in a phase error, where
  /// that means something (USB mass storage). False: not supported.
  virtual bool inject_phase_error() { return false; }

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

/**
 * \brief Faults a test injects on one root hub port (ALPHABOX_KEYPIPE /
 * ALPHABOX_KEYSCRIPT tokens usb:detach, usb:attach, usb:stall, usb:nak;
 * docs/headless.md). The controller asks intercept() before each transfer
 * to the port's device, under its own lock.
 *
 * A stalled endpoint behaves as a halted one does: every transaction to it
 * is answered STALL until the host clears the halt with CLEAR_FEATURE
 * (ENDPOINT_HALT) -- seen here as it goes past to the device -- and it
 * halts again on its next transaction while injections remain.
 **/
struct CUsbPortFaults {
  bool unplugged = false; // usb:detach: the port shows nothing connected
  int stall_ep = -1;      // endpoint address to halt (0x81, 0x02, ...)
  int stall_left = 0;     // halts still to inject there
  u32 halted = 0;         // halted endpoints: bit (num | in << 4)
  int nak_ep = -1;        // usb:nak: this endpoint NAKs ...
  u64 nak_until_ms = 0;   // ... until then (steady clock, ms)

  static int bit(int ep_addr) {
    return (ep_addr & 15) | ((ep_addr & 0x80) >> 3);
  }
  /// Before the device sees a transfer: true, with the answer in r, when
  /// the fault answers it instead.
  bool intercept(CUsbDevice *dev, int pid, int ep, const u8 *buf, int len,
                 CUsbDevice::Result &r);
};

/// A controller that takes injected faults: the EHCI card when there is
/// one, else the ALi's OHCI. op is "detach", "attach", "stall", "phase" or
/// "nak"; port counts from 1. False: no such port or device.
class CUsbFaultTarget {
public:
  virtual ~CUsbFaultTarget() = default;
  virtual bool inject_fault(const char *op, int port, int ep, int arg) = 0;
};
/// Where usb:... tokens go (see CUsbFaultTarget).
extern std::atomic<CUsbFaultTarget *> theUsbFaultTarget;

#endif
