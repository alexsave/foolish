#!/usr/bin/env python3
"""Train the torpex value net on cnitro_gen records.

Input: 72-byte records (see src/main_gen.c). Features are expanded on the
fly per batch (7 x 52-bit masks unpacked + 14 scalars = 378 dims). Target:
the seat's final finish position, normalized to [0,1] (0 = first out /
winner, 1 = durak).

Architecture: 378 -> 256 (ReLU) -> 64 (ReLU) -> 1 (sigmoid). ~123k params.
Trained with Adam + MSE, pure numpy (CPU). Exports weights as a flat
float32 binary consumed by src/torpex_value.c:
  [W1 (378*256) b1(256) W2 (256*64) b2(64) W3 (64) b3(1)]  row-major,
  preceded by a 16-byte header: magic 'TPX1', i32 in_dim, i32 h1, i32 h2.

Usage:
  python3 train_torpex.py --data f1.bin f2.bin --out torpex_weights.bin \
      --epochs 3 --batch 4096 --lr 1e-3 [--val-frac 0.05]
"""
import argparse
import numpy as np
import sys
import time

REC = 72
IN_DIM = 7 * 52 + 14
H1, H2 = 256, 64


def load(paths):
    # Truncate each file to a whole number of records BEFORE concatenating:
    # a killed generator can leave a partial trailing record, and one short
    # file misaligns every byte after it in the concatenated stream.
    bufs = []
    for p in paths:
        b = np.fromfile(p, dtype=np.uint8)
        n = len(b) // REC
        if len(b) != n * REC:
            print(f"# {p}: trimming {len(b) - n * REC} trailing bytes")
        bufs.append(b[: n * REC])
    raw = np.concatenate(bufs)
    n = len(raw) // REC
    raw = raw.reshape(n, REC)
    print(f"# loaded {n} records")
    return raw


def expand(raw):
    """raw (B,72) uint8 -> X (B,IN_DIM) float32, y (B,) float32"""
    B = raw.shape[0]
    masks = raw[:, :56].reshape(B, 7, 8)
    # 52-bit masks stored little-endian; unpackbits per byte (LSB order)
    bits = np.unpackbits(masks, axis=2, bitorder="little")  # (B,7,64)
    bits = bits[:, :, :52].reshape(B, 7 * 52).astype(np.float32)
    s = raw[:, 56:].astype(np.float32)
    np_players = s[:, 0]
    scal = np.stack([
        np_players / 8.0,
        s[:, 1] / 8.0,                     # in_count
        s[:, 2] / 52.0,                    # deck_count
        s[:, 3] / 52.0,                    # discard
        s[:, 4],                           # has_flipped
        np.where(s[:, 5] < 255, (s[:, 5] % 13) / 13.0, 0.0),  # flipped value
        np.where(s[:, 5] < 255, (s[:, 5] < 13).astype(np.float32), 0.0),  # flipped is trump
        s[:, 6],                           # is_defender
        s[:, 7] / 8.0,                     # def_rel
        s[:, 8] / 8.0,                     # fa_rel
        s[:, 9] / 18.0,                    # my_cnt
        s[:, 10] / 18.0, s[:, 11] / 18.0, s[:, 12] / 18.0,  # opp counts
    ], axis=1).astype(np.float32)
    X = np.concatenate([bits, scal], axis=1)
    tgt = s[:, 14]
    y = (tgt - 1.0) / np.maximum(np_players - 1.0, 1.0)
    return X, y.astype(np.float32)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", nargs="+", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--epochs", type=int, default=3)
    ap.add_argument("--batch", type=int, default=4096)
    ap.add_argument("--lr", type=float, default=1e-3)
    ap.add_argument("--val-frac", type=float, default=0.05)
    ap.add_argument("--seed", type=int, default=7)
    args = ap.parse_args()

    rng = np.random.default_rng(args.seed)
    raw = load(args.data)
    n = raw.shape[0]
    perm = rng.permutation(n)
    raw = raw[perm]
    nval = int(n * args.val_frac)
    val_raw, trn_raw = raw[:nval], raw[nval:]
    Xv, yv = expand(val_raw)

    def init(fan_in, fan_out):
        return (rng.standard_normal((fan_in, fan_out)) *
                np.sqrt(2.0 / fan_in)).astype(np.float32)

    W1, b1 = init(IN_DIM, H1), np.zeros(H1, np.float32)
    W2, b2 = init(H1, H2), np.zeros(H2, np.float32)
    W3, b3 = init(H2, 1), np.zeros(1, np.float32)
    params = [W1, b1, W2, b2, W3, b3]
    m = [np.zeros_like(p) for p in params]
    v = [np.zeros_like(p) for p in params]
    beta1, beta2, eps = 0.9, 0.999, 1e-8
    t = 0

    def fwd(X):
        h1 = np.maximum(X @ W1 + b1, 0)
        h2 = np.maximum(h1 @ W2 + b2, 0)
        z = h2 @ W3 + b3
        p = 1.0 / (1.0 + np.exp(-z))
        return h1, h2, p.reshape(-1)

    def val_mse():
        preds = []
        for i in range(0, len(Xv), 16384):
            preds.append(fwd(Xv[i:i+16384])[2])
        p = np.concatenate(preds)
        return float(np.mean((p - yv) ** 2)), float(np.mean(np.abs(p - yv)))

    base = float(np.mean((yv - np.mean(yv)) ** 2))
    print(f"# train={len(trn_raw)} val={nval} baseline-mse={base:.4f}")

    steps_per_epoch = len(trn_raw) // args.batch
    t0 = time.time()
    for ep in range(args.epochs):
        order = rng.permutation(len(trn_raw))
        run = 0.0
        for si in range(steps_per_epoch):
            idx = order[si*args.batch:(si+1)*args.batch]
            X, y = expand(trn_raw[idx])
            h1, h2, p = fwd(X)
            d = (p - y) * p * (1 - p)          # MSE + sigmoid
            d = (2.0 / len(y)) * d
            gW3 = h2.T @ d[:, None]; gb3 = d.sum(keepdims=True)
            dh2 = np.outer(d, W3.reshape(-1)) * (h2 > 0)
            gW2 = h1.T @ dh2; gb2 = dh2.sum(0)
            dh1 = dh2 @ W2.T * (h1 > 0)
            gW1 = X.T @ dh1; gb1 = dh1.sum(0)
            grads = [gW1, gb1, gW2, gb2, gW3, gb3]
            t += 1
            for j, (pp, g) in enumerate(zip(params, grads)):
                m[j] = beta1 * m[j] + (1 - beta1) * g
                v[j] = beta2 * v[j] + (1 - beta2) * g * g
                mh = m[j] / (1 - beta1 ** t)
                vh = v[j] / (1 - beta2 ** t)
                pp -= args.lr * mh / (np.sqrt(vh) + eps)
            run += float(np.mean((p - y) ** 2))
            if (si + 1) % 100 == 0:
                mse, mae = val_mse()
                print(f"ep{ep} step {si+1}/{steps_per_epoch} "
                      f"train-mse={run/100:.4f} val-mse={mse:.4f} val-mae={mae:.4f} "
                      f"({time.time()-t0:.0f}s)", flush=True)
                run = 0.0
        mse, mae = val_mse()
        print(f"# epoch {ep} done: val-mse={mse:.4f} val-mae={mae:.4f}")

    with open(args.out, "wb") as f:
        f.write(b"TPX1")
        np.array([IN_DIM, H1, H2], dtype=np.int32).tofile(f)
        for p in params:
            p.astype(np.float32).tofile(f)
    print(f"# wrote {args.out}")


if __name__ == "__main__":
    sys.exit(main())
