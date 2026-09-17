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
 * The PCF8584 I2C bus controller. See PCF8584.hpp for where a board uses it.
 *
 * The control register S1 (written at base+1) carries, from bit 7 down:
 * PIN, ESO, ES1, ES2, ENI, STA, STO, ACK. With ESO clear, a write to S0
 * lands in the register ES1 selects -- the controller's own address, or the
 * clock divider -- which is how firmware initialises the part. With ESO
 * set, S0 is the data shift register and STA/STO start and finish a
 * transfer.
 *
 * The status register (read at base+1) returns, from bit 7 down: PIN, 0,
 * STS, BER, LRB, AAS, LAB, BB. Two bits matter to a master: LRB, the last
 * bit received, which is 0 when the addressed part acknowledged, and BB,
 * which is 1 while the bus is free. A console waits for BB before it starts
 * (the DS10's waits a hundred thousand times), so a machine whose
 * controller is missing never gets off the ground.
 *
 * Timing is not modelled: a byte goes out on the wire while the register
 * write is being handled, so PIN is already clear by the time the firmware
 * looks. That is what firmware polling for completion expects to find.
 **/
#include "PCF8584.hpp"
#include "StdAfx.hpp"

#include "System.hpp"

// Control register (written to S1).
static const u8 CTL_PIN = 0x80;
static const u8 CTL_ESO = 0x40;
static const u8 CTL_ES1 = 0x20;
static const u8 CTL_STA = 0x04;
static const u8 CTL_STO = 0x02;
static const u8 CTL_ACK = 0x01;

// Status register (read from S1).
static const u8 STS_PIN = 0x80;
static const u8 STS_LRB = 0x08;
static const u8 STS_BB = 0x01;

CPCF8584::CPCF8584(CConfigurator *cfg, CSystem *c, u64 base)
    : CSystemComponent(cfg, c) {
  c->RegisterMemory(this, 0, base, 2);
  memset(&state, 0, sizeof(state));
  state.status = STS_PIN | STS_BB; // idle, bus free
  m_bus.drive_from_host(true, true);
}

CPCF8584::~CPCF8584() {}

/* ===== the wire ===== */

void CPCF8584::scl_sda(bool scl, bool sda) { m_bus.drive_from_host(scl, sda); }

void CPCF8584::send_start() {
  scl_sda(false, true);
  scl_sda(true, true);
  scl_sda(true, false); // SDA falls while SCL is high
  scl_sda(false, false);
}

void CPCF8584::send_stop() {
  scl_sda(false, false);
  scl_sda(true, false);
  scl_sda(true, true); // SDA rises while SCL is high
}

bool CPCF8584::send_byte(u8 b) {
  for (int i = 7; i >= 0; i--) {
    bool bit = ((b >> i) & 1) != 0;
    scl_sda(false, bit);
    scl_sda(true, bit);
    scl_sda(false, bit);
  }

  // The ninth clock: the receiver holds SDA low to acknowledge.
  scl_sda(false, true);
  scl_sda(true, true);
  bool acked = !m_bus.sda();
  scl_sda(false, true);
  return acked;
}

u8 CPCF8584::recv_byte(bool ack) {
  u8 b = 0;
  for (int i = 0; i < 8; i++) {
    scl_sda(false, true);
    scl_sda(true, true);
    b = u8(b << 1 | (m_bus.sda() ? 1 : 0));
    scl_sda(false, true);
  }

  // The ninth clock is ours: hold SDA low to ask for another byte.
  scl_sda(false, !ack);
  scl_sda(true, !ack);
  scl_sda(false, !ack);
  scl_sda(false, true);
  return b;
}

void CPCF8584::set_bus_free(bool free) {
  if (free)
    state.status |= STS_BB;
  else
    state.status &= u8(~STS_BB);
}

/* ===== the registers ===== */

/**
 * START, then the address byte the firmware left in S0.
 *
 * Bit 0 of that byte says which way the transfer goes, and the controller
 * remembers it: a read has to clock the first byte in before the firmware
 * can fetch it from S0.
 **/
void CPCF8584::begin_transfer() {
  send_start();
  set_bus_free(false);

  state.reading = (state.s0 & 1) != 0;
  state.addressed = send_byte(state.s0);

  state.status &= u8(~(STS_PIN | STS_LRB));
  if (!state.addressed)
    state.status |= STS_LRB; // nobody answered
  else if (state.reading)
    state.s0 = recv_byte(true);
}

/// Report the register traffic itself, under ALPHABOX_TRACE_I2C: the bus
/// trace shows what reached the wire, this shows what the firmware asked
/// for.
void CPCF8584::trace_reg(const char *what, u64 address, u8 value) {
  if (!I2CBus::trace_on())
    return;
  printf("%%SYS-T-I2C: %s %s = %02x\n", (address & 1) ? "S1" : "S0", what,
         value);
}

u64 CPCF8584::ReadMem(int index, u64 address, int dsize) {
  if ((address & 1) == 0) {
    u8 data = state.s0;
    // Reading S0 in a master read hands over the byte just clocked in and
    // starts the next one, unless the firmware has already asked for a stop.
    if (state.reading && state.addressed && (state.s1 & CTL_ESO))
      state.s0 = recv_byte((state.s1 & CTL_ACK) != 0);
    trace_reg("read ", address, data);
    return data;
  }
  trace_reg("read ", address, state.status);
  return state.status;
}

void CPCF8584::WriteMem(int index, u64 address, int dsize, u64 data) {
  u8 b = (u8)data;

  trace_reg("write", address, b);

  if ((address & 1) == 0) {
    if (!(state.s1 & CTL_ESO)) {
      // Initialisation: S0 stands in for the register ES1 selects.
      if (state.s1 & CTL_ES1)
        state.clock = b;
      else
        state.own = b;
      return;
    }
    state.s0 = b;
    if (state.status & STS_BB)
      return; // no transfer under way: nothing to send yet
    state.addressed = send_byte(b);
    state.status &= u8(~(STS_PIN | STS_LRB));
    if (!state.addressed)
      state.status |= STS_LRB;
    return;
  }

  state.s1 = b;

  if (b & CTL_STA) {
    begin_transfer();
    return;
  }

  if (b & CTL_STO) {
    send_stop();
    set_bus_free(true);
    state.reading = false;
    state.addressed = false;
    state.status |= STS_PIN;
    return;
  }

  // No transfer asked for: this is the firmware changing how the next byte
  // will be acknowledged, and clearing the pending interrupt on its way
  // past. PIN follows the part, not the write: idle between transfers, and
  // clear inside one, where every byte is already on the wire by the time
  // the write returns.
  if (state.status & STS_BB)
    state.status |= STS_PIN;
  else
    state.status &= u8(~STS_PIN);
}

/* ===== state file ===== */

static u32 pcf_magic1 = 0x85841122;
static u32 pcf_magic2 = 0x22118584;

int CPCF8584::SaveState(FILE *f) {
  long ss = sizeof(state);

  fwrite(&pcf_magic1, sizeof(u32), 1, f);
  fwrite(&ss, sizeof(long), 1, f);
  fwrite(&state, sizeof(state), 1, f);
  fwrite(&pcf_magic2, sizeof(u32), 1, f);
  printf("%s: %ld bytes saved.\n", devid_string, ss);
  return 0;
}

int CPCF8584::RestoreState(FILE *f) {
  long ss;
  u32 m1;
  u32 m2;
  size_t r;

  r = fread(&m1, sizeof(u32), 1, f);
  if (r != 1) {
    printf("%s: unexpected end of file!\n", devid_string);
    return -1;
  }

  if (m1 != pcf_magic1) {
    printf("%s: MAGIC 1 does not match!\n", devid_string);
    return -1;
  }

  r = fread(&ss, sizeof(long), 1, f);
  if (r != 1) {
    printf("%s: unexpected end of file!\n", devid_string);
    return -1;
  }

  if (ss != sizeof(state)) {
    printf("%s: STRUCT SIZE does not match!\n", devid_string);
    return -1;
  }

  r = fread(&state, sizeof(state), 1, f);
  if (r != 1) {
    printf("%s: unexpected end of file!\n", devid_string);
    return -1;
  }

  r = fread(&m2, sizeof(u32), 1, f);
  if (r != 1) {
    printf("%s: unexpected end of file!\n", devid_string);
    return -1;
  }

  if (m2 != pcf_magic2) {
    printf("%s: MAGIC 2 does not match!\n", devid_string);
    return -1;
  }

  printf("%s: %ld bytes restored.\n", devid_string, ss);
  return 0;
}
