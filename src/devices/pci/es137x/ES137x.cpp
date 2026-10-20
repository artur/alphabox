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
 * Modifications copyright (C) 2026 Cacodemon345 and the ES40-Emu
 * contributors
 * Website: https://github.com/ES40-Emu/es40
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
  // The recording device is opened when the guest first turns its ADC on,
  // not here: asking for it costs the user a microphone permission prompt on
  // some hosts, and a machine with a sound card in it is not a machine that
  // is recording anything.
  state.audio_be_in = 0;
  state.audio_be_out =
      SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, nullptr);
  if (!state.audio_be_out) {
    FAILURE_1(SDL, "Failed to initialize SDL audio output: %s", SDL_GetError());
  }
  es137x_wav_tap(state.audio_be_out);
  state.adc_voice = SDL_CreateAudioStream(NULL, &as);
  state.dac_voice[0] = SDL_CreateAudioStream(&as, NULL);
  state.dac_voice[1] = SDL_CreateAudioStream(&as, NULL);

  SDL_SetAudioStreamPutCallback(state.adc_voice, dac_callback_adc, this);
  SDL_SetAudioStreamGetCallback(state.dac_voice[0], dac_callback_dac1, this);
  SDL_SetAudioStreamGetCallback(state.dac_voice[1], dac_callback_dac2, this);
}

CES137x::~CES137x() {
  selftest_join();
  SDL_DestroyAudioStream(state.adc_voice);
  SDL_DestroyAudioStream(state.dac_voice[0]);
  SDL_DestroyAudioStream(state.dac_voice[1]);
  if (state.audio_be_in)
    SDL_CloseAudioDevice(state.audio_be_in);
  SDL_CloseAudioDevice(state.audio_be_out);
}

/**
 * Open the host's recording device, the first time the guest asks to record.
 *
 * A host may have no input at all, or may refuse it -- macOS asks the user
 * whether this program may use the microphone. Neither is a reason to stop
 * the machine: the guest gets silence, which is what a sound card with
 * nothing plugged into its line-in gives you anyway.
 **/
bool CES137x::open_capture() {
  if (state.audio_be_in)
    return true;
  if (state.capture_refused)
    return false;

  state.audio_be_in =
      SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_RECORDING, nullptr);
  if (!state.audio_be_in) {
    printf("%s: no recording device (%s); the guest will record silence.\n",
           devid_string, SDL_GetError());
    state.capture_refused = true;
    return false;
  }
  return true;
}

void CES137x::init() {
  es137x_config_space(chip, cfg_data, cfg_mask);
  add_function(0, cfg_data, cfg_mask);
  ResetPCI(); // which also resets the card itself
  selftest_start();

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
  }
  do_pci_interrupt(0, 0);
  /* Forced: the streams may be bound and set to anything, and even the
     power-on registers describe a format (DAC1 at 5.512 kHz, 8-bit mono)
     the host stream is not in. */
  update_voices(s, s->ctl, s->sctl, true);
}

SDL_AudioStream *CES137x::voice_stream(int voice) {
  return voice == ADC_CHANNEL ? state.adc_voice : state.dac_voice[voice];
}

void CES137x::queue_sdl(const SdlAction &a) { pending.push_back(a); }

/**
 * Do what the registers asked of the host streams. The caller holds
 * sdl_lock and not device_lock (see the lock order at device_lock), so SDL
 * can take its stream lock while an audio callback, which holds that lock
 * and wants device_lock, is waiting for nothing we hold.
 **/
void CES137x::apply_sdl_actions() {
  std::vector<SdlAction> todo;
  {
    std::lock_guard<std::recursive_mutex> lock(device_lock);
    todo.swap(pending);
  }
  for (const SdlAction &a : todo) {
    SDL_AudioStream *st = voice_stream(a.voice);
    switch (a.kind) {
    case SdlAction::Format:
      SDL_SetAudioStreamFormat(st, &a.spec, &a.spec);
      break;
    case SdlAction::Bind: {
      SDL_AudioDeviceID dev = 0;
      if (a.voice == ADC_CHANNEL) {
        if (open_capture())
          dev = state.audio_be_in;
      } else {
        dev = state.audio_be_out;
      }
      if (dev)
        SDL_BindAudioStream(dev, st);
      break;
    }
    case SdlAction::Unbind:
      SDL_UnbindAudioStream(st);
      break;
    }
  }
}

/**
 * A machine reset, or the guest's own: the card goes back to its power-on
 * state. The DMA channels stop (the old rings are memory the guest has
 * since reused), the interrupt latch and the line clear, and the host
 * streams are unbound.
 **/
void CES137x::ResetPCI() {
  CPCIDevice::ResetPCI();
  std::lock_guard<std::mutex> sl(sdl_lock);
  {
    std::lock_guard<std::recursive_mutex> lock(device_lock);
    reset(&state);
  }
  apply_sdl_actions();
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

void CES137x::update_voices(ES137xState *s, uint32_t ctl, uint32_t sctl,
                            bool force) {
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

    if (force || (old_fmt != new_fmt) || (old_freq != new_freq)) {
      d->shift = (new_fmt & 1) + (new_fmt >> 1);
      if (new_freq) {
        SdlAction a = {};
        a.kind = SdlAction::Format;
        a.voice = (int)i;
        a.spec.freq = new_freq;
        a.spec.channels = 1 << (new_fmt & 1);
        a.spec.format = (new_fmt & 2) ? SDL_AUDIO_S16LE : SDL_AUDIO_U8;
        queue_sdl(a);
      }
    }
    d->freq = new_freq;

    /* The ADC has a second gate besides its enable: the stop bit, which
       holds off the record transfers (ES1370 specification Rev 2.3 p10,
       control register bit 31; ES1371 specification Rev 1.1 p15, bit 13). */
    const uint32_t adc_stop =
        (i == ADC_CHANNEL) ? (chip.ac97 ? CTRL_1371_ADC_STOP : CTRL_ADC_STOP)
                           : 0;

    if (force || ((ctl ^ s->ctl) & (b->ctl_en | adc_stop)) ||
        ((sctl ^ s->sctl) & b->sctl_pause)) {
      const bool on =
          (ctl & b->ctl_en) && !(sctl & b->sctl_pause) && !(ctl & adc_stop);
      SdlAction a = {};
      a.kind = on ? SdlAction::Bind : SdlAction::Unbind;
      a.voice = (int)i;
      queue_sdl(a);
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
  /* sdl_lock, then device_lock for the register work, then neither while
     SDL is called: see the lock order at device_lock. */
  std::lock_guard<std::mutex> sl(sdl_lock);
  {
    std::lock_guard<std::recursive_mutex> lock(device_lock);
    write_bar(func, bar, address, dsize, data);
  }
  apply_sdl_actions();
}

void CES137x::write_bar(int func, int bar, u32 address, int dsize, u32 data) {
  if (bar != 0)
    return;

  if (dsize < 32) {
    auto val = CES137x::ReadMem_Bar(func, bar, address & ~3, 32);
    switch (dsize) {
    case 8: {
      val = (val & ~(0xff << ((address & 3) * 8))) |
            ((data & 0xff) << ((address & 3) * 8));
      write_bar(func, bar, address & ~3, 32, val);
      return;
    }
    case 16: {
      if (address & 2) {
        val = (val & 0xffff) | (data << 16);
      } else {
        val = (val & 0xffff0000) | data;
      }
      write_bar(func, bar, address & ~3, 32, val);
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
     * Bytes left in the guest's buffer. The ring is size + 1 dwords long
     * and the address below already includes `leftover`, the partial-dword
     * offset into dword `cnt`, so the span that remains is the ring's end
     * less that offset. (Adding it, as this once did, believed twice
     * `leftover`, up to 6 bytes, more remained than do, and a refill ending
     * at the seam read or, for capture, wrote past the guest's ring.)
     */
    const int left = ((size - cnt + 1) << 2) - (int)d->leftover;
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
  if (chan == ADC_CHANNEL &&
      (s->ctl & (chip.ac97 ? CTRL_1371_ADC_STOP : CTRL_ADC_STOP))) {
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
/**
 * The card's state in a snapshot, ahead of the PCI block that
 * CPCIDevice::SaveState writes: the registers, the three channels' DMA
 * positions, the converter's RAM and the codec's register file. The host
 * streams are not in it; RestoreState binds them again from the control
 * registers.
 *
 * Files written before this block existed carry nothing for this card at
 * all (not even its PCI block), so they go straight to the next device's
 * data. RestoreState recognises that by the absence of the magic, rewinds,
 * and leaves the card as it was at power-on, as such a file always did.
 **/
namespace {
constexpr u32 kEsMagic = 0x45533731; // 'ES71'
constexpr u32 kEsVersion = 1;
struct EsChanState {
  u32 shift, leftover, scount, frame_addr, frame_cnt, freq;
};
struct EsCardState {
  u32 version;
  u32 ctl, status, mempage, codec, sctl;
  u32 legacy, smprate, src_phase, codec_result;
  EsChanState chan[NB_CHANNELS];
  u16 src_ram[SRC_RAM_WORDS];
  u16 ac97[64];
};
} // namespace

int CES137x::SaveState(FILE *f) {
  EsCardState x;
  memset(&x, 0, sizeof(x));
  {
    std::lock_guard<std::recursive_mutex> lock(device_lock);
    x.version = kEsVersion;
    x.ctl = state.ctl;
    x.status = state.status;
    x.mempage = state.mempage;
    x.codec = state.codec;
    x.sctl = state.sctl;
    x.legacy = state.legacy;
    x.smprate = state.smprate;
    x.src_phase = state.src_phase;
    x.codec_result = state.codec_result;
    for (int i = 0; i < NB_CHANNELS; i++) {
      const struct chan &c = state.chan[i];
      x.chan[i] = {c.shift,      c.leftover,  c.scount,
                   c.frame_addr, c.frame_cnt, c.freq};
    }
    memcpy(x.src_ram, state.src_ram, sizeof(x.src_ram));
    ac97.dump(x.ac97);
  }
  const long n = sizeof(x);
  fwrite(&kEsMagic, sizeof(u32), 1, f);
  fwrite(&n, sizeof(long), 1, f);
  if (fwrite(&x, 1, sizeof(x), f) != sizeof(x))
    return -1;
  fwrite(&kEsMagic, sizeof(u32), 1, f);
  return CPCIDevice::SaveState(f);
}

int CES137x::RestoreState(FILE *f) {
  u32 m;
  const long pos = ftell(f);
  if (fread(&m, sizeof(u32), 1, f) != 1 || m != kEsMagic) {
    // An older file: nothing was written for this card.
    if (fseek(f, pos, SEEK_SET)) {
      printf("%s: cannot reposition the state file!\n", devid_string);
      return -1;
    }
    return 0;
  }
  long n;
  EsCardState x;
  if (fread(&n, sizeof(long), 1, f) != 1 || n != (long)sizeof(x)) {
    printf("%s: ES137x STRUCT SIZE does not match!\n", devid_string);
    return -1;
  }
  if (fread(&x, 1, sizeof(x), f) != sizeof(x) || fread(&m, 4, 1, f) != 1 ||
      m != kEsMagic) {
    printf("%s: ES137x end MAGIC does not match!\n", devid_string);
    return -1;
  }
  if (x.version != kEsVersion) {
    printf("%s: ES137x state version %u is not supported!\n", devid_string,
           x.version);
    return -1;
  }

  {
    std::lock_guard<std::mutex> sl(sdl_lock);
    {
      std::lock_guard<std::recursive_mutex> lock(device_lock);
      ES137xState *s = &state;
      pending.clear();
      /* The streams are where the registers last put them, which is not
         where the file's registers want them: start from "nothing enabled"
         and let a forced update_voices() set every format and binding. */
      s->ctl = 0;
      s->sctl = 0;
      s->mempage = x.mempage & 0xf;
      s->codec = x.codec;
      s->status = x.status;
      s->legacy = x.legacy;
      s->smprate = x.smprate;
      s->src_phase = x.src_phase;
      s->codec_result = x.codec_result;
      memcpy(s->src_ram, x.src_ram, sizeof(s->src_ram));
      ac97.load(x.ac97);
      update_voices(s, x.ctl, x.sctl, true);
      for (int i = 0; i < NB_CHANNELS; i++) {
        struct chan &c = s->chan[i];
        c.leftover = x.chan[i].leftover & 3;
        c.scount = x.chan[i].scount;
        c.frame_addr = x.chan[i].frame_addr;
        c.frame_cnt = x.chan[i].frame_cnt;
      }
    }
    apply_sdl_actions();
  }
  return CPCIDevice::RestoreState(f);
}
#endif /* HAVE_SDL */
