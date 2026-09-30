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

#include "UsbAudio.hpp"
#include "StdAfx.hpp"
#include <cmath>
#include <cstring>

#ifdef HAVE_SDL
#include <SDL3/SDL.h>
#endif

static const bool g_usbtrace = getenv("ALPHABOX_USBTRACE") != nullptr;

// The terminal and unit IDs of the audio function.
enum { ID_INPUT = 1, ID_FEATURE = 2, ID_OUTPUT = 3 };
// Sampling frequencies offered, and the largest packet one frame can need
// (48 kHz: 48 frames of 4 bytes; 44.1 kHz alternates 44 and 45 frames).
static const int kRates[] = {48000, 44100};
static const int kMaxPacket = 192;
// The feature unit's volume range, 1/256 dB: -60 dB to 0 dB in 1 dB steps.
static const s16 kVolMin = -60 * 256, kVolMax = 0, kVolRes = 256;

static void wav_header(FILE *f, int rate, u64 bytes);

static const std::vector<u8> kDeviceDescriptor = {
    18,   1,       // bLength, DEVICE
    0x10, 0x01,    // USB 1.1
    0,    0,    0, // class in the interfaces
    8,             // endpoint 0 max packet
    0x09, 0x12,    // idVendor 0x1209 (pid.codes)
    0x03, 0xa1,    // idProduct 0xa103
    0x00, 0x01,    // bcdDevice 1.00
    1,    2,    3, // manufacturer, product, serial strings
    1,             // one configuration
};

static std::vector<u8> make_configuration() {
  // The AudioControl interface's class-specific block: header, input
  // terminal (USB streaming), feature unit (master mute + volume), output
  // terminal (speaker).
  std::vector<u8> ac = {
      // Header: ADC 1.00, total length (patched below), one streaming
      // interface in the collection: interface 1.
      9, 0x24, 0x01, 0x00, 0x01, 0, 0, 1, 1,
      // Input terminal 1: USB streaming, two channels, left + right front.
      12, 0x24, 0x02, ID_INPUT, 0x01, 0x01, 0, 2, 0x03, 0x00, 0, 0,
      // Feature unit 2, fed by 1: one byte of controls per channel; master
      // has mute (bit 0) and volume (bit 1), the channels none.
      10, 0x24, 0x06, ID_FEATURE, ID_INPUT, 1, 0x03, 0x00, 0x00, 0,
      // Output terminal 3: speaker, fed by 2.
      9, 0x24, 0x03, ID_OUTPUT, 0x01, 0x03, 0, ID_FEATURE, 0};
  ac[5] = (u8)(ac.size() & 0xff);
  ac[6] = (u8)(ac.size() >> 8);

  std::vector<u8> fmt = {
      // Type I format: two channels, two-byte subframes, 16 bits, and a
      // list of discrete sampling frequencies.
      0, 0x24, 0x02, 0x01, 2, 2, 16, (u8)(sizeof(kRates) / sizeof(kRates[0]))};
  for (int hz : kRates) {
    fmt.push_back((u8)(hz & 0xff));
    fmt.push_back((u8)((hz >> 8) & 0xff));
    fmt.push_back((u8)(hz >> 16));
  }
  fmt[0] = (u8)fmt.size();

  std::vector<u8> d = {
      // Configuration: total length patched below, two interfaces.
      9, 2, 0, 0, 2, 1, 0, 0x80, 50,
      // Interface 0: AudioControl, no endpoints.
      9, 4, 0, 0, 0, 0x01, 0x01, 0, 0};
  d.insert(d.end(), ac.begin(), ac.end());
  const std::vector<u8> as = {
      // Interface 1, setting 0: AudioStreaming, zero bandwidth.
      9, 4, 1, 0, 0, 0x01, 0x02, 0, 0,
      // Interface 1, setting 1: AudioStreaming, one endpoint.
      9, 4, 1, 1, 1, 0x01, 0x02, 0, 0,
      // AS general: linked to terminal 1, one frame of delay, PCM.
      7, 0x24, 0x01, ID_INPUT, 1, 0x01, 0x00};
  d.insert(d.end(), as.begin(), as.end());
  d.insert(d.end(), fmt.begin(), fmt.end());
  const std::vector<u8> ep = {
      // Endpoint 1 OUT: isochronous, adaptive, every frame (the audio
      // class's nine-byte form: bRefresh and bSynchAddress unused).
      9, 5, 0x01, 0x09, (u8)(kMaxPacket & 0xff), (u8)(kMaxPacket >> 8), 1, 0, 0,
      // Class-specific endpoint: sampling frequency control; no pitch
      // control; packets need not be full.
      7, 0x25, 0x01, 0x01, 0, 0, 0};
  d.insert(d.end(), ep.begin(), ep.end());
  d[2] = (u8)(d.size() & 0xff);
  d[3] = (u8)(d.size() >> 8);
  return d;
}
static const std::vector<u8> kConfigurationDescriptor = make_configuration();

CUsbAudio::CUsbAudio() {
  if (const char *w = getenv("ALPHABOX_USBAUDIO_WAV"))
    m_wav_name = w;
#ifdef HAVE_SDL
  // The host side: an SDL stream the received PCM is pushed into, playing
  // on the default device. No device (a headless host) is not an error.
  if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
    printf("USB audio: no host audio (%s); the stream is not played.\n",
           SDL_GetError());
    return;
  }
  SDL_AudioSpec spec;
  spec.format = SDL_AUDIO_S16LE;
  spec.channels = 2;
  spec.freq = m_rate;
  m_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec,
                                       nullptr, nullptr);
  if (!m_stream) {
    printf("USB audio: no host audio device (%s); the stream is not "
           "played.\n",
           SDL_GetError());
    return;
  }
  m_stream_rate = m_rate;
  SDL_ResumeAudioStreamDevice(m_stream);
#endif
}

CUsbAudio::~CUsbAudio() {
  wav_close();
#ifdef HAVE_SDL
  if (m_stream)
    SDL_DestroyAudioStream(m_stream);
#endif
}

void CUsbAudio::reset() {
  CUsbDevice::reset();
  m_alt = 0;
}

const std::vector<u8> &CUsbAudio::device_descriptor() const {
  return kDeviceDescriptor;
}

const std::vector<u8> &CUsbAudio::configuration_descriptor() const {
  return kConfigurationDescriptor;
}

std::vector<u8> CUsbAudio::string_descriptor(int index) const {
  switch (index) {
  case 1:
    return utf16_string("Alphabox");
  case 2:
    return utf16_string("Alphabox USB Speaker");
  case 3:
    return utf16_string("1");
  default:
    return {};
  }
}

bool CUsbAudio::set_interface(int iface, int alt) {
  if (iface == 0)
    return alt == 0;
  if (iface != 1 || alt > 1)
    return false;
  if (g_usbtrace || alt != m_alt)
    printf("USB audio: streaming %s (%d Hz)\n", alt ? "on" : "off", m_rate);
  m_alt = alt;
  if (!alt && m_wav) { // a pause: bring the file's header up to date
    wav_header(m_wav, m_wav_rate, m_wav_bytes);
    fflush(m_wav);
    m_wav_synced = m_wav_bytes;
  }
  return true;
}

int CUsbAudio::get_interface(int iface) const { return iface == 1 ? m_alt : 0; }

// The class requests (Audio 1.0, 5.2): SET_CUR/GET_CUR/GET_MIN/GET_MAX/
// GET_RES, bRequest 0x01 and 0x81..0x84, to the feature unit (through the
// control interface) or to the streaming endpoint (sampling frequency).
bool CUsbAudio::class_request(const u8 *setup, const std::vector<u8> &data,
                              std::vector<u8> &out) {
  const int type = setup[0], request = setup[1];
  const int cs = setup[3], cn = setup[2];
  const int index = setup[4] | (setup[5] << 8);
  if ((type & 0x60) != 0x20) // class requests only
    return false;
  const bool get = (type & 0x80) != 0;
  auto le16 = [](int v) {
    return std::vector<u8>{(u8)(v & 0xff), (u8)((v >> 8) & 0xff)};
  };

  if ((type & 0x1f) == 2) { // endpoint: the streaming endpoint's controls
    if ((index & 0xff) != 0x01 || cs != 0x01) // sampling frequency only
      return false;
    if (!get) {
      if (request != 0x01 || data.size() < 3)
        return false;
      const int hz = data[0] | (data[1] << 8) | (data[2] << 16);
      for (int r : kRates)
        if (r == hz) {
          set_rate(hz);
          return true;
        }
      return false;
    }
    if (request != 0x81)
      return false;
    out = {(u8)(m_rate & 0xff), (u8)((m_rate >> 8) & 0xff), (u8)(m_rate >> 16)};
    return true;
  }

  if ((type & 0x1f) != 1 || (index & 0xff) != 0 || (index >> 8) != ID_FEATURE)
    return false;
  if (cn != 0) // the channels have no controls of their own
    return false;
  if (cs == 0x01) { // mute
    if (!get) {
      if (request != 0x01 || data.empty())
        return false;
      m_mute = data[0] != 0;
      apply_volume();
      return true;
    }
    if (request != 0x81)
      return false;
    out = {(u8)m_mute};
    return true;
  }
  if (cs == 0x02) { // volume
    if (!get) {
      if (request != 0x01 || data.size() < 2)
        return false;
      s16 v = (s16)(data[0] | (data[1] << 8));
      if (v != (s16)0x8000) // -infinity is allowed; else clamp to the range
        v = std::max(kVolMin, std::min(kVolMax, v));
      m_volume = v;
      apply_volume();
      return true;
    }
    switch (request) {
    case 0x81:
      out = le16(m_volume);
      return true;
    case 0x82:
      out = le16(kVolMin);
      return true;
    case 0x83:
      out = le16(kVolMax);
      return true;
    case 0x84:
      out = le16(kVolRes);
      return true;
    default:
      return false;
    }
  }
  return false;
}

void CUsbAudio::set_rate(int hz) {
  if (g_usbtrace || hz != m_rate)
    printf("USB audio: sampling frequency %d Hz\n", hz);
  m_rate = hz;
#ifdef HAVE_SDL
  if (m_stream && m_stream_rate != hz) {
    SDL_AudioSpec spec;
    spec.format = SDL_AUDIO_S16LE;
    spec.channels = 2;
    spec.freq = hz;
    SDL_SetAudioStreamFormat(m_stream, &spec, nullptr);
    m_stream_rate = hz;
  }
#endif
}

void CUsbAudio::apply_volume() {
#ifdef HAVE_SDL
  if (!m_stream)
    return;
  float gain = 0.0f;
  if (!m_mute && m_volume != (s16)0x8000)
    gain = std::pow(10.0f, (float)m_volume / 256.0f / 20.0f);
  SDL_SetAudioStreamGain(m_stream, gain);
#endif
}

// One packet from the host: a frame's worth of samples (Type I: whole
// audio frames, 4 bytes each here).
int CUsbAudio::iso_transfer(int pid, int ep, u8 *buf, int len) {
  if (pid != PID_OUT || ep != 1 || m_alt != 1 || m_configuration == 0)
    return -1;
  len = std::min(len, kMaxPacket);
  ++m_packets;
  m_bytes += (u64)len;
  if (!len)
    return 0;
#ifdef HAVE_SDL
  // Keep at most a quarter of a second queued: a host that plays slower
  // than the guest sends drops the excess rather than lagging ever more.
  if (m_stream && SDL_GetAudioStreamQueued(m_stream) < m_rate)
    SDL_PutAudioStreamData(m_stream, buf, len);
#endif
  if (!m_wav_name.empty())
    wav_write(buf, len);
  return len;
}

static void put32(u8 *p, u32 v) {
  p[0] = (u8)v;
  p[1] = (u8)(v >> 8);
  p[2] = (u8)(v >> 16);
  p[3] = (u8)(v >> 24);
}

// The WAV header: RIFF, a PCM fmt chunk, the data chunk's size.
static void wav_header(FILE *f, int rate, u64 bytes) {
  u8 h[44];
  memcpy(h, "RIFF", 4);
  put32(h + 4, (u32)(36 + bytes));
  memcpy(h + 8, "WAVEfmt ", 8);
  put32(h + 16, 16);
  put32(h + 20, 1 | (2u << 16)); // PCM, two channels
  put32(h + 24, (u32)rate);
  put32(h + 28, (u32)rate * 4);
  put32(h + 32, 4 | (16u << 16)); // block align 4, 16 bits
  memcpy(h + 36, "data", 4);
  put32(h + 40, (u32)bytes);
  fseek(f, 0, SEEK_SET);
  fwrite(h, 1, sizeof(h), f);
  fseek(f, 0, SEEK_END);
}

void CUsbAudio::wav_write(const u8 *pcm, int len) {
  if (m_wav && m_wav_rate != m_rate)
    wav_close(); // a new rate: a new file
  if (!m_wav) {
    std::string name = m_wav_name;
    if (m_wav_count) {
      const size_t dot = name.rfind('.');
      const std::string n = "-" + std::to_string(m_wav_count);
      name = dot == std::string::npos ? name + n : name.insert(dot, n);
    }
    ++m_wav_count;
    m_wav = fopen(name.c_str(), "wb");
    if (!m_wav) {
      printf("USB audio: cannot write %s\n", name.c_str());
      m_wav_name.clear();
      return;
    }
    m_wav_rate = m_rate;
    m_wav_bytes = m_wav_synced = 0;
    wav_header(m_wav, m_wav_rate, 0);
    printf("USB audio: writing %s (%d Hz, 16-bit stereo)\n", name.c_str(),
           m_rate);
  }
  fwrite(pcm, 1, (size_t)len, m_wav);
  m_wav_bytes += (u64)len;
  // The header follows every quarter second, so the file is whole even if
  // the emulator never gets to close it.
  if (m_wav_bytes - m_wav_synced >= (u64)m_wav_rate) {
    wav_header(m_wav, m_wav_rate, m_wav_bytes);
    fflush(m_wav);
    m_wav_synced = m_wav_bytes;
  }
}

void CUsbAudio::wav_close() {
  if (!m_wav)
    return;
  wav_header(m_wav, m_wav_rate, m_wav_bytes);
  fclose(m_wav);
  m_wav = nullptr;
}
