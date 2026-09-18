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
 * The Ensoniq AudioPCI family: the ES1370 and the ES1371.
 *
 * The two parts are one design either side of a redesign. The DMA engine
 * is the same on both -- the same three channels, each walking a ring of
 * frames in guest memory with the same frame address, frame count and
 * sample count registers, raising the same interrupt bits -- and so is the
 * serial control register that says what a channel's samples look like.
 * What changed is everything around the sound itself: the ES1370 has a
 * fixed set of DAC1 rates and a divider for DAC2, and its own mixer chip
 * written a register at a time through 0x10; the ES1371 put an AC'97 codec
 * on the other end of a serial link at 0x14, and a sample rate converter
 * whose coefficient RAM at 0x10 is now where a channel's rate is set.
 *
 * So the family shares one class with a row per part, the way the other
 * chip families here do, and the parts that only the ES1371 has -- the
 * codec and the rate converter -- live in ES137xCodec.cpp beside it.
 **/
#include "StdAfx.hpp"

#ifdef HAVE_SDL
#if !defined(__ES137X_H__)
#define __ES137X_H__

#include "ES137xRegs.hpp"
#include "PCIDevice.hpp"
#include "System.hpp"
#include "SystemComponent.hpp"

#include <SDL3/SDL.h>
#include <mutex>

/**
 * \brief One part of the AudioPCI family.
 *
 * What a row says is what the guest can see from the outside: the name it
 * is configured by, what the part is called, and what it answers in PCI
 * configuration space. `ac97` is the one thing that changes the emulation
 * itself -- it selects the ES1371's codec and rate converter over the
 * ES1370's fixed rates and its own mixer.
 **/
struct es137x_chip_config {
  const char *name;    ///< configuration class name
  const char *part;    ///< what we call the chip in messages
  u16 device;          ///< PCI device id
  u8 revision;         ///< PCI revision id
  u32 bar_mask;        ///< BAR0's writable bits, which is its size
  u16 subsys_vendor;   ///< subsystem vendor id
  u16 subsys_device;   ///< subsystem id
  bool ac97;           ///< AC'97 codec and rate converter, not a fixed mixer
  u32 codec_vendor_id; ///< what the AC'97 codec calls itself, if there is one
};

/**
 * \brief The AC'97 codec on the far end of an ES1371's serial link.
 *
 * Nothing of the sound passes through here -- the samples go straight from
 * guest memory to the host, as they do on the ES1370. What a codec is, to
 * a driver, is a register file it reads its identity and its mixer out of,
 * and that is what this is: 64 sixteen-bit registers with the reset values
 * the specification gives, and an identity in the last two.
 **/
class CAc97Codec {
public:
  explicit CAc97Codec(u32 vendor_id);
  void reset();
  u16 read(u8 reg) const;
  void write(u8 reg, u16 value);

private:
  u32 id;
  u16 reg[64];
};

void es137x_config_space(const es137x_chip_config &chip, u32 *data, u32 *mask);

/**
 * \brief Ensoniq AudioPCI (ES1370, ES1371) sound card.
 **/
class CES137x : public CPCIDevice {
public:
  virtual int SaveState(FILE *f) { return 0; }
  virtual int RestoreState(FILE *f) { return 0; }
  virtual void check_state() {}
  virtual void init();

  virtual void WriteMem_Bar(int func, int bar, u32 address, int dsize,
                            u32 data);
  virtual u32 ReadMem_Bar(int func, int bar, u32 address, int dsize);

  CES137x(CConfigurator *cfg, class CSystem *c, int pcibus, int pcidev,
          const es137x_chip_config &chip);
  virtual ~CES137x();

  static const es137x_chip_config *find_chip(const char *name);

private:
  const es137x_chip_config chip;

  u32 cfg_data[64] = {};
  u32 cfg_mask[64] = {};

  /// Serializes the device against SDL's audio callback thread, which runs
  /// run_channel() concurrently with guest MMIO from the CPU threads:
  /// both mutate chan[] (frame_cnt/scount/leftover) and s->status, and the
  /// callback DMAs into guest memory. Recursive because ReadMem_Bar calls
  /// itself for sub-32-bit accesses and WriteMem_Bar calls both.
  std::recursive_mutex device_lock;

  struct chan {
    uint32_t shift;
    uint32_t leftover;
    uint32_t scount;
    uint32_t frame_addr;
    uint32_t frame_cnt;
    /// The rate the stream was last set to. Only the ES1371 needs it: its
    /// rate comes out of the converter's RAM rather than out of a field of
    /// the control register, so there is no earlier value of a register to
    /// compare against.
    uint32_t freq;
  };

  struct ES137xState {
    SDL_AudioDeviceID audio_be_out;
    SDL_AudioDeviceID audio_be_in;
    struct chan chan[3];
    SDL_AudioStream *dac_voice[2];
    SDL_AudioStream *adc_voice;

    uint32_t ctl;
    uint32_t status;
    uint32_t mempage;
    uint32_t codec;
    uint32_t sctl;

    /// ES1371 only: the legacy-decode register, the rate converter's
    /// interface register as last written and the phase its state bits
    /// read back in, the value a codec read left behind, and the
    /// converter's RAM.
    uint32_t legacy;
    uint32_t smprate;
    uint32_t src_phase;
    uint32_t codec_result;
    uint16_t src_ram[SRC_RAM_WORDS];
  } state;

  CAc97Codec ac97;

  struct chan_bits {
    uint32_t ctl_en;
    uint32_t stat_int;
    uint32_t sctl_pause;
    uint32_t sctl_inten;
    uint32_t sctl_fmt;
    uint32_t sctl_sh_fmt;
    uint32_t sctl_loopsel;
    void (*calc_freq)(ES137xState *s, uint32_t ctl, uint32_t *old_freq,
                      uint32_t *new_freq);
  };

  static void es1370_dac1_calc_freq(ES137xState *s, uint32_t ctl,
                                    uint32_t *old_freq, uint32_t *new_freq);

  static void es1370_dac2_and_adc_calc_freq(ES137xState *s, uint32_t ctl,
                                            uint32_t *old_freq,
                                            uint32_t *new_freq);

  struct chan_bits es137x_chan_bits[3] = {
      {CTRL_DAC1_EN, STAT_DAC1, SCTRL_P1PAUSE, SCTRL_P1INTEN, SCTRL_P1FMT,
       SCTRL_SH_P1FMT, SCTRL_P1LOOPSEL, es1370_dac1_calc_freq},

      {CTRL_DAC2_EN, STAT_DAC2, SCTRL_P2PAUSE, SCTRL_P2INTEN, SCTRL_P2FMT,
       SCTRL_SH_P2FMT, SCTRL_P2LOOPSEL, es1370_dac2_and_adc_calc_freq},

      {CTRL_ADC_EN, STAT_ADC, 0, SCTRL_R1INTEN, SCTRL_R1FMT, SCTRL_SH_R1FMT,
       SCTRL_R1LOOPSEL, es1370_dac2_and_adc_calc_freq}};
  void update_status(ES137xState *s, uint32_t new_status);
  void reset(ES137xState *s);
  void maybe_lower_irq(ES137xState *s, uint32_t sctl);
  void update_voices(ES137xState *s, uint32_t ctl, uint32_t sctl);
  uint32_t fixup(ES137xState *s, uint32_t addr);
  void write_reg(void *opaque, u64 addr, uint64_t val, unsigned size);
  uint64_t read_reg(void *opaque, u64 addr, unsigned size);
  void transfer_audio(ES137xState *s, struct chan *d, int loop_sel, int max,
                      bool *irq);
  void run_channel(ES137xState *s, size_t chan, int free_or_avail);

  /* ES137xCodec.cpp: the ES1371's serial codec interface and its sample
     rate converter. */
  void codec_write(ES137xState *s, uint32_t val);
  uint32_t codec_read(ES137xState *s);
  void src_write(ES137xState *s, uint32_t val);
  uint32_t src_read(ES137xState *s);
  uint32_t src_rate(ES137xState *s, size_t chan);
  void es1371_src_calc_freq(ES137xState *s, size_t chan, uint32_t *old_freq,
                            uint32_t *new_freq);

  static void dac_callback_dac1(void *userdata, SDL_AudioStream *stream,
                                int additional_amount, int total_amount);
  static void dac_callback_dac2(void *userdata, SDL_AudioStream *stream,
                                int additional_amount, int total_amount);
  static void dac_callback_adc(void *userdata, SDL_AudioStream *stream,
                               int additional_amount, int total_amount);
};
#endif //! defined(__ES137X_H__)
#endif /* HAVE_SDL */
