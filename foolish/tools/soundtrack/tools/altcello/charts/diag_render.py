"""Rendering helpers of the diagnosis: one 4 s probe per pitch and condition (baseline = the stage2c settings)."""
import os, subprocess, sys
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import diagnose as D  # noqa: E402

EXE = os.path.join(ROOT, "altcello")
OUTD = os.path.join(ROOT, "out", "diag")
L = 0.69
OPEN = {"C2": 65.41, "G2": 98.00, "D3": 146.83, "A3": 220.00}
LOWER = {"G2": "C2", "D3": "G2", "A3": "D3"}  # the string one below, played 7 semitones up for F6
ANCHOR = {"C2": 1.22127, "G2": 0.67082, "D3": 0.67082, "A3": 0.49717}


def finger_pos(string, f):
    return L * (1.0 - OPEN[string] / f)


def render(pitch, tag, beta=0.1, vB=0.1, F=None, secs=4.0, noise=None, knobs=(), stopped=False, force_scale=1.0, wav_on=True, string=None):
    """returns (wav, prefix). stopped: the same pitch on the string below (7 semitones up), force of that string's anchor"""
    os.makedirs(OUTD, exist_ok=True)
    string = string or pitch
    fb = D.FORCE[pitch] if F is None else F
    if stopped:
        string = LOWER[pitch]
        fb = ANCHOR[string] if F is None else F
    fb *= force_scale
    prefix = os.path.join(OUTD, "%s_%s" % (tag, pitch))
    wav = prefix + ".wav"
    cmd = ["nice", EXE, "probe", string, str(beta), str(vB), "%.5f" % fb, str(secs), prefix, "--body", "on", "--strings", "all"] + (["--wav", wav] if wav_on else [])
    if stopped: cmd += ["--finger", "%.6f" % finger_pos(string, 2 ** (7 / 12) * OPEN[string])]
    if noise is not None: cmd += ["--noise", str(noise)]
    cmd += [str(k) for k in knobs]
    subprocess.run(cmd, capture_output=True, text=True, check=True)
    return wav, prefix
