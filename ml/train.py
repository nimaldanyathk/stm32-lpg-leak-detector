#!/usr/bin/env python3
"""
train.py - trains the gas classifier and writes firmware/model.h
Copyright (c) 2026 Nimal Danyath K (nimaldanyathk). All rights reserved.

Usage:
    pip install -r requirements.txt
    python train.py logs          (folder of saved Bluetooth logs)
    python train.py a.txt b.txt   (or individual files)

Reads the B (baseline) and D (data) lines the board sends in log mode,
ignores everything else. Feature maths is copied from features.h, so
keep the two in sync if you change either.
"""
import sys
import os
import glob
import datetime

import numpy as np
from sklearn.tree import DecisionTreeClassifier
from sklearn.model_selection import GroupShuffleSplit
from sklearn.metrics import confusion_matrix, accuracy_score

# ---------------- must match features.h exactly ----------------
SENSOR_VC = 5.0
ADC_VREF = 3.3
ADC_FULL = 4095.0
DIVIDER_GAIN = 2.0
LAG = 30                      # 3 s at 10 Hz
SAMPLE_MS = 100
GAP_MS = 150                  # bigger jump between rows = data was lost

CLASSES = ["CLEAN AIR", "LPG", "SMOKE", "STEAM", "ALCOHOL/VOC"]
FEATURES = ["r6", "r2", "r135", "r6/r135", "r2/r6", "r2/r135", "d6", "d2",
            "drh", "rh_rise"]

MAX_DEPTH = 5                 # small tree: tiny, fast, hard to overfit
MIN_LEAF = 20                 # each leaf needs 2 s of data behind it


def f32(x):
    """Round to 32-bit float, like the STM32 FPU."""
    return float(np.float32(x))


# constants rounded exactly the way the C compiler rounds the float literals
K_VOLT = f32(f32(ADC_VREF) / f32(ADC_FULL))
V_MIN = f32(0.01)
V_MAX = f32(f32(SENSOR_VC) - f32(0.01))


def rs_from_raw(raw):
    v = f32(f32(f32(raw) * K_VOLT) * f32(DIVIDER_GAIN))
    if v < V_MIN:
        v = V_MIN
    if v > V_MAX:
        v = V_MAX
    return f32(f32(f32(SENSOR_VC) - v) / v)


def compute_features(rows, base):
    """rows: list of (ms, run, label, raw6, raw2, raw135, rh_c, rhb_c),
    contiguous. Returns (features, label, run) for rows with a full window."""
    rs0 = [rs_from_raw(b) for b in base]
    hist6, hist2, histrh = [], [], []
    out = []
    for ms, run, label, a6, a2, a135, rh_c, rhb_c in rows:
        r6 = f32(rs_from_raw(a6) / rs0[0])
        r2 = f32(rs_from_raw(a2) / rs0[1])
        r135 = f32(rs_from_raw(a135) / rs0[2])
        if len(hist6) >= LAG:
            d6 = f32(r6 - hist6[-LAG])
            d2 = f32(r2 - hist2[-LAG])
            drh = f32(f32(rh_c - histrh[-LAG]) / f32(100.0))
            full = True
        else:
            d6 = d2 = drh = 0.0
            full = False
        hist6.append(r6)
        hist2.append(r2)
        histrh.append(rh_c)
        feats = [r6, r2, r135,
                 f32(r6 / r135), f32(r2 / r6), f32(r2 / r135),
                 d6, d2,
                 drh, f32(f32(rh_c - rhb_c) / f32(100.0))]
        if full and label >= 0:
            out.append((feats, label, run))
    return out


def parse_file(path, file_index):
    """Split a log into contiguous segments and compute features."""
    samples = []
    base = None
    segment = []
    last_ms = None

    def flush():
        if segment and base is not None:
            samples.extend(compute_features(segment, base))
        segment.clear()

    with open(path, "r", errors="ignore") as fh:
        for line in fh:
            line = line.strip()
            parts = line.split(",")
            if parts[0].endswith("B") and len(parts) == 4:
                try:
                    nb = [int(p) for p in parts[1:]]
                except ValueError:
                    continue
                flush()
                base = nb
                last_ms = None
            elif parts[0].endswith("D") and len(parts) == 10:
                try:
                    ms, run, label, a6, a2, a135, rh_c, _t_c, rhb_c = (int(p) for p in parts[1:])
                except ValueError:
                    continue
                if last_ms is not None and not (0 < ms - last_ms <= GAP_MS):
                    flush()                      # lost data: restart window
                last_ms = ms
                # run ids restart on every power cycle, so make them unique
                segment.append((ms, file_index * 100000 + run, label,
                                a6, a2, a135, rh_c, rhb_c))
    flush()
    return samples


def export_c(tree, path, report):
    t = tree.tree_
    classes = [int(c) for c in tree.classes_]
    lines = []

    def rec(node, depth):
        ind = "    " * depth
        if t.children_left[node] == -1:
            cls = classes[int(np.argmax(t.value[node][0]))]
            lines.append(f"{ind}return {cls};{' ' * 4}/* {CLASSES[cls]} */")
            return
        fi = int(t.feature[node])
        thr = float(np.float32(t.threshold[node]))
        lines.append(f"{ind}if (f[{fi}] <= {thr:.9g}f) {{    /* {FEATURES[fi]} */")
        rec(t.children_left[node], depth + 1)
        lines.append(f"{ind}}} else {{")
        rec(t.children_right[node], depth + 1)
        lines.append(f"{ind}}}")

    rec(0, 1)
    stamp = datetime.datetime.now().strftime("%Y-%m-%d %H:%M")
    body = "\n".join(lines)
    text = f"""/* =====================================================================
   model.h  -  TRAINED decision tree, generated by train.py on {stamp}
   Copyright (c) 2026 Nimal Danyath K (nimaldanyathk). All rights reserved.
   Do not edit by hand. Re-run train.py to regenerate.

{report}
   Classes:  0 CLEAN AIR   1 LPG   2 SMOKE   3 STEAM   4 ALCOHOL/VOC
   Features: see features.h
   ===================================================================== */
#ifndef MODEL_H
#define MODEL_H

#include <stdint.h>

#define MODEL_IS_TRAINED  1

static uint8_t model_classify(const float *f)
{{
{body}
}}

#endif /* MODEL_H */
"""
    with open(path, "w") as fh:
        fh.write(text)


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        sys.exit(1)

    files = []
    for a in args:
        if os.path.isdir(a):
            files += sorted(glob.glob(os.path.join(a, "*.txt")))
            files += sorted(glob.glob(os.path.join(a, "*.csv")))
        else:
            files += sorted(glob.glob(a)) or [a]

    samples = []
    for i, p in enumerate(files):
        got = parse_file(p, i + 1)
        print(f"{p}: {len(got)} usable samples")
        samples += got
    if not samples:
        print("No labelled samples found. Did you set a label (0-4) while logging?")
        sys.exit(1)

    X = np.array([s[0] for s in samples], dtype=np.float32)
    y = np.array([s[1] for s in samples], dtype=np.int32)
    g = np.array([s[2] for s in samples], dtype=np.int64)

    print("\nSAMPLES PER CLASS (runs in brackets)")
    for c in range(len(CLASSES)):
        n = int((y == c).sum())
        runs = len(set(g[y == c].tolist()))
        warn = "   <-- need more runs" if 0 < runs < 5 else ("   <-- MISSING" if n == 0 else "")
        print(f"  {c} {CLASSES[c]:<12} {n:>7}  ({runs} runs){warn}")

    params = dict(max_depth=MAX_DEPTH, min_samples_leaf=MIN_LEAF,
                  class_weight="balanced", random_state=0)

    # Test on whole recording runs the model never saw. Splitting individual
    # rows instead would leak near-identical neighbours into the test set.
    report = ""
    if len(set(g.tolist())) >= 4:
        split = GroupShuffleSplit(n_splits=1, test_size=0.25, random_state=0)
        tr, te = next(split.split(X, y, g))
        clf = DecisionTreeClassifier(**params).fit(X[tr], y[tr])
        pred = clf.predict(X[te])
        acc = accuracy_score(y[te], pred)
        labels = sorted(set(y.tolist()))
        cm = confusion_matrix(y[te], pred, labels=labels)
        print(f"\nTEST ACCURACY on unseen runs: {acc * 100:.1f} %")
        print("CONFUSION MATRIX (rows = true, columns = predicted)")
        print("      " + " ".join(f"{CLASSES[l][:5]:>6}" for l in labels))
        for l, row in zip(labels, cm):
            print(f"{CLASSES[l][:5]:>6}" + " ".join(f"{v:>6}" for v in row))
        report = f"   Test accuracy on unseen runs: {acc * 100:.1f} %\n"
    else:
        print("\nToo few runs for a fair test split, training on everything.")

    final = DecisionTreeClassifier(**params).fit(X, y)
    nodes = final.tree_.node_count
    report += (f"   Training samples: {len(y)}   Tree depth: {final.get_depth()}"
               f"   Nodes: {nodes}   Approx flash: {nodes * 12} bytes")
    here = os.path.dirname(os.path.abspath(__file__))
    fw_dir = os.path.join(here, "..", "firmware")
    out_dir = fw_dir if os.path.isdir(fw_dir) else here
    out_path = os.path.abspath(os.path.join(out_dir, "model.h"))
    export_c(final, out_path, report)
    print(f"\nTree depth {final.get_depth()}, {nodes} nodes, about {nodes * 12} bytes of flash")
    print(f"Wrote {out_path}")
    print("Now rebuild in Keil (F7) and flash.")


if __name__ == "__main__":
    main()
