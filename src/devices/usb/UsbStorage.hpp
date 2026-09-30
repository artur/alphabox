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

#if !defined(INCLUDED_USBSTORAGE_H)
#define INCLUDED_USBSTORAGE_H

#include "SCSIDevice.hpp"
#include "UsbDevice.hpp"

class CDisk;
class CSCSIBus;

/**
 * \brief A USB mass storage device: one disk behind the Bulk-Only Transport.
 *
 * The host sends a Command Block Wrapper (a SCSI CDB with the expected
 * length and direction) on the bulk OUT endpoint, moves the data on the
 * bulk IN or OUT endpoint, and collects a Command Status Wrapper from the
 * bulk IN endpoint. The SCSI command itself is CDisk's: this device is the
 * initiator on a private SCSI bus with the disk as target 0, the way the
 * IDE controller's ATAPI path drives it (no message phases).
 *
 * Documentation consulted: USB Mass Storage Class Bulk-Only Transport 1.0;
 * USB Mass Storage Class Specification Overview 1.2 (subclass 06h, the SCSI
 * transparent command set).
 **/
class CUsbStorage : public CUsbDevice, public CSCSIDevice {
public:
  CUsbStorage(CSCSIBus *bus, CDisk *disk);
  const char *name() const override { return "storage"; }
  bool can_high_speed() const override { return true; }
  void reset() override;
  void endpoint_halted(int ep_addr) override;
  bool inject_phase_error() override {
    m_phase_error = true;
    return true;
  }
  bool save(CUsbSaved &s) const override;
  bool load(CUsbSaved &s) override;

protected:
  const std::vector<u8> &device_descriptor() const override;
  const std::vector<u8> &configuration_descriptor() const override;
  std::vector<u8> string_descriptor(int index) const override;
  bool other_descriptor(const u8 *setup, std::vector<u8> &out) override;
  bool class_request(const u8 *setup, const std::vector<u8> &data,
                     std::vector<u8> &out) override;
  Result data_in(int ep, u8 *buf, int &len) override;
  Result data_out(int ep, const u8 *buf, int len) override;

private:
  void command(const u8 *cbw);
  void transport_reset();
  void finish(); // the SCSI status, and the CSW it becomes

  CDisk *m_disk;
  char m_serial[16]; // the iSerialNumber string, one per disk
  // Bulk-Only Transport state: waiting for a CBW, moving data, or holding
  // the CSW for the host to collect.
  enum Stage { BOT_CBW, BOT_DATA_IN, BOT_DATA_OUT, BOT_CSW };
  Stage m_stage = BOT_CBW;
  u32 m_tag = 0;
  u32 m_expected = 0;    // dCBWDataTransferLength
  u32 m_moved = 0;       // bytes of the data stage moved so far
  std::vector<u8> m_buf; // the data stage (IN: the disk's answer)
  size_t m_pos = 0;
  u8 m_status = 0;            // bCSWStatus
  bool m_phase_error = false; // usb:phase: the next CSW says phase error
};

#endif
