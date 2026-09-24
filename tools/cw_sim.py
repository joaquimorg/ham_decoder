# Pure-Python mirror of src/cw_decoder.cpp, fed with synthetic CW, to check
# the algorithm (thresholds, debounce, timing adaptation) without hardware.
# Keep it in sync with the C++ when the algorithm changes.
#
#   python tools/cw_sim.py [MIN_CONTRAST] [WIN_DIV]      (defaults 5.0 3)
#   python tools/cw_sim.py --file audio.mp3|.wav [--tone HZ] [--ref texto.txt] [--seconds N]
#       decodes a real recording (needs `pip install miniaudio`) and, with
#       --ref, reports the character error rate against the reference text.
import math, random, sys

FS = 12000
TICK_MS = 5
DEBOUNCE = 2
SHORT_SHARE = 0.4           # share of short samples that proves the estimate wrong
DEBOUNCE_DIT = 0.4          # ignore marks/spaces shorter than this many dits
DEBOUNCE_MAX = 3            # ...but never more than 15 ms: a wrong (too slow) speed
                            # estimate must not hide the real elements
MIN_CONTRAST = 5.0
TICK = FS * TICK_MS // 1000
TPS = 1000.0 / TICK_MS
WARMUP = int(0.5 * TPS)
DIT_MIN = 20.0 / TICK_MS
DIT_MAX = 240.0 / TICK_MS

MORSE = {'A': '.-', 'B': '-...', 'C': '-.-.', 'D': '-..', 'E': '.', 'F': '..-.', 'G': '--.',
         'H': '....', 'I': '..', 'J': '.---', 'K': '-.-', 'L': '.-..', 'M': '--', 'N': '-.',
         'O': '---', 'P': '.--.', 'Q': '--.-', 'R': '.-.', 'S': '...', 'T': '-', 'U': '..-',
         'V': '...-', 'W': '.--', 'X': '-..-', 'Y': '-.--', 'Z': '--..',
         '0': '-----', '1': '.----', '2': '..---', '3': '...--', '4': '....-', '5': '.....',
         '6': '-....', '7': '--...', '8': '---..', '9': '----.', '/': '-..-.', '?': '..--..',
         '.': '.-.-.-', ',': '--..--', '=': '-...-', '+': '.-.-.', '-': '-....-', '@': '.--.-.',
         ':': '---...', '(': '-.--.', ')': '-.--.-', "'": '.----.', '"': '.-..-.', '*': '...-.-'}
REV = {v: k for k, v in MORSE.items()}


HIST = int(1.5 * TPS)          # envelope history for the percentiles
TOP_P = 0.95
LEVEL_EVERY = 20               # ticks between percentile updates
WIN_MAX = 4                    # longest integration window, in ticks (20 ms)
WIN_DIV = 3.0


class Decoder:
    def __init__(self, tone):
        self.inc = 2 * math.pi * tone / FS
        self.phase = 0.0
        self.ai = self.aq = 0.0
        self.n = 0
        self.ring_i = [0.0] * WIN_MAX
        self.ring_q = [0.0] * WIN_MAX
        self.ring_pos = 0
        self.hist = []
        self.floor = self.top = 0.0
        self.ticks = 0
        self.key = False
        self.pending = 0
        self.run = 0
        self.carrier = False
        self.dit = 60.0 / TICK_MS
        self.dah = 3 * self.dit
        self.short_share = 0.0
        self.sym = ''
        self.gap_sent = True
        self.text = ''

    def flush(self):
        if not self.sym:
            return
        self.text += REV.get(self.sym, '_')
        self.sym = ''
        self.gap_sent = False

    def end_mark(self, t):
        if self.carrier:
            self.carrier = False
            self.sym = ''
            return
        # Two clusters, split at their geometric mean: a dah never drags the
        # dit estimate (a single speed estimate ran away to 6 WPM on real audio:
        # once the dit guess rose, 120 ms dahs counted as dits and pushed it up).
        dah = t * t >= self.dit * self.dah
        self.sym += '-' if dah else '.'
        if t > 2.0 * self.dah:
            return                      # merged elements: don't learn from it
        if dah:
            self.dah = self.learn(self.dah, t)
        else:
            self.dit = self.learn(self.dit, t)
        self.keep_ratio()

    def learn(self, est, t):
        # A sample under half the estimate is a glitch (noise burst) when such
        # samples are rare: ignore it, or a stream of blips walks the dit
        # estimate down to the 60 WPM floor. When they become common the
        # estimate itself is too slow: then they count, clamped.
        short = t < 0.5 * est
        self.short_share += ((1.0 if short else 0.0) - self.short_share) * 0.1
        if short:
            if self.short_share < SHORT_SHARE:
                return est
            t = 0.5 * est
        t = min(t, 2.0 * est)
        return est + 0.25 * (t - est)

    def keep_ratio(self):
        # Morse dahs are ~3 dits; keep the clusters between 2x and 4x apart.
        self.dit = min(max(self.dit, DIT_MIN), DIT_MAX)
        self.dah = min(max(self.dah, 2.0 * self.dit), 4.0 * self.dit)

    def end_space(self, t):
        # Gaps inside a character last one dit: the most common interval.
        if t * t < self.dit * self.dah:
            self.dit = self.learn(self.dit, t)
            self.keep_ratio()

    def space_tick(self, t):
        if self.sym and t >= 2.0 * self.dit:
            self.flush()
        if not self.gap_sent and t >= 5.0 * self.dit:
            self.text += ' '
            self.gap_sent = True

    def env(self, mag):
        self.ticks += 1
        self.hist.append(mag)
        if len(self.hist) > HIST:
            self.hist.pop(0)
        usable_now = self.top > self.floor * MIN_CONTRAST
        every = LEVEL_EVERY if usable_now else 4
        if self.ticks % every == 0 or self.ticks == WARMUP:
            srt = sorted(self.hist)
            self.floor = srt[int(0.2 * (len(srt) - 1))]
            self.top = srt[int(TOP_P * (len(srt) - 1))]
        if self.ticks < WARMUP:
            return
        usable = self.top > self.floor * MIN_CONTRAST
        span = self.top - self.floor
        if not usable:
            raw = False
        elif self.key:
            raw = mag > self.floor + 0.35 * span
        else:
            raw = mag > self.floor + 0.5 * span
        if raw != self.key:
            self.pending += 1
            if self.pending < min(max(DEBOUNCE, int(round(DEBOUNCE_DIT * self.dit))), DEBOUNCE_MAX):
                self.run += 1
                if not self.key:
                    self.space_tick(self.run)
                return
            finished = self.run - (self.pending - 1)
            if self.key:
                self.end_mark(finished)
            else:
                self.end_space(finished)
            self.key = raw
            self.run = self.pending
            self.pending = 0
        else:
            self.pending = 0
            self.run += 1
        if self.key:
            if self.run > 10.0 * self.dit:
                self.carrier = True
        else:
            self.space_tick(self.run)

    def process(self, x):
        for s in x:
            self.ai += s * math.cos(self.phase)
            self.aq += s * math.sin(self.phase)
            self.phase += self.inc
            if self.phase > 2 * math.pi:
                self.phase -= 2 * math.pi
            self.n += 1
            if self.n < TICK:
                continue
            self.ring_i[self.ring_pos] = self.ai
            self.ring_q[self.ring_pos] = self.aq
            self.ring_pos = (self.ring_pos + 1) % WIN_MAX
            self.ai = self.aq = 0.0
            self.n = 0
            # Integrate over ~half a dit: narrower bandwidth for slower CW.
            w = min(max(int(round(self.dit / WIN_DIV)), 2), WIN_MAX)
            wi = wq = 0.0
            for k in range(1, w + 1):
                wi += self.ring_i[(self.ring_pos - k) % WIN_MAX]
                wq += self.ring_q[(self.ring_pos - k) % WIN_MAX]
            self.env(math.hypot(wi, wq) / (w * TICK))


def keying(text, wpm, jitter=0.0):
    dit = 1.2 / wpm
    seq = [(False, 0.5)]
    for w, word in enumerate(text.split(' ')):
        if w:
            seq.append((False, 7 * dit))
        for c, ch in enumerate(word):
            if c:
                seq.append((False, 3 * dit))
            for e, el in enumerate(MORSE[ch]):
                if e:
                    seq.append((False, dit))
                seq.append((True, (1 if el == '.' else 3) * dit))
    seq.append((False, 1.0))
    return [(on, d * (1 + random.uniform(-jitter, jitter))) for on, d in seq]


def add_blips(seq, per_s, ms=10.0):
    """Random short false toggles: dropouts inside marks, blips inside spaces."""
    out = []
    for on, dur in seq:
        t = 0.0
        while True:
            gap = random.expovariate(per_s)
            if t + gap + ms / 1000 >= dur:
                out.append((on, dur - t))
                break
            out.append((on, gap))
            out.append((not on, ms / 1000))
            t += gap + ms / 1000
    return out


def synth(seq, tone, amp, noise_rms, detune=0.0, rise_ms=4.0):
    out = []
    ph = 0.0
    w = 2 * math.pi * (tone + detune) / FS
    env = 0.0
    step = 1.0 / (rise_ms * FS / 1000.0)
    for on, dur in seq:
        for _ in range(int(dur * FS)):
            env = min(1.0, env + step) if on else max(0.0, env - step)
            out.append(amp * env * math.sin(ph) + random.gauss(0, noise_rms))
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


def run_file(argv):
    import miniaudio, re
    args = dict(zip(argv[0::2], argv[1::2]))
    audio = miniaudio.decode_file(args['--file'], output_format=miniaudio.SampleFormat.FLOAT32,
                                  nchannels=1, sample_rate=FS)
    x = audio.samples
    if '--seconds' in args:
        x = x[:int(float(args['--seconds']) * FS)]
    d = Decoder(float(args.get('--tone', 750)))
    d.process(x)
    d.flush()
    out = ' '.join(d.text.split())
    print(f'{len(x) / FS:.0f} s, dit final {d.dit * TICK_MS:.0f} ms ({1200 / (d.dit * TICK_MS):.1f} wpm)')
    print(out)
    if '--ref' in args:
        ref = open(args['--ref'], encoding='utf-8', errors='ignore').read().upper()
        ref = ' '.join(re.sub(r"[^A-Z0-9.,?/=+@:() -]", ' ', ref).split())
        # the recording may cover only part of the reference: align on the best window
        n = len(out)
        best = None
        for start in range(0, max(1, len(ref) - n + 1), max(1, n // 20)):
            e = edits(out, ref[start:start + n])
            if best is None or e < best[0]:
                best = (e, start)
        print(f'CER vs referencia: {100.0 * best[0] / max(1, n):.1f} %  ({best[0]} edicoes em {n} caracteres)')


if len(sys.argv) > 1 and sys.argv[1] == '--file':
    run_file(sys.argv[1:])
    sys.exit(0)

import sys
MIN_CONTRAST = float(sys.argv[1]) if len(sys.argv) > 1 else MIN_CONTRAST
WIN_DIV = float(sys.argv[2]) if len(sys.argv) > 2 else WIN_DIV
MSG = 'CQ CQ DE CT1ABC K'
cases = [
    ('20 wpm limpo', 20, 0.0, 0.0, 'clean'),
    ('12 wpm limpo', 12, 0.0, 0.0, 'clean'),
    ('30 wpm limpo', 30, 0.0, 0.0, 'clean'),
    ('20 wpm SNR 10 dB/100 Hz', 20, 0.0, 0.0, 10),
    ('20 wpm SNR 6 dB/100 Hz', 20, 0.0, 0.0, 6),
    ('20 wpm jitter 15%', 20, 0.0, 0.15, 'clean'),
    ('20 wpm desvio 8 Hz', 20, 8.0, 0.0, 'clean'),
    ('20 wpm desvio 8 Hz SNR 10', 20, 8.0, 0.0, 10),
    ('25 wpm SNR 15 dB + jitter', 25, 0.0, 0.1, 15),
    ('30 wpm arranque a 10 wpm', 30, 0.0, 0.05, 'slowstart'),
    ('30 wpm impulsos 10 ms 2/s', 30, 0.0, 0.05, 'blips'),
    ('20 wpm impulsos 10 ms 2/s', 20, 0.0, 0.05, 'blips'),
    ('20 wpm impulsos 10 ms 6/s', 20, 0.0, 0.05, 'blips6'),
    ('30 wpm impulsos 10 ms 6/s', 30, 0.0, 0.05, 'blips6'),
]
amp = 0.1
def noise_rms_for(noise):
    if noise in ('clean', 'blips', 'blips6', 'slowstart'):
        return amp * 0.003
    # tone power amp^2/2 vs noise power in 100 Hz out of 6 kHz
    return math.sqrt(amp * amp / 2 / 10 ** (noise / 10) * 6000 / 100)

SEEDS = 5
print(f'MIN_CONTRAST = {MIN_CONTRAST}')
bad = 0
for seed in range(SEEDS):
    random.seed(100 + seed)
    d = Decoder(700.0)
    d.process([random.gauss(0, noise_rms_for(10)) for _ in range(10 * FS)])
    d.flush()
    bad += bool(d.text.strip())
print(f'{"so ruido 10 s":28s} {SEEDS - bad}/{SEEDS} sem texto')
TAIL = MSG.split(' ', 1)[1]        # the first word may be lost while locking
print(f'{"caso":28s} {"exato":>5s}  {"CER apos 1a palavra":>20s}  exemplo')
for name, wpm, detune, jitter, noise in cases:
    ok = 0
    errs = 0
    example = ''
    for seed in range(SEEDS):
        random.seed(seed)
        d = Decoder(700.0)
        if noise == 'slowstart':
            d.dit, d.dah = 120.0 / TICK_MS, 360.0 / TICK_MS
        seq = keying(MSG, wpm, jitter)
        if noise == 'blips':
            seq = add_blips(seq, 2.0)
        elif noise == 'blips6':
            seq = add_blips(seq, 6.0)
        d.process(synth(seq, 700.0, amp, noise_rms_for(noise), detune))
        d.flush()
        out = d.text.strip()
        tail = out.split(' ', 1)[1] if ' ' in out else ''
        errs += edits(tail, TAIL)
        if out == MSG:
            ok += 1
        elif not example:
            example = out
    print(f'{name:28s} {ok}/{SEEDS}    {100.0 * errs / (SEEDS * len(TAIL)):17.1f} %  {example}')
