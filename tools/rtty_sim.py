# Pure-Python mirror of src/rtty_decoder.cpp, fed with synthetic RTTY, to
# check the algorithm without hardware. Keep it in sync with the C++.
#
#   python tools/rtty_sim.py                    synthetic test cases
#   python tools/rtty_sim.py --wav out.wav      writes a test signal (for a phone)
#   python tools/rtty_sim.py --file audio.wav --mark 2125 --space 2295 [--baud 45.45]
import math, random, sys, wave, array

FS = 12000

LTRS = "\0E\nA SIU\rDRJNFCKTZLWHYPQOBG\x0fMXV\x0e"
FIGS = "\x003\n- \x0787\r$4',!:(5\")2#6019?&\x0f./;\x0e"
FIGS_CODE, LTRS_CODE = 0x1B, 0x1F
POL_ALPHA = 1.0 / (3.0 * FS)    # polarity statistics time constant: ~3 s
SQ_ALPHA = 1.0 / (0.15 * FS)    # squelch: mean |d| over ~150 ms
# Mean |d|: noise alone 0.27..0.33, FSK at 3 dB SNR (300 Hz) 0.43..0.53.
SQ_OPEN, SQ_CLOSE = 0.42, 0.38
# Frame quality: share of recent characters with a valid stop bit. Text is
# only shown, and RTTY only counts as active, above FQ_SHOW.
FQ_ALPHA, FQ_START, FQ_SHOW = 0.15, 0.6, 0.5
# FSK keeps one tone on all the time, so the total level has no gaps; two keyed
# CW carriers 170 Hz apart (seen on the air) often have both off.
LEVEL_ALPHA = 1.0 / (1.0 * FS)
GAP_ALPHA = 1.0 / (0.5 * FS)
GAP_LEVEL, GAP_MAX = 0.3, 0.15
ENC_L = {c: i for i, c in enumerate(LTRS) if c not in '\0\x0e\x0f'}
ENC_F = {c: i for i, c in enumerate(FIGS) if c not in '\0\x0e\x0f\x07'}


class Decoder:
    """Two-tone quadrature detector + start-bit UART, as in rtty_decoder.cpp."""

    def __init__(self, f_lo, f_hi, baud=45.45, reverse=None):
        self.bit = FS / baud                        # samples per bit
        self.win = max(8, int(round(self.bit / 2)))  # detector window: half a bit
        self.w = [2 * math.pi * f / FS for f in (f_lo, f_hi)]
        self.ph = [0.0, 0.0]
        self.ring = [[0j] * self.win, [0j] * self.win]
        self.acc = [0j, 0j]
        self.pos = 0
        self.n = 0
        self.reverse = reverse                      # None = auto
        self.hi_share = 0.5                         # fraction of time the high tone dominates
        self.hi_mark = False                        # auto polarity decision
        self.quality = 0.0                          # mean |d|, for the squelch
        self.level = 0.0                            # slow mean of the two-tone level
        self.gaps = 0.0                             # share of time that level drops out
        self.open = False
        self.state = 'idle'
        self.mark_run = 0
        self.t_edge = 0.0
        self.bits = []
        self.figs = False
        self.text = ''
        self.frames = self.errors = 0
        self.fq = FQ_START

    def polarity_hi_is_mark(self):
        if self.reverse is not None:
            return not self.reverse                 # normal: mark = low tone
        # RTTY idles on mark and every stop bit is mark: the tone that
        # dominates over time (seconds, with hysteresis) is the mark.
        if self.hi_share > 0.55:
            self.hi_mark = True
        elif self.hi_share < 0.45:
            self.hi_mark = False
        return self.hi_mark

    def emit(self, code):
        self.frames += 1
        self.fq += (1.0 - self.fq) * FQ_ALPHA
        if code == LTRS_CODE:
            self.figs = False
        elif code == FIGS_CODE:
            self.figs = True
        else:
            c = (FIGS if self.figs else LTRS)[code]
            if c == ' ':
                self.figs = False                   # unshift on space (USOS)
            if c == '\r' or c == '\0' or c == '\x07':
                return
            if self.fq < FQ_SHOW:
                return                              # mostly framing errors: not RTTY
            self.text += ' ' if c == '\n' else c

    def process(self, x):
        for s in x:
            for k in range(2):
                z = s * complex(math.cos(self.ph[k]), -math.sin(self.ph[k]))
                self.ph[k] += self.w[k]
                if self.ph[k] > 2 * math.pi:
                    self.ph[k] -= 2 * math.pi
                self.acc[k] += z - self.ring[k][self.pos]
                self.ring[k][self.pos] = z
            self.pos = (self.pos + 1) % self.win
            lo, hi = abs(self.acc[0]), abs(self.acc[1])
            self.n += 1
            tot = lo + hi
            if tot <= 1e-9:
                continue
            self.hi_share += ((1.0 if hi > lo else 0.0) - self.hi_share) * POL_ALPHA
            d = (hi - lo) / tot                     # +1 high tone, -1 low tone
            self.quality += (abs(d) - self.quality) * SQ_ALPHA
            self.level += (tot - self.level) * LEVEL_ALPHA
            self.gaps += ((1.0 if tot < GAP_LEVEL * self.level else 0.0) - self.gaps) * GAP_ALPHA
            if self.quality > SQ_OPEN and self.gaps < GAP_MAX:
                self.open = True
            elif self.quality < SQ_CLOSE or self.gaps > GAP_MAX:
                self.open = False
            if not self.open:
                self.state = 'idle'                 # squelched: noise, not FSK
                self.mark_run = 0
                continue
            if not self.polarity_hi_is_mark():
                d = -d                              # d > 0 means mark
            self.uart(d)

    def uart(self, d):
        # The half-bit window centred at t - T/4 reports the line state; a
        # mark->space step at e crosses zero at e + T/4.
        T = self.bit
        if self.state == 'idle':
            if d > 0.2:
                self.mark_run += 1
            elif d < 0 and self.mark_run > T / 2:
                self.state = 'frame'
                self.t_edge = self.n - T / 4        # estimated start-bit edge
                self.bits = []
                self.mark_run = 0
            elif d <= 0.2:
                self.mark_run = max(0, self.mark_run - 1)
            return
        # sample bit k (0 = start, 1..5 data, 6 stop) at its centre + T/4
        k = len(self.bits)
        if self.n >= self.t_edge + (k + 0.5) * T + T / 4:
            self.bits.append(1 if d > 0 else 0)
            if k == 0 and self.bits[0] != 0:
                self.state = 'idle'                 # false start
            elif k == 6:
                if self.bits[6] == 1:
                    code = sum(b << i for i, b in enumerate(self.bits[1:6]))
                    self.emit(code)
                else:
                    self.errors += 1
                    self.fq += (0.0 - self.fq) * FQ_ALPHA
                self.state = 'idle'
                self.mark_run = int(T / 2) + 1      # stop bit counts as idle mark


def encode(text):
    """Text -> list of 5-bit codes with LTRS/FIGS shifts."""
    codes, figs = [LTRS_CODE, LTRS_CODE], False
    for c in text.upper():
        if c in ENC_L and not (figs and c != ' ' and c in ENC_F and c not in ENC_L):
            if figs and c != ' ':
                codes.append(LTRS_CODE)
                figs = False
            codes.append(ENC_L[c])
            if c == ' ':
                figs = False
        elif c in ENC_F:
            if not figs:
                codes.append(FIGS_CODE)
                figs = True
            codes.append(ENC_F[c])
    return codes


def keying(codes, baud, idle_s=0.6):
    """Codes -> list of (is_mark, duration) with 1 start, 5 data, 1.5 stop bits."""
    T = 1.0 / baud
    seq = [(True, idle_s)]
    for code in codes:
        seq.append((False, T))
        for i in range(5):
            seq.append((bool(code >> i & 1), T))
        seq.append((True, 1.5 * T))
    seq.append((True, idle_s))
    return seq


def synth(seq, f_mark, f_space, amp, noise_rms, fs=FS):
    out, ph = [], 0.0
    for is_mark, dur in seq:
        w = 2 * math.pi * (f_mark if is_mark else f_space) / fs
        for _ in range(int(round(dur * fs))):
            out.append(amp * math.sin(ph) + random.gauss(0, noise_rms))
            ph += w
    return out


def edits(a, b):
    prev = list(range(len(b) + 1))
    for i, ca in enumerate(a, 1):
        cur = [i]
        for j, cb in enumerate(b, 1):
            cur.append(min(prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (ca != cb)))
        prev = cur
    return prev[-1]


MSG = 'RYRYRY CQ CQ DE CT1ABC CT1ABC PSE K 73 AND 599 TU'


def noise_for_snr(amp, snr_db, bw=300.0):
    # tone power vs noise in `bw` Hz out of FS/2
    return math.sqrt(amp * amp / 2 / 10 ** (snr_db / 10) * (FS / 2) / bw)


def run_cases():
    cases = [
        ('45.45 bd 170 Hz limpo', 45.45, 2125, 2295, None, 0.0, None),
        ('45.45 bd invertido (auto)', 45.45, 2295, 2125, None, 0.0, None),
        ('45.45 bd SNR 10 dB/300 Hz', 45.45, 2125, 2295, 10, 0.0, None),
        ('45.45 bd SNR 6 dB/300 Hz', 45.45, 2125, 2295, 6, 0.0, None),
        ('45.45 bd desvio 10 Hz', 45.45, 2135, 2305, None, 0.0, None),
        ('45.45 bd baud +1%', 45.9, 2125, 2295, None, 0.0, None),
        ('50 bd 170 Hz', 50.0, 1000, 1170, None, 0.0, 50.0),
        ('75 bd 850 Hz', 75.0, 1275, 2125, None, 0.0, 75.0),
    ]
    amp = 0.1
    print(f'{"caso":30s} CER    exemplo')
    for name, baud, fm, fsp, snr, _, dec_baud in cases:
        errs, total, example = 0, 0, ''
        for seed in range(3):
            random.seed(seed)
            noise = amp * 0.003 if snr is None else noise_for_snr(amp, snr)
            x = synth(keying(encode(MSG), baud), fm, fsp, amp, noise)
            lo, hi = sorted((fm, fsp))
            d = Decoder(lo, hi, dec_baud or 45.45)
            d.process(x)
            out = ' '.join(d.text.split())
            errs += edits(out, MSG)
            total += len(MSG)
            if not example and out != MSG:
                example = out
        print(f'{name:30s} {100.0 * errs / total:5.1f}%  {example}')
    random.seed(5)
    x = [0.0] * (10 * FS)
    for f, period in ((850.0, 0.24), (1020.0, 0.17)):     # two keyed CW carriers
        for n in range(len(x)):
            if (n / FS) % period < period * 0.55:
                x[n] += 0.1 * math.sin(2 * math.pi * f * n / FS)
    x = [v + random.gauss(0, 0.002) for v in x]
    d = Decoder(850, 1020)
    d.process(x)
    print(f'{"dois CW a 850/1020 Hz":30s} {len(d.text.strip())} caracteres: {d.text.strip()[:40]!r}')
    random.seed(9)
    d = Decoder(2125, 2295)
    d.process([random.gauss(0, 0.05) for _ in range(10 * FS)])
    print(f'{"so ruido 10 s":30s} {len(d.text.strip())} caracteres: {d.text.strip()[:40]!r}')


def write_wav(path):
    fs = 48000
    random.seed(1)
    text = ('RYRYRYRYRY CQ CQ CQ DE CT1ABC CT1ABC CT1ABC PSE K\n'
            'THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG 0123456789\n'
            'RST 599 599 QTH LISBOA NAME JOAQUIM 73 SK ') * 3
    x = synth(keying(encode(text), 45.45, 1.0), 2125, 2295, 0.3, 0.003, fs)
    pcm = array.array('h', [max(-32767, min(32767, int(v * 32767))) for v in x])
    with wave.open(path, 'wb') as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(fs)
        w.writeframes(pcm.tobytes())
    print(f'escrito {path}: {len(x) / fs:.0f} s, mark 2125 Hz, space 2295 Hz, 45.45 baud')


def run_file(args):
    import miniaudio
    a = dict(zip(args[0::2], args[1::2]))
    x = miniaudio.decode_file(a['--file'], output_format=miniaudio.SampleFormat.FLOAT32,
                              nchannels=1, sample_rate=FS).samples
    lo, hi = sorted((float(a.get('--mark', 2125)), float(a.get('--space', 2295))))
    d = Decoder(lo, hi, float(a.get('--baud', 45.45)))
    d.process(x)
    print(f'{d.frames} caracteres, {d.errors} erros de enquadramento')
    print(' '.join(d.text.split()))


if __name__ == '__main__':
    if len(sys.argv) > 2 and sys.argv[1] == '--wav':
        write_wav(sys.argv[2])
    elif len(sys.argv) > 2 and sys.argv[1] == '--file':
        run_file(sys.argv[1:])
    else:
        run_cases()
