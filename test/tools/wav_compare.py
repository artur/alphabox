#!/usr/bin/env python3
"""Compare a captured WAV with the one it should be (no numpy needed).

usage: wav_compare.py <reference.wav> <captured.wav>

Both are brought to mono at 48 kHz (linear interpolation) and to their
envelope (rectified, averaged over 1 ms). The capture is aligned to the
reference by the lag that correlates best (the capture may start with
silence, or late), and the script prints the lag, the durations of sound in
each, and the correlation of the aligned envelopes and of the aligned
waveforms, low-passed to 4 kHz and decimated to 8 kHz. A guest mixer that
resamples plays at a slightly different rate, so the waveform correlation of
a long sound is lower than the envelope's; a stream that lost or repeated
packets drops both.
"""
import struct
import sys
import wave


def load(path):
    w = wave.open(path, 'rb')
    ch, width, rate, n = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
    raw = w.readframes(n)
    if width == 1:
        s = [(b - 128) * 256 for b in raw]
    elif width == 2:
        s = list(struct.unpack('<%dh' % (len(raw) // 2), raw))
    else:
        raise SystemExit('%s: %d-byte samples not handled' % (path, width))
    mono = [sum(s[i:i + ch]) / ch for i in range(0, len(s) - ch + 1, ch)]
    return mono, rate


def resample(x, rate, to):
    if rate == to or not x:
        return x
    n = int(len(x) * to / rate)
    out = []
    for i in range(n):
        t = i * rate / to
        k = int(t)
        f = t - k
        a = x[k]
        b = x[k + 1] if k + 1 < len(x) else a
        out.append(a + (b - a) * f)
    return out


def boxcar(x, n):
    out, acc = [], 0.0
    for i, v in enumerate(x):
        acc += v
        if i >= n:
            acc -= x[i - n]
        out.append(acc / n)
    return out


def decimate(x, n):
    return x[::n]


def corr(a, b):
    n = min(len(a), len(b))
    if n < 2:
        return 0.0
    a, b = a[:n], b[:n]
    ma, mb = sum(a) / n, sum(b) / n
    sab = sum((p - ma) * (q - mb) for p, q in zip(a, b))
    saa = sum((p - ma) ** 2 for p in a)
    sbb = sum((q - mb) ** 2 for q in b)
    return sab / (saa * sbb) ** 0.5 if saa and sbb else 0.0


def sound_span(env, rate):
    floor = 0.02 * max(env + [1])
    idx = [i for i, v in enumerate(env) if v > floor]
    return (idx[-1] - idx[0]) / rate if idx else 0.0


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    ref, rr = load(sys.argv[1])
    cap, cr = load(sys.argv[2])
    ref, cap = resample(ref, rr, 48000), resample(cap, cr, 48000)
    # Envelopes at 1 kHz.
    er = decimate(boxcar([abs(v) for v in ref], 48), 48)
    ec = decimate(boxcar([abs(v) for v in cap], 48), 48)
    best, lag = -2.0, 0
    for k in range(0, max(1, len(ec) - len(er) // 2)):
        c = corr(er, ec[k:k + len(er)])
        if c > best:
            best, lag = c, k
    # Waveforms low-passed and at 8 kHz, aligned around the envelope's lag.
    wr = decimate(boxcar(ref, 6), 6)
    wc = decimate(boxcar(cap, 6), 6)
    wbest, wlag = -2.0, lag * 8
    for k in range(max(0, lag * 8 - 16), lag * 8 + 17):
        c = corr(wr, wc[k:k + len(wr)])
        if c > wbest:
            wbest, wlag = c, k
    print('reference: %.3f s of sound; capture: %.3f s of sound, %.3f s in all'
          % (sound_span(er, 1000), sound_span(ec, 1000), len(cap) / 48000))
    print('capture starts %.3f s late' % (wlag / 8000))
    print('envelope correlation %.3f, waveform correlation %.3f' % (best, wbest))


main()
