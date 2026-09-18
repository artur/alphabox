/*
 * QEMU ES1370 emulation
 *
 * Copyright (c) 2005 Vassili Karpov (malc)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 * Modifications copyright (C) 2026 Artur Goulão
 * Website: https://github.com/artur/alphabox
 */
// Straight port to es40 by Cacodemon345.
#include "StdAfx.hpp"

#ifdef HAVE_SDL
#include "ES137x.hpp"

#include <algorithm>

#ifndef MAX
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#endif

#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif

/* Missing stuff:
   SCTRL_P[12](END|ST)INC
   SCTRL_P1SCTRLD
   SCTRL_P2DACSEN
   CTRL_DAC_SYNC
   MIDI
   non looped mode
   surely more
*/

CES137x::CES137x(CConfigurator *cfg, class CSystem *c, int pcibus, int pcidev,
                 const es137x_chip_config &chip_config)
    : CPCIDevice(cfg, c, pcibus, pcidev), chip(chip_config),
      ac97(chip_config.codec_vendor_id) {
  SDL_AudioSpec as;

  as.freq = 44100;
  as.channels = 2;
  as.format = SDL_AUDIO_S16LE;
  memset((void *)&state, 0, sizeof(state));
  if (!SDL_Init(SDL_INIT_AUDIO)) {
    FAILURE_1(SDL, "Failed to initialize SDL audio: %s", SDL_GetError());
  }
  state.audio_be_in =
      SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_RECORDING, nullptr);
  if (!state.audio_be_in) {
    FAILURE_1(SDL, "Failed to initialize SDL audio input: %s", SDL_GetError());
  }
  state.audio_be_out =
      SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, nullptr);
  if (!state.audio_be_out) {
    FAILURE_1(SDL, "Failed to initialize SDL audio output: %s", SDL_GetError());
  }
  state.adc_voice = SDL_CreateAudioStream(NULL, &as);
  state.dac_voice[0] = SDL_CreateAudioStream(&as, NULL);
  state.dac_voice[1] = SDL_CreateAudioStream(&as, NULL);

  SDL_SetAudioStreamPutCallback(state.adc_voice, dac_callback_adc, this);
  SDL_SetAudioStreamGetCallback(state.dac_voice[0], dac_callback_dac1, this);
  SDL_SetAudioStreamGetCallback(state.dac_voice[1], dac_callback_dac2, this);
}

CES137x::~CES137x() {
  SDL_DestroyAudioStream(state.adc_voice);
  SDL_DestroyAudioStream(state.dac_voice[0]);
  SDL_DestroyAudioStream(state.dac_voice[1]);
  SDL_CloseAudioDevice(state.audio_be_in);
  SDL_CloseAudioDevice(state.audio_be_out);
}

void CES137x::init() {
  es137x_config_space(chip, cfg_data, cfg_mask);
  add_function(0, cfg_data, cfg_mask);
  ResetPCI();
  reset(&state);
  update_voices(&state, state.ctl, state.sctl);

  printf("%s: Ensoniq %s AudioPCI sound card.\n", devid_string, chip.part);
}

void CES137x::update_status(ES137xState *s, uint32_t new_status) {
  uint32_t level = new_status & (STAT_DAC1 | STAT_DAC2 | STAT_ADC);

  if (level) {
    s->status = new_status | STAT_INTR;
  } else {
    s->status = new_status & ~STAT_INTR;
  }
  do_pci_interrupt(0, !!level);
}

void CES137x::reset(ES137xState *s) {
  size_t i;

  s->ctl = 1;
  s->status = 0x60;
  s->mempage = 0;
  s->codec = 0;
  s->sctl = 0;
  s->legacy = 0;
  s->smprate = 0;
  s->codec_result = 0;
  s->src_phase = 0;
  memset(s->src_ram, 0, sizeof(s->src_ram));
  ac97.reset();

  for (i = 0; i < NB_CHANNELS; ++i) {
    struct chan *d = &s->chan[i];
    d->scount = 0;
    d->leftover = 0;
    d->freq = 0;
    if (i == ADC_CHANNEL) {
      SDL_UnbindAudioStream(s->adc_voice);
    } else {
      SDL_UnbindAudioStream(s->dac_voice[i]);
    }
  }
  do_pci_interrupt(0, 0);
  update_voices(s, s->ctl, s->sctl);
}

void CES137x::maybe_lower_irq(ES137xState *s, uint32_t sctl) {
  uint32_t new_status = s->status;

  if (!(sctl & SCTRL_P1INTEN) && (s->sctl & SCTRL_P1INTEN)) {
    new_status &= ~STAT_DAC1;
  }

  if (!(sctl & SCTRL_P2INTEN) && (s->sctl & SCTRL_P2INTEN)) {
    new_status &= ~STAT_DAC2;
  }

  if (!(sctl & SCTRL_R1INTEN) && (s->sctl & SCTRL_R1INTEN)) {
    new_status &= ~STAT_ADC;
  }

  if (new_status != s->status) {
    update_status(s, new_status);
  }
}

void CES137x::es1370_dac1_calc_freq(ES137xState *s, uint32_t ctl,
                                    uint32_t *old_freq, uint32_t *new_freq) {
  *old_freq = dac1_samplerate[(s->ctl & CTRL_WTSRSEL) >> CTRL_SH_WTSRSEL];
  *new_freq = dac1_samplerate[(ctl & CTRL_WTSRSEL) >> CTRL_SH_WTSRSEL];
}

void CES137x::es1370_dac2_and_adc_calc_freq(ES137xState *s, uint32_t ctl,
                                            uint32_t *old_freq,
                                            uint32_t *new_freq) {
  uint32_t old_pclkdiv, new_pclkdiv;

  new_pclkdiv = (ctl & CTRL_PCLKDIV) >> CTRL_SH_PCLKDIV;
  old_pclkdiv = (s->ctl & CTRL_PCLKDIV) >> CTRL_SH_PCLKDIV;
  *new_freq = DAC2_DIVTOSR(new_pclkdiv);
  *old_freq = DAC2_DIVTOSR(old_pclkdiv);
}

void CES137x::update_voices(ES137xState *s, uint32_t ctl, uint32_t sctl) {
  size_t i;
  uint32_t old_freq, new_freq, old_fmt, new_fmt;

  for (i = 0; i < NB_CHANNELS; ++i) {
    struct chan *d = &s->chan[i];
    const struct chan_bits *b = &es137x_chan_bits[i];

    new_fmt = (sctl & b->sctl_fmt) >> b->sctl_sh_fmt;
    old_fmt = (s->sctl & b->sctl_fmt) >> b->sctl_sh_fmt;

    /* On the ES1370 a channel's rate is a field of the control register,
       so the value about to be written is the new rate and the one still
       in the register is the old one. The ES1371 reads its rate out of the
       converter's RAM instead, which changes on its own schedule, so there
       the old rate is simply the one the stream was last set to. */
    if (chip.ac97)
      es1371_src_calc_freq(s, i, &old_freq, &new_freq);
    else
      b->calc_freq(s, ctl, &old_freq, &new_freq);

    if ((old_fmt != new_fmt) || (old_freq != new_freq)) {
      d->shift = (new_fmt & 1) + (new_fmt >> 1);
      if (new_freq) {
        // struct audsettings as;
        SDL_AudioSpec as;

        as.freq = new_freq;
        as.channels = 1 << (new_fmt & 1);
        as.format = (new_fmt & 2) ? SDL_AUDIO_S16LE : SDL_AUDIO_U8;

        if (i == ADC_CHANNEL) {
          SDL_SetAudioStreamFormat(s->adc_voice, &as, &as);
        } else {
          SDL_SetAudioStreamFormat(s->dac_voice[i], &as, &as);
        }
      }
    }
    d->freq = new_freq;

    if (((ctl ^ s->ctl) & b->ctl_en) || ((sctl ^ s->sctl) & b->sctl_pause)) {
      int on = (ctl & b->ctl_en) && !(sctl & b->sctl_pause);

      if (i == ADC_CHANNEL) {
        if (on) {
          SDL_BindAudioStream(s->audio_be_in, s->adc_voice);
        } else {
          SDL_UnbindAudioStream(s->adc_voice);
        }
      } else {
        if (on) {
          SDL_BindAudioStream(s->audio_be_out, s->dac_voice[i]);
        } else {
          SDL_UnbindAudioStream(s->dac_voice[i]);
        }
      }
    }
  }

  s->ctl = ctl;
  s->sctl = sctl;
}

uint32_t CES137x::fixup(ES137xState *s, uint32_t addr) {
  addr &= 0xff;
  if (addr >= 0x30 && addr <= 0x3f) {
    addr |= s->mempage << 8;
  }
  return addr;
}

void CES137x::write_reg(void *opaque, u64 addr, uint64_t val, unsigned size) {
  ES137xState *s = (ES137xState *)opaque;
  struct chan *d = &s->chan[0];

  addr = fixup(s, addr);

  switch (addr) {
  case ES_REG_CONTROL:
    update_voices(s, val, s->sctl);
    // print_ctl(val);
    break;

  case ES_REG_MEMPAGE:
    s->mempage = val & 0xf;
    break;

  case ES1371_REG_SMPRATE: /* ES1370_REG_CODEC on the older part */
    if (chip.ac97)
      src_write(s, val);
    break;

  case ES1371_REG_CODEC:
    if (chip.ac97)
      codec_write(s, val);
    break;

  case ES1371_REG_LEGACY:
    /* Which of the Sound Blaster and joystick addresses the card decodes
       on an ISA bus it does not have here. The driver writes it to switch
       that decoding off; nothing but the driver reads it back. */
    if (chip.ac97)
      s->legacy = val;
    break;

  case ES_REG_SERIAL_CONTROL:
    maybe_lower_irq(s, val);
    update_voices(s, s->ctl, val);
    // print_sctl(val);
    break;

  case ES_REG_DAC1_SCOUNT:
  case ES_REG_DAC2_SCOUNT:
  case ES_REG_ADC_SCOUNT:
    d += (addr - ES_REG_DAC1_SCOUNT) >> 2;
    d->scount = (val & 0xffff) << 16 | (val & 0xffff);
    // trace_es1370_sample_count_wr(d - &s->chan[0],
    //    d->scount >> 16, d->scount & 0xffff);
    break;

  case ES_REG_ADC_FRAMEADR:
    d += 2;
    goto frameadr;
  case ES_REG_DAC1_FRAMEADR:
  case ES_REG_DAC2_FRAMEADR:
    d += (addr - ES_REG_DAC1_FRAMEADR) >> 3;
  frameadr:
    d->frame_addr = val;
    // trace_es1370_frame_address_wr(d - &s->chan[0], d->frame_addr);
    break;

  case ES_REG_PHANTOM_FRAMECNT:
    // lwarn("writing to phantom frame count 0x%" PRIx64, val);
    break;
  case ES_REG_PHANTOM_FRAMEADR:
    // lwarn("writing to phantom frame address 0x%" PRIx64, val);
    break;

  case ES_REG_ADC_FRAMECNT:
    d += 2;
    goto framecnt;
  case ES_REG_DAC1_FRAMECNT:
  case ES_REG_DAC2_FRAMECNT:
    d += (addr - ES_REG_DAC1_FRAMECNT) >> 3;
  framecnt:
    d->frame_cnt = val;
    d->leftover = 0;
    // trace_es1370_frame_count_wr(d - &s->chan[0],
    //     d->frame_cnt >> 16, d->frame_cnt & 0xffff);
    break;

  default:
    // lwarn("writel 0x%" PRIx64 " <- 0x%" PRIx64, addr, val);
    break;
  }
}

uint64_t CES137x::read_reg(void *opaque, u64 addr, unsigned size) {
  ES137xState *s = (ES137xState *)opaque;
  uint32_t val;
  struct chan *d = &s->chan[0];

  addr = fixup(s, addr);

  switch (addr) {
  case ES_REG_CONTROL:
    val = s->ctl;
    break;
  case ES_REG_STATUS:
    val = s->status;
    break;
  case ES_REG_MEMPAGE:
    val = s->mempage;
    break;
  case ES1371_REG_SMPRATE: /* ES1370_REG_CODEC on the older part */
    val = chip.ac97 ? src_read(s) : s->codec;
    break;
  case ES1371_REG_CODEC:
    val = chip.ac97 ? codec_read(s) : ~0U;
    break;
  case ES1371_REG_LEGACY:
    val = chip.ac97 ? s->legacy : ~0U;
    break;
  case ES_REG_SERIAL_CONTROL:
    val = s->sctl;
    break;

  case ES_REG_DAC1_SCOUNT:
  case ES_REG_DAC2_SCOUNT:
  case ES_REG_ADC_SCOUNT:
    d += (addr - ES_REG_DAC1_SCOUNT) >> 2;
    val = d->scount;
    break;

  case ES_REG_ADC_FRAMECNT:
    d += 2;
    goto framecnt;
  case ES_REG_DAC1_FRAMECNT:
  case ES_REG_DAC2_FRAMECNT:
    d += (addr - ES_REG_DAC1_FRAMECNT) >> 3;
  framecnt:
    val = d->frame_cnt;
    break;

  case ES_REG_ADC_FRAMEADR:
    d += 2;
    goto frameadr;
  case ES_REG_DAC1_FRAMEADR:
  case ES_REG_DAC2_FRAMEADR:
    d += (addr - ES_REG_DAC1_FRAMEADR) >> 3;
  frameadr:
    val = d->frame_addr;
    break;

  case ES_REG_PHANTOM_FRAMECNT:
    val = ~0U;
    // lwarn("reading from phantom frame count");
    break;
  case ES_REG_PHANTOM_FRAMEADR:
    val = ~0U;
    // lwarn("reading from phantom frame address");
    break;

  default:
    val = ~0U;
    // lwarn("readl 0x%" PRIx64 " -> 0x%x", addr, val);
    break;
  }
  return val;
}

u32 CES137x::ReadMem_Bar(int func, int bar, u32 address, int dsize) {
  std::lock_guard<std::recursive_mutex> lock(device_lock);
  if (bar != 0)
    return ~0U;
  if (dsize < 32) {
    auto val = CES137x::ReadMem_Bar(func, bar, address & ~3, 32);
    switch (dsize) {
    case 8:
      return (val >> ((address & 3) * 8)) & 0xff;
    case 16:
      if (address & 2) {
        return (val >> 16) & 0xffff;
      } else {
        return val & 0xffff;
      }
    }
  }
  if (dsize == 32) {
    return read_reg(&state, address, dsize);
  }
  if (dsize == 64) {
    uint64_t low = read_reg(&state, address, 32);
    uint64_t high = read_reg(&state, address + 4, 32);
    return low | (high << 32);
  }
  return ~0U;
}

void CES137x::WriteMem_Bar(int func, int bar, u32 address, int dsize,
                           u32 data) {
  std::lock_guard<std::recursive_mutex> lock(device_lock);
  if (bar != 0)
    return;

  if (dsize < 32) {
    auto val = CES137x::ReadMem_Bar(func, bar, address & ~3, 32);
    switch (dsize) {
    case 8: {
      val = (val & ~(0xff << ((address & 3) * 8))) |
            ((data & 0xff) << ((address & 3) * 8));
      CES137x::WriteMem_Bar(func, bar, address & ~3, 32, val);
      return;
    }
    case 16: {
      if (address & 2) {
        val = (val & 0xffff) | (data << 16);
      } else {
        val = (val & 0xffff0000) | data;
      }
      CES137x::WriteMem_Bar(func, bar, address & ~3, 32, val);
      return;
    }
    }
  }

  if (dsize == 32) {
    write_reg(&state, address, data, dsize);
    return;
  }
}

void CES137x::transfer_audio(ES137xState *s, struct chan *d, int loop_sel,
                             int maxb, bool *irq) {
  uint8_t tmpbuf[4096];
  const int index = d - &s->chan[0];
  const int sc = d->scount & 0xffff;
  int csc_bytes = ((d->scount >> 16) + 1) << d->shift;
  const bool nonloop = (s->sctl & loop_sel) != 0;
  int remaining = maxb;
  bool completed_period = false;

  /*
   * SDL asks for one refill of `maxb` bytes. Filling only up to the end of
   * the current sample period left the stream short whenever a refill
   * spanned a period boundary, so keep going until SDL's request is
   * satisfied (or the guest buffer / the stream runs out), re-reading the
   * frame counter each pass because the guest may advance it underneath us.
   */
  while (remaining > 0) {
    int cnt = d->frame_cnt >> 16;
    const int size = d->frame_cnt & 0xffff;
    if (size < cnt)
      break;

    /*
     * Bytes left in the guest's buffer. `leftover` is the partial-dword
     * offset already included in the address below, so it ADDS to the span
     * that remains -- upstream subtracts it here, which under-counts by up
     * to 3 bytes and can end the refill early.
     */
    const int left = ((size - cnt + 1) << 2) + (int)d->leftover;
    if (left <= 0)
      break;

    const int target = MIN(remaining, MIN(left, csc_bytes));
    if (target <= 0)
      break;

    uint32_t addr = d->frame_addr + (cnt << 2) + d->leftover;
    int transferred = 0;

    while (transferred < target) {
      const int to_copy = MIN(target - transferred, (int)sizeof(tmpbuf));
      int copied;

      if (index == ADC_CHANNEL) {
        copied = SDL_GetAudioStreamData(s->adc_voice, tmpbuf, to_copy);
        if (copied <= 0)
          break;
        do_pci_write(addr, tmpbuf, 1, copied);
      } else {
        do_pci_read(addr, tmpbuf, 1, to_copy);
        copied =
            SDL_PutAudioStreamData(s->dac_voice[index], tmpbuf, to_copy)
                ? to_copy
                : 0;
        if (!copied)
          break;
      }
      addr += copied;
      transferred += copied;
    }

    remaining -= transferred;
    csc_bytes -= transferred;
    if (csc_bytes <= 0) {
      /* A sample period finished: the guest gets an interrupt for it. */
      completed_period = true;
      csc_bytes = (sc + 1) << d->shift;
    }
    d->scount = sc | (((csc_bytes - 1) >> d->shift) << 16);

    cnt += (transferred + d->leftover) >> 2;
    if (!nonloop) {
      /*
       * loop_sel picks this channel's bit in SCTL: 0 means loop (interrupt
       * and keep going at the end of the buffer), 1 means stop.
       */
      d->frame_cnt = size;
      if (cnt <= size)
        d->frame_cnt |= cnt << 16;
    }
    d->leftover = (transferred + d->leftover) & 3;

    /* SDL refused more data, the capture queue ran dry, or we must stop. */
    if (transferred < target || nonloop)
      break;
  }

  /*
   * Report the interrupt for this refill only. Upstream latches it true and
   * never clears it, which leaves a completed period permanently sticky
   * once run_channel seeds *irq from the status register.
   */
  *irq = completed_period;
}

void CES137x::run_channel(ES137xState *s, size_t chan, int free_or_avail) {
  /* SDL's audio thread enters here; the guest's CPU threads enter through
     ReadMem_Bar/WriteMem_Bar. Both touch chan[] and s->status. */
  std::lock_guard<std::recursive_mutex> lock(device_lock);

  uint32_t new_status = s->status;
  int max_bytes;
  bool irq;
  struct chan *d = &s->chan[chan];
  const struct chan_bits *b = &es137x_chan_bits[chan];

  if (!(s->ctl & b->ctl_en) || (s->sctl & b->sctl_pause)) {
    return;
  }

  max_bytes = free_or_avail;
  max_bytes &= ~((1 << d->shift) - 1);
  if (max_bytes <= 0) { /* SDL can hand us 0 or a negative amount */
    return;
  }

  irq = s->sctl & b->sctl_inten && s->status & b->stat_int;

  transfer_audio(s, d, b->sctl_loopsel, max_bytes, &irq);

  if (irq) {
    if (s->sctl & b->sctl_inten) {
      new_status |= b->stat_int;
    }
  }

  if (new_status != s->status) {
    update_status(s, new_status);
  }
}

void CES137x::dac_callback_dac1(void *userdata, SDL_AudioStream *stream,
                                int additional_amount, int total_amount) {
  CES137x *dev = (CES137x *)userdata;
  ES137xState *s = &dev->state;
  dev->run_channel(s, 0, additional_amount);
}

void CES137x::dac_callback_dac2(void *userdata, SDL_AudioStream *stream,
                                int additional_amount, int total_amount) {
  CES137x *dev = (CES137x *)userdata;
  ES137xState *s = &dev->state;
  dev->run_channel(s, 1, additional_amount);
}

void CES137x::dac_callback_adc(void *userdata, SDL_AudioStream *stream,
                               int additional_amount, int total_amount) {
  CES137x *dev = (CES137x *)userdata;
  ES137xState *s = &dev->state;
  dev->run_channel(s, 2, additional_amount);
}
#endif /* HAVE_SDL */
