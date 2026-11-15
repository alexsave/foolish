# test_glitch.py - tests for compare.py's glitch measures on synthetic stems.
#
# What it does: builds a 6 s synthetic stem (a few slowly moving tones plus noise, so the original is always within 30 dB of its
#   peak), damages copies of it in known ways (4 ms gaps at 10 joins, a gain step, one click, a gain wobble every 0.5 s, a delay, a
#   gain change) and asserts that compare.glitch() measures each damage and nothing else.
# Inputs: none (environment GLITCH_COMPARE_DIR names the directory holding the compare.py to test; mutation checks use it).
#   Outputs: one line per assertion, exit code 1 on any failure. How to run: python3 -I test_glitch.py
# Randomness: ONE generator, seeded by SEED below.
import os, sys
import numpy as np

sys.path.insert(0, os.environ.get('GLITCH_COMPARE_DIR', os.path.dirname(os.path.abspath(__file__))))
import compare as C

SEED = 20261116
SR = 44100
DUR = 6.0
FAILS = []


def check(name, ok, msg):
    print('  %s %s: %s' % ('ok  ' if ok else 'FAIL', name, msg))
    if not ok:
        FAILS.append(name)


def make():
    rng = np.random.default_rng(SEED)
    n = int(DUR * SR); t = np.arange(n) / SR
    x = np.zeros(n)
    for f, a, m in ((110, 0.5, 0.3), (220, 0.3, 0.5), (331, 0.2, 0.7), (1170, 0.1, 0.9)):
        x += a * np.sin(2 * np.pi * f * t + 2 * np.pi * m * t) * (1 + 0.3 * np.sin(2 * np.pi * 0.37 * t + f))
    x += 0.3 * rng.standard_normal(n)
    x[:int(0.05 * SR)] = 0; x[-int(0.05 * SR):] = 0     # silent ends, so a delayed copy loses nothing
    return x * 0.5


JOINS = [0.5 + 0.55 * k for k in range(10)]


def gaps(x, joins, fade=0.0005, zero=0.004):
    y = x.copy(); n = len(x)
    for tj in joins:
        i0 = int(round((tj - fade - zero / 2) * SR)); nf = int(round(fade * SR)); nz = int(round(zero * SR))
        g = np.concatenate([np.linspace(1, 0, nf, endpoint=False), np.zeros(nz), np.linspace(0, 1, nf, endpoint=False)])
        y[i0:i0 + len(g)] *= g
    return y


def run():
    x = make()
    g = lambda r, joins=None: {k: v[0] for k, v in C.glitch(x, r, SR, joins).items()}
    print('  identical')
    G = g(x, JOINS)
    check('ident_null', G['glitch_null_residual_db'] < -80, 'null residual %.1f dB, expected < -80' % G['glitch_null_residual_db'])
    zero = ['glitch_dip_frames', 'glitch_bump_frames', 'glitch_level_steps', 'glitch_clicks', 'glitch_joins_over1db', 'glitch_joins_over3db']
    check('ident_zero', all(G[k] == 0 for k in zero), 'counts %s' % {k: G[k] for k in zero})
    print('  4 ms gaps (0.5 ms linear edges) at 10 joins')
    G = g(gaps(x, JOINS), JOINS)
    check('gaps_joins', G['glitch_joins'] == 10 and G['glitch_joins_over3db'] == 10, 'joins %d, over 3 dB %d, expected 10 and 10' % (G['glitch_joins'], G['glitch_joins_over3db']))
    check('gaps_residual', -25 <= G['glitch_null_residual_db'] <= -18, 'null residual %.1f dB, expected -25 to -18 (a 4 ms gap with 0.5 ms linear edges costs 4.3 ms of signal)' % G['glitch_null_residual_db'])
    check('gaps_dips', G['glitch_dip_frames'] >= 10 and G['glitch_deepest_dip_db'] < -3, 'dip runs %d, deepest %.1f dB' % (G['glitch_dip_frames'], G['glitch_deepest_dip_db']))
    print('  joins listed where there is no gap (100 ms off)')
    G = g(gaps(x, JOINS), [t + 0.1 for t in JOINS])
    check('false_joins', G['glitch_joins_over3db'] == 0, 'joins over 3 dB = %d, expected 0 (the join window is +-20 ms)' % G['glitch_joins_over3db'])
    print('  2 dB gain step at 3 s, and an 8 dB step')
    n = len(x); step = np.ones(n)
    step[n // 2:] = 10 ** (2 / 20)
    G = g(x * step)
    check('step2_residual', -22 <= G['glitch_null_residual_db'] <= -14, 'null residual %.1f dB, expected -22 to -14' % G['glitch_null_residual_db'])
    check('step2_quiet', G['glitch_level_steps'] == 0 and G['glitch_dip_frames'] == 0, 'level steps %d, dip runs %d, expected 0 and 0 (2 dB is under every threshold)' % (G['glitch_level_steps'], G['glitch_dip_frames']))
    step8 = np.ones(n); step8[n // 2:] = 10 ** (8 / 20)
    G = g(x * step8)
    check('step8_found', G['glitch_level_steps'] == 1, 'level steps %d, expected 1' % G['glitch_level_steps'])
    G = {k: v[0] for k, v in C.glitch(x * step8, x * step8, SR).items()}
    check('step_in_both', G['glitch_level_steps'] == 0, 'a step in both sides: level steps %d, expected 0' % G['glitch_level_steps'])
    print('  one click, and the same click on both sides')
    y = x.copy(); y[int(2.5 * SR)] += 1.0
    G = g(y)
    check('click', G['glitch_clicks'] == 1, 'clicks %d, expected 1' % G['glitch_clicks'])
    xc = x.copy(); xc[int(2.5 * SR)] += 1.0
    G = {k: v[0] for k, v in C.glitch(xc, y, SR).items()}
    check('click_in_both', G['glitch_clicks'] == 0, 'a click on both sides: clicks %d, expected 0' % G['glitch_clicks'])
    print('  gain wobble every 0.5 s')
    w = np.ones(n)
    for k in range(1, 12):
        i = int(k * 0.5 * SR); w[i:i + int(0.03 * SR)] = 0.5
    G = g(x * w)
    check('wobble_lag', abs(G['glitch_residual_periodicity_lag_s'] - 0.5) <= 0.02 and G['glitch_residual_periodicity'] > 0.5,
          'periodicity %.2f at lag %.2f s, expected > 0.5 at 0.50' % (G['glitch_residual_periodicity'], G['glitch_residual_periodicity_lag_s']))
    check('ident_periodicity', g(x)['glitch_residual_periodicity'] < 0.5, 'identical: periodicity %.2f, expected < 0.5' % g(x)['glitch_residual_periodicity'])
    print('  delay of 2 ms, and a gain of 0.5')
    d = np.concatenate([np.zeros(88), x[:-88]])
    G = g(d)
    check('delay_aligned', G['glitch_null_residual_db'] < -60 and abs(G['glitch_lag_ms'] + 88 / SR * 1000) < 0.05, 'null residual %.1f dB (expected < -60), lag %.2f ms (expected -2.00)' % (G['glitch_null_residual_db'], G['glitch_lag_ms']))
    G = g(0.5 * x)
    check('gain_matched', G['glitch_null_residual_db'] < -80 and G['glitch_dip_frames'] == 0, 'null residual %.1f dB, dip runs %d, expected < -80 and 0' % (G['glitch_null_residual_db'], G['glitch_dip_frames']))


if __name__ == '__main__':
    run()
    if FAILS:
        print('FAILED: ' + ', '.join(FAILS)); sys.exit(1)
    print('PASS test_glitch')
