# Python mirror of src/sstv_decoder.cpp (with fm_demod from fax_sim.py), fed
# with synthetic SSTV, to check the algorithm without hardware. Keep it in
# sync with the C++.
#
#   python tools/sstv_sim.py                          synthetic test cases
#   python tools/sstv_sim.py --wav out.wav [mode]     writes a test transmission
#   python tools/sstv_sim.py --file audio.wav         -> audio.ppm
import math, sys, wave
import numpy as np
from fax_sim import fm_demod, FS

MS = FS / 1000
BLACK, WHITE = 1500.0, 2300.0
BIN = FS // 1000
VIS_BITS_END, VIS_TOL, VIS_RUN_MAX = 300, 70.0, 40
VIS_MAX_OFF, VIS_LEADER_SD = 250.0, 150.0
SYNC_MAX_HZ, WIN_SHARE, WIN_MAX_MS = 1350.0, 0.08, 30.0
FIT_MIN, SLANT_MAX = 8, 0.02
PIX_MIN_HZ, PIX_MAX_HZ = 1400.0, 2400.0
# Auto adjust (as slowrx): the image's frequency track is kept (1 byte per
# sample, 6 Hz steps from 1000 Hz) and, at the end, the sync samples are folded
# over candidate line lengths; the sharpest fold gives line length and start,
# and the whole image is drawn again from the track.
TRACK_HZ0, TRACK_STEP = 1000.0, 6.0
SYNC_Q = int((SYNC_MAX_HZ - TRACK_HZ0) / TRACK_STEP)
ADJ_MAX_SYNC = 20000                       # sync samples folded (subsampled above)
GALLERY_MIN_ROWS = 32


def pd(name, vis, w, lines, s):
    return dict(name=name, vis=vis, kind='PD', width=w, lines=lines, line_ms=22.08 + 4 * s,
                sync_ms=0.0, sync_len_ms=20.0, scan_ms=s, scan2_ms=s, sep_ms=0.0,
                ch=[22.08, 22.08 + s, 22.08 + 2 * s, 22.08 + 3 * s], first_ms=0.0)


def rgb(name, vis, line, sync, sync_len, scan, ch, first):
    return dict(name=name, vis=vis, kind='RGB', width=320, lines=256, line_ms=line, sync_ms=sync,
                sync_len_ms=sync_len, scan_ms=scan, scan2_ms=0, sep_ms=0, ch=ch, first_ms=first)


MODES = [
    rgb('Martin M1', 44, 446.446, 0.0, 4.862, 146.432, [5.434, 152.438, 299.442], 0.0),
    rgb('Martin M2', 40, 226.798, 0.0, 4.862, 73.216, [5.434, 79.222, 153.010], 0.0),
    rgb('Scottie S1', 60, 428.22, 279.48, 9.0, 138.24, [1.5, 141.24, 289.98], 9.0),
    rgb('Scottie S2', 56, 277.692, 180.628, 9.0, 88.064, [1.5, 91.064, 191.128], 9.0),
    rgb('Scottie DX', 76, 1050.3, 695.7, 9.0, 345.6, [1.5, 348.6, 706.2], 9.0),
    dict(name='Robot 36', vis=8, kind='R36', width=320, lines=240, line_ms=150.0, sync_ms=0.0,
         sync_len_ms=9.0, scan_ms=88.0, scan2_ms=44.0, sep_ms=100.0, ch=[12.0, 106.0], first_ms=0.0),
    dict(name='Robot 72', vis=12, kind='R72', width=320, lines=240, line_ms=300.0, sync_ms=0.0,
         sync_len_ms=9.0, scan_ms=138.0, scan2_ms=69.0, sep_ms=0.0, ch=[12.0, 156.0, 231.0], first_ms=0.0),
    pd('PD50', 93, 320, 128, 91.52),
    pd('PD90', 99, 320, 128, 170.24),
    pd('PD120', 95, 640, 248, 121.6),
    pd('PD160', 98, 512, 200, 195.584),
    pd('PD180', 96, 640, 248, 183.04),
    pd('PD240', 97, 640, 248, 244.48),
]
BY_NAME = {m['name']: m for m in MODES}


def level(hz):
    return np.clip(np.floor((hz - BLACK) * (255.0 / (WHITE - BLACK)) + 0.5), 0, 255)


def yuv_to_rgb(y, ry, by):
    R, B = ry - 128.0, by - 128.0
    c = np.stack([y + 1.402 * R, y - 0.344 * B - 0.714 * R, y + 1.772 * B], -1)
    return np.clip(np.floor(c + 0.5), 0, 255)


class Sstv:
    def __init__(self):
        self.images = []       # (mode name, rows x width x 3), after the auto adjust
        self.live = []         # the same, as drawn while receiving
        self.receiving = False
        self.run, self.run_code, self.run_first = 0, -1, 0

    # --- VIS
    def vis_at(self, e):
        if e < 600:
            return -1
        b = self.bins
        seg = lambda from_end, n: b[e - from_end:e - from_end + n].mean()
        leader = b[e - (VIS_BITS_END + 250):e - (VIS_BITS_END + 250) + 200]
        off = leader.mean() - 1900
        if abs(off) > VIS_MAX_OFF or leader.var() > VIS_LEADER_SD ** 2:
            return -1
        el = lambda k: seg(VIS_BITS_END - 1 - 30 * k - 5, 20) - off
        if abs(el(0) - 1200) > VIS_TOL or abs(el(9) - 1200) > VIS_TOL:
            return -1
        self.vis_off = off
        code = ones = 0
        for k in range(8):
            f = el(1 + k)
            one = abs(f - 1100) <= VIS_TOL
            if not one and abs(f - 1300) > VIS_TOL:
                return -1
            if one:
                ones += 1
                if k < 7:
                    code |= 1 << k
        return code if ones % 2 == 0 else -1

    def vis_bin(self, nb):
        code = self.vis_at(nb)
        if code >= 0 and (code == self.run_code or self.run == 0):
            if self.run == 0:
                self.run_first = nb
            self.run_code = code
            self.run += 1
        if self.run > 0 and (code != self.run_code or self.run >= VIS_RUN_MAX):
            end_bin = self.run_first + (self.run - 1) // 2
            m = next((m for m in MODES if m['vis'] == self.run_code), None)
            self.run, self.run_code = 0, -1
            if m and self.vis_at(end_bin) >= 0:
                self.begin(m, (end_bin + 1) * BIN, self.vis_off)

    # --- image
    def end(self):
        if self.receiving:
            self.live.append((self.mode['name'], np.array(self.rows)))
            if len(self.rows) >= GALLERY_MIN_ROWS and self.adjust():
                self.render_all()
            self.images.append((self.mode['name'], np.array(self.rows)))
        self.receiving = False

    def track(self):
        """Quantised frequency track from the VIS end to the samples received."""
        t0 = int(self.t0)
        h = self.h[t0:self.count_now]
        return t0, np.clip(np.round((h - TRACK_HZ0) / TRACK_STEP), 0, 255).astype(np.uint8)

    def fold(self, sync, L, ln):
        ph = sync - L * np.floor(sync / L)
        nb = int(np.ceil(L))
        hist = np.bincount(np.minimum(ph.astype(np.int64), nb - 1), minlength=nb).astype(float)
        c = np.concatenate((hist, hist[:ln]))
        win = np.convolve(c, np.ones(ln), 'valid')[:nb]
        k = int(np.argmax(win))
        return win[k], k

    def adjust(self):
        m = self.mode
        t0, q = self.track()
        sync = np.nonzero(q <= SYNC_Q)[0].astype(float)
        if len(sync) < 10:
            return False
        if len(sync) > ADJ_MAX_SYNC:
            sync = sync[::int(np.ceil(len(sync) / ADJ_MAX_SYNC))]
        ln = int(m['sync_len_ms'] * MS)
        lines = max(self.line_no, 1)
        step = max(ln / lines, 0.05)
        lo, hi = self.b_nom * (1 - SLANT_MAX), self.b_nom * (1 + SLANT_MAX)
        best = (-1, 0, 0)
        for L in np.arange(lo, hi, step):
            sc, k = self.fold(sync, L, ln)
            if sc > best[0]:
                best = (sc, L, k)
        L0 = best[1]
        for L in np.arange(L0 - step, L0 + step, step / 10):
            sc, k = self.fold(sync, L, ln)
            if sc > best[0]:
                best = (sc, L, k)
        _, L, k = best
        phase = (k - m['sync_ms'] * MS) % L          # line start, mod L, from t0
        self.b = L
        self.a = (t0 - self.t0) + phase + L * round((self.a0 - (t0 - self.t0) - phase) / L)
        self.adj = (L / self.b_nom - 1, self.a)
        self.hq_t0, self.hq = t0, TRACK_HZ0 + TRACK_STEP * q.astype(float)
        return True

    def render_all(self):
        """Draws every line again from the track with the adjusted a, b."""
        m = self.mode
        hp = np.clip(self.hq, PIX_MIN_HZ, PIX_MAX_HZ)
        cum = np.concatenate(([0.0], np.cumsum(hp)))
        base = self.hq_t0
        def mh(frm, to):
            i = np.ceil(frm).astype(np.int64) - base
            e = np.ceil(to).astype(np.int64) - base
            ok = (i >= 0) & (e <= len(hp))
            i, e = np.clip(i, 0, len(hp)), np.clip(e, 0, len(hp))
            out = (cum[e] - cum[i]) / np.maximum(e - i, 1)
            empty = e <= i
            out[empty] = hp[np.clip(np.round(frm[empty]).astype(np.int64) - base, 0, len(hp) - 1)]
            return out
        saved = self.mean_hz
        self.mean_hz = mh
        n = self.line_no
        self.rows = []
        self.ry_last = np.full(m['width'], 128.0)
        self.by_last = np.full(m['width'], 128.0)
        for k in range(n):
            self.render_line(k)
        self.mean_hz = saved

    def begin(self, m, vis_end, off=0.0):
        self.end()
        self.foff = off
        # The image is judged against the tuning offset measured on the VIS.
        self.h = self.h_raw - off
        self.cum = np.concatenate(([0.0], np.cumsum(self.h)))
        self.hp = np.clip(self.h, PIX_MIN_HZ, PIX_MAX_HZ)
        self.cump = np.concatenate(([0.0], np.cumsum(self.hp)))
        self.mode = m
        self.receiving = True
        self.line_no, self.missed = 0, 0
        self.rows = []
        self.t0 = vis_end
        self.b_nom = self.b = m['line_ms'] * MS
        self.a = self.a0 = m['first_ms'] * MS
        self.sk = self.st = self.skk = self.skt = 0.0
        self.n_fit = 0
        self.ry_last = np.full(m['width'], 128.0)
        self.by_last = np.full(m['width'], 128.0)

    def mean_hz(self, frm, to):
        i, e = np.ceil(frm).astype(np.int64), np.ceil(to).astype(np.int64)
        cs = self.cump
        out = (cs[e] - cs[i]) / np.maximum(e - i, 1)
        empty = e <= i
        if np.any(empty):
            out[empty] = self.hp[np.round(frm[empty]).astype(np.int64)]
        return out

    def read_channel(self, ls, start_ms, scan_ms):
        r = self.b / self.b_nom * MS
        w = self.mode['width']
        j = np.arange(w)
        frm = ls + r * (start_ms + scan_ms * j / w)
        to = ls + r * (start_ms + scan_ms * (j + 1) / w)
        return level(self.mean_hz(frm, to))

    def track_sync(self, k):
        m = self.mode
        pred = self.t0 + self.a + self.b * k
        win = min(WIN_SHARE * m['line_ms'], WIN_MAX_MS) * MS
        ln = int(m['sync_len_ms'] * MS)
        s0 = int(round(pred + m['sync_ms'] * MS - win))
        span = int(2 * win)
        sums = self.cum[s0 + ln:s0 + span + ln + 1] - self.cum[s0:s0 + span + 1]
        best_at = int(np.argmin(sums))
        best = sums[best_at]
        t = s0 + best_at - m['sync_ms'] * MS - self.t0
        if best / ln > SYNC_MAX_HZ or (self.n_fit >= FIT_MIN and abs(t - (self.a + self.b * k)) > 0.5 * win):
            self.missed += 1
            return
        self.missed = 0
        self.n_fit += 1
        self.sk += k; self.st += t; self.skk += k * k; self.skt += k * t
        den = self.n_fit * self.skk - self.sk ** 2
        if self.n_fit >= FIT_MIN and den > 0:
            self.b = (self.n_fit * self.skt - self.sk * self.st) / den
            self.b = min(max(self.b, self.b_nom * (1 - SLANT_MAX)), self.b_nom * (1 + SLANT_MAX))
        self.a = (self.st - self.b * self.sk) / self.n_fit

    def decode_line(self):
        m = self.mode
        self.track_sync(self.line_no)
        self.render_line(self.line_no)
        self.line_no += 1
        if self.line_no >= m['lines']:
            self.end()

    def render_line(self, k):
        m = self.mode
        ls = self.t0 + self.a + self.b * k
        rc = lambda c, scan: self.read_channel(ls, m['ch'][c], scan)
        if m['kind'] == 'RGB':
            g, b, r = rc(0, m['scan_ms']), rc(1, m['scan_ms']), rc(2, m['scan_ms'])
            self.rows.append(np.stack([r, g, b], -1))
        elif m['kind'] == 'R36':
            y = rc(0, m['scan_ms'])
            rr = self.b / self.b_nom * MS
            is_ry = self.mean_hz(np.array([ls + rr * m['sep_ms']]), np.array([ls + rr * (m['sep_ms'] + 4.5)]))[0] < 1900
            if is_ry:
                self.ry_last = rc(1, m['scan2_ms'])
            else:
                self.by_last = rc(1, m['scan2_ms'])
            self.rows.append(yuv_to_rgb(y, self.ry_last, self.by_last))
        elif m['kind'] == 'R72':
            self.rows.append(yuv_to_rgb(rc(0, m['scan_ms']), rc(1, m['scan2_ms']), rc(2, m['scan2_ms'])))
        else:
            y0, ry, by, y1 = (rc(c, m['scan_ms']) for c in range(4))
            self.rows.append(yuv_to_rgb(y0, ry, by))
            self.rows.append(yuv_to_rgb(y1, ry, by))

    def line_ready(self, count):
        m = self.mode
        win = min(WIN_SHARE * m['line_ms'], WIN_MAX_MS) * MS
        return count > self.t0 + self.a + self.b * self.line_no + self.b + win + 2

    def process(self, hz):
        self.h = self.h_raw = np.clip(hz, 0, 4000).astype(np.int16).astype(float)
        self.cum = np.concatenate(([0.0], np.cumsum(self.h)))
        self.hp = np.clip(self.h, PIX_MIN_HZ, PIX_MAX_HZ)       # pixels: FM clicks limited
        self.cump = np.concatenate(([0.0], np.cumsum(self.hp)))
        nb = len(self.h) // BIN
        self.bins = self.h[:nb * BIN].reshape(nb, BIN).mean(1)
        for k in range(nb):
            self.vis_bin(k)
            count = (k + 1) * BIN
            self.count_now = count
            while self.receiving and self.line_ready(count):
                self.decode_line()
        self.count_now = len(self.h)
        self.end()


# ---------------------------------------------------------------------------
# Synthetic transmission

def test_image(w, h):
    """Colour bars on top, grey ramp, a red/blue gradient and a white cross."""
    x = np.arange(w) / w
    img = np.zeros((h, w, 3))
    bars = np.array([[255, 255, 255], [255, 255, 0], [0, 255, 255], [0, 255, 0],
                     [255, 0, 255], [255, 0, 0], [0, 0, 255], [0, 0, 0]])
    for y in range(h):
        if y < h // 3:
            img[y] = bars[(x * 8).astype(int)]
        elif y < 2 * h // 3:
            img[y] = (x * 255)[:, None]
        else:
            img[y, :, 0] = x * 255
            img[y, :, 2] = (1 - x) * 255
            img[y, :, 1] = 80
    img[h // 2 - 3:h // 2 + 3, :] = 255
    img[:, w // 2 - 3:w // 2 + 3] = 255
    return img


def rgb_to_yuv(img):
    y = 0.299 * img[..., 0] + 0.587 * img[..., 1] + 0.114 * img[..., 2]
    return y, 128 + (img[..., 0] - y) / 1.402, 128 + (img[..., 2] - y) / 1.772


def hz(v):
    return BLACK + (WHITE - BLACK) * np.asarray(v) / 255.0


def transmit(m, img, snr_db=30.0, clock=1.0, lead_ms=500.0, seed=1):
    """Audio at FS. clock: transmitter time scale (1.001 = 0.1% slow)."""
    seg = []                                   # (duration ms, freq or array of freqs)
    add = lambda d, f: seg.append((d, f))
    add(lead_ms, 1900.0)
    add(300, 1900); add(10, 1200); add(300, 1900); add(30, 1200)
    bits = [(m['vis'] >> k) & 1 for k in range(7)]
    bits.append(sum(bits) % 2)
    for bit in bits:
        add(30, 1100.0 if bit else 1300.0)
    add(30, 1200)
    w, s = m['width'], m['scan_ms']
    if m['first_ms']:
        add(9.0, 1200)                          # Scottie starting sync
    y, ry, by = rgb_to_yuv(img)
    for ln in range(m['lines']):
        if m['kind'] == 'RGB':
            r, g, b = (hz(img[ln, :, c]) for c in range(3))
            if m['name'].startswith('Martin'):
                add(4.862, 1200); add(0.572, 1500)
                add(s, g); add(0.572, 1500); add(s, b); add(0.572, 1500); add(s, r); add(0.572, 1500)
            else:
                add(1.5, 1500); add(s, g); add(1.5, 1500); add(s, b)
                add(9.0, 1200); add(1.5, 1500); add(s, r)
        elif m['kind'] == 'R36':
            add(9, 1200); add(3, 1500); add(s, hz(y[ln]))
            even = ln % 2 == 0
            add(4.5, 1500 if even else 2300); add(1.5, 1900)
            add(m['scan2_ms'], hz(ry[ln] if even else by[ln]))
        elif m['kind'] == 'R72':
            add(9, 1200); add(3, 1500); add(s, hz(y[ln]))
            add(4.5, 1500); add(1.5, 1900); add(m['scan2_ms'], hz(ry[ln]))
            add(4.5, 2300); add(1.5, 1900); add(m['scan2_ms'], hz(by[ln]))
        else:
            r0, r1 = 2 * ln, 2 * ln + 1
            add(20, 1200); add(2.08, 1500)
            add(s, hz(y[r0])); add(s, hz((ry[r0] + ry[r1]) / 2)); add(s, hz((by[r0] + by[r1]) / 2))
            add(s, hz(y[r1]))
    add(500, 1900)
    # Piecewise frequency -> samples.
    starts, freqs = [], []
    t = 0.0
    for d, f in seg:
        if np.ndim(f) == 0:
            starts.append(t); freqs.append(f)
        else:
            n = len(f)
            starts.extend(t + d * np.arange(n) / n); freqs.extend(f)
        t += d
    starts, freqs = np.array(starts) * clock, np.array(freqs)
    ts = np.arange(int(t * clock * MS)) / MS
    f = freqs[np.searchsorted(starts, ts, 'right') - 1]
    x = 0.5 * np.sin(np.cumsum(2 * np.pi * f / FS))
    noise = 0.5 / math.sqrt(2) / 10 ** (snr_db / 20) * math.sqrt(FS / 2 / 2500)
    return x + np.random.default_rng(seed).normal(0, noise, len(x))


def source_rows(m, img):
    """What a perfect decoder would show (PD and Robot chroma are shared)."""
    y, ry, by = rgb_to_yuv(img)
    if m['kind'] == 'RGB':
        return img
    if m['kind'] == 'PD':
        ry = (ry[0::2] + ry[1::2]).repeat(2, 0) / 2
        by = (by[0::2] + by[1::2]).repeat(2, 0) / 2
    return yuv_to_rgb(y, ry, by)


def run_case(name, snr_db=30.0, clock=1.0):
    m = BY_NAME[name]
    rows = m['lines'] * (2 if m['kind'] == 'PD' else 1)
    img = test_image(m['width'], rows)
    x = transmit(m, img, snr_db, clock)
    dec = Sstv()
    dec.process(fm_demod(x))
    if not dec.images:
        print(f"{name:12s} snr {snr_db:4.0f} clock {clock:.4f}: sem imagem")
        return 999.0, 0
    got_name, got = dec.images[0]
    ref = source_rows(m, img)
    n = min(len(got), len(ref))
    # Robot 36 mixes chroma of adjacent lines; skip the first rows.
    err = float(np.mean(np.abs(got[2:n] - ref[2:n])))
    print(f"{name:12s} snr {snr_db:4.0f} clock {clock:.4f}: {got_name} {len(got)}/{rows} linhas, erro {err:5.1f}")
    return err, len(got) if got_name == name else -1


def selftest():
    ok = True
    for m in MODES:
        rows = m['lines'] * (2 if m['kind'] == 'PD' else 1)
        # The fastest modes (PD50, Scottie S2) lose the sharpest edges of the
        # test image to the FM low-pass: mean error ~10-15 with median ~4.
        err, n = run_case(m['name'])
        ok &= n == rows and err < 16
    # Slant (transmitter clock off by 0.1..0.2%) and noise (the error is then
    # mostly noise in the pixels, not misalignment).
    for name, snr, clock, max_err in [('Martin M1', 10.0, 1.0, 25), ('Scottie S1', 30.0, 1.002, 10),
                                      ('PD120', 30.0, 0.998, 12), ('Robot 36', 12.0, 1.001, 36),
                                      ('Martin M2', 15.0, 1.008, 25), ('Robot 36', 20.0, 0.988, 25)]:
        rows = BY_NAME[name]['lines'] * (2 if BY_NAME[name]['kind'] == 'PD' else 1)
        err, n = run_case(name, snr, clock)
        ok &= n == rows and err < max_err
    rng = np.random.default_rng(3)
    dec = Sstv()
    dec.process(fm_demod(rng.normal(0, 0.3, 120 * FS)))
    print(f"ruído 120 s: {len(dec.images)} imagens")
    ok &= not dec.images
    print('OK' if ok else 'FALHOU')
    return ok


def write_wav(path, name='Martin M1'):
    m = BY_NAME[name]
    rows = m['lines'] * (2 if m['kind'] == 'PD' else 1)
    x = transmit(m, test_image(m['width'], rows), snr_db=40.0)
    pcm = (np.clip(x, -1, 1) * 32000).astype('<i2')
    with wave.open(path, 'wb') as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(FS)
        w.writeframes(pcm.tobytes())
    print(f"{path}: {name}, {len(x) / FS:.0f} s")


def run_file(path):
    with wave.open(path) as w:
        rate, ch = w.getframerate(), w.getnchannels()
        x = np.frombuffer(w.readframes(w.getnframes()), '<i2').astype(float) / 32768
    x = x[::ch]
    if rate != FS:
        x = np.interp(np.arange(0, len(x), rate / FS), np.arange(len(x)), x)
    dec = Sstv()
    dec.process(fm_demod(x))
    for k, (name, img) in enumerate(dec.images):
        out = path.rsplit('.', 1)[0] + (f'_{k}' if k else '') + '.ppm'
        with open(out, 'wb') as f:
            f.write(b'P6 %d %d 255\n' % (img.shape[1], img.shape[0]) + img.astype(np.uint8).tobytes())
        print(f"{out}: {name}, {len(img)} linhas")
    if not dec.images:
        print('nenhum cabeçalho VIS encontrado')


if __name__ == '__main__':
    if len(sys.argv) > 2 and sys.argv[1] == '--wav':
        write_wav(sys.argv[2], ' '.join(sys.argv[3:]) or 'Martin M1')
    elif len(sys.argv) > 2 and sys.argv[1] == '--file':
        run_file(sys.argv[2])
    else:
        sys.exit(0 if selftest() else 1)
