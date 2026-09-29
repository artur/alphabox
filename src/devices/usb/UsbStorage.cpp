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

#include "UsbStorage.hpp"
#include "Disk.hpp"
#include "SCSIBus.hpp"
#include "StdAfx.hpp"
#include <algorithm>
#include <cstring>

static const bool g_usbtrace = getenv("ALPHABOX_USBTRACE") != nullptr;

static const u32 kCbwSignature = 0x43425355; // "USBC"
static const u32 kCswSignature = 0x53425355; // "USBS"
enum { CSW_PASSED = 0, CSW_FAILED = 1, CSW_PHASE_ERROR = 2 };

static const std::vector<u8> kDeviceDescriptor = {
    18,   1,       // bLength, DEVICE
    0x10, 0x01,    // USB 1.1
    0,    0,    0, // class in the interface
    64,            // endpoint 0 max packet
    0x09, 0x12,    // idVendor 0x1209 (pid.codes)
    0x02, 0xa1,    // idProduct 0xa102
    0x00, 0x01,    // bcdDevice 1.00
    1,    2,    3, // manufacturer, product, serial strings
    1,             // one configuration
};

static const std::vector<u8> kConfigurationDescriptor = {
    // Configuration
    9, 2, 32, 0, // bLength, CONFIGURATION, wTotalLength 32
    1,           // one interface
    1,           // bConfigurationValue
    0,           // no string
    0x80,        // bus-powered
    50,          // 100 mA
    // Interface 0: mass storage, SCSI transparent command set, Bulk-Only
    9, 4, 0, 0, 2, 0x08, 0x06, 0x50, 0,
    // Endpoint 1 IN, bulk, 64 bytes
    7, 5, 0x81, 2, 64, 0, 0,
    // Endpoint 2 OUT, bulk, 64 bytes
    7, 5, 0x02, 2, 64, 0, 0};

CUsbStorage::CUsbStorage(CSCSIBus *bus, CDisk *disk) : m_disk(disk) {
  // Numbered in configuration order, so a machine keeps its serials.
  static int instances = 0;
  snprintf(m_serial, sizeof(m_serial), "0000A1FA%04X", ++instances);
  scsi_register(0, bus, 7);
  disk->scsi_register(0, bus, 0);
  disk->set_atapi_mode(); // command, data, status: no message phases
}

void CUsbStorage::reset() {
  CUsbDevice::reset();
  // A command the host abandoned mid-data leaves the disk selected; free
  // the bus so the next command selects it afresh.
  if (scsi_get_phase(0) != SCSI_PHASE_FREE)
    scsi_bus[0]->reset_bus();
  m_stage = BOT_CBW;
  m_buf.clear();
}

// At high speed: USB 2.0, and 512-byte bulk packets (the only size a
// high-speed bulk endpoint may have).
static std::vector<u8> high_speed_form(std::vector<u8> d, bool config) {
  if (!config) {
    d[2] = 0x00; // bcdUSB 2.00
    d[3] = 0x02;
    return d;
  }
  for (size_t p = 0; p + 2 <= d.size() && d[p] >= 2; p += d[p])
    if (d[p + 1] == 5) { // endpoint: 512 bytes
      d[p + 4] = 0x00;
      d[p + 5] = 0x02;
    }
  return d;
}
static const std::vector<u8> kDeviceDescriptorHS =
    high_speed_form(kDeviceDescriptor, false);
static const std::vector<u8> kConfigurationDescriptorHS =
    high_speed_form(kConfigurationDescriptor, true);

const std::vector<u8> &CUsbStorage::device_descriptor() const {
  return m_hs ? kDeviceDescriptorHS : kDeviceDescriptor;
}

const std::vector<u8> &CUsbStorage::configuration_descriptor() const {
  return m_hs ? kConfigurationDescriptorHS : kConfigurationDescriptor;
}

// DEVICE_QUALIFIER (6): what the device would be at the other speed -- a
// USB 2.0 host asks, to tell a high-speed-capable device from one that is
// not. OTHER_SPEED_CONFIGURATION (7): that speed's configuration.
bool CUsbStorage::other_descriptor(const u8 *setup, std::vector<u8> &out) {
  if ((setup[0] & 0x1f) != 0)
    return false;
  if (setup[3] == 6) {
    out = {10, 6, 0x00, 0x02, 0, 0, 0, 64, 1, 0};
    return true;
  }
  if (setup[3] == 7) {
    out = m_hs ? kConfigurationDescriptor : kConfigurationDescriptorHS;
    out[1] = 7;
    return true;
  }
  return false;
}

std::vector<u8> CUsbStorage::string_descriptor(int index) const {
  switch (index) {
  case 1:
    return utf16_string("Alphabox");
  case 2:
    return utf16_string("Alphabox USB Disk");
  case 3:
    // Bulk-Only Transport asks for at least 12 hexadecimal digits. Each disk
    // has its own: a host sees two disks with one serial as one device
    // (Windows 2000 bugchecks 0xCA on two copies of an image).
    return utf16_string(m_serial);
  default:
    return {};
  }
}

bool CUsbStorage::class_request(const u8 *setup, const std::vector<u8> &data,
                                std::vector<u8> &out) {
  (void)data;
  switch (setup[1]) {
  case 0xff: // Bulk-Only Mass Storage Reset
    reset();
    return true;
  case 0xfe: // Get Max LUN: one logical unit
    out = {0};
    return true;
  default:
    return false;
  }
}

// A Command Block Wrapper: run the SCSI command up to its data stage. A
// device-to-host command runs to completion here (the disk's answer is held
// for the IN transfers that follow); a host-to-device one waits for its data.
void CUsbStorage::command(const u8 *cbw) {
  auto le32 = [](const u8 *p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
  };
  m_tag = le32(cbw + 4);
  m_expected = le32(cbw + 8);
  const bool host_in = (cbw[12] & 0x80) != 0;
  const int cblen = cbw[14] & 0x1f;
  m_moved = 0;
  m_pos = 0;
  m_buf.clear();
  if (g_usbtrace)
    printf("USBT storage cbw tag %08x len %u %s cdb %02x\n", m_tag, m_expected,
           host_in ? "in" : "out", cbw[15]);

  if ((cbw[13] & 0x0f) != 0 || cblen < 1 || cblen > 16) {
    m_status = CSW_FAILED; // no such LUN, or a malformed CDB
    m_stage = m_expected && host_in ? BOT_DATA_IN : BOT_CSW;
    if (m_expected && !host_in)
      m_stage = BOT_DATA_OUT; // swallow the data, then report
    return;
  }

  if (!scsi_arbitrate(0) || !scsi_select(0, 0))
    FAILURE(IllegalState, "USB storage: disk not responding to selection");
  memcpy(scsi_xfer_ptr(0, cblen), cbw + 15, cblen);
  scsi_xfer_done(0);

  switch (scsi_get_phase(0)) {
  case SCSI_PHASE_DATA_IN:
    while (scsi_get_phase(0) == SCSI_PHASE_DATA_IN) {
      const size_t n = scsi_expected_xfer(0);
      const u8 *p = (const u8 *)scsi_xfer_ptr(0, n);
      m_buf.insert(m_buf.end(), p, p + n);
      scsi_xfer_done(0);
    }
    finish();
    if (!host_in || m_buf.size() > m_expected) {
      // The host asked for less (or for the other direction): phase error.
      m_status = CSW_PHASE_ERROR;
      m_buf.resize(host_in ? m_expected : 0);
    }
    m_stage = host_in && m_expected    ? BOT_DATA_IN
              : !host_in && m_expected ? BOT_DATA_OUT
                                       : BOT_CSW;
    break;

  case SCSI_PHASE_DATA_OUT:
    if (host_in || !m_expected) {
      // The disk wants data the host will not send.
      scsi_bus[0]->reset_bus();
      m_status = CSW_PHASE_ERROR;
      m_stage = host_in && m_expected ? BOT_DATA_IN : BOT_CSW;
    } else {
      m_stage = BOT_DATA_OUT;
    }
    break;

  default: // no data stage
    finish();
    m_stage = m_expected ? (host_in ? BOT_DATA_IN : BOT_DATA_OUT) : BOT_CSW;
    break;
  }
}

// The SCSI status phase: GOOD passes, anything else fails (the host then
// asks for the sense data with REQUEST SENSE).
void CUsbStorage::finish() {
  u8 status = 0;
  if (scsi_get_phase(0) == SCSI_PHASE_STATUS) {
    const size_t n = scsi_expected_xfer(0);
    if (n)
      status = *(const u8 *)scsi_xfer_ptr(0, n);
    scsi_xfer_done(0);
  }
  if (scsi_get_phase(0) != SCSI_PHASE_FREE)
    scsi_bus[0]->reset_bus();
  m_status = status ? CSW_FAILED : CSW_PASSED;
}

CUsbDevice::Result CUsbStorage::data_out(int ep, const u8 *buf, int len) {
  if (ep != 2 || !m_configuration)
    return USB_STALL;
  if (m_stage == BOT_CBW) {
    if (len != 31 || (u32)(buf[0] | (buf[1] << 8) | (buf[2] << 16) |
                           ((u32)buf[3] << 24)) != kCbwSignature)
      return USB_STALL; // not a valid CBW: the host resets
    command(buf);
    return USB_ACK;
  }
  if (m_stage != BOT_DATA_OUT)
    return USB_STALL;
  const int n = std::min(len, (int)(m_expected - m_moved));
  m_buf.insert(m_buf.end(), buf, buf + n);
  m_moved += n;
  if (m_moved < m_expected)
    return USB_ACK;
  // The host's data is all here: hand the disk what it asked for. The
  // residue counts what the command did not take.
  u32 taken = 0;
  if (scsi_get_phase(0) == SCSI_PHASE_DATA_OUT) {
    const size_t want = scsi_expected_xfer(0);
    if (m_buf.size() >= want) {
      memcpy(scsi_xfer_ptr(0, want), m_buf.data(), want);
      scsi_xfer_done(0);
      finish();
      taken = (u32)want;
    } else {
      scsi_bus[0]->reset_bus(); // too little data: the command cannot run
      m_status = CSW_PHASE_ERROR;
    }
  }
  m_moved = taken;
  m_buf.clear();
  m_stage = BOT_CSW;
  return USB_ACK;
}

CUsbDevice::Result CUsbStorage::data_in(int ep, u8 *buf, int &len) {
  if (ep != 1 || !m_configuration) {
    len = 0;
    return USB_STALL;
  }
  if (m_stage == BOT_DATA_IN) {
    // The data, then -- when the disk had less than the host asked for -- a
    // short (possibly empty) packet to end the stage.
    const int n = std::min(len, (int)(m_buf.size() - m_pos));
    memcpy(buf, m_buf.data() + m_pos, n);
    m_pos += n;
    m_moved += n;
    if (m_pos == m_buf.size() && (m_moved == m_expected || n < len)) {
      m_stage = BOT_CSW;
      m_buf.clear();
    }
    len = n;
    return USB_ACK;
  }
  if (m_stage == BOT_CSW) {
    if (len < 13) {
      len = 0;
      return USB_STALL;
    }
    const u32 residue = m_expected - std::min(m_moved, m_expected);
    const u32 w[3] = {kCswSignature, m_tag, residue};
    for (int i = 0; i < 12; ++i)
      buf[i] = (u8)(w[i / 4] >> (8 * (i % 4)));
    buf[12] = m_status;
    len = 13;
    if (g_usbtrace)
      printf("USBT storage csw tag %08x residue %u status %u\n", m_tag, residue,
             m_status);
    m_stage = BOT_CBW;
    return USB_ACK;
  }
  len = 0;
  return USB_NAK; // no command outstanding
}
