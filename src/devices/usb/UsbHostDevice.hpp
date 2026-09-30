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

#if !defined(INCLUDED_USBHOSTDEVICE_H)
#define INCLUDED_USBHOSTDEVICE_H

#include "UsbDevice.hpp"
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>

struct libusb_device_handle;
struct libusb_transfer;

/**
 * \brief A real USB device of the host, passed through to the guest (libusb).
 *
 * The guest's transfers go to the device as they are: control, bulk,
 * interrupt and isochronous. Nothing waits on the hardware in the
 * controller's frame: a transfer is submitted to libusb the first time the
 * guest's TD asks for it, and the guest's retries are NAKed until it
 * completes -- which is what a real device does while it has nothing to
 * say. An isochronous pipe has no NAK: its packets stream through libusb
 * isochronous transfers (IsoStream), and a packet that is not there when
 * the guest's (micro)frame wants it is reported as the controller's own
 * overrun or underrun. A few requests cannot be forwarded and are carried
 * out here: SET_ADDRESS (the host already addressed the device),
 * SET_CONFIGURATION and SET_INTERFACE (libusb must know, to claim the
 * interfaces; the host device's configuration is changed only to a
 * different, non-zero one), and clearing an endpoint's halt.
 *
 * On the OHCI (full-speed) controller, a high-speed device's descriptors
 * are rewritten on their way to the guest to what a full-speed port allows
 * (64-byte bulk and interrupt packets, intervals in frames); on an EHCI
 * port they pass unchanged. libusb moves the data at whatever speed the
 * device really runs -- which a high-speed isochronous endpoint cannot do
 * through a full-speed controller: such a device belongs on the EHCI.
 *
 * On macOS the host keeps interfaces its own drivers hold (keyboards, mice,
 * storage, audio): the guest can enumerate such a device, but not claim
 * those interfaces. A device with no host driver is fully usable.
 **/
class CUsbHostDevice : public CUsbDevice {
public:
  /// `spec` is "vvvv:pppp", the device's vendor and product IDs in hex.
  explicit CUsbHostDevice(const char *spec);
  ~CUsbHostDevice() override;
  const char *name() const override { return m_name.c_str(); }
  Result transfer(int pid, int ep, u8 *buf, int &len) override;
  int iso_transfer(int pid, int ep, u8 *buf, int len) override;
  bool iso_endpoint(int pid, int ep) const override;
  void reset() override;
  bool low_speed() const override { return m_low_speed; }
  bool can_high_speed() const override { return m_high_speed; }
  /// Real hardware: its state cannot be saved with the machine.
  bool save(CUsbSaved &) const override { return false; }

protected:
  // Endpoint 0 is served by transfer(); the base class's descriptors are
  // never asked for.
  const std::vector<u8> &device_descriptor() const override;
  const std::vector<u8> &configuration_descriptor() const override;

private:
  static constexpr int kControl = 32; // slot of endpoint 0
  /// One endpoint's transfer on its way through libusb: in flight while
  /// `xfer` is set, then `done` with the result until the guest takes it.
  struct Slot {
    libusb_transfer *xfer = nullptr;
    unsigned gen = 0; // bumped by a cancel: a late completion is dropped
    bool done = false;
    int status = 0;
    std::vector<u8> data;
  };
  friend struct HostXfer;
  static void completed(libusb_transfer *t);

  /// An isochronous endpoint's stream through libusb. OUT: each packet the
  /// guest sends is submitted at once as a transfer of its own, and forgotten
  /// (a full pipeline refuses it: ISO_OVERRUN). IN: while the guest reads
  /// the endpoint, kIsoInXfers transfers of kIsoPackets packets each are
  /// kept in flight, and each packet that arrives waits in `ready` for the
  /// guest's next read; a read that finds none is ISO_OVERRUN. The stream
  /// stops when the guest has not read for a while, and with a reset or a
  /// change of configuration or alternate setting (bumping `gen`, so late
  /// completions are dropped).
  struct IsoStream {
    unsigned gen = 0;
    bool on = false;
    int packet = 0; // IN: the endpoint's packet size
    std::vector<libusb_transfer *> xfers;
    std::deque<std::vector<u8>> ready;
    std::chrono::steady_clock::time_point last_ask;
  };
  static constexpr int kIsoOutMax = 32;   // OUT packets in flight
  static constexpr int kIsoInXfers = 4;   // IN transfers in flight
  static constexpr int kIsoPackets = 8;   // packets in each
  static constexpr size_t kIsoReady = 64; // IN packets waiting for the guest
  friend struct IsoXfer;
  static void iso_completed(libusb_transfer *t);
  bool iso_submit(int slot, u8 ep_addr, const u8 *out, int len, int packets);
  void iso_stop(int slot); // with m_mx held
  IsoStream m_iso[32];
  bool submit(int slot, u8 ep_addr, int type, const u8 *out, int len);
  void cancel(int slot);
  Result take(int slot, u8 *buf, int &len, size_t skip);

  Result control(int pid, u8 *buf, int &len);
  bool local_request(bool &ok);
  bool set_configuration(int value);
  bool set_interface(int iface, int alt);
  void learn_endpoints();
  void to_full_speed(std::vector<u8> &d) const;

  libusb_device_handle *m_h = nullptr;
  std::string m_name;
  bool m_low_speed = false;
  bool m_high_speed = false; // or faster: descriptors are rewritten

  std::mutex m_mx; // the slots: written by libusb's event thread
  std::condition_variable m_idle;
  int m_in_flight = 0;
  Slot m_slot[kControl + 1]; // [ep] OUT, [16 + ep] IN, [kControl]
  int m_ep_type[32];         // per slot: libusb's transfer type, -1 = none
  std::vector<int> m_claimed;

  // Endpoint 0: the request being served and how far it has got.
  enum Stage { C_IDLE, C_DATA_IN, C_DATA_OUT, C_STATUS_IN };
  Stage m_stage = C_IDLE;
  u8 m_setup[8] = {};
  std::vector<u8> m_out; // an OUT request's data stage, collected
  size_t m_pos = 0;      // bytes of an IN request's answer handed out
  bool m_local = false;  // answered here, not by the device
  bool m_local_ok = true;
  int m_pending_address = -1;
};

#endif
