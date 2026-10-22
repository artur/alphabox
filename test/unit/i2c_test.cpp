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

/* Unit test of the I2C bus and the write-protected 24C02
 * (src/devices/common/i2c_spd.cpp), driven bit by bit as a console drives
 * the Tsunami's MPD pins: the bytes against the AT24C01D/02D data sheet
 * (8871F), and the line itself against the I2C-bus specification (UM10204):
 * SDA may change only while SCL is low (3.1.3). Then the DS20E's PCF8584
 * bus controller (src/platforms/ds10/PCF8584.cpp) with the board's parts is
 * fed what its console did at boot (test/unit/data/i2c_ds20e_boot.rec), and
 * every read must answer what the console was told. Built with every lane as
 * `i2c_test`; exit status 0 when every check passes.
 */

#include "AccessReplay.hpp"
#include "PCF8584.hpp"
#include "StdAfx.hpp"
#include "System.hpp"
#include "i2c_spd.hpp"

#include <cstdio>
#include <string>

// ---- stubs for what PCF8584.cpp needs from the rest of the machine ----

CSystemComponent::CSystemComponent(CConfigurator *cfg, CSystem *system)
    : devid_string((char *)"sys0(test)"), cSystem(system), myCfg(cfg) {}
CSystemComponent::~CSystemComponent() {}
int CSystem::RegisterMemory(CSystemComponent *, int, u64, u64) { return 0; }
alignas(CSystem) static unsigned char sys_storage[sizeof(CSystem)];

static int checks = 0, failures = 0;

static void check(const char *what, unsigned got, unsigned want) {
  checks++;
  if (got == want)
    return;
  failures++;
  printf("FAIL %s: got %02x, want %02x\n", what, got, want);
}

static I2CBus *bus;
static bool host_sda = true;
/// SDA changed by a device with SCL high (or rising): a device broke the
/// data-validity rule. The host's own START and STOP are its business.
static int sda_glitches = 0;

static void drive(bool scl, bool sda) {
  const bool sda_before = bus->sda();
  const bool host_moved_sda = sda != host_sda;
  host_sda = sda;
  bus->drive_from_host(scl, sda);
  if (!host_moved_sda && bus->scl() && bus->sda() != sda_before)
    sda_glitches++;
}

static void start() {
  drive(false, true);
  drive(true, true);
  drive(true, false);
  drive(false, false);
}

static void stop() {
  drive(false, false);
  drive(true, false);
  drive(true, true);
}

/// Send a byte; true when the ninth clock found SDA held low.
static bool send(unsigned b) {
  for (int i = 7; i >= 0; i--) {
    const bool bit = (b >> i) & 1;
    drive(false, bit);
    drive(true, bit);
    drive(false, bit);
  }
  drive(false, true);
  drive(true, true);
  const bool acked = !bus->sda();
  drive(false, true);
  return acked;
}

/// Receive a byte and answer ACK (more wanted) or NACK on the ninth clock.
static unsigned recv(bool ack) {
  unsigned b = 0;
  for (int i = 0; i < 8; i++) {
    drive(false, true);
    drive(true, true);
    b = b << 1 | (bus->sda() ? 1 : 0);
    drive(false, true);
  }
  drive(false, !ack);
  drive(true, !ack);
  drive(false, !ack);
  drive(false, true);
  return b;
}

static std::vector<uint8_t> image() {
  std::vector<uint8_t> m(256);
  for (int i = 0; i < 256; i++)
    m[i] = (uint8_t)(i ^ 0x5a);
  return m;
}

int main(int argc, char **argv) {
  I2CBus b;
  bus = &b;
  b.attach(std::make_shared<Eeprom24C02>(0x50, image()));
  b.attach(std::make_shared<Eeprom24C02>(0x51, std::vector<uint8_t>()));

  // Random read: dummy write of the word address, repeated START, read.
  start();
  check("address 50 write acknowledged", send(0xa0), 1);
  check("word address acknowledged", send(0x10), 1);
  start();
  check("address 50 read acknowledged", send(0xa1), 1);
  check("random read", recv(true), 0x10 ^ 0x5a);
  check("sequential read", recv(true), 0x11 ^ 0x5a);
  check("sequential read, last", recv(false), 0x12 ^ 0x5a);
  stop();

  // Current-address read goes on from where the last one stopped.
  start();
  send(0xa1);
  check("current-address read", recv(false), 0x13 ^ 0x5a);
  stop();

  // The counter wraps at the end of the part.
  start();
  send(0xa0);
  send(0xff);
  start();
  send(0xa1);
  check("read of the last byte", recv(true), 0xff ^ 0x5a);
  check("read wraps to byte 0", recv(false), 0x00 ^ 0x5a);
  stop();

  // A part that is not there does not answer, and the other one stays out.
  start();
  check("absent address 52 not acknowledged", send(0xa4), 0);
  stop();
  start();
  send(0xa2);
  send(0x00);
  start();
  send(0xa3);
  check("erased part reads ff", recv(false), 0xff);
  stop();

  // A write: every byte acknowledged, nothing written (WP tied high), the
  // counter moving on within its 8-byte page (5.2, 5.5).
  start();
  check("write: address acknowledged", send(0xa0), 1);
  check("write: word address acknowledged", send(0x24), 1);
  check("write: data byte acknowledged", send(0x37), 1);
  check("write: second data byte acknowledged", send(0x00), 1);
  check("write: third data byte acknowledged", send(0x00), 1);
  stop();
  start();
  send(0xa1);
  check("current address after three bytes from 24 is 27", recv(false),
        0x27 ^ 0x5a);
  stop();
  start();
  send(0xa0);
  send(0x26);
  start();
  send(0xa1);
  check("write-protected byte unchanged", recv(false), 0x26 ^ 0x5a);
  stop();
  start();
  send(0xa0);
  send(0x2e);
  for (int i = 0; i < 3; i++)
    send(0);
  stop();
  start();
  send(0xa1);
  check("page write rolls over within its page", recv(false), 0x29 ^ 0x5a);
  stop();

  check("SDA never changed by a device while SCL was high", sda_glitches, 0);

  // The DS20E console's boot through its PCF8584 (Ds20e.cpp's parts).
  CPCF8584 pcf(nullptr, reinterpret_cast<CSystem *>(sys_storage), 0);
  for (uint8_t a = 0x60; a <= 0x67; a++)
    pcf.bus().attach(std::make_shared<Eeprom24C02>(a, std::vector<uint8_t>()));
  pcf.bus().attach(std::make_shared<Eeprom24C02>(0x27, std::vector<uint8_t>()));
  pcf.bus().attach(std::make_shared<Eeprom24C02>(0x4f, std::vector<uint8_t>()));
  const char *path = argc > 1 ? argv[1] : TEST_DATA_DIR "/i2c_ds20e_boot.rec";
  const std::vector<RecordedAccess> rec = load_record(path, "i2c");
  check("record has the console's accesses", rec.size() > 400, 1);
  int bad = 0;
  for (const RecordedAccess &r : rec) {
    if (r.write) {
      pcf.WriteMem(0, r.address, r.bits, r.value);
      continue;
    }
    const unsigned got = (unsigned)pcf.ReadMem(0, r.address, r.bits);
    checks++;
    if (got == r.value)
      continue;
    failures++;
    if (bad++ < 10)
      printf("FAIL replay line %d: S%d read %02x, the console was told %02x\n",
             r.line, (int)r.address, got, (unsigned)r.value);
  }

  printf("i2c_test: %d checks, %d failed\n", checks, failures);
  return failures ? 1 : 0;
}
