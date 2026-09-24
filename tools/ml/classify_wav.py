# Runs the trained model (tools/ml/model.npz) over a WAV, one line per ~1 s.
#   python tools/ml/classify_wav.py audio.wav
import os, sys, wave
import numpy as np

sys.path.insert(0, os.path.dirname(__file__))
from features import features, CLASSES, FS, N, FRAMES
from train import forward_q

d = np.load(os.path.join(os.path.dirname(__file__), "model.npz"))
q = [d[f"arr_{i}"] for i in range(9)]
w = wave.open(sys.argv[1])
x = np.frombuffer(w.readframes(w.getnframes()), np.int16).astype(np.float64) / 32768
x = x[::w.getnchannels()]
if w.getframerate() != FS:
    x = np.interp(np.arange(0, len(x), w.getframerate() / FS), np.arange(len(x)), x)
step = N * FRAMES
for i in range(0, len(x) - step, step):
    z = forward_q(q, features(x[i:i + step])[None])[0]
    p = np.exp(z - z.max()); p /= p.sum()
    print(f"{i / FS:6.1f}s  {CLASSES[p.argmax()]:6} {p.max():.2f}")
