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

/**
 * \file
 * The monitor on the Mach64's DDC channel.
 *
 * The chip has two general-purpose open-drain lines in DAC_CNTL, byte 3:
 * bit 29 drives line 0 (SCL) low when bit 26 is clear, bit 28 drives line
 * 1 (SDA) low when bit 25 is clear, and bits 2 and 1 of the byte read the
 * lines back. The BIOS and the Windows miniport bit-bang the DDC2 protocol
 * on them to read the monitor's EDID, and both want a monitor to be
 * there: the BIOS turns to it for the timings of an accelerated mode when
 * the EEPROM holds none. The monitor is a 24C02 at 0x50 (the shared
 * Eeprom24C02) holding a generic multisync CRT's EDID 1.3 block --
 * 640x480 to 1600x1200, 50-100 Hz, 1024x768 at 60 Hz preferred.
 **/

#include "Mach64.hpp"

using namespace mach64;

// A generic 17" multisync CRT ("ALB", model 0x64), analogue input, DPMS,
// 1024x768@60 detailed timing, range limits, name and serial descriptors.
// clang-format off
static const u8 kMonitorEdid[128] = {
    0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x05, 0x82, 0x64, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x06, 0x01, 0x03, 0x0e, 0x22, 0x1b, 0x78, 0xe8, 0xee, 0x91, 0xa3, 0x54, 0x4c, 0x99, 0x26,
    0x0f, 0x50, 0x54, 0xa5, 0xcf, 0x00, 0x81, 0x80, 0x71, 0x4a, 0xa9, 0x40, 0x45, 0x59, 0x61, 0x59,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x64, 0x19, 0x00, 0x40, 0x41, 0x00, 0x26, 0x30, 0x18, 0x88,
    0x36, 0x00, 0x54, 0x0e, 0x11, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00, 0xfd, 0x00, 0x32, 0x64, 0x1e,
    0x52, 0x0e, 0x00, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x00, 0x00, 0x00, 0xfc, 0x00, 0x41,
    0x6c, 0x70, 0x68, 0x61, 0x62, 0x6f, 0x78, 0x20, 0x43, 0x52, 0x54, 0x0a, 0x00, 0x00, 0x00, 0xff,
    0x00, 0x45, 0x53, 0x34, 0x30, 0x2d, 0x30, 0x30, 0x30, 0x31, 0x0a, 0x20, 0x20, 0x20, 0x00, 0x98,
};
// clang-format on

void CMach64::ddc_attach_monitor() {
  m_ddc.attach(std::make_shared<Eeprom24C02>(
      0x50, std::vector<u8>(kMonitorEdid, kMonitorEdid + sizeof(kMonitorEdid))));
  ddc_drive();
}

/**
 * Put DAC_CNTL's line drivers on the bus. A line is released unless its
 * direction bit says the chip drives it and its state bit is low.
 **/
void CMach64::ddc_drive() {
  const bool scl_release =
      !(r.dac_cntl & DAC_GIO_DIR_0) || (r.dac_cntl & DAC_GIO_STATE_0);
  const bool sda_release =
      !(r.dac_cntl & DAC_GIO_DIR_1) || (r.dac_cntl & DAC_GIO_STATE_1);
  m_ddc.drive_from_host(scl_release, sda_release);
}

/**
 * DAC_CNTL byte 3 with the lines' levels in bits 2 (SCL) and 1 (SDA).
 **/
u8 CMach64::dac_gio_read(u8 byte3) const {
  u8 v = byte3 & 0xf9;
  if (m_ddc.scl())
    v |= 0x04;
  if (m_ddc.sda())
    v |= 0x02;
  return v;
}
