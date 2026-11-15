#!/usr/bin/env python3
"""guess.py - turn one voice's measurements into a best guess at the instrument family, with confidence and evidence.

This is a RULE TABLE over describe.py measurements, not a listening result.
Each family has weighted rules; a rule that cannot be evaluated (measurement missing) is left out of both the score and
the maximum.  fraction = score / evaluable maximum; a family is only considered when its evaluable rules carry at least
60 percent of its total weight, and confidence "high" needs at least 80 percent (else it is capped at medium).  Confidence of the top family: high at fraction 0.75 or more,
medium at 0.55 or more, low at 0.40 or more, otherwise "undetermined".  A second family within 0.10 of the top
fraction is listed as well ("or").  "undetermined" is also returned (with the measurements) when fewer than 5 notes
sounded alone in their stem, or when fewer than 5 notes could be measured.
Rule table (weight in brackets):
 plucked string : attack under 25 ms (2); brightness falls (2); sustain at least 6 dB under the peak (2);
                  inharmonicity 1 to 40 cents (1); no vibrato (1); release under 0.6 s (1)
 struck (piano, mallet) : attack under 20 ms (2); sustain at least 10 dB under the peak (2); inharmonicity 3 cents or more
                  and rising with pitch (correlation over 0.3) (2); brightness falls (1); no vibrato (1)
 bowed string   : attack 40 to 400 ms (2); vibrato 4.5 to 7.5 Hz in at least 30 percent of long notes (2); glide into
                  notes or portamento in at least 20 percent (1); 8 or more harmonics within 30 dB (1); noise share over
                  0.04 (1); sustain within 6 dB of the peak (1)
 blown reed or brass : attack 30 to 120 ms (1.5); brightness rises with level (correlation over 0.4) (2); odd-to-even over
                  6 dB (clarinet-like, 1 extra and the evidence says "clarinet-like tube"); 2 to 5 kHz share over 0.08 of the
                  energy (brass-like, 1); sustain within 6 dB of the peak (1)
 flute-like     : 4 or fewer harmonics within 30 dB (2); noise share over 0.08 (2); attack 30 to 150 ms (1); median pitch
                  above MIDI 72 (1); sustain within 6 dB of the peak (1)
 sung voice or choir : vocals stem (2); vibrato 4.5 to 7.5 Hz (1.5); formant-like peak that does not move with f0 (2);
                  noise share over 0.04 (1); glide or portamento share 20 percent or more (1); polyphony over 1.5 (choir, 0.5)
 saw-type synth lead : harmonic slope between -9 and -3 dB per octave (2); inharmonicity under 6 cents (1.5); no vibrato
                  (1); brightness static or swept (always evaluable, 0.5); odd-to-even between -8 and 2 dB (1); noise share
                  under 0.05 (1)
 square-type    : odd-to-even over 10 dB (3); inharmonicity under 6 cents (1); no vibrato (0.5); sustain within 6 dB (1)
 sine or sub bass : 2 or fewer harmonics within 30 dB (3); median MIDI under 45 or the bass stem (1.5); noise share under
                  0.05 (1); no vibrato (0.5)
 supersaw stack : fundamental peak excess width 20 cents or more (2); beating under 12 Hz in at least 30 percent of long
                  notes (1.5); side/mid over -6 dB (1.5); harmonic slope between -9 and -3 dB per octave (1); no vibrato (1)
 FM or bell-like : inharmonicity 25 cents or more (2.5); sustain at least 10 dB under the peak (1.5); attack under 25 ms (1);
                  no vibrato (0.5)
 pad            : attack 150 ms or more (2); release 300 ms or more (2); side/mid over -9 dB (1); voice rule is chords
                  or pad, or polyphony 2 or more (1); median note length 1.5 s or more (1)
 drums          : low band voice with decay and pitch drop (kick-like); mid (snare, clap-like); high (hat, shaker-like);
                  reported by band and measured decay, centroid and drop, always "medium" at most.
Output: guess(R, stem, voice_name, prompt_text) returns dict(families=[(name, fraction)], text, confidence, evidence,
  prompt_asked). The prompt instrument words are printed as "the prompt asked for ..." and never as something heard.
CLI: guess.py PROMPT_FILE prints the instrument words found in a prompt.
Failure modes: the table is only as good as the transcription and the stem separation (a stem leak puts other
  instruments in the voice); a family absent from the table (e.g. accordion, guitar strum, choir with synth) is forced
  into the nearest listed one or "undetermined"; weights are judgement, not trained. Deterministic.
"""
import re
import sys

import numpy as np

INSTR_WORDS = ["haegeum", "gayageum", "janggu", "piano", "grand piano", "strings", "violin", "viola", "cello", "cellos",
               "double bass", "bass", "808", "sub bass", "supersaw", "saw", "synth", "synth lead", "lead", "pad", "flute",
               "piccolo", "oboe", "clarinet", "bassoon", "trumpet", "trumpets", "trombone", "horn", "horns", "tuba",
               "brass", "choir", "voice", "vocals", "vocal", "organ", "harp", "celesta", "glockenspiel", "bells", "bell",
               "balalaika", "accordion", "guitar", "flamenco guitar", "nylon guitar", "bongos", "congas", "timbales",
               "claves", "cowbell", "kick", "snare", "hi-hat", "hats", "claps", "clap", "toms", "timpani", "orchestra",
               "marimba", "vibraphone", "harpsichord", "mandolin", "bandura", "gusli", "domra", "trombones", "marimba",
               "sax", "saxophone", "pluck", "plucks", "stabs", "arpeggio", "fiddle", "bass drum", "tambourine",
               "shaker", "castanets", "palmas", "violins", "violas", "contrabass", "bassline"]


def prompt_instruments(text, negated=False):
    """Instrument words found in a prompt, in order of first appearance (whole-word, case-insensitive).
    A word whose clause (up to the previous comma, period or semicolon) holds no, not, without or never is "excluded":
    returned only when negated=True (the prompt said it did NOT want it)."""
    low = text.lower()
    found = []
    for w in sorted(set(INSTR_WORDS), key=lambda s: (-len(s), s)):
        pos = [m.start() for m in re.finditer(r"(?<![a-z0-9])" + re.escape(w) + r"(?![a-z0-9])", low)]
        for p in pos:
            cl = max(low.rfind(c, 0, p) for c in ",.;:") + 1
            neg = bool(re.search(r"\b(no|not|without|never|none)\b", low[cl:p]))
            if neg == negated:
                found.append((p, w))
                break
    found.sort()
    out = []
    for _, w in found:
        if not any(w != o and w in o.split() for o in out):
            out.append(w)
    return out


def _g(R, key, sub="median"):
    v = R.get(key)
    if v is None:
        return None
    if isinstance(v, dict):
        return v.get(sub)
    return v


def _rules(R, stem, name):
    """Return {family: [(weight, test_result or None, evidence_text)]}."""
    att = _g(R, "attack_s")
    sus = _g(R, "sustain_db")
    rel = _g(R, "release_s")
    inh = _g(R, "inharm_cents")
    ipc = R.get("inharm_pitch_corr")
    oe = _g(R, "odd_even_db")
    noise = _g(R, "noise_share")
    slope = _g(R, "slope_db_oct")
    nh = _g(R, "n_harm_30db")
    trend = R.get("bright_trend")
    blc = _g(R, "bright_level_corr")
    vs = R.get("vib_share")
    vr = _g(R, "vib_rate_hz")
    gs = R.get("glide_share")
    ps = R.get("portamento_share")
    gp = max([x for x in (gs, ps) if x is not None], default=None)
    width = _g(R, "fund_width_cents")
    beat_share = R.get("beat_share")
    sm = R.get("side_mid_db")
    s25 = _g(R, "share_2_5k")
    fm = R.get("formant")
    medmidi = R["pitch"]["median"] if R.get("pitch") else None
    lenmed = _g(R, "length_s")
    poly = R.get("polyphony_median")

    def T(cond, txt):
        return (None, "") if cond is None else (bool(cond), txt)

    def novib():
        return (None if vs is None else vs < 0.15)

    def vib_ok():
        return (None if vs is None or vr is None and vs > 0 and False else (vs >= 0.3 and (vr is None or 4.5 <= vr <= 7.5)))

    def f(w, cond, txt):
        return (w, None if cond is None else bool(cond), txt)

    F = {}
    F["plucked string"] = [
        f(2, None if att is None else att < 0.025, "attack %s ms (rule: under 25)" % _ms(att)),
        f(2, None if trend is None else trend == "falls", "brightness trend %s (rule: falls)" % trend),
        f(2, None if sus is None else sus <= -6, "sustain %s dB re peak (rule: at most -6)" % _n(sus, 1)),
        f(1, None if inh is None else 1 <= inh <= 40, "inharmonicity %s cents (rule: 1 to 40)" % _n(inh, 1)),
        f(1, novib(), "vibrato share %s (rule: under 0.15)" % _n(vs, 2)),
        f(1, None if rel is None else rel < 0.6, "release %s ms (rule: under 600)" % _ms(rel)),
    ]
    F["struck (piano or mallet)"] = [
        f(2, None if att is None else att < 0.020, "attack %s ms (rule: under 20)" % _ms(att)),
        f(2, None if sus is None else sus <= -10, "sustain %s dB re peak (rule: at most -10)" % _n(sus, 1)),
        f(2, None if inh is None else (inh >= 3 and (ipc is None or ipc > 0.3)),
          "inharmonicity %s cents, correlation with pitch %s (rule: 3 or more and over 0.3)" % (_n(inh, 1), _n(ipc, 2))),
        f(1, None if trend is None else trend == "falls", "brightness trend %s (rule: falls)" % trend),
        f(1, novib(), "vibrato share %s (rule: under 0.15)" % _n(vs, 2)),
    ]
    F["bowed string"] = [
        f(2, None if att is None else 0.04 <= att <= 0.4, "attack %s ms (rule: 40 to 400)" % _ms(att)),
        f(2, None if vs is None else (vs >= 0.3 and (vr is None or 4.5 <= vr <= 7.5)),
          "vibrato share %s at %s Hz (rule: 0.3 or more at 4.5 to 7.5)" % (_n(vs, 2), _n(vr, 1))),
        f(1, None if gp is None else gp >= 0.2, "glide share %s, portamento share %s (rule: 0.2 or more)" % (_n(gs, 2), _n(ps, 2))),
        f(1, None if nh is None else nh >= 8, "%s harmonics within 30 dB (rule: 8 or more)" % _n(nh, 0)),
        f(1, None if noise is None else noise > 0.04, "noise share %s (rule: over 0.04)" % _n(noise, 3)),
        f(1, None if sus is None else sus >= -6, "sustain %s dB re peak (rule: within 6 dB)" % _n(sus, 1)),
    ]
    F["blown reed or brass"] = [
        f(1.5, None if att is None else 0.03 <= att <= 0.12, "attack %s ms (rule: 30 to 120)" % _ms(att)),
        f(2, None if blc is None else blc > 0.4, "centroid/level correlation %s (rule: over 0.4)" % _n(blc, 2)),
        f(1, None if oe is None else oe > 6, "odd-to-even %s dB (rule: over 6, clarinet-like tube)" % _n(oe, 1)),
        f(1, None if s25 is None else s25 > 0.08, "2 to 5 kHz energy share %s (rule: over 0.08, brass-like)" % _n(s25, 3)),
        f(1, None if sus is None else sus >= -6, "sustain %s dB re peak (rule: within 6 dB)" % _n(sus, 1)),
    ]
    F["flute-like"] = [
        f(2, None if nh is None else nh <= 4, "%s harmonics within 30 dB (rule: 4 or fewer)" % _n(nh, 0)),
        f(2, None if noise is None else noise > 0.08, "noise share %s (rule: over 0.08)" % _n(noise, 3)),
        f(1, None if att is None else 0.03 <= att <= 0.15, "attack %s ms (rule: 30 to 150)" % _ms(att)),
        f(1, None if medmidi is None else medmidi > 72, "median note MIDI %s (rule: over 72)" % medmidi),
        f(1, None if sus is None else sus >= -6, "sustain %s dB re peak (rule: within 6 dB)" % _n(sus, 1)),
    ]
    fm_fixed = None if not fm or fm.get("moves_with_f0") is None else (not fm["moves_with_f0"])
    F["sung voice or choir"] = [
        f(2, stem == "vocals", "stem is %s (rule: vocals)" % stem),
        f(1.5, None if vs is None else (vs >= 0.3 and (vr is None or 4.5 <= vr <= 7.5)),
          "vibrato share %s at %s Hz (rule: 0.3 or more at 4.5 to 7.5)" % (_n(vs, 2), _n(vr, 1))),
        f(2, fm_fixed, "formant-like peak %s" % (("at %.0f Hz, cv %.2f, correlation with f0 %.2f" % (fm["peak_hz_median"], fm["cv"], fm["corr_with_f0"])) if fm else "not measurable")),
        f(1, None if noise is None else noise > 0.04, "noise share %s (rule: over 0.04)" % _n(noise, 3)),
        f(1, None if gp is None else gp >= 0.2, "glide share %s, portamento share %s (rule: 0.2 or more)" % (_n(gs, 2), _n(ps, 2))),
        f(0.5, None if poly is None else poly > 1.5, "polyphony %s (rule: over 1.5, choir)" % _n(poly, 1)),
    ]
    F["saw-type synth lead"] = [
        f(2, None if slope is None else -9 <= slope <= -3, "harmonic slope %s dB/octave (rule: -9 to -3)" % _n(slope, 1)),
        f(1.5, None if inh is None else inh < 6, "inharmonicity %s cents (rule: under 6)" % _n(inh, 1)),
        f(1, novib(), "vibrato share %s (rule: under 0.15)" % _n(vs, 2)),
        f(0.5, True, "brightness %s (static or swept both allowed)" % trend),
        f(1, None if oe is None else -8 <= oe <= 2, "odd-to-even %s dB (rule: -8 to 2)" % _n(oe, 1)),
        f(1, None if noise is None else noise < 0.05, "noise share %s (rule: under 0.05)" % _n(noise, 3)),
    ]
    F["square-type synth"] = [
        f(3, None if oe is None else oe > 10, "odd-to-even %s dB (rule: over 10)" % _n(oe, 1)),
        f(1, None if inh is None else inh < 6, "inharmonicity %s cents (rule: under 6)" % _n(inh, 1)),
        f(0.5, novib(), "vibrato share %s (rule: under 0.15)" % _n(vs, 2)),
        f(1, None if sus is None else sus >= -6, "sustain %s dB re peak (rule: within 6 dB)" % _n(sus, 1)),
    ]
    F["sine or sub bass"] = [
        f(3, None if nh is None else nh <= 2, "%s harmonics within 30 dB (rule: 2 or fewer)" % _n(nh, 0)),
        f(1.5, None if medmidi is None else (medmidi < 45 or stem == "bass"), "median note MIDI %s, stem %s (rule: under 45 or bass stem)" % (medmidi, stem)),
        f(1, None if noise is None else noise < 0.05, "noise share %s (rule: under 0.05)" % _n(noise, 3)),
        f(0.5, novib(), "vibrato share %s (rule: under 0.15)" % _n(vs, 2)),
    ]
    F["supersaw stack"] = [
        f(2, None if width is None else width >= 20, "fundamental peak excess width %s cents (rule: 20 or more)" % _n(width, 1)),
        f(1.5, None if beat_share is None else beat_share >= 0.3, "beating in %s of long notes (rule: 0.3 or more)" % _n(beat_share, 2)),
        f(1.5, None if sm is None else sm > -6, "side/mid %s dB (rule: over -6)" % _n(sm, 1)),
        f(1, None if slope is None else -9 <= slope <= -3, "harmonic slope %s dB/octave (rule: -9 to -3)" % _n(slope, 1)),
        f(1, novib(), "vibrato share %s (rule: under 0.15)" % _n(vs, 2)),
    ]
    F["FM or bell-like"] = [
        f(2.5, None if inh is None else inh >= 25, "inharmonicity %s cents (rule: 25 or more)" % _n(inh, 1)),
        f(1.5, None if sus is None else sus <= -10, "sustain %s dB re peak (rule: at most -10)" % _n(sus, 1)),
        f(1, None if att is None else att < 0.025, "attack %s ms (rule: under 25)" % _ms(att)),
        f(0.5, novib(), "vibrato share %s (rule: under 0.15)" % _n(vs, 2)),
    ]
    F["pad"] = [
        f(2, None if att is None else att >= 0.15, "attack %s ms (rule: 150 or more)" % _ms(att)),
        f(2, None if rel is None else rel >= 0.3, "release %s ms (rule: 300 or more)" % _ms(rel)),
        f(1, None if sm is None else sm > -9, "side/mid %s dB (rule: over -9)" % _n(sm, 1)),
        f(1, (name == "chords") or (poly is not None and poly >= 2), "voice rule %s, polyphony %s (rule: chords or pad, or 2 or more)" % (name, _n(poly, 1))),
        f(1, None if lenmed is None else lenmed >= 1.5, "median note length %s s (rule: 1.5 or more)" % _n(lenmed, 2)),
    ]
    return F


def _n(v, d):
    return "n/a" if v is None or (isinstance(v, float) and not np.isfinite(v)) else ("%.*f" % (d, v))


def _ms(v):
    return "n/a" if v is None else "%.0f" % (v * 1000)


def guess_drum(R, name):
    band = name.replace("drums_", "")
    dec = _g(R, "decay_20db_s")
    cen = _g(R, "centroid_hz")
    drop = _g(R, "pitch_drop_hz")
    pk = _g(R, "peak_hz")
    fam = {"low": "kick-like", "mid": "snare, clap or tom-like", "high": "hat, shaker or cymbal-like"}[band]
    ev = "band %s; decay to -10%% amplitude %s ms, centroid %s Hz, peak %s Hz, pitch drop %s Hz" % (band, _ms(dec), _n(cen, 0), _n(pk, 0), _n(drop, 0))
    return dict(families=[(fam, 0.5)], text="%s drum voice (by band only)" % fam, confidence="medium", evidence=ev)


def guess(R, stem, name, prompt_text=""):
    asked = prompt_instruments(prompt_text) if prompt_text else []
    if R["kind"] == "drum":
        g = guess_drum(R, name)
        g["prompt_asked"] = asked
        return g
    meas = ("%d notes, %d alone in their stem, %d measured (%s)" % (R["n_notes"], R["alone_notes"], R["measured_n"], R["measured_on"]))
    if R["measured_on"] != "alone" or R["measured_n"] < 5:
        txt = "undetermined: fewer than 5 isolated notes (%s)" % meas
        return dict(families=[], text=txt, confidence="undetermined", evidence=meas, prompt_asked=asked)
    F = _rules(R, stem, name)
    res = []
    for fam, rules in F.items():
        tot = sum(w for w, c, t in rules)
        mx = sum(w for w, c, t in rules if c is not None)
        sc = sum(w for w, c, t in rules if c)
        if mx >= 0.6 * tot:
            res.append((fam, sc / mx, [(w, c, t) for w, c, t in rules if c is not None], mx / tot))
    if not res:
        return dict(families=[], text="undetermined: too few evaluable measurements (%s)" % meas,
                    confidence="undetermined", evidence=meas, prompt_asked=asked)
    res.sort(key=lambda r: -r[1])
    top = res[0]
    frac = top[1]
    conf = "high" if frac >= 0.75 else "medium" if frac >= 0.55 else "low" if frac >= 0.40 else "undetermined"
    if conf == "high" and top[3] < 0.8:
        conf = "medium"
    fams = [(top[0], top[1])]
    for r in res[1:3]:
        if r[1] >= top[1] - 0.10 and r[1] >= 0.40:
            fams.append((r[0], r[1]))
    if conf == "undetermined":
        return dict(families=fams, text="undetermined: best fit %s at fraction %.2f (%s)" % (top[0], frac, meas),
                    confidence="undetermined", evidence=meas, prompt_asked=asked)
    ev = "; ".join("%s%s" % ("+" if c else "-", t) for w, c, t in top[2])
    text = " or ".join(f for f, _ in fams)
    if top[0] == "blown reed or brass":
        oe = _g(R, "odd_even_db")
        s25 = _g(R, "share_2_5k")
        sub = []
        if oe is not None and oe > 6:
            sub.append("clarinet-like tube")
        if s25 is not None and s25 > 0.08:
            sub.append("brass-like 2-5 kHz energy")
        if sub:
            text += " (%s)" % ", ".join(sub)
    return dict(families=fams, text=text, confidence=conf, evidence=ev + " | " + meas + " | evaluable weight share %.2f" % top[3], prompt_asked=asked,
                all_fractions=[(f, round(fr, 2)) for f, fr, _, _c in res])


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print("usage: guess.py PROMPT_FILE", file=sys.stderr)
        sys.exit(2)
    t = open(sys.argv[1]).read()
    print("the prompt asked for: " + ", ".join(prompt_instruments(t)))
    print("the prompt excluded: " + ", ".join(prompt_instruments(t, negated=True)))
