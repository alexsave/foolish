#!/usr/bin/env python3
"""Plot nitro_progress.csv: row on X axis, three lines.

Primary Y axis (0..1): win rate vs random and vs espresso.
Secondary Y axis (log scale): training_range_size — grows fast.

Usage: python3 plot_progress.py [csv_path] [out_png]
"""
import csv
import os
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt


def main() -> None:
    csv_path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.path.dirname(__file__), "nitro_progress.csv"
    )
    out_png = sys.argv[2] if len(sys.argv) > 2 else os.path.join(
        os.path.dirname(__file__), "nitro_progress.png"
    )

    rows = []
    with open(csv_path, newline="") as f:
        reader = csv.DictReader(f)
        for r in reader:
            rows.append(r)

    xs = [int(r["row"]) for r in rows]
    train = [int(r["training_range_size"]) for r in rows]
    rand = [float(r["win_rate_eval_random_10k"]) for r in rows]
    esp = [float(r["win_rate_eval_espresso_10k"]) for r in rows]

    fig, ax1 = plt.subplots(figsize=(10, 6))
    ax1.set_xlabel("row (each frontier advance)")
    ax1.set_ylabel("eval win rate (1v1, 10k seeds)")
    ax1.set_ylim(0.0, 1.0)
    ax1.plot(xs, rand, marker="o", label="vs random (eval)")
    ax1.plot(xs, esp, marker="s", label="vs espresso (eval)")
    ax1.grid(True, alpha=0.3)
    ax1.legend(loc="upper left")

    ax2 = ax1.twinx()
    ax2.set_ylabel("training frontier (seeds 1..N passing) — log scale")
    ax2.set_yscale("log")
    # Avoid log(0) when starting baseline — clamp lower bound to 1.
    safe_train = [max(t, 1) for t in train]
    ax2.plot(xs, safe_train, marker="^", color="tab:green", label="frontier")
    ax2.legend(loc="upper right")

    plt.title("Nitro progress")
    fig.tight_layout()
    plt.savefig(out_png, dpi=120)
    print(f"wrote {out_png}")


if __name__ == "__main__":
    main()
