/* ES40 emulator.
 * Copyright (C) 2026 by the ES40 Emulator Project
 * Copyright (C) 2025 by Kisara Development LLC.
 * All rights reserved.
 *
 * WWW    : https://github.com/ES40-Emu/es40
 *
 * SPDX-License-Identifier: BSD-1-Clause
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS AND CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "i2c_spd.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>

/* ===== I2CDevice defaults ===== */
void I2CDevice::on_start() {}
void I2CDevice::on_stop() {}
void I2CDevice::on_scl_rise(bool) {}
void I2CDevice::on_scl_fall(bool) {}
bool I2CDevice::pull_sda_low() const { return false; }
bool I2CDevice::pull_scl_low() const { return false; }

/* ===== I2CBus ===== */
I2CBus::I2CBus()
    : host_scl_(true), host_sda_(true), line_scl_(true), line_sda_(true) {}

void I2CBus::attach(const std::shared_ptr<I2CDevice> &dev) {
  devs_.push_back(dev);
  recompute_lines();
}

bool I2CBus::scl() const { return line_scl_; }
bool I2CBus::sda() const { return line_sda_; }

void I2CBus::recompute_lines() {
  bool dev_scl_pull = false, dev_sda_pull = false;
  for (const auto &d : devs_) {
    dev_scl_pull = dev_scl_pull || d->pull_scl_low();
    dev_sda_pull = dev_sda_pull || d->pull_sda_low();
  }
  line_scl_ = host_scl_ && !dev_scl_pull; // wired-AND (open-drain)
  line_sda_ = host_sda_ && !dev_sda_pull;
}

bool I2CBus::trace_on() {
  static const bool on = getenv("ALPHABOX_TRACE_I2C") != nullptr;
  return on;
}

void I2CBus::notify_start() {
  trace_bits_ = 0;
  trace_byte_ = 0;
  for (auto &d : devs_)
    d->on_start();
}
void I2CBus::notify_stop() {
  trace_bits_ = -1;
  for (auto &d : devs_)
    d->on_stop();
}
void I2CBus::notify_scl_rise() {
  for (auto &d : devs_)
    d->on_scl_rise(line_sda_);

  if (trace_bits_ < 0 || !trace_on())
    return;
  if (trace_bits_ < 8) {
    trace_byte_ = uint8_t(trace_byte_ << 1 | (line_sda_ ? 1 : 0));
    trace_bits_++;
    return;
  }
  // The ninth clock: a device that recognises the address holds SDA low.
  printf("%%SYS-T-I2C: address %02x %s %s\n", trace_byte_ >> 1,
         (trace_byte_ & 1) ? "read " : "write",
         line_sda_ ? "-- nobody answered" : "acknowledged");
  trace_bits_ = -1;
}
void I2CBus::notify_scl_fall() {
  for (auto &d : devs_)
    d->on_scl_fall(line_sda_);
}

void I2CBus::drive_from_host(bool scl_release, bool sda_release) {
  // Save old line levels before change
  bool old_scl = line_scl_;
  bool old_sda = line_sda_;

  host_scl_ = scl_release;
  host_sda_ = sda_release;

  // First recompute lines with new host drives (devices might already be
  // pulling)
  recompute_lines();

  // START/STOP detection (based on *line* edges while SCL is high)
  if (old_sda && !line_sda_ && line_scl_) {
    notify_start();
  } else if (!old_sda && line_sda_ && line_scl_) {
    notify_stop();
  }

  // SCL edges
  if (!old_scl && line_scl_) {
    notify_scl_rise();
  } else if (old_scl && !line_scl_) {
    notify_scl_fall();
  }

  // Devices may drive ACK/data while SCL low; recompute one more time.
  recompute_lines();
}

/* ===== Eeprom24C02 (read-only SPD) ===== */
Eeprom24C02::Eeprom24C02(uint8_t addr7, const std::vector<uint8_t> &image)
    : addr7_(addr7), mem_(image), st_(IDLE), ack_phase_(NONE), shreg_(0),
      bitpos_(0), wordptr_(0), rw_(false), ack_pull_(false), addr_match_(false),
      ack_seen_(false) {
  if (mem_.size() < 256)
    mem_.resize(256, 0xff);
}

bool Eeprom24C02::pull_sda_low() const { return ack_pull_; }

void Eeprom24C02::enter_idle() {
  st_ = IDLE;
  ack_phase_ = NONE;
  shreg_ = 0;
  bitpos_ = 0;
  ack_pull_ = false;
  addr_match_ = false;
  ack_seen_ = false;
}

void Eeprom24C02::enter_addr() {
  st_ = ADDR;
  shreg_ = 0;
  bitpos_ = 0;
  ack_phase_ = ACK_ADDR;
  ack_pull_ = false;
  addr_match_ = false;
  ack_seen_ = false;
}

void Eeprom24C02::enter_word() {
  st_ = WORD;
  shreg_ = 0;
  bitpos_ = 0;
  ack_phase_ = ACK_WORD;
  ack_pull_ = false;
  ack_seen_ = false;
}

void Eeprom24C02::enter_xmit() {
  st_ = XMIT;
  bitpos_ = 0;
  ack_phase_ = ACK_DATA;
  ack_seen_ = false;
  // Prepare first data bit while SCL is low (caller ensures this is called on
  // SCL low path)
  shreg_ = mem_[wordptr_++];
  // MSB first
  ack_pull_ = ((shreg_ & 0x80) == 0); // '0' bit -> pull low
}

void Eeprom24C02::on_start() {
  // Repeated START restarts addressing regardless of previous state
  enter_addr();
}

void Eeprom24C02::on_stop() { enter_idle(); }

void Eeprom24C02::on_scl_rise(bool sda_line) {
  switch (st_) {
  case ADDR:
    // Shift in 7-bit address + R/W on rising edges
    shreg_ = static_cast<uint8_t>((shreg_ << 1) | (sda_line ? 1 : 0));
    if (++bitpos_ == 8) {
      rw_ = (shreg_ & 1) != 0;
      addr_match_ = (static_cast<uint8_t>(shreg_ >> 1) == addr7_);
      // Drive ACK during the 9th clock if we match
      ack_pull_ = addr_match_; // ACK low
      st_ = RECV_ACK;
      bitpos_ = 0;
    }
    break;

  case WORD:
    // Shift in 8-bit memory pointer
    shreg_ = static_cast<uint8_t>((shreg_ << 1) | (sda_line ? 1 : 0));
    if (++bitpos_ == 8) {
      if (addr_match_)
        wordptr_ = shreg_;
      ack_pull_ = addr_match_; // ACK low
      st_ = RECV_ACK;
      bitpos_ = 0;
    }
    break;

  case XMIT:
    // Host samples our data while SCL is high; bitpos_ counts 0..7. The
    // eighth bit stays on the wire until SCL falls -- releasing it here, as
    // this used to, turned every byte's least significant bit into a 1 for
    // a host that samples after the rising edge (perm2 reading the EDID).
    if (++bitpos_ == 8) {
      st_ = RECV_ACK;
      bitpos_ = 0;
    }
    break;

  case RECV_ACK:
    // Host drives ACK/NACK (low/high) on this 9th rising edge.
    ack_seen_ = true;
    if (ack_phase_ == ACK_DATA) {
      // If NACK, stop transmitting; if ACK, continue reading
      bool host_ack = (sda_line == false);
      if (!host_ack) {
        enter_idle();
      }
    }
    // For ADDR/WORD ACKs we don't need to read host; we already drove ACK.
    break;

  default:
    break;
  }
}

void Eeprom24C02::prepare_next_tx_bit() {
  // bitpos_ in 0..7 for data bits; prepare SDA while SCL is low
  if (bitpos_ < 8) {
    uint8_t mask = static_cast<uint8_t>(0x80 >> bitpos_);
    ack_pull_ = ((shreg_ & mask) == 0);
  }
}

void Eeprom24C02::on_scl_fall(bool /*sda_line*/) {
  switch (st_) {
  case RECV_ACK:
    // Transition to next state after the ACK bit low-to-high phase. The
    // falling edge that ends the eighth clock comes first: the acknowledge
    // has to stay on the wire until the ninth clock has been taken.
    if (!ack_seen_) {
      // The fall that ends the eighth clock: a transmitter now lets go of
      // SDA for the host's ACK/NACK on the ninth.
      if (ack_phase_ == ACK_DATA)
        ack_pull_ = false;
      break;
    }

    if (!addr_match_) {
      enter_idle();
      break;
    }

    if (ack_phase_ == ACK_ADDR) {
      // After address ACK: if WRITE -> expect word address; if READ -> begin
      // transmit
      if (!rw_) {
        enter_word();
      } else {
        enter_xmit(); // prepare first data byte
      }
    } else if (ack_phase_ == ACK_WORD) {
      // After word pointer ACK: expect repeated START + READ address
      // We simply wait in ADDR for the next START (already handled in
      // on_start()).
      enter_addr();
    } else if (ack_phase_ == ACK_DATA) {
      // After ACKing a transmitted byte: continue sending next byte
      enter_xmit();
    }
    break;

  case XMIT:
    // Prepare subsequent data bits while SCL is low.
    prepare_next_tx_bit();
    break;

  default:
    break;
  }
}
