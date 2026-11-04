#!/usr/bin/env python3
"""Turns the original Elemental Defense sounds into the raw PCM the 3DS build
links in from data/.

Not part of the build; like tools/make_icon.py this keeps the asset pipeline
around so the audio can be regenerated/tweaked without hand-editing binaries.

    pip install soundfile numpy
    python tools/make_audio.py [sounds_dir]

`sounds_dir` defaults to the original LÖVE2D project on the user's desktop
(C:\\Users\\Raine\\Desktop\\ElementalDefense\\game\\sounds).  Output goes to
`data/` as mono PCM16 little-endian:

  * SFX    -> 22050 Hz   (bin/sfx_*.bin)      trimmed, peak-normalised, faded
  * music  -> 16000 Hz   (bin/music_*.bin)    loop-friendly segment, faded

Every file is trimmed of leading/trailing silence, peak-normalised to -3 dBFS
(SFX) / -6 dBFS (music) and gets short fades so there are no clicks.  The whole
set stays well under the 1.5 MB budget (~1.2 MB).

The 3DS Makefile turns each data/*.bin into an object via bin2o; the generated
headers (e.g. build/sfx_build_bin.h) declare `sfx_build_bin[]`,
`sfx_build_bin_end[]` and `sfx_build_bin_size` for source/audio_3ds.c.
"""

import os
import sys

import numpy as np
import soundfile as sf

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
OUT_DIR = os.path.join(REPO, "data")
DEFAULT_SOUNDS = r"C:\Users\Raine\Desktop\ElementalDefense\game\sounds"

SFX_RATE = 22050
MUSIC_RATE = 16000
SFX_PEAK_DB = -3.0
MUSIC_PEAK_DB = -6.0
SFX_FADE_MS = 5.0
MUSIC_FADE_MS = 10.0

# name -> (source relative to sounds dir, max seconds after silence trim)
SFX_SPEC = [
    ("sfx_build",      "build.wav",      0.65),
    ("sfx_sell",       "sell.wav",       0.45),
    ("sfx_laser",      "laser.wav",      0.13),
    ("sfx_enemy_die",  "enemy_die.wav",  0.25),
    ("sfx_wave_start", "wave_start.wav", 0.90),
    ("sfx_gameover",   "gameover.wav",   2.00),
    ("sfx_upgrade_1",  "upgrade_1.wav",  0.40),
    ("sfx_upgrade_2",  "upgrade_2.wav",  0.40),
    ("sfx_upgrade_3",  "upgrade_3.wav",  0.40),
    ("sfx_upgrade_4",  "upgrade_4.wav",  0.40),
    ("sfx_upgrade_5",  "upgrade_5.wav",  0.40),
    ("sfx_upgrade_6",  "upgrade_6.wav",  0.40),
]

# name -> (source, start seconds, segment seconds)
MUSIC_SPEC = [
    ("music_menu", "menu_music.ogg", 2.0, 12.0),
    ("music_game", "game_music.ogg", 2.0, 16.0),
]


def to_mono(x):
    """Always_2d soundfile read -> 1-D mono float32."""
    if x.ndim > 1:
        x = x.mean(axis=1) if x.shape[1] > 1 else x[:, 0]
    return np.asarray(x, dtype=np.float32)


def resample(x, sr, target):
    if sr == target or len(x) == 0:
        return x
    n_out = max(1, int(round(len(x) * float(target) / float(sr))))
    t_in = np.arange(len(x), dtype=np.float64) / float(sr)
    t_out = np.arange(n_out, dtype=np.float64) / float(target)
    return np.interp(t_out, t_in, x).astype(np.float32)


def trim_silence(x, sr, threshold=0.003, pad_ms=2.0):
    """Trim leading/trailing near-silence, keeping a small pad. Never empty."""
    n = len(x)
    if n == 0:
        return x
    idx = np.nonzero(np.abs(x) > threshold)[0]
    if len(idx) == 0:
        return x
    pad = max(1, int(sr * pad_ms / 1000.0))
    start = max(0, int(idx[0]) - pad)
    end = min(n, int(idx[-1]) + 1 + pad)
    return x[start:end]


def fade(x, sr, ms):
    n = len(x)
    k = max(1, min(n // 2, int(sr * ms / 1000.0)))
    if k <= 0 or n < 4:
        return x
    ramp = np.linspace(0.0, 1.0, k, dtype=np.float32)
    x = x.copy()
    x[:k] *= ramp
    x[-k:] *= ramp[::-1]
    return x


def normalize(x, peak_db):
    peak = float(np.max(np.abs(x))) if len(x) else 0.0
    if peak <= 1e-9:
        return x
    target = 10.0 ** (peak_db / 20.0)
    return (x * (target / peak)).astype(np.float32)


def cap_seconds(x, sr, sec):
    n = int(round(sec * sr))
    return x[:n] if n > 0 else x


def write_pcm16(path, x):
    x = np.asarray(x, dtype=np.float32)
    pcm = np.clip(np.round(x * 32767.0), -32768, 32767).astype("<i2")
    with open(path, "wb") as f:
        f.write(pcm.tobytes())
    return len(pcm)


def synth_leak():
    """Low descending buzz, ~0.3 s."""
    sr = SFX_RATE
    n = int(0.3 * sr)
    t = np.arange(n, dtype=np.float32) / sr
    freq = np.linspace(220.0, 90.0, n)
    phase = np.cumsum(freq / sr)
    saw = 2.0 * (phase % 1.0) - 1.0
    env = np.exp(-3.5 * t)
    x = (0.55 * saw + 0.25 * np.sin(2.0 * np.pi * phase)) * env
    return x, sr


def synth_victory():
    """Rising arpeggio C5-E5-G5-C6, ~1.2 s."""
    sr = SFX_RATE
    out = np.zeros(int(1.2 * sr), dtype=np.float32)
    notes = [523.25, 659.25, 783.99, 1046.50]
    step = len(out) // len(notes)
    for i, f0 in enumerate(notes):
        a = i * step
        b = len(out) if i == len(notes) - 1 else (i + 1) * step
        t = np.arange(b - a, dtype=np.float32) / sr
        env = np.minimum(1.0, t / 0.01) * np.exp(-4.0 * t)
        tone = np.sin(2.0 * np.pi * f0 * t) + 0.35 * np.sin(4.0 * np.pi * f0 * t)
        out[a:b] += (0.55 * tone * env).astype(np.float32)
    return out, sr


def sfx_from_file(path, seconds):
    x, sr = sf.read(path, dtype="float32", always_2d=True)
    x = to_mono(x)
    x = resample(x, sr, SFX_RATE)
    x = trim_silence(x, SFX_RATE)
    x = cap_seconds(x, SFX_RATE, seconds)
    x = normalize(x, SFX_PEAK_DB)
    x = fade(x, SFX_RATE, SFX_FADE_MS)
    return x


def music_segment(path, start_s, seconds):
    x, sr = sf.read(path, dtype="float32", always_2d=True)
    x = to_mono(x)
    x = resample(x, sr, MUSIC_RATE)
    a = int(start_s * MUSIC_RATE)
    if a >= len(x):
        a = 0
    x = x[a:]
    x = cap_seconds(x, MUSIC_RATE, seconds)
    x = normalize(x, MUSIC_PEAK_DB)
    x = fade(x, MUSIC_RATE, MUSIC_FADE_MS)
    return x


def main():
    sounds = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_SOUNDS
    if not os.path.isdir(sounds):
        sys.exit("sounds directory not found: %s" % sounds)
    os.makedirs(OUT_DIR, exist_ok=True)

    rows = []
    total = 0

    for name, src, seconds in SFX_SPEC:
        path = os.path.join(sounds, src)
        if not os.path.exists(path):
            sys.exit("missing source: %s" % path)
        x = sfx_from_file(path, seconds)
        if len(x) == 0:
            sys.exit("empty result for %s" % name)
        out = os.path.join(OUT_DIR, name + ".bin")
        samples = write_pcm16(out, x)
        size = os.path.getsize(out)
        total += size
        rows.append((name, SFX_RATE, samples, size))

    for name, fn in (("sfx_leak", synth_leak), ("sfx_victory", synth_victory)):
        x, sr = fn()
        if sr != SFX_RATE:
            x = resample(x, sr, SFX_RATE)
        x = normalize(x, SFX_PEAK_DB)
        x = fade(x, SFX_RATE, SFX_FADE_MS)
        out = os.path.join(OUT_DIR, name + ".bin")
        samples = write_pcm16(out, x)
        size = os.path.getsize(out)
        total += size
        rows.append((name, SFX_RATE, samples, size))

    for name, src, start_s, seconds in MUSIC_SPEC:
        path = os.path.join(sounds, src)
        if not os.path.exists(path):
            sys.exit("missing source: %s" % path)
        x = music_segment(path, start_s, seconds)
        if len(x) == 0:
            sys.exit("empty result for %s" % name)
        out = os.path.join(OUT_DIR, name + ".bin")
        samples = write_pcm16(out, x)
        size = os.path.getsize(out)
        total += size
        rows.append((name, MUSIC_RATE, samples, size))

    print("%-16s %-7s %10s %9s %7s" % ("file", "rate", "samples", "seconds", "bytes"))
    print("-" * 56)
    for name, rate, samples, size in rows:
        print("%-16s %-7d %10d %9.3f %7d"
              % (name + ".bin", rate, samples, samples / float(rate), size))
    print("-" * 56)
    print("total data/: %d bytes (%.2f MB)" % (total, total / (1024.0 * 1024.0)))
    if total > 1536 * 1024:
        sys.exit("ERROR: data exceeds the 1.5 MB budget")


if __name__ == "__main__":
    main()
