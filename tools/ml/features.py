# Feature extraction for the TinyML signal classifier. Mirror of
# src/classifier.cpp: keep both in sync (same constants, same order).
#
# One feature vector per analyzer report (FFT_FRAMES_PER_REPORT frames of
# FFT_SIZE samples at 12 kHz, ~1 s). Everything is relative to the noise
# floor, so the input level does not matter.
import numpy as np

FS = 12000
N = 1024
HALF = N // 2
FRAMES = 12
BIN_HZ = FS / N
KMIN = int(np.ceil(100 / BIN_HZ))
KMAX = min(int(np.floor(3500 / BIN_HZ)), HALF - 2)

ENV_BLOCK = FS // 100           # 10 ms envelope
MOD_LEN = 100                   # 1 s of envelope for the modulation spectrum
MOD_BINS = 40                   # 1..40 Hz

LOCAL_BANDS = 48                # around the strongest peak
LOCAL_W = 3                     # bins per band (35 Hz): +-840 Hz
WIDE_BANDS = 16                 # 200 Hz bands from 200 Hz
WIDE_HZ0, WIDE_HZ = 200.0, 200.0
N_FEATURES = LOCAL_BANDS + WIDE_BANDS + MOD_BINS + 6

CLASSES = ["RUIDO", "TOM", "CW", "RTTY", "PSK31", "VOZ"]

_hann = (0.5 - 0.5 * np.cos(2 * np.pi * np.arange(N) / N)).astype(np.float32)
POWER_NORM = 16.0 / (N * N)


def _psd_env(x):
    """x: FRAMES*N samples (full scale 1.0). Returns (psd avg, env dB)."""
    fr = x[:FRAMES * N].reshape(FRAMES, N) * _hann
    spec = np.fft.rfft(fr, axis=1)[:, :HALF]
    psd = (np.abs(spec) ** 2 * POWER_NORM).mean(axis=0)
    nenv = len(x[:FRAMES * N]) // ENV_BLOCK
    e = (x[:nenv * ENV_BLOCK].reshape(nenv, ENV_BLOCK) ** 2).mean(axis=1)
    return psd, 10 * np.log10(e + 1e-14)


def features_from(psd, env_db):
    db = 10 * np.log10(psd + 1e-14)
    floor = np.sort(db[KMIN:KMAX + 1])[int(0.5 * (KMAX - KMIN) + 0.5)]
    rel = db - floor
    kp = KMIN + int(np.argmax(rel[KMIN:KMAX + 1]))
    snr = rel[kp]

    f = np.zeros(N_FEATURES, np.float32)
    i = 0
    for b in range(LOCAL_BANDS):
        c = kp + (b - LOCAL_BANDS // 2) * LOCAL_W
        m = 0.0
        for k in range(c - 1, c + 2):
            if 0 <= k < HALF:
                m = max(m, rel[k])
        f[i] = min(max(m / 40.0, 0.0), 1.5); i += 1
    for b in range(WIDE_BANDS):
        k0 = int((WIDE_HZ0 + b * WIDE_HZ) / BIN_HZ)
        k1 = int((WIDE_HZ0 + (b + 1) * WIDE_HZ) / BIN_HZ)
        m = rel[k0:k1].mean()
        f[i] = min(max(m / 20.0, 0.0), 1.5); i += 1

    env = env_db[:MOD_LEN]
    n = len(env)
    p10 = np.sort(env)[int(0.1 * (n - 1) + 0.5)]
    p90 = np.sort(env)[int(0.9 * (n - 1) + 0.5)]
    rng = p90 - p10
    lin = 10 ** ((env - env.max()) / 20)       # amplitude, peak = 1
    lin = lin - lin.mean()
    t = np.arange(n)
    tot = 1e-9
    mags = np.zeros(MOD_BINS)
    for m in range(MOD_BINS):
        w = 2 * np.pi * (m + 1) * t / MOD_LEN
        mags[m] = np.hypot((lin * np.cos(w)).sum(), (lin * np.sin(w)).sum())
    tot += mags.sum()
    for m in range(MOD_BINS):
        f[i] = mags[m] / tot * 4.0; i += 1

    occ = (rel[KMIN:KMAX + 1] >= 10.0).sum() / (KMAX - KMIN + 1)
    lp = psd[KMIN:KMAX + 1] + 1e-14
    flat = np.exp(np.log(lp).mean()) / lp.mean()
    f[i] = min(snr / 60.0, 1.5); i += 1
    f[i] = min(rng / 30.0, 1.5); i += 1
    f[i] = occ * 4.0; i += 1
    f[i] = flat; i += 1
    f[i] = kp * BIN_HZ / 3500.0; i += 1
    f[i] = min(np.std(env) / 15.0, 1.5); i += 1
    assert i == N_FEATURES
    return f


def features(x):
    return features_from(*_psd_env(np.asarray(x, np.float64)))
