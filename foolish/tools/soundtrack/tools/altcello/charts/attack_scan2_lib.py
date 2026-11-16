"""Level re sustain helper shared by charts/wedge_scan.py and charts/onset2.py (the lev function of attack_scan2.py)."""
import numpy as np
import onset_lib as L
M = L.M


def lev(y, on):
    env = 20 * np.log10(M.rms_env(y) + 1e-12)
    a = int(round((on + 0.5) * 100)); b = int(round((on + 1.4) * 100))
    sus = float(np.median(env[a:b]))
    return [float(env[int(round((on + d) * 100))] - sus) for d in (0.02, 0.05, 0.1)]
