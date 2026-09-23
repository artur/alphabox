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
 * the EEPROM holds none. The monitor is the shared one (MonitorEdid.hpp).
 **/

#include "Mach64.hpp"
#include "MonitorEdid.hpp"

using namespace mach64;

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

/**
 * The Rage Pro's I2C engine, as atirage.sys drives it: one byte per
 * command. The driver clears I2C_CNTL_0's status nibble, puts a byte to
 * send in I2C_CNTL_1 byte 0, and writes the command to I2C_CNTL_0 byte 1:
 * START first (the byte is then an address), RECEIVE to read a byte into
 * I2C_CNTL_1 byte 0, STOP to end the transfer after this byte. It then
 * polls the status for DONE, NACK or HALT. No register guide in hand
 * documents the engine; this is the protocol the driver's code implies.
 * The engine clocks the same lines as DAC_CNTL, with the transfer done by
 * the time the command's write returns.
 **/
void CMach64::i2c_line(bool scl, bool sda) { m_ddc.drive_from_host(scl, sda); }

void CMach64::i2c_engine_command(u8 cmd) {
  u32 &cntl0 = r.gt[I2C_CNTL_0 >> 2];
  u32 &cntl1 = r.gt[I2C_CNTL_1 >> 2];
  u8 status = I2C_DONE;

  if (cmd & I2C_CMD_START) { // from idle, or a repeated start
    i2c_line(false, true);
    i2c_line(true, true);
    i2c_line(true, false);
    i2c_line(false, false);
  }
  if (cmd & I2C_CMD_RECEIVE) {
    u8 v = 0;
    for (int i = 0; i < 8; i++) {
      i2c_line(false, true);
      i2c_line(true, true);
      v = u8((v << 1) | (m_ddc.sda() ? 1 : 0));
      i2c_line(false, true);
    }
    // Acknowledge, unless this is the last byte before the stop.
    const bool ack = !(cmd & I2C_CMD_STOP);
    i2c_line(false, !ack);
    i2c_line(true, !ack);
    i2c_line(false, !ack);
    cntl1 = (cntl1 & ~0xffu) | v;
  } else {
    const u8 v = u8(cntl1);
    for (int i = 7; i >= 0; i--) {
      const bool bit = (v >> i) & 1;
      i2c_line(false, bit);
      i2c_line(true, bit);
      i2c_line(false, bit);
    }
    i2c_line(false, true); // release SDA for the slave's acknowledge
    i2c_line(true, true);
    if (m_ddc.sda())
      status |= I2C_NACK;
    i2c_line(false, true);
  }
  if (cmd & I2C_CMD_STOP) {
    i2c_line(false, false);
    i2c_line(true, false);
    i2c_line(true, true);
    ddc_drive(); // the bus back to DAC_CNTL's drivers
  }
  cntl0 = (cntl0 & ~0xfu) | status;
}
