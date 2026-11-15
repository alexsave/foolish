# test_drum_events.py - test of drum_events.py: a synthetic drum stem with known hits, then the mutation check.
#
# What it does: builds a stem of 4 kicks (sine sweep), 4 claps (3 kHz noise) and 4 hats (9 kHz noise) at known times, the
#   second hit of each kind 12 dB quieter, writes it as a wav, runs drum_events.build and asserts: every known hit has an event
#   of its stream within 15 ms, the quiet hit's velocity is under half the loud one's, and no stream has extra events.
#   `python3 -I test_drum_events.py` then runs the mutation (every velocity 1), which must FAIL on the velocity assertion.
# Randomness: noise from numpy's default_rng with the one seed SEED.
import os, sys
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import drum_events as D
import compare as C
from scipy import signal

SEED = 11
SR = 44100
TIMES = {'kick': [1.0, 2.0, 3.0, 4.0], 'clap': [1.5, 2.5, 3.5, 4.5], 'hat': [1.25, 2.25, 3.25, 4.25]}


def stem():
    rng = np.random.default_rng(SEED)
    x = np.zeros(6 * SR)
    t = np.arange(int(0.3 * SR)) / SR
    for name, ts in TIMES.items():
        for i, ti in enumerate(ts):
            amp = 1.0 if i % 2 == 0 else 0.25
            if name == 'kick':
                h = np.sin(2 * np.pi * (50 + 150 * np.exp(-t / 0.02)) * t) * np.exp(-t / 0.1)
            else:
                lo, hi = (2500, 3500) if name == 'clap' else (8000, 10000)
                sos = signal.butter(4, [lo / (SR / 2), hi / (SR / 2)], 'band', output='sos')
                h = signal.sosfilt(sos, rng.standard_normal(len(t))) * np.exp(-t / 0.05) * 4
            a = int(ti * SR); x[a:a + len(t)] += h * amp
    return x


def check():
    out = D.build(stem(), SR, 0.0, 6.0, 120.0)
    for name, ts in TIMES.items():
        ev = out[name]
        assert len(ev) == len(ts), '%s: %d events, expected %d' % (name, len(ev), len(ts))
        for ti, e in zip(ts, ev):
            assert abs(e[0] - ti) < 0.015, '%s hit at %.3f found at %.3f' % (name, ti, e[0])
        assert ev[1][2] < 0.5 * ev[0][2], '%s: quiet hit velocity %.3f not under half of loud %.3f' % (name, ev[1][2], ev[0][2])


if __name__ == '__main__':
    check(); print('PASS')
    D.USE_PEAK_LEVEL = False
    try:
        check(); print('MUTATION SURVIVED'); sys.exit(1)
    except AssertionError as e:
        print('mutation failed as it must:', e)
