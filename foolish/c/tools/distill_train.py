#!/usr/bin/env python3
"""Train the distilled linear ranker from cnitro_distill CSV dumps.

Bradley-Terry pairwise imitation: for every decision, the chosen move's
feature vector minus each rejected candidate's is a positive example (and the
reverse pair a negative one, for balance). A no-intercept L2 logistic
regression on those differences IS a linear ranking function w.x; argmax over
candidates replays the ranker at decision level.

Splits are BY GAME (game_seed within each input file), never by row, so
candidates of one decision (and decisions of one game) can't leak across the
train/val/test boundary. C is chosen on the validation games; all reported
match rates are on the untouched test games.

Usage:
  python3 tools/distill_train.py dumps/pc2.csv dumps/pc4.csv dumps/pc6.csv \
      [--out src/distilled_weights.h]

Emits the weight header for distilled_strategy.c and prints held-out
move-match % (overall / per player count / per move type) plus the held-out
top1-top2 margin distribution used to pick DL_TAU gate values.
"""

import argparse
import hashlib
import os
import sys

import numpy as np
from sklearn.linear_model import LogisticRegression

NUM_F = None  # inferred from the CSV header (must match DISTILL_NUM_FEATURES)
# Feature indices (see distill_feat.c layout).
F_NUM_PLAYERS = 0
F_TYPE_BASE = 15  # attack, cover, pass, pickup, good
TYPE_NAMES = ["attack", "cover", "pass", "pickup", "good", "other"]


def split_of(fname, seed):
    """Deterministic 80/10/10 game split by (file, seed) hash."""
    h = hashlib.md5(f"{os.path.basename(fname)}:{seed}".encode()).digest()
    r = h[0] / 255.0
    return "train" if r < 0.8 else ("val" if r < 0.9 else "test")


def load(paths):
    """Returns list of decisions: (file, seed, X[n,NUM_F], chosen_idx)."""
    global NUM_F
    decisions = []
    for path in paths:
        with open(path) as fh:
            header = fh.readline().strip().split(",")
            assert header[:4] == ["game_seed", "decision_id", "candidate_index", "chosen"], path
            if NUM_F is None:
                NUM_F = len(header) - 4
            assert len(header) == 4 + NUM_F, f"{path}: {len(header)} cols, want {4 + NUM_F}"
            cur_key, rows, chosen = None, [], -1
            for line in fh:
                p = line.rstrip("\n").split(",")
                key = (p[0], p[1])
                if key != cur_key:
                    if cur_key is not None:
                        decisions.append((path, int(cur_key[0]), np.array(rows), chosen))
                    cur_key, rows, chosen = key, [], -1
                if p[3] == "1":
                    chosen = len(rows)
                rows.append([float(x) for x in p[4:]])
            if cur_key is not None:
                decisions.append((path, int(cur_key[0]), np.array(rows), chosen))
    return [d for d in decisions if d[3] >= 0 and len(d[2]) >= 2]


def pairs(decisions):
    """Stacked (x_chosen - x_other) diffs with label 1, plus reversed label-0."""
    diffs = []
    for _, _, X, c in decisions:
        d = X[c][None, :] - np.delete(X, c, axis=0)
        diffs.append(d)
    D = np.vstack(diffs)
    X = np.vstack([D, -D])
    y = np.concatenate([np.ones(len(D)), np.zeros(len(D))])
    return X, y


def match_stats(decisions, w):
    """Decision-level argmax match plus margins; returns (hits, details)."""
    per_pc, per_type = {}, {}
    margins, hits = [], 0
    for _, _, X, c in decisions:
        s = X @ w
        top = int(np.argmax(s))
        ss = np.sort(s)
        margins.append(ss[-1] - ss[-2])
        ok = top == c
        hits += ok
        pc = int(X[0, F_NUM_PLAYERS])
        per_pc.setdefault(pc, [0, 0])
        per_pc[pc][0] += ok
        per_pc[pc][1] += 1
        onehots = X[c, F_TYPE_BASE:F_TYPE_BASE + 5]
        t = TYPE_NAMES[int(np.argmax(onehots))] if onehots.max() > 0 else "other"
        per_type.setdefault(t, [0, 0])
        per_type[t][0] += ok
        per_type[t][1] += 1
    return hits, per_pc, per_type, np.array(margins)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csvs", nargs="+")
    ap.add_argument("--out", default=os.path.join(os.path.dirname(__file__),
                                                  "..", "src", "distilled_weights.h"))
    args = ap.parse_args()

    decisions = load(args.csvs)
    print(f"loaded {len(decisions)} decisions "
          f"({sum(len(d[2]) for d in decisions)} candidate rows)")

    tr = [d for d in decisions if split_of(d[0], d[1]) == "train"]
    va = [d for d in decisions if split_of(d[0], d[1]) == "val"]
    te = [d for d in decisions if split_of(d[0], d[1]) == "test"]
    print(f"split by game: train={len(tr)} val={len(va)} test={len(te)} decisions")

    Xtr, ytr = pairs(tr)
    print(f"pairwise training rows: {len(Xtr)}")

    best_w, best_va, best_c = None, -1.0, None
    for C in (0.1, 1.0, 10.0):
        lr = LogisticRegression(fit_intercept=False, C=C, penalty="l2",
                                solver="lbfgs", max_iter=2000)
        lr.fit(Xtr, ytr)
        w = lr.coef_[0]
        hits, _, _, _ = match_stats(va, w)
        acc = hits / len(va)
        print(f"  C={C:<4} val decision match = {acc * 100:.2f}%")
        if acc > best_va:
            best_w, best_va, best_c = w, acc, C

    w = best_w
    hits, per_pc, per_type, margins = match_stats(te, w)
    print(f"\nchosen C={best_c}")
    print(f"TEST decision match = {hits / len(te) * 100:.2f}%  ({hits}/{len(te)})")
    for pc in sorted(per_pc):
        h, n = per_pc[pc]
        print(f"  pc={pc}: {h / n * 100:.2f}%  ({h}/{n})")
    for t in TYPE_NAMES:
        if t in per_type:
            h, n = per_type[t]
            print(f"  type={t:<7}: {h / n * 100:.2f}%  ({h}/{n})")

    # Gate guidance: DL_TAU is hundredths of a logit; the tau at which X% of
    # held-out decisions would defer is the X-th percentile of the margin.
    print("\nheld-out top1-top2 margin percentiles (DL_TAU units = 100*logit):")
    for q in (5, 10, 25, 50, 75):
        print(f"  defer {q:>2}% of decisions: DL_TAU ~ {np.percentile(margins, q) * 100:.0f}")

    out = os.path.normpath(args.out)
    with open(out, "w") as fh:
        fh.write(
            "// Distilled linear-policy weights. GENERATED by "
            "c/tools/distill_train.py\n"
            f"// from {len(decisions)} cordite(prod) self-play decisions "
            f"({', '.join(os.path.basename(p) for p in args.csvs)});\n"
            f"// C={best_c}, held-out decision match {hits / len(te) * 100:.2f}%. "
            "Do not edit by hand.\n"
            "#ifndef CNITRO_DISTILLED_WEIGHTS_H\n"
            "#define CNITRO_DISTILLED_WEIGHTS_H\n\n"
            f"#define DISTILLED_W_FEATURES {NUM_F}\n\n"
            "static const double DISTILLED_W[DISTILLED_W_FEATURES] = {\n")
        for i in range(0, NUM_F, 4):
            chunk = ", ".join(f"{v:.17g}" for v in w[i:i + 4])
            fh.write(f"    {chunk},\n")
        fh.write("};\n\n#endif\n")
    print(f"\nwrote {out}")


if __name__ == "__main__":
    main()
