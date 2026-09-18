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
 * What the ES1371 has and the ES1370 has not: an AC'97 codec at the end of
 * a serial link, and a sample rate converter between the DMA channels and
 * that link.
 *
 * Neither of them touches a sample here. The codec is where a driver finds
 * out what card it is talking to and what mixer controls to offer, so what
 * it needs to be is a register file that answers the way the specification
 * says one does. The converter is where a driver sets a channel's rate,
 * which on the ES1370 was a field of the control register; what matters is
 * reading that rate back out of the words the driver wrote, so the host
 * stream can be told what rate the samples arriving in it are at.
 **/
#include "StdAfx.hpp"

#ifdef HAVE_SDL
#include "ES137x.hpp"

/* The AC'97 register map, by byte address on the link. Most of these are
   plain storage -- a mixer setting the host writes and reads back -- and
   are named here because a map with holes in it is harder to read than
   one without. */
#define AC97_RESET 0x00
#define AC97_MASTER_VOL 0x02
#define AC97_HEADPHONE_VOL 0x04
#define AC97_MASTER_MONO_VOL 0x06
#define AC97_MASTER_TONE 0x08
#define AC97_PC_BEEP_VOL 0x0a
#define AC97_PHONE_VOL 0x0c
#define AC97_MIC_VOL 0x0e
#define AC97_LINE_IN_VOL 0x10
#define AC97_CD_VOL 0x12
#define AC97_VIDEO_VOL 0x14
#define AC97_AUX_VOL 0x16
#define AC97_PCM_OUT_VOL 0x18
#define AC97_RECORD_SELECT 0x1a
#define AC97_RECORD_GAIN 0x1c
#define AC97_RECORD_GAIN_MIC 0x1e
#define AC97_GENERAL_PURPOSE 0x20
#define AC97_3D_CONTROL 0x22
#define AC97_POWERDOWN 0x26
#define AC97_EXTENDED_ID 0x28
#define AC97_EXTENDED_CTRL 0x2a
#define AC97_FRONT_DAC_RATE 0x2c
#define AC97_LR_ADC_RATE 0x32
#define AC97_MIC_ADC_RATE 0x34
#define AC97_VENDOR_ID1 0x7c
#define AC97_VENDOR_ID2 0x7e

/* Reg 0x26 reads back as the power-down bits the host wrote, with the
   four "this section has come up" bits set underneath: the ADC, the DAC,
   the analogue mixer and its voltage reference are all ready, always,
   because nothing here has to power anything up. A driver that waits for
   them waits seconds before giving up, so they must never read as
   anything else. */
#define AC97_POWERDOWN_READY 0x000f

CAc97Codec::CAc97Codec(u32 vendor_id) : id(vendor_id) { reset(); }

/**
 * The state a codec comes out of reset in, from the AC'97 specification's
 * table of register defaults: every output muted at 0 dB of attenuation,
 * every input at its 0 dB setting and muted, recording off the microphone
 * at no gain.
 **/
void CAc97Codec::reset() {
  memset(reg, 0, sizeof(reg));

  /* What this codec can do beyond the baseline. Nothing: no tone control,
     no headphone output, no loudness, no 3D enhancement, 16-bit
     converters -- which is the truth, because the samples go to the host
     untouched and this end of the card mixes nothing. */
  reg[AC97_RESET >> 1] = 0x0000;

  reg[AC97_MASTER_VOL >> 1] = 0x8000;
  reg[AC97_MASTER_MONO_VOL >> 1] = 0x8000;
  reg[AC97_PHONE_VOL >> 1] = 0x8008;
  reg[AC97_MIC_VOL >> 1] = 0x8008;
  reg[AC97_LINE_IN_VOL >> 1] = 0x8808;
  reg[AC97_CD_VOL >> 1] = 0x8808;
  reg[AC97_PCM_OUT_VOL >> 1] = 0x8808;
  reg[AC97_RECORD_GAIN >> 1] = 0x8000;
  reg[AC97_POWERDOWN >> 1] = AC97_POWERDOWN_READY;

  /* No variable rate audio: the link runs at 48 kHz and always will,
     because it is the card's own converter and not the codec that puts a
     channel on the rate the guest asked for. So the rate registers are
     pinned, and the driver is told not to try. */
  reg[AC97_EXTENDED_ID >> 1] = 0x0000;
  reg[AC97_FRONT_DAC_RATE >> 1] = 48000;
  reg[AC97_LR_ADC_RATE >> 1] = 48000;
  reg[AC97_MIC_ADC_RATE >> 1] = 48000;

  reg[AC97_VENDOR_ID1 >> 1] = (u16)(id >> 16);
  reg[AC97_VENDOR_ID2 >> 1] = (u16)id;
}

u16 CAc97Codec::read(u8 r) const { return reg[(r & 0x7f) >> 1]; }

void CAc97Codec::write(u8 r, u16 value) {
  const unsigned i = (r & 0x7f) >> 1;

  switch (r) {
  case AC97_RESET:
    /* A write of anything resets the codec; the value itself is ignored,
       and the register goes on reading back what the part can do. */
    reset();
    break;

  case AC97_POWERDOWN:
    /* The low four bits are the codec saying which of its sections are
       up, not the host saying anything, so they survive the write. */
    reg[i] = (value & ~AC97_POWERDOWN_READY) | AC97_POWERDOWN_READY;
    break;

  case AC97_EXTENDED_ID:
  case AC97_FRONT_DAC_RATE:
  case AC97_LR_ADC_RATE:
  case AC97_MIC_ADC_RATE:
  case AC97_VENDOR_ID1:
  case AC97_VENDOR_ID2:
    /* Read-only: what the part is, and what rate its link runs at, are
       not the host's to change. */
    break;

  default:
    reg[i] = value;
    break;
  }
}

/**
 * A codec access from the guest. The register number and, for a write, the
 * value are in the one word; PIRD says which it is. Both finish before the
 * write returns -- there is no AC-link here to wait for -- so a read leaves
 * the answer sitting in the register with RDY up, which is where the
 * driver's poll will find it, and a write takes RDY down again because the
 * word waiting there is no longer an answer to anything.
 **/
void CES137x::codec_write(ES137xState *s, uint32_t val) {
  const u8 reg = (val & CODEC_ADDR) >> CODEC_SH_ADDR;

  if (val & CODEC_PIRD)
    s->codec_result = CODEC_RDY | ((uint32_t)ac97.read(reg) << 0);
  else {
    ac97.write(reg, (u16)(val & CODEC_DATA));
    s->codec_result = 0;
  }
}

uint32_t CES137x::codec_read(ES137xState *s) {
  /* WIP is never set: a write is over by the time the guest's store
     returns, so there is never one in progress to report. */
  return s->codec_result;
}

/**
 * The rate converter's interface register.
 *
 * Address, data and a write-enable in one word: with WE the data goes into
 * the RAM at that address, without it the address is simply selected and
 * reads of this register return what is there.
 **/
void CES137x::src_write(ES137xState *s, uint32_t val) {
  const unsigned addr = (val & SRC_RAM_ADDR) >> SRC_SH_RAM_ADDR;

  /* Everything but BUSY, which is the converter's to set and is never set
     here, is kept as written -- including the three state bits, which on
     the way in are the guest asking to be shown the interface's state. */
  s->smprate = val & ~SRC_RAM_BUSY;

  if (val & SRC_RAM_WE) {
    s->src_ram[addr] = (u16)(val & SRC_RAM_DATA);
    /* A rate word changed, so a channel may now want to be running at a
       different rate than the host stream is set to. */
    update_voices(s, s->ctl, s->sctl);
  }
}

uint32_t CES137x::src_read(ES137xState *s) {
  const unsigned addr = (s->smprate & SRC_RAM_ADDR) >> SRC_SH_RAM_ADDR;
  uint32_t val = (s->smprate & (SRC_RAM_ADDR | SRC_RAM_WE | SRC_DISABLE |
                                SRC_DIS_P1 | SRC_DIS_P2 | SRC_DIS_R1)) |
                 s->src_ram[addr];

  /* Bits 16 to 18 are the interface's own state. No datasheet we have says
     what they count, but every driver of this part reads them the same
     way: it sets bit 16 to ask for the state to be shown, then waits first
     for the three to read back as zero and then for exactly bit 16 -- it
     is waiting for a window in a cycle that runs off the AC-link frame
     clock. There is no such clock here, so the cycle is made out of the
     reads themselves: while the guest asks to see the state, successive
     reads alternate, and either of those waits comes back within two of
     them. RAM_BUSY never comes up at all, because the access the guest is
     waiting on has already happened. */
  if (s->smprate & SRC_STATE) {
    s->src_phase ^= 1;
    if (s->src_phase)
      val |= 0x00010000;
  }
  return val;
}

/**
 * What rate the converter has been told to run a channel at. Both halves
 * of the step the converter walks its 48 kHz input with are in the RAM:
 * six bits of whole steps in the channel's INT_REGS word and fifteen bits
 * of fraction in its VFREQ_FRAC word, in units of a 32768th of a step. A
 * step of one is 3 kHz, which is how the driver's rate becomes these two
 * words and how they become a rate again here.
 **/
uint32_t CES137x::src_rate(ES137xState *s, size_t chan) {
  static const unsigned base[NB_CHANNELS] = {SRC_RAM_DAC1, SRC_RAM_DAC2,
                                             SRC_RAM_ADC};
  const uint32_t step =
      ((uint32_t)(s->src_ram[base[chan] + SRC_RAM_INT_REGS] & 0xfc00) << 5) |
      (s->src_ram[base[chan] + SRC_RAM_VFREQ_FRAC] & 0x7fff);

  return (uint32_t)(((u64)step * 3000 + 16384) / 32768);
}

void CES137x::es1371_src_calc_freq(ES137xState *s, size_t chan,
                                   uint32_t *old_freq, uint32_t *new_freq) {
  *old_freq = s->chan[chan].freq;
  *new_freq = src_rate(s, chan);
}
#endif /* HAVE_SDL */
