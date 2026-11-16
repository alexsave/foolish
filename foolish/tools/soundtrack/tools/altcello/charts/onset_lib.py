"""Shared helpers of the onset work (charts/onset_scan.py, onset.py, candidate2.py): render notes with the altcello CLI, the audio time to
periodic by the C measure (onset_tool wav, analysis.h) and the player gates (player_gates.py)."""
import os, subprocess, sys
import numpy as np
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import player_gates as G  # noqa: E402
M = G.M
SR = 48000
ROOT = G.ROOT
EXE = os.path.join(ROOT, "altcello")
TOOL = os.path.join(ROOT, "onset_tool")
TMP = os.path.join(ROOT, "out", "onset")
SC = "/Users/alex/Music/foolish_cities_v5/alt_cello/scores"


def render(score, wav, extra=(), style="expert", room="off", trace=None):
    os.makedirs(os.path.dirname(wav), exist_ok=True)
    cmd = ["nice", EXE, "render", score if os.path.isabs(score) else os.path.join(SC, score), wav, "--style", style, "--room", str(room)] + [str(e) for e in extra]
    if trace: cmd += ["--trace", trace]
    r = subprocess.run(cmd, capture_output=True, text=True, check=True)
    return r.stdout.strip()


def t_periodic(y, f0, t_from=-1.0, tag="x"):
    """audio time to periodic (r > 0.95 for 10 periods) after the onset (the 2 percent rms rule, searched from t_from on), s; -1 never"""
    os.makedirs(TMP, exist_ok=True)
    p = os.path.join(TMP, "tp_%s.f32" % tag)
    np.asarray(y, dtype=np.float32).tofile(p)
    cmd = ["nice", TOOL, "wav", p, str(SR), "%.4f" % f0] + (["--from", "%.4f" % t_from] if t_from >= 0 else [])
    out = subprocess.run(cmd, capture_output=True, text=True, check=True).stdout.split()
    os.remove(p)
    return float(out[1]), float(out[3])  # onset, time to periodic


def note_segment(y, t_start, pre=0.2, post=1.2):
    a = max(int((t_start - pre) * SR), 0)
    return y[a:a + int((pre + post) * SR)], t_start - a / SR
