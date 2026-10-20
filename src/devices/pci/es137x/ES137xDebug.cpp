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
 * Test hooks for the AudioPCI, both off unless an environment variable
 * asks (the same family as ALPHABOX_USBAUDIO_WAV).
 *
 * ALPHABOX_ES137X_WAV=<file> writes what the card plays, after the mixer's
 * gain, as a 16-bit WAV file (test/tools/wav_compare.py reads it).
 *
 * ALPHABOX_ES137X_SELFTEST=<mode>[:<seconds>] runs a guest-less driver of
 * the card from a thread of its own, going through WriteMem_Bar as a CPU
 * thread would:
 *   tone    plays a looping 440 Hz tone from a ring of guest memory
 *   stress  starts, stops and re-rates a playing channel as fast as it can
 *           for the given seconds, then reports; if it never reports the
 *           card has deadlocked
 *   mute    plays the tone with the mixer at 0 dB, then at -12 dB, then
 *           muted, three seconds each
 *   ring    records from the ADC into an 8-bit mono ring that has a guard
 *           region after it, and reports whether the guard was touched
 * The thread waits for the console to have set up the PCI DMA windows
 * before it starts.
 **/
#include "StdAfx.hpp"

#ifdef HAVE_SDL
#include "ES137x.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

namespace {

/* ---- the WAV tap -------------------------------------------------------- */

struct WavTap {
  FILE *f = nullptr;
  int rate = 0;
  uint64_t bytes = 0;
  uint64_t synced = 0;
  bool started = false; ///< no file until the first non-silent buffer
  std::string name;
};
WavTap g_tap;

void wav_header(FILE *f, int rate, uint64_t bytes) {
  uint8_t h[44];
  auto le32 = [&](int o, uint32_t v) {
    for (int i = 0; i < 4; i++)
      h[o + i] = (uint8_t)(v >> (8 * i));
  };
  auto le16 = [&](int o, uint16_t v) {
    h[o] = (uint8_t)v;
    h[o + 1] = (uint8_t)(v >> 8);
  };
  memcpy(h, "RIFF", 4);
  le32(4, (uint32_t)(36 + bytes));
  memcpy(h + 8, "WAVEfmt ", 8);
  le32(16, 16);
  le16(20, 1);
  le16(22, 2);
  le32(24, (uint32_t)rate);
  le32(28, (uint32_t)rate * 4);
  le16(32, 4);
  le16(34, 16);
  memcpy(h + 36, "data", 4);
  le32(40, (uint32_t)bytes);
  fseek(f, 0, SEEK_SET);
  fwrite(h, 1, sizeof(h), f);
  fseek(f, 0, SEEK_END);
}

void wav_close() {
  if (g_tap.f) {
    wav_header(g_tap.f, g_tap.rate, g_tap.bytes);
    fclose(g_tap.f);
    g_tap.f = nullptr;
  }
}

void SDLCALL postmix(void *, const SDL_AudioSpec *spec, float *buf,
                     int buflen) {
  const int n = buflen / (int)sizeof(float);
  if (!g_tap.f) {
    if (g_tap.started)
      return;
    bool loud = false;
    for (int i = 0; i < n && !loud; i++)
      loud = buf[i] != 0.0f;
    if (!loud)
      return;
    g_tap.f = fopen(g_tap.name.c_str(), "wb");
    if (!g_tap.f)
      return;
    g_tap.started = true;
    g_tap.rate = spec->freq;
    wav_header(g_tap.f, g_tap.rate, 0);
    atexit(wav_close);
  }
  const int ch = spec->channels;
  for (int i = 0; i + ch <= n; i += ch) {
    for (int c = 0; c < 2; c++) {
      float v = buf[i + (ch > 1 ? c : 0)];
      v = v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v);
      int16_t s = (int16_t)lrintf(v * 32767.0f);
      fwrite(&s, 2, 1, g_tap.f);
    }
    g_tap.bytes += 4;
  }
  if (g_tap.bytes - g_tap.synced >= (uint64_t)g_tap.rate * 4) {
    wav_header(g_tap.f, g_tap.rate, g_tap.bytes);
    fflush(g_tap.f);
    g_tap.synced = g_tap.bytes;
  }
}

} // namespace

/** Hook the tap onto the playback device, if ALPHABOX_ES137X_WAV is set. */
void es137x_wav_tap(SDL_AudioDeviceID dev) {
  const char *w = getenv("ALPHABOX_ES137X_WAV");
  if (!w || !*w || !dev)
    return;
  g_tap.name = w;
  SDL_SetAudioPostmixCallback(dev, postmix, nullptr);
}

/* ---- the self-test ------------------------------------------------------ */

void CES137x::selftest_start() {
  const char *e = getenv("ALPHABOX_ES137X_SELFTEST");
  if (!e || !*e)
    return;
  selftest_thread =
      std::thread([this, mode = std::string(e)] { selftest(mode); });
}

void CES137x::selftest_join() {
  selftest_stop = true;
  if (selftest_thread.joinable())
    selftest_thread.join();
}

void CES137x::selftest(const std::string &spec) {
  std::string mode = spec;
  int secs = 10;
  const size_t colon = spec.find(':');
  if (colon != std::string::npos) {
    mode = spec.substr(0, colon);
    secs = atoi(spec.c_str() + colon + 1);
  }
  auto nap = [&](int ms) {
    for (int t = 0; t < ms && !selftest_stop; t += 20)
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
  };
  auto wr = [&](u32 reg, u32 val) { WriteMem_Bar(0, 0, reg, 32, val); };
  auto rd = [&](u32 reg) { return ReadMem_Bar(0, 0, reg, 32); };

  /* A PCI address that is main memory: the console programs the DMA
     windows during its boot, so keep looking until one answers. */
  u32 ring = 0;
  for (int tries = 0; tries < 600 && !ring && !selftest_stop; tries++) {
    for (u32 base : {0x40000000u, 0x80000000u, 0x00800000u, 0u}) {
      const u32 a = base + 0x01000000;
      u64 phys = cSystem->PCI_Phys(myPCIBus, a);
      if (cSystem->PtrToMem(phys) && cSystem->PtrToMem(phys + 0x7fffff)) {
        ring = a;
        break;
      }
    }
    if (!ring)
      nap(500);
  }
  if (!ring) {
    printf("%s: selftest: no DMA window to put a ring in.\n", devid_string);
    return;
  }
  nap(2000);

  const u32 kDwords = 44100; // one second of 16-bit stereo at 44.1 kHz
  auto set_rate = [&](u32 base, u32 hz) {
    /* the inverse of src_rate(): a step in units of 1/32768 of 3 kHz */
    const u32 step = (u32)(((u64)hz * 32768 + 1500) / 3000);
    const u32 ip = step >> 15, fp = step & 0x7fff;
    wr(ES1371_REG_SMPRATE, ((base + SRC_RAM_INT_REGS) << SRC_SH_RAM_ADDR) |
                               SRC_RAM_WE | ((ip << 10) & 0xfc00));
    wr(ES1371_REG_SMPRATE,
       ((base + SRC_RAM_VFREQ_FRAC) << SRC_SH_RAM_ADDR) | SRC_RAM_WE | fp);
  };
  auto codec = [&](u32 reg, u32 val) {
    wr(ES1371_REG_CODEC, (reg << CODEC_SH_ADDR) | val);
  };
  auto ring_for_dac2 = [&](u32 dwords) {
    wr(ES_REG_MEMPAGE, 0xc);
    wr(0x38, ring);
    wr(0x3c, (dwords - 1) & 0xffff);
  };
  auto tone = [&] {
    std::vector<int16_t> pcm((size_t)kDwords * 2);
    for (u32 i = 0; i < kDwords; i++) {
      const int16_t v = (int16_t)(12000.0 * sin(2 * M_PI * 440.0 * i / 44100));
      pcm[2 * i] = pcm[2 * i + 1] = v;
    }
    do_pci_write(ring, pcm.data(), 1, pcm.size() * 2);
  };

  if (mode == "tone" || mode == "mute") {
    printf("%s: selftest %s: ring at PCI 0x%08x\n", devid_string, mode.c_str(),
           ring);
    tone();
    codec(0x02, 0x0000); // master: 0 dB, unmuted
    codec(0x18, 0x0808); // PCM out: 0 dB, unmuted
    set_rate(SRC_RAM_DAC2, 44100);
    ring_for_dac2(kDwords);
    wr(ES_REG_SERIAL_CONTROL, SCTRL_P2FMT);
    wr(ES_REG_CONTROL, rd(ES_REG_CONTROL) | CTRL_DAC2_EN);
    if (mode == "tone") {
      nap(secs * 1000);
    } else {
      nap(3000);
      codec(0x02, 0x0808); // -12 dB on both channels (8 steps of 1.5)
      nap(3000);
      codec(0x02, 0x8000); // muted
      nap(3000);
      codec(0x02, 0x0000);
    }
    wr(ES_REG_CONTROL, rd(ES_REG_CONTROL) & ~(u32)CTRL_DAC2_EN);
    printf("%s: selftest %s: done\n", devid_string, mode.c_str());
    return;
  }

  if (mode == "stress") {
    printf("%s: selftest stress for %d s\n", devid_string, secs);
    tone();
    codec(0x02, 0x0000);
    codec(0x18, 0x0808);
    ring_for_dac2(kDwords);
    wr(ES_REG_SERIAL_CONTROL, SCTRL_P2FMT);
    uint64_t ops = 0;
    const auto t0 = std::chrono::steady_clock::now();
    const u32 rates[] = {44100, 22050, 48000, 11025, 32000};
    while (!selftest_stop &&
           std::chrono::steady_clock::now() - t0 < std::chrono::seconds(secs)) {
      for (int k = 0; k < 5; k++) {
        set_rate(SRC_RAM_DAC2, rates[(ops + k) % 5]);
        wr(ES_REG_CONTROL, rd(ES_REG_CONTROL) | CTRL_DAC2_EN);
        std::this_thread::sleep_for(std::chrono::microseconds(500));
        wr(ES_REG_SERIAL_CONTROL, SCTRL_P2FMT | ((k & 1) ? SCTRL_P2PAUSE : 0));
        wr(ES_REG_CONTROL, rd(ES_REG_CONTROL) & ~(u32)CTRL_DAC2_EN);
        ops += 4;
      }
      wr(ES_REG_SERIAL_CONTROL, SCTRL_P2FMT);
    }
    printf("%s: selftest stress: %llu operations, no hang\n", devid_string,
           (unsigned long long)ops);
    return;
  }

  if (mode == "ring") {
    /* 100 bytes of 8-bit mono: 25 dwords, ring size field 24, wrapped
       many times a second so that a refill ends at every offset in the
       dword. The bytes after the ring are the guard. */
    const u32 bytes = 100, dwords = bytes / 4;
    std::vector<uint8_t> fill(bytes + 64, 0xaa);
    do_pci_write(ring, fill.data(), 1, fill.size());
    set_rate(SRC_RAM_ADC, 8000);
    wr(ES_REG_MEMPAGE, 0xd);
    wr(0x30, ring);
    wr(0x34, (dwords - 1) & 0xffff);
    wr(ES_REG_SERIAL_CONTROL, 0); // ADC: 8-bit mono
    wr(ES_REG_ADC_SCOUNT, 255);
    wr(ES_REG_CONTROL, rd(ES_REG_CONTROL) | CTRL_ADC_EN);
    nap(secs * 1000);
    wr(ES_REG_CONTROL, rd(ES_REG_CONTROL) & ~(u32)CTRL_ADC_EN);
    std::vector<uint8_t> back(bytes + 64);
    do_pci_read(ring, back.data(), 1, back.size());
    int touched = 0, written = 0;
    for (u32 i = bytes; i < bytes + 64; i++)
      touched += back[i] != 0xaa;
    for (u32 i = 0; i < bytes; i++)
      written += back[i] != 0xaa;
    printf("%s: selftest ring: %d of %u ring bytes written, %d guard bytes "
           "touched\n",
           devid_string, written, bytes, touched);
    return;
  }
  printf("%s: selftest: unknown mode '%s'\n", devid_string, mode.c_str());
}
#endif /* HAVE_SDL */
