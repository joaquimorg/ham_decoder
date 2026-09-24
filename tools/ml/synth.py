# Synthetic radio audio (12 kHz) for the TinyML classifier dataset.
# Each generator returns ~1.1 s of one signal kind, already mixed with
# receiver noise (SSB passband), fading and the odd interfering carrier.
import numpy as np

FS = 12000
LEN = 12 * 1024


def bandpass_noise(rng, n, lo, hi):
    w = rng.standard_normal(n)
    W = np.fft.rfft(w)
    f = np.fft.rfftfreq(n, 1 / FS)
    edge = 80.0
    mask = np.clip((f - lo) / edge + 0.5, 0, 1) * np.clip((hi - f) / edge + 0.5, 0, 1)
    tilt = 10 ** (rng.uniform(-6, 3) * (f / 3000) / 20)
    y = np.fft.irfft(W * mask * tilt, n)
    return y / (y.std() + 1e-12)


def ramp(keys, rise_ms):
    """Smooth 0/1 keying with a raised-cosine edge of rise_ms."""
    n = max(1, int(rise_ms * FS / 1000))
    k = np.hanning(2 * n + 1)
    k /= k.sum()
    return np.convolve(keys, k, mode="same")


def fading(rng, n):
    if rng.random() < 0.5:
        return np.ones(n)
    rate = rng.uniform(0.1, 2.0)
    depth = rng.uniform(0.2, 0.8)
    t = np.arange(n) / FS
    return 1 - depth * 0.5 * (1 + np.sin(2 * np.pi * rate * t + rng.uniform(0, 6.3)))


def tone(rng, n, f, drift=True):
    t = np.arange(n) / FS
    fd = f + (rng.uniform(-3, 3) * t if drift else 0)
    return np.sin(2 * np.pi * np.cumsum(fd) / FS + rng.uniform(0, 6.3))


def sig_cw(rng, n):
    wpm = rng.uniform(8, 40)
    dit = 1.2 / wpm * FS
    keys = np.zeros(n + int(20 * dit))
    pos = -int(rng.uniform(0, 8) * dit)
    while pos < n:
        # One character: 1..5 elements, then a letter or word gap.
        for _ in range(rng.integers(1, 6)):
            d = dit * (3 if rng.random() < 0.45 else 1) * rng.uniform(0.85, 1.15)
            a = max(pos, 0)
            keys[a:max(int(pos + d), 0)] = 1
            pos += int(d + dit * rng.uniform(0.85, 1.2))
        pos += int(dit * (2 if rng.random() < 0.8 else 6) * rng.uniform(0.9, 1.3))
    keys = ramp(keys[:n], rng.uniform(2, 8))
    return keys * tone(rng, n, rng.uniform(300, 2000))


def sig_tone(rng, n):
    return tone(rng, n, rng.uniform(250, 3000))


def sig_rtty(rng, n):
    baud = rng.choice([45.45, 45.45, 45.45, 50, 75, 100])
    shift = rng.choice([170, 170, 170, 200, 425, 450, 850])
    mark = rng.uniform(600, 2600 - shift) if rng.random() < 0.5 else rng.uniform(1900, 2300)
    mark = min(mark, 3000 - shift)
    spb = FS / baud
    bits = []
    while len(bits) * spb < n + 20 * spb:
        if rng.random() < 0.05:
            bits += [1] * rng.integers(1, 10)      # idle mark
        bits += [0] + list(rng.integers(0, 2, 5)) + [1, 1]
    start = int(rng.uniform(0, 8 * spb))
    idx = ((np.arange(n) + start) / spb).astype(int)
    b = np.array(bits)[idx].astype(float)
    b = ramp(b, rng.uniform(0.5, 4))
    f = mark + shift * (1 - b) * (1 if rng.random() < 0.5 else -1)
    return np.sin(2 * np.pi * np.cumsum(f) / FS + rng.uniform(0, 6.3))


def sig_psk(rng, n):
    baud = 31.25 if rng.random() < 0.8 else 62.5
    spb = FS / baud
    nsym = int(n / spb) + 3
    # Idle (all reversals) or random varicode-like bits (a 0 = reversal).
    p_rev = rng.choice([1.0, rng.uniform(0.4, 0.7)])
    rev = rng.random(nsym) < p_rev
    phase = np.cumsum(rev) % 2
    t = np.arange(n)
    sym = ((t + rng.uniform(0, spb)) / spb)
    i = sym.astype(int)
    frac = sym - i
    a0 = np.where(phase[i] == 0, 1.0, -1.0)
    a1 = np.where(phase[i + 1] == 0, 1.0, -1.0)
    # Raised-cosine transition across each symbol boundary.
    w = 0.5 * (1 - np.cos(np.pi * frac))
    amp = a0 * (1 - w) + a1 * w
    f = rng.uniform(500, 2500)
    return amp * np.cos(2 * np.pi * f * t / FS + rng.uniform(0, 6.3))


def sig_voice(rng, n):
    out = np.zeros(n)
    pos = 0
    male = rng.random() < 0.6
    while pos < n:
        seg = int(rng.uniform(0.08, 0.35) * FS)
        f0 = rng.uniform(85, 150) if male else rng.uniform(160, 280)
        f0 = f0 * (1 + np.linspace(0, rng.uniform(-0.25, 0.25), seg))
        ph = np.cumsum(f0 / FS)
        pulses = np.diff(np.floor(ph), prepend=0.0)
        voiced = rng.random() < 0.8
        src = pulses if voiced else rng.standard_normal(seg) * 0.1
        # Three formants via resonators (vowel-like).
        spec = np.fft.rfft(src)
        f = np.fft.rfftfreq(seg, 1 / FS)
        h = np.zeros_like(f)
        for fc, bw, g in ((rng.uniform(300, 900), 90, 1.0),
                          (rng.uniform(900, 2300), 120, 0.6),
                          (rng.uniform(2300, 3200), 180, 0.3)):
            h += g / (1 + ((f - fc) / bw) ** 2)
        s = np.fft.irfft(spec * h, seg)
        s *= np.hanning(seg) ** 0.5
        if rng.random() < 0.15:
            s *= 0.02                               # pause between words
        out[pos:pos + seg] += s[:n - pos] / (np.abs(s).max() + 1e-9) * rng.uniform(0.3, 1)
        pos += int(seg * rng.uniform(0.7, 1.0))
    return out


def distort(rng, s):
    """Speaker/audio chain: soft clipping (harmonics) and room reverberation."""
    if rng.random() < 0.4:
        s = s / (np.abs(s).max() + 1e-12)
        g = rng.uniform(1, 6)
        s = np.tanh(g * s + rng.uniform(0, 0.3) * g * s * s) / np.tanh(g)
    if rng.random() < 0.3:
        t60 = rng.uniform(0.05, 1.0)
        m = int(t60 * FS)
        ir = rng.standard_normal(m) * np.exp(-6.9 * np.arange(m) / m)
        ir[0] = rng.uniform(0.5, 6) * ir.std() * np.sqrt(m)
        s = np.convolve(s, ir)[:len(s)]
    return s


GENERATORS = {
    "TOM": sig_tone,
    "CW": sig_cw,
    "RTTY": sig_rtty,
    "PSK31": sig_psk,
    "VOZ": sig_voice,
}


def example(rng, label, n=LEN):
    lo, hi = rng.uniform(150, 400), rng.uniform(2400, 3300)
    noise = bandpass_noise(rng, n, lo, hi)
    if rng.random() < 0.3:
        noise += rng.standard_normal(n) * rng.uniform(0.05, 0.5)
    if label == "RUIDO":
        x = noise
        if rng.random() < 0.2:                      # static crashes
            for _ in range(rng.integers(1, 6)):
                p = rng.integers(0, n - 200)
                x[p:p + 150] += rng.standard_normal(150) * rng.uniform(2, 8)
    else:
        s = GENERATORS[label](rng, n) * fading(rng, n)
        s = distort(rng, s)
        s /= np.sqrt(np.mean(s ** 2)) + 1e-12
        # Signal power vs noise power over the whole ~2.5 kHz passband.
        if label == "VOZ":
            snr = rng.uniform(-3, 30)
        elif label == "TOM":
            snr = rng.uniform(-20, 25)
        elif label == "CW":
            snr = rng.uniform(-14, 25)
        else:
            snr = rng.uniform(-8, 25)
        x = s * 10 ** (snr / 20) + noise
    if label not in ("RUIDO", "TOM") and rng.random() < 0.25:   # weak interfering carrier
        x += tone(rng, n, rng.uniform(200, 3200)) * 10 ** (rng.uniform(-25, -5) / 20)
    if rng.random() < 0.3:                          # low-frequency rumble
        x += bandpass_noise(rng, n, 20, rng.uniform(120, 300)) * rng.uniform(0.2, 2) * x.std()
    if rng.random() < 0.2:                          # mains hum / board spurs
        x += np.sin(2 * np.pi * rng.choice([50, 100, 150]) * np.arange(n) / FS) * rng.uniform(0.02, 0.3)
    x *= 10 ** (rng.uniform(-45, -12) / 20) / (np.abs(x).max() + 1e-12)
    # 12-bit ADC after 4x decimation: ~13 bits of resolution.
    return np.round(x * 8192) / 8192
