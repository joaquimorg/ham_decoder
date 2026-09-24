# Converts monitor logs recorded with ML_LOG_FEATURES 1 into a dataset for
# train.py --extra. Lines look like "ML_F,<label>,<v1>,...". The label is
# ML_LOG_LABEL from config.h, or pass --label to override it for the whole log.
#   python tools/ml/log_to_npz.py logs/x.log --label CW -o real_cw.npz
import argparse, os, sys
import numpy as np

sys.path.insert(0, os.path.dirname(__file__))
from features import CLASSES, N_FEATURES, LOCAL_BANDS, WIDE_BANDS, MOD_BINS

SNR_INDEX = LOCAL_BANDS + WIDE_BANDS + MOD_BINS

ap = argparse.ArgumentParser()
ap.add_argument("logs", nargs="+")
ap.add_argument("--label")
ap.add_argument("--noise-below", type=float, default=0,
                help="label seconds with SNR below this (dB) as RUIDO (pauses)")
ap.add_argument("-o", "--out", required=True)
a = ap.parse_args()

X, y = [], []
for path in a.logs:
    for line in open(path, encoding="utf-8", errors="replace"):
        i = line.find("ML_F,")
        if i < 0:
            continue
        parts = line[i:].strip().split(",")
        label = a.label or parts[1]
        if label not in CLASSES or len(parts) != N_FEATURES + 2:
            continue
        v = [float(x) for x in parts[2:]]
        if v[SNR_INDEX] * 60.0 < a.noise_below:
            label = "RUIDO"
        X.append(v)
        y.append(CLASSES.index(label))
np.savez(a.out, X=np.array(X, np.float32), y=np.array(y, np.int64))
print(f"{len(X)} exemplos -> {a.out}:", {c: y.count(i) for i, c in enumerate(CLASSES)})
