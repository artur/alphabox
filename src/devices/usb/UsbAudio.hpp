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

#if !defined(INCLUDED_USBAUDIO_H)
#define INCLUDED_USBAUDIO_H

#include "UsbDevice.hpp"
#include <cstdio>
#include <string>

/**
 * \brief A USB speaker: an Audio Class 1.0 device with one output.
 *
 * The audio function is the textbook one: an input terminal fed by the
 * streaming interface, a feature unit with master mute and volume, and a
 * speaker as the output terminal. The streaming interface's alternate
 * setting 0 has no endpoint (zero bandwidth); setting 1 has an adaptive
 * isochronous OUT endpoint taking 16-bit stereo PCM at 48 or 44.1 kHz,
 * chosen with SET_CUR on the endpoint's sampling frequency control. Windows
 * 2000 drives it with its own usbaudio.sys.
 *
 * What arrives goes to the host's default playback device through SDL (at
 * the guest's volume), and, when ALPHABOX_USBAUDIO_WAV names a file, into
 * that file as a WAV exactly as the guest sent it -- the way to check the
 * stream in a headless run. A change of sampling rate part way through
 * starts a new file, named with a counter.
 *
 * Documentation consulted: Universal Serial Bus Device Class Definition
 * for Audio Devices 1.0; for Audio Data Formats 1.0 (Type I, PCM);
 * Universal Serial Bus Specification 1.1, chapter 5 (isochronous transfers).
 **/
class CUsbAudio : public CUsbDevice {
public:
  CUsbAudio();
  ~CUsbAudio() override;
  const char *name() const override { return "audio"; }
  void reset() override;
  int iso_transfer(int pid, int ep, u8 *buf, int len) override;
  bool iso_endpoint(int pid, int ep) const override {
    return pid == PID_OUT && ep == 1 && m_alt == 1 && m_configuration;
  }
  bool save(CUsbSaved &s) const override;
  bool load(CUsbSaved &s) override;

protected:
  const std::vector<u8> &device_descriptor() const override;
  const std::vector<u8> &configuration_descriptor() const override;
  std::vector<u8> string_descriptor(int index) const override;
  bool class_request(const u8 *setup, const std::vector<u8> &data,
                     std::vector<u8> &out) override;
  bool set_interface(int iface, int alt) override;
  int get_interface(int iface) const override;

private:
  void set_rate(int hz);
  void apply_volume();
  void wav_write(const u8 *pcm, int len);
  void wav_close();

  int m_alt = 0;       // the streaming interface's alternate setting
  int m_rate = 48000;  // sampling frequency, Hz
  bool m_mute = false; // feature unit, master channel
  s16 m_volume = 0;    // 1/256 dB
  u64 m_bytes = 0;     // received since power-on
  u64 m_packets = 0;

  // SDL output (none when there is no audio device).
  struct SDL_AudioStream *m_stream = nullptr;
  int m_stream_rate = 0;

  // ALPHABOX_USBAUDIO_WAV
  std::string m_wav_name;
  FILE *m_wav = nullptr;
  int m_wav_rate = 0;
  int m_wav_count = 0;
  u64 m_wav_bytes = 0;
  u64 m_wav_synced = 0;
};

#endif
