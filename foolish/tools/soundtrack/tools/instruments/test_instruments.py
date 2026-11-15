#!/usr/bin/env python3
"""test_instruments.py - deterministic synthetic test of voices.py, describe.py and guess.py (plain asserts, exit code).

What it does: synthesises notes with known properties, writes nothing, and checks that the tools recover them:
  a saw tone with a closing low-pass (falling brightness, harmonics about -6 dB per octave before the filter bites,
  guess saw-type), a square tone (odd harmonics, guess square-type), a plucked decaying tone with slight inharmonicity
  (falling brightness, decay, guess plucked or struck), a tone with 6 Hz 30 cent vibrato and a 150 ms glide up into the
  note (vibrato rate and depth within 15 percent, glide detected and its length), a sine sub (guess sine or sub bass),
  a tone with added noise (noise share), a left-only and a mono file (side/mid), a two-line other-stem file with an inner
  voice (voices.py separates the top line from the line more than an octave under it and leaves the inner notes
  unassigned), the vocals rule (a stem 20 dB under the mix gives no voice), the 6-note minimum, and drums (a kick and
  a hat pattern split into low and high voices; decay, peak frequency and pitch drop of the kick).
Randomness: one seedable source, SEED below (numpy default_rng); the only random signal is the noise added to one tone.
Inputs: none.  Output: one PASS or FAIL line per named check, then a summary; exit code 0 only if all passed.
Example: /Users/alex/Dev/soundtrack-tools/venv/bin/python test_instruments.py
Runtime: about 30 s.  Failure modes: needs numpy and scipy (the soundtrack venv); a failing check names the assertion.
"""
import os
import sys
import time

import numpy as np
from scipy import signal

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import describe as ds  # noqa: E402
import guess as gs  # noqa: E402
import voices as vc  # noqa: E402

SEED = 20260101
RNG = np.random.default_rng(SEED)
SR = 44100
RESULTS = []


def check(name, cond, detail=""):
    RESULTS.append((name, bool(cond)))
    print("%s %s %s" % ("PASS" if cond else "FAIL", name, detail), flush=True)


def tone(f0, dur, harm, phase_fn=None, glide=None, vib=None, decay=None):
    """Additive tone. harm(h, f0) amplitude per harmonic; glide=(ms, cents_below); vib=(Hz, cents, delay_s);
    decay(h) returns the decay time constant of harmonic h in s (None for no decay)."""
    t = np.arange(int(dur * SR)) / SR
    cents = np.zeros_like(t)
    if glide:
        cents += np.where(t < glide[0], -glide[1] * (1 - t / glide[0]), 0.0)
    if vib:
        cents += np.where(t >= vib[2], vib[1] * np.sin(2 * np.pi * vib[0] * (t - vib[2])), 0.0)
    fi = f0 * 2 ** (cents / 1200.0)
    ph = 2 * np.pi * np.cumsum(fi) / SR
    y = np.zeros_like(t)
    for h in range(1, 60):
        if h * f0 * 1.05 > 20000:
            break
        a = harm(h, f0)
        if a == 0:
            continue
        env = np.exp(-t / decay(h)) if decay else 1.0
        y += a * env * np.sin(h * ph)
    return y


def lay(notes_audio, midis, gap=0.35, lead=0.5, tail=1.0):
    """Concatenate note signals with gaps; returns mono signal and the note array (on, off, midi, vel)."""
    x = [np.zeros(int(lead * SR))]
    t = lead
    notes = []
    for y, m in zip(notes_audio, midis):
        x.append(y)
        notes.append((t, t + len(y) / SR, m, 0.8))
        t += len(y) / SR
        x.append(np.zeros(int(gap * SR)))
        t += gap
    x.append(np.zeros(int(tail * SR)))
    x = np.concatenate(x)
    return x / (np.abs(x).max() + 1e-12) * 0.5, np.array(notes)


def stereo(x):
    return np.stack([x, x], axis=1).astype(np.float32)


def hz(m):
    return 440.0 * 2 ** ((m - 69) / 12.0)


def run(x, notes, stem="other", name="top"):
    X = stereo(x) if x.ndim == 1 else x
    R = ds.describe_voice(X, SR, notes, notes)
    return R, gs.guess(R, stem, name, "")


def closing_lp(y, f_start, f_end, dur):
    out = np.zeros_like(y)
    z = 0.0
    n = len(y)
    for i in range(n):
        fc = f_start * (f_end / f_start) ** (i / (dur * SR))
        a = 1 - np.exp(-2 * np.pi * fc / SR)
        z += a * (y[i] - z)
        out[i] = z
    return out


def edge(y, a=0.01, r=0.02):
    t = np.arange(len(y)) / SR
    return y * np.minimum(1, t / a) * np.minimum(1, (len(y) / SR - t) / r)


def test_saw():
    midis = [45, 48, 52, 48, 45, 50, 53, 50, 45, 48]
    ys = []
    for m in midis:
        y = tone(hz(m), 0.9, lambda h, f: 1.0 / h)
        ys.append(edge(closing_lp(y, 9000, 700, 0.9)))
    x, notes = lay(ys, midis)
    R, G = run(x, notes)
    hd = R["harm_db"]
    check("saw_h2_h4_octaves", -9.5 < hd[1] < -3.0 and -16 < hd[3] < -8, "h2 %.1f h4 %.1f dB" % (hd[1], hd[3]))
    check("saw_brightness_falls", R["bright_trend"] == "falls", "trend %s counts %s" % (R["bright_trend"], R["bright_trend_counts"]))
    check("saw_last_over_early_centroid", R["bright_last_over_50_150"]["median"] < 0.7, "ratio %.2f" % R["bright_last_over_50_150"]["median"])
    check("saw_inharm_low", R["inharm_cents"]["median"] < 3.0, "inharm %.2f cents" % R["inharm_cents"]["median"])
    check("saw_pitch_range", (R["pitch"]["lo"], R["pitch"]["hi"]) == (45, 53))
    check("saw_entrance_silent_before", R["entrance"]["pre_silent"] is True and abs(R["entrance"]["t"] - 0.5) < 1e-6)
    # static saw for the slope
    ys2 = [edge(tone(hz(m), 0.9, lambda h, f: 1.0 / h)) for m in midis]
    x2, n2 = lay(ys2, midis)
    R2, G2 = run(x2, n2)
    check("saw_slope_minus6", -7.5 < R2["slope_db_oct"]["median"] < -4.5, "slope %.2f dB/oct" % R2["slope_db_oct"]["median"])
    check("saw_guess_saw_type", "saw-type" in G2["text"] and G2["confidence"] in ("medium", "high"), "%s (%s)" % (G2["text"], G2["confidence"]))
    check("saw_brightness_holds_static", R2["bright_trend"] == "holds", "trend %s" % R2["bright_trend"])
    # a steep spectrum (about -18 dB per octave) must not reach a high-confidence saw-type guess
    ys3 = [edge(tone(hz(m), 0.9, lambda h, f: 1.0 / h ** 3)) for m in midis]
    x3, n3 = lay(ys3, midis)
    R3, G3 = run(x3, n3)
    check("steep_slope_measured", R3["slope_db_oct"]["median"] < -12, "slope %.1f dB/oct" % R3["slope_db_oct"]["median"])
    check("steep_slope_not_high_saw", not ("saw-type" in G3["text"] and G3["confidence"] == "high"), "%s (%s)" % (G3["text"], G3["confidence"]))


def test_square():
    midis = [48, 52, 55, 52, 48, 50, 53, 50, 48, 52]
    ys = [edge(tone(hz(m), 0.8, lambda h, f: (1.0 / h) if h % 2 else 0.0)) for m in midis]
    x, notes = lay(ys, midis)
    R, G = run(x, notes)
    check("square_odd_even_high", R["odd_even_db"]["median"] > 15, "odd/even %.1f dB" % R["odd_even_db"]["median"])
    check("square_guess", "square-type" in G["text"], "%s (%s)" % (G["text"], G["confidence"]))


def test_pluck():
    midis = [57, 60, 64, 67, 64, 60, 57, 62, 66, 62]
    B = 3e-4
    ys = []
    for m in midis:
        f0 = hz(m)
        t = np.arange(int(0.9 * SR)) / SR
        y = np.zeros_like(t)
        for h in range(1, 30):
            fh = h * f0 * np.sqrt(1 + B * h * h)
            if fh > 18000:
                break
            y += (1.0 / h) * np.exp(-t / (0.6 / (1 + 0.5 * (h - 1)))) * np.sin(2 * np.pi * fh * t)
        ys.append(y * np.minimum(1, t / 0.003))
    x, notes = lay(ys, midis, gap=0.5)
    R, G = run(x, notes)
    check("pluck_brightness_falls", R["bright_trend"] == "falls", "trend %s" % R["bright_trend"])
    check("pluck_sustain_decays", R["sustain_db"]["median"] < -6, "sustain %.1f dB" % R["sustain_db"]["median"])
    check("pluck_attack_fast", R["attack_s"] is not None and R["attack_s"]["median"] < 0.025, "attack %s" % R["attack_s"])
    check("pluck_inharm_measured", R["inharm_cents"] is not None and 2.0 < R["inharm_cents"]["median"] < 80, "inharm %s" % R["inharm_cents"])
    check("pluck_guess", "plucked" in G["text"] or "struck" in G["text"], "%s (%s)" % (G["text"], G["confidence"]))


def test_vibrato_glide():
    midis = [60, 62, 64, 65, 64, 62, 60, 67]
    ys = [edge(tone(hz(m), 1.4, lambda h, f: 1.0 / h if h <= 10 else 0.0, glide=(0.15, 200), vib=(6.0, 30, 0.25))) for m in midis]
    x, notes = lay(ys, midis, gap=0.6)
    R, G = run(x, notes)
    vr = R["vib_rate_hz"]["median"] if R["vib_rate_hz"] else None
    vd = R["vib_depth_cents"]["median"] if R["vib_depth_cents"] else None
    check("vibrato_rate_6hz_15pct", vr is not None and abs(vr - 6.0) <= 0.9, "rate %s Hz" % vr)
    check("vibrato_depth_30c_15pct", vd is not None and abs(vd - 30.0) <= 4.5, "depth %s cents" % vd)
    check("vibrato_share_high", R["vib_share"] is not None and R["vib_share"] >= 0.8, "share %s" % R["vib_share"])
    check("glide_detected", R["glide_share"] is not None and R["glide_share"] >= 0.8, "glide share %s" % R["glide_share"])
    gc = R["glide_cents"]["median"] if R["glide_cents"] else None
    gm = R["glide_ms"]["median"] if R["glide_ms"] else None
    check("glide_cents_near_200", gc is not None and 120 <= gc <= 260, "glide %s cents" % gc)
    check("glide_ms_near_150", gm is not None and 90 <= gm <= 220, "glide %s ms" % gm)
    check("vibrato_delay_near_0p25", R["vib_delay_s"] is not None and R["vib_delay_s"]["median"] is not None and 0.1 <= R["vib_delay_s"]["median"] <= 0.5, "delay %s" % R["vib_delay_s"])
    # no vibrato, no glide for a plain tone
    ys0 = [edge(tone(hz(m), 1.4, lambda h, f: 1.0 / h if h <= 10 else 0.0)) for m in midis]
    x0, n0 = lay(ys0, midis, gap=0.6)
    R0, _ = run(x0, n0)
    check("plain_tone_no_vibrato_no_glide", R0["vib_share"] == 0.0 and R0["glide_share"] == 0.0, "vib %s glide %s" % (R0["vib_share"], R0["glide_share"]))


def test_sub():
    midis = [28, 31, 33, 28, 31, 36, 33, 28]
    ys = [edge(tone(hz(m), 0.9, lambda h, f: 1.0 if h == 1 else 0.0)) for m in midis]
    x, notes = lay(ys, midis, gap=0.3)
    R, G = run(x, notes, stem="bass", name="bass")
    check("sub_few_harmonics", R["n_harm_30db"]["median"] <= 2, "harmonics %s" % R["n_harm_30db"])
    check("sub_guess", "sine or sub bass" in G["text"], "%s (%s)" % (G["text"], G["confidence"]))


def test_noise_and_stereo():
    midis = [60, 62, 64, 62, 60, 65, 67, 65]
    ys = [edge(tone(hz(m), 0.8, lambda h, f: 1.0 / h)) for m in midis]
    x, notes = lay(ys, midis)
    R, _ = run(x, notes)
    xn = x + RNG.standard_normal(len(x)) * 0.4
    Rn, _ = run(xn, notes)
    check("noise_share_clean_low", R["noise_share"]["median"] < 0.02, "clean %.3f" % R["noise_share"]["median"])
    check("noise_share_noisy_high", Rn["noise_share"]["median"] > 0.1, "noisy %.3f" % Rn["noise_share"]["median"])
    Lonly = np.stack([x, np.zeros_like(x)], axis=1).astype(np.float32)
    RL, _ = run(Lonly, notes)
    check("stereo_mono_narrow", R["side_mid_db"] < -40, "mono side/mid %.1f dB" % R["side_mid_db"])
    check("stereo_left_only_wide", RL["side_mid_db"] > -3, "left-only side/mid %.1f dB" % RL["side_mid_db"])


def test_voices():
    # top line E5-ish notes 0.4 s, low line under it, an inner line 5 semitones under the top line
    rows = []
    t = 1.0
    top_m = [76, 79, 77, 76, 74, 76, 79, 81]
    low_m = [43, 45, 40, 43, 47, 45, 40, 43]
    for i in range(8):
        rows.append((t, t + 0.4, top_m[i], 0.8))
        rows.append((t + 0.05, t + 0.45, low_m[i], 0.8))
        t += 0.6
    inner = [(1.0 + 0.6 * i + 0.02 + 0.0, 1.0 + 0.6 * i + 0.40, 71, 0.8) for i in (1, 3, 5)]
    notes = np.array(sorted(rows + inner))
    vs, left = vc.split_other(notes, 0.5, 6)
    names = {v[0]: v for v in vs}
    check("voices_two_lines_found", "top" in names and "lowmid" in names, "voices %s" % list(names))
    if "top" in names and "lowmid" in names:
        top, low = names["top"][2], names["lowmid"][2]
        check("voices_register_gap_over_octave", top[:, 2].min() - low[:, 2].max() > 12, "gap %.0f semitones" % (top[:, 2].min() - low[:, 2].max()))
        check("voices_counts", len(top) == 8 and len(low) == 8, "top %d low %d" % (len(top), len(low)))
    check("voices_inner_notes_unassigned", len(left) == 3 and (left[:, 2] == 71).all(), "unassigned %d" % len(left))
    few, _ = vc.split_other(notes[:5], 0.5, 6)
    check("voices_min_6_notes", len(few) == 0, "voices from 5 notes: %d" % len(few))
    # chords
    ch = []
    for i in range(4):
        for m in (60, 64, 67):
            ch.append((2.0 * i, 2.0 * i + 1.0, m, 0.8))
    cv, _ = vc.split_other(np.array(ch), 0.5, 6)
    check("voices_chord_group", len(cv) == 1 and cv[0][0] == "chords", "voices %s" % [v[0] for v in cv])
    # vocals rule through split_all
    n = SR * 10
    mix = np.zeros(n)
    mix[:] = 0.3 * np.sin(2 * np.pi * 220 * np.arange(n) / SR)
    quiet = stereo(mix * 0.03)
    loud = stereo(mix * 0.5)
    vn = np.array([(1 + 0.8 * i, 1.5 + 0.8 * i, 60 + i, 0.8) for i in range(8)])
    sn = {"vocals": vn}
    r_q, _ = vc.split_all(sn, {"vocals": quiet}, SR, mix, 0.0, 10.0)
    r_l, _ = vc.split_all(sn, {"vocals": loud}, SR, mix, 0.0, 10.0)
    check("vocals_rule_quiet_none", not any(v["stem"] == "vocals" for v in r_q), "20 dB under the mix gives %d voices" % len(r_q))
    check("vocals_rule_close_one", sum(1 for v in r_l if v["stem"] == "vocals") == 1, "within 15 dB gives %d voices" % len(r_l))
    bn = {"bass": vn}
    r_b, _ = vc.split_all(bn, {"bass": loud}, SR, mix, 0.0, 10.0)
    check("bass_one_voice", sum(1 for v in r_b if v["stem"] == "bass") == 1)


def test_drums():
    dur = 8.0
    x = np.zeros(int(dur * SR))
    kicks = [0.5 * i + 0.1 for i in range(14)]
    for t0 in kicks:
        tt = np.arange(int(0.4 * SR)) / SR
        f = 50 + 150 * np.exp(-tt / 0.03)
        k = np.sin(2 * np.pi * np.cumsum(f) / SR) * np.exp(-tt / 0.12)
        a = int(t0 * SR)
        x[a:a + len(k)] += 0.8 * k
    hats = [0.5 * i + 0.4 for i in range(14)]  # 0.3 s after each kick, so no kick tail under them
    for t0 in hats:
        n = int(0.05 * SR)
        h = signal.sosfilt(signal.butter(4, 7000 / (SR / 2), "high", output="sos"), RNG.standard_normal(n)) * np.exp(-np.arange(n) / SR / 0.012)
        a = int(t0 * SR)
        x[a:a + n] += 0.15 * h
    X = stereo(x)
    vs = vc.split_drums(x, SR, 0.0, dur)
    nm = {v[0]: v for v in vs}
    check("drums_low_voice_kicks", "drums_low" in nm and abs(len(nm["drums_low"][2]) - 14) <= 1, "low onsets %s" % (len(nm["drums_low"][2]) if "drums_low" in nm else None))
    check("drums_high_voice_hats", "drums_high" in nm and len(nm["drums_high"][2]) >= 12, "high onsets %s" % (len(nm["drums_high"][2]) if "drums_high" in nm else None))
    R = ds.describe_drum(X, SR, kicks, "drums_low", None)
    check("drum_pitch_drop", R["pitch_drop_hz"] is not None and R["pitch_drop_hz"]["median"] is not None and R["pitch_drop_hz"]["median"] > 30, "drop %s" % R["pitch_drop_hz"])
    check("drum_peak_freq_low", R["peak_hz"]["median"] < 120, "peak %s" % R["peak_hz"])
    check("drum_decay_in_range", R["decay_20db_s"]["median"] is not None and 0.1 < R["decay_20db_s"]["median"] < 0.45, "decay %s" % R["decay_20db_s"])
    G = gs.guess(R, "drums", "drums_low", "")
    check("drum_guess_kick_like", "kick" in G["text"], G["text"])
    R2 = ds.describe_drum(X, SR, kicks, "drums_low", ([0.1 + 2.0 * i for i in range(3)], 2.0, 4))
    check("drum_pattern_16_steps", R2["pattern"] is not None and R2["pattern"]["steps"] == 16 and R2["pattern"]["line"][0] == "x" and R2["pattern"]["line"][1:4] == "...", "pattern %s" % (R2["pattern"] or {}).get("line"))


def test_prompt():
    t = "A haegeum melody, gayageum plucks and an 808 bass. No vocals, no janggu."
    check("prompt_words", gs.prompt_instruments(t)[:2] == ["haegeum", "gayageum"] and "vocals" not in gs.prompt_instruments(t) and "vocals" in gs.prompt_instruments(t, negated=True))


def main():
    t0 = time.time()
    for f in (test_saw, test_square, test_pluck, test_vibrato_glide, test_sub, test_noise_and_stereo, test_voices, test_drums, test_prompt):
        try:
            f()
        except Exception as ex:  # a crash is a failure of the named test function
            import traceback
            traceback.print_exc()
            check(f.__name__ + "_ran", False, repr(ex))
    bad = [n for n, ok in RESULTS if not ok]
    print("%d checks, %d failed, %.1f s" % (len(RESULTS), len(bad), time.time() - t0))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
