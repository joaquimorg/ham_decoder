# Python mirror of src/fm_demod.cpp + src/fax_decoder.cpp, fed with synthetic
# WEFAX, to check the algorithm without hardware. Keep it in sync with the C++.
#
#   python tools/fax_sim.py                    synthetic test cases
#   python tools/fax_sim.py --wav out.wav      writes a test transmission (for a phone)
#   python tools/fax_sim.py --file audio.wav [--lpm 120] [--ioc 576]   -> audio.pgm
import math, sys, wave
import numpy as np

FS = 12000
FM_CENTER, FM_CUTOFF, FM_TAPS = 1700.0, 1000.0, 63
W = 904
BLACK, WHITE = 1500.0, 2300.0
MID = 1900.0
TONE_WIN, TONE_RUNS, STOP_HZ, TONE_SHARE = FS // 4, 6, 450.0, 0.1
PULSE, PULSE_CONTRAST = W // 20, 0.3
PHASE_TOL, PHASE_MAX_LINES, PHASE_FLAT = W // 100, 80, 0.15
PH_MIN, PH_MAX, PH_END_MISSES, PH_OUTLIER, PH_RMS, SLANT_MAX = 12, 64, 1, 6.0, 3.0, 0.05
PH_EDGE_EXCUSE, PH_SEG_MIN = 2, 3
MAX_LINES = 3000


def fm_demod(x):
    """Instantaneous frequency, as fm_demod_process()."""
    fc = FM_CUTOFF / FS
    m = np.arange(FM_TAPS) - (FM_TAPS - 1) / 2
    taps = np.where(m == 0, 2 * fc, np.sin(2 * np.pi * fc * m) / (np.pi * np.where(m == 0, 1, m)))
    taps *= 0.54 - 0.46 * np.cos(2 * np.pi * np.arange(FM_TAPS) / (FM_TAPS - 1))
    taps /= taps.sum()
    n = np.arange(len(x))
    z = np.convolve(x * np.exp(-2j * np.pi * FM_CENTER / FS * n), taps)[:len(x)]
    prev = np.concatenate(([1 + 0j], z[:-1]))
    return FM_CENTER + np.angle(z * np.conj(prev)) * FS / (2 * np.pi)


class Fax:
    def __init__(self, lpm=120, ioc=576, auto=True):
        self.lpm, self.ioc, self.auto = lpm, ioc, auto
        self.state = 'idle'
        self.images = []              # finished images: lists of lines (0..1)
        self.image = None
        self.phase_at = None          # pulse centre used to align
        self.win_x = []
        self.start_run = self.stop_run = 0
        self.events = []

    def start_tone(self):
        return 675.0 if self.ioc == 288 else 300.0

    @staticmethod
    def tone_share(x, f):
        """Share of the window's variance at f (Goertzel in fax_decoder.cpp)."""
        x = np.asarray(x)
        n = len(x)
        g = abs(np.dot(x, np.exp(-2j * np.pi * f / FS * np.arange(n)))) ** 2
        var = x.var()
        return 2 * g / (n * n * var) if var > 0 else 0.0

    def begin(self, phasing, t):
        self.image = []
        self.slant = 1.0
        self.line_samples = FS * 60.0 / self.lpm
        self.pos, self.col, self.acc, self.acc_n = 0.0, 0, 0.0, 0
        self.line = np.zeros(W)
        self.lines = 0
        self.ph = []                  # (line, pulse column, at the edge, segment)
        self.ph_misses = 0
        self.ph_excused = 0
        self.state = 'phasing' if phasing else 'rx'
        self.events.append(('start', round(t, 2)))

    def end(self, t, why):
        if self.state != 'idle':
            self.images.append(self.image)
            self.events.append((why, round(t, 2), len(self.image)))
        self.state = 'idle'

    def pulse_centre(self):
        line = self.line
        mean = line.mean()
        win = np.convolve(np.concatenate((line, line[:PULSE - 1])), np.ones(PULSE), 'valid')[:W] / PULSE
        d = np.abs(win - mean)
        s = int(np.argmax(d))
        return (s + PULSE // 2) % W if d[s] >= PULSE_CONTRAST else -1

    @staticmethod
    def circ(a, b):
        d = abs(a - b) % W
        return min(d, W - d)

    def looks_like_phasing(self):
        c = self.pulse_centre()
        if c < 0 or self.circ(c, 0) > PHASE_TOL:
            return False
        rest = self.line[PULSE:W - PULSE]
        return rest.var() < PHASE_FLAT ** 2

    def phase_fit(self):
        """Common slope, one intercept per segment (see phase_fit() in C++)."""
        k = np.array([p[0] for p in self.ph], float)
        u = np.array([p[1] for p in self.ph], float)
        seg = np.array([p[3] for p in self.ph])
        inner = ~np.array([p[2] for p in self.ph], bool)
        base = inner if inner.sum() >= PH_MIN else np.ones(len(k), bool)
        use = base.copy()
        n_seg = seg[-1] + 1
        for it in range(3):
            if use.sum() < PH_MIN:
                return None
            gk = np.zeros(n_seg); gu = np.zeros(n_seg); gn = np.zeros(n_seg)
            for g in range(n_seg):
                m = use & (seg == g)
                gn[g] = m.sum()
                if gn[g]:
                    gk[g] = k[m].mean(); gu[g] = u[m].mean()
            dk = k - gk[seg]; du = u - gu[seg]
            sxx = np.sum(dk[use] ** 2)
            if sxx <= 0:
                return None
            s_ = np.sum(dk[use] * du[use]) / sxx
            has = gn[seg] > 0
            r = u - (gu[seg] + s_ * dk)
            rms = np.sqrt(np.mean(r[use] ** 2))
            if it == 2:
                g = n_seg - 1
                while g > 0 and gn[g] < PH_SEG_MIN:
                    g -= 1
                if not gn[g]:
                    return None
                a_ = gu[g] - s_ * gk[g] + (n_seg - 1 - g) * ((W + s_) if s_ < 0 else -(W + s_))
                ok = rms <= PH_RMS and abs(s_) <= SLANT_MAX * W
                return (a_, s_) if ok else None
            use = base & has & (np.abs(r) <= PH_OUTLIER)

    def phasing_line(self):
        c = self.pulse_centre()
        wrapped = c >= 0 and bool(self.ph) and abs(c - self.ph_last) > W // 2
        ok, at_edge = c >= 0, False
        if len(self.ph) >= PH_MIN:
            fit = self.phase_fit()
            if fit:
                raw = fit[0] + fit[1] * self.lines
                pred = raw % W
                at_edge = raw < PULSE // 2 + PH_OUTLIER or raw > W - PULSE // 2 - PH_OUTLIER
                if ok:
                    ok = self.circ(c, int(round(pred)) % W) <= PH_OUTLIER + (abs(fit[1]) if wrapped or at_edge else 0)
        if ok:
            if len(self.ph) < PH_MAX:
                seg = self.ph[-1][3] + (1 if wrapped else 0) if self.ph else 0
                self.ph.append((self.lines, float(c), self.circ(c, 0) <= PULSE // 2 + 2, seg))
            self.ph_last = c
            self.ph_misses = self.ph_excused = 0
        elif at_edge and self.ph_excused < PH_EDGE_EXCUSE:
            self.ph_excused += 1
        else:
            self.ph_misses += 1
        over = (len(self.ph) >= PH_MIN and self.ph_misses >= PH_END_MISSES) or len(self.ph) >= PH_MAX \
            or self.lines >= PHASE_MAX_LINES
        if not over:
            return
        fit = self.phase_fit()
        if fit:
            a_, s_ = fit
            p = a_ + s_ * (self.lines + 1)
            while p < 0:
                p += W + min(s_, 0)
            self.phase_at = round(p) % W
            self.pos -= p * self.line_samples / W
            self.slant *= 1 + s_ / W
            self.line_samples = FS * 60.0 / self.lpm * self.slant
            if self.pos >= 0:
                self.pos -= self.line_samples
        self.state = 'rx'

    def end_of_line(self, t):
        self.lines += 1
        if self.state == 'phasing':
            self.phasing_line()
            return
        if self.lines < PHASE_MAX_LINES and self.looks_like_phasing():
            return
        self.image.append(self.line.copy())
        if len(self.image) >= MAX_LINES:
            self.end(t, 'max')

    def put_pixel(self):
        if self.acc_n:
            self.line[self.col] = self.acc / self.acc_n
        self.acc, self.acc_n = 0.0, 0

    def add_sample(self, v, t):
        if self.pos >= 0:
            c = min(int(self.pos * W / self.line_samples), W - 1)
            if c != self.col:
                self.put_pixel()
                self.line[self.col + 1:c] = self.line[self.col]
                self.col = c
            self.acc += v
            self.acc_n += 1
        self.pos += 1.0
        if self.pos >= self.line_samples:
            self.put_pixel()
            self.pos -= self.line_samples
            self.col = 0
            self.end_of_line(t)

    def tone_sample(self, hz, t):
        self.win_x.append(hz - MID)
        if len(self.win_x) < TONE_WIN:
            return
        x, self.win_x = self.win_x, []
        self.start_run = self.start_run + 1 if self.tone_share(x, self.start_tone()) >= TONE_SHARE else 0
        self.stop_run = self.stop_run + 1 if self.tone_share(x, STOP_HZ) >= TONE_SHARE else 0
        if self.stop_run == TONE_RUNS:
            self.end(t, 'stop')
        elif self.start_run == TONE_RUNS and self.auto and self.state != 'phasing':
            self.end(t, 'restart')
            self.begin(True, t)

    def process(self, hz):
        for i, f in enumerate(hz):
            t = i / FS
            self.tone_sample(f, t)
            if self.state != 'idle':
                self.add_sample(min(1.0, max(0.0, (f - BLACK) / (WHITE - BLACK))), t)

    def finish(self, t):
        self.end(t, 'eof')


# ---------------------------------------------------------------------------
# Synthetic transmission

def test_pattern(n_lines, width=1810):
    """Grey ramp, vertical bars, a diagonal line and a black box."""
    img = np.zeros((n_lines, width))
    x = np.arange(width) / width
    for y in range(n_lines):
        row = x.copy()                                   # ramp
        row[(np.arange(width) // 60) % 2 == 0] *= 0.5    # bars
        d = int(y / n_lines * width)
        row[max(0, d - 6):d + 6] = 0.0                   # diagonal
        if n_lines // 3 < y < n_lines // 2:
            row[width // 2:width * 3 // 4] = 1.0         # white box
        img[y] = row
    return img


def transmit(img, lpm=120, ioc=576, lead=2.0, offset=0.37, snr_db=20.0, mistune=0.0,
             start=True, phasing=True, stop=True, seed=1, clock=1.0):
    """Audio at FS: start tone, phasing lines, image, stop tone.
    offset: where (share of a line) the receiver's line clock starts.
    clock: transmitter line length relative to nominal (receiver clock error)."""
    ls = FS * 60.0 / lpm * clock
    freq = [np.full(int(lead * FS), 1900.0)]
    t_tone = np.arange(int(5 * FS)) / FS
    square = lambda f: np.where(np.sin(2 * np.pi * f * t_tone) >= 0, WHITE, BLACK)
    if start:
        freq.append(square(675.0 if ioc == 288 else 300.0))
    body = []
    if phasing:
        for _ in range(60):
            u = np.arange(int(ls)) / ls
            body.append(np.where((u < 0.025) | (u >= 0.975), WHITE, BLACK))
    for row in img:
        u = np.arange(int(ls)) / ls
        body.append(BLACK + (WHITE - BLACK) * row[np.minimum((u * len(row)).astype(int), len(row) - 1)])
    freq.append(np.concatenate(body))
    if stop:
        freq.append(square(STOP_HZ))
    freq.append(np.full(int(2 * FS), 1900.0))
    f = np.concatenate(freq) + mistune
    phase = np.cumsum(2 * np.pi * f / FS)
    x = 0.5 * np.sin(phase)
    rng = np.random.default_rng(seed)
    # SNR in a 2.5 kHz bandwidth.
    noise = 0.5 / math.sqrt(2) / 10 ** (snr_db / 20) * math.sqrt(FS / 2 / 2500)
    x += rng.normal(0, noise, len(x))
    skip = int(offset * ls)
    return x[skip:] if offset else x


def compare(decoded, img, lpm):
    """Mean abs error per pixel (0..1) after resampling the source to W."""
    src = img[:, (np.arange(W) * img.shape[1] / W).astype(int)]
    n = min(len(decoded), len(src))
    if n == 0:
        return 1.0, 0
    # The line that ends phasing is not shown: allow the image to start a
    # couple of lines late.
    dec = np.array(decoded)
    best = min(float(np.mean(np.abs(dec[5:m - 5] - src[5 + k:m - 5 + k])))
               for k in range(3) for m in [min(len(dec), len(src) - k)])
    return best, len(decoded)


def run_case(name, lpm=120, ioc=576, lines=120, **kw):
    img = test_pattern(lines)
    x = transmit(img, lpm, ioc, **kw)
    fax = Fax(lpm, ioc)
    fax.process(fm_demod(x))
    fax.finish(len(x) / FS)
    got = fax.images[0] if fax.images else []
    err, n = compare(got, img, lpm)
    print(f"{name:34s} events {fax.events}  phase {fax.phase_at}  lines {n}/{lines}  err {err:.3f}")
    return fax, err, n


def selftest():
    ok = True
    for name, kw, max_err in [
        ('120 lpm, clean', {}, 0.05),
        ('120 lpm, 10 dB SNR', {'snr_db': 10.0}, 0.10),
        ('120 lpm, 5 dB SNR', {'snr_db': 5.0}, 0.15),
        ('120 lpm, offset 0.8', {'offset': 0.8}, 0.05),
        ('120 lpm, mistuned +40 Hz', {'mistune': 40.0}, 0.08),
        ('60 lpm', {'lpm': 60, 'lines': 60}, 0.05),
        ('IOC 288', {'ioc': 288}, 0.05),
        ('relogio +0,4 %', {'clock': 1.004}, 0.06),
        ('relogio -0,4 %', {'clock': 0.996}, 0.06),
        ('relogio +0,15 %, 10 dB', {'clock': 1.0015, 'snr_db': 10.0}, 0.10),
        ('relogio +1 %, pulso no bordo', {'clock': 1.01, 'offset': 0.1}, 0.05),
        ('relogio -1 %, pulso no bordo', {'clock': 0.99, 'offset': 0.1}, 0.05),
        ('relogio +3 %', {'clock': 1.03}, 0.05),
        ('relogio -3 %', {'clock': 0.97}, 0.05),
    ]:
        fax, err, n = run_case(name, **kw)
        lines = kw.get('lines', 120)
        # n includes the ~1.5 s of stop tone before it is recognised.
        ok &= err <= max_err and lines - 2 <= n <= lines + 4 and fax.events[-1][0] == 'stop'
    # Noise only: must not start.
    rng = np.random.default_rng(7)
    fax = Fax()
    fax.process(fm_demod(rng.normal(0, 0.3, 120 * FS)))
    print(f"{'noise only, 120 s':34s} events {fax.events}")
    ok &= not fax.events
    print('OK' if ok else 'FALHOU')
    return ok


def write_wav(path):
    x = transmit(test_pattern(240), snr_db=30.0)
    pcm = (np.clip(x, -1, 1) * 32000).astype('<i2')
    with wave.open(path, 'wb') as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(FS)
        w.writeframes(pcm.tobytes())
    print(f"{path}: {len(x) / FS:.0f} s")


def run_file(args):
    path = args[1]
    lpm = int(args[args.index('--lpm') + 1]) if '--lpm' in args else 120
    ioc = int(args[args.index('--ioc') + 1]) if '--ioc' in args else 576
    with wave.open(path) as w:
        rate, ch = w.getframerate(), w.getnchannels()
        x = np.frombuffer(w.readframes(w.getnframes()), '<i2').astype(float) / 32768
    x = x[::ch]
    if rate != FS:
        x = np.interp(np.arange(0, len(x), rate / FS), np.arange(len(x)), x)
    fax = Fax(lpm, ioc, auto=True)
    hz = fm_demod(x)
    fax.process(hz)
    if not fax.images and fax.state == 'idle':
        fax.begin(False, 0)            # no start tone found: decode everything
        fax.process(hz)
    fax.finish(len(x) / FS)
    print(fax.events)
    for k, img in enumerate(fax.images):
        out = path.rsplit('.', 1)[0] + (f'_{k}' if k else '') + '.pgm'
        a = (np.clip(np.array(img), 0, 1) * 255).astype(np.uint8)
        with open(out, 'wb') as f:
            f.write(b'P5 %d %d 255\n' % (W, len(a)) + a.tobytes())
        print(f"{out}: {len(a)} linhas")


if __name__ == '__main__':
    if len(sys.argv) > 2 and sys.argv[1] == '--wav':
        write_wav(sys.argv[2])
    elif len(sys.argv) > 2 and sys.argv[1] == '--file':
        run_file(sys.argv[1:])
    else:
        sys.exit(0 if selftest() else 1)
