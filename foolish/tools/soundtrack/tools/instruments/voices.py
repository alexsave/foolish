#!/usr/bin/env python3
"""voices.py - split the transcribed notes of each stem, inside one time window, into VOICES (instrument candidates).

What it does: for one clip and one window it takes the basic-pitch note CSVs of the bass, other and vocals stems and
the drums stem audio, and returns (and prints) the voices, each with the rule that made it:
  bass stem   : one voice ("bass stem: one voice").
  vocals stem : one voice if the vocals-stem RMS inside the window is within 15 dB of the mix RMS, else none.
  other stem  : at most four voices, notes assigned once, in this order of precedence:
      chords or pad : notes that start in a group of 3 or more notes within 40 ms of each other, or notes longer than
                      1.5 beats that overlap another note of the stem by more than 50 ms;
      short notes   : of the rest, notes under 0.2 s (plucks, arpeggios, stabs);
      top line      : of the rest, notes that are the highest sounding note of the stem at their onset (monophonic line);
      low-mid line  : of the rest, notes more than 12 semitones under the median pitch of the top line.
      Notes that fit none of these are listed as "unassigned".  A voice needs at least --min-notes notes (default 6).
  drums stem  : three voices by the spectral band of each onset (low under 150 Hz, mid 150 Hz to 2 kHz, high above
      5 kHz), classified by window_report.drum_onsets (the same classifier the window reports use); needs 6 onsets.
Notes shorter than 60 ms are ignored (the transcriber's own floor is about 128 ms).
Inputs (CLI): --clip NAME --stems-dir DIR (bass.wav drums.wav other.wav vocals.wav) --csv-dir DIR
  (files NAME_bass_basic_pitch.csv, NAME_other_basic_pitch.csv, NAME_vocals_basic_pitch.csv) --mix MIXFILE
  --start S --end E|end --bpm BPM (beat length for the 1.5 beat rule; default 120).
Output: one line per voice on stdout: id-less summary "stem<TAB>name<TAB>n_notes<TAB>rule".
Example: voices.py --clip se_12_song_traditional_fusion --stems-dir stems/htdemucs/se_12_song_traditional_fusion \\
  --csv-dir transcribe/3_seoul --mix se_12_song_traditional_fusion.mp3 --start 50 --end 105 --bpm 150
Failure modes: basic-pitch ghost notes and merged notes pass through (a voice can contain other instruments); the
  register rules are statistical, so an arpeggio that overlaps chords lands in "chords or pad"; a window with fewer than
  6 notes in a stem yields no voice for that stem; the drum classes are per-onset spectral-flux shares.
  Deterministic; no randomness.
"""
import argparse
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "transcribe"))
import envelopes as env  # noqa: E402

MIN_NOTE_S = 0.06


def load_stem(path):
    """(stereo float32 array [n, 2], sample rate) of a wav stem; mono input is duplicated to two channels."""
    from scipy.io import wavfile
    sr, x = wavfile.read(path)
    if x.dtype == np.int16:
        x = x.astype(np.float32) / 32768.0
    elif x.dtype == np.int32:
        x = x.astype(np.float32) / 2147483648.0
    else:
        x = x.astype(np.float32)
    if x.ndim == 1:
        x = np.stack([x, x], axis=1)
    return x, sr


def rms_db(x):
    return 20 * np.log10(np.sqrt(np.mean(np.asarray(x, np.float64) ** 2) + 1e-20))


def window_notes(notes, ws, we):
    """Notes with onset in [ws, we) and length of at least 60 ms."""
    if len(notes) == 0:
        return np.zeros((0, 4))
    m = (notes[:, 0] >= ws) & (notes[:, 0] < we) & (notes[:, 1] - notes[:, 0] >= MIN_NOTE_S)
    return notes[m]


def split_other(notes, beat, min_notes=6):
    """Split an other-stem note array into (voices, unassigned). Each voice is (short_name, rule_text, note_array)."""
    n = len(notes)
    if n == 0:
        return [], notes
    on, off, pitch = notes[:, 0], notes[:, 1], notes[:, 2]
    dur = off - on
    # onset groups: three or more notes whose onsets lie within 40 ms of the group's first onset
    order = np.argsort(on, kind="stable")
    in_chord = np.zeros(n, bool)
    i = 0
    while i < n:
        j = i
        while j + 1 < n and on[order[j + 1]] - on[order[i]] <= 0.040:
            j += 1
        if j - i + 1 >= 3:
            in_chord[order[i:j + 1]] = True
        i = j + 1
    # long notes that overlap others
    long_overlap = np.zeros(n, bool)
    for k in range(n):
        if dur[k] > 1.5 * beat:
            ov = (np.minimum(off[k], off) - np.maximum(on[k], on)) > 0.05
            ov[k] = False
            long_overlap[k] = ov.any()
    chord = in_chord | long_overlap
    rest = ~chord
    short = rest & (dur < 0.2)
    rest2 = rest & ~short
    # top line: highest sounding note of the whole stem at the note's onset
    top = np.zeros(n, bool)
    for k in np.where(rest2)[0]:
        sounding = (on <= on[k] + 1e-9) & (off > on[k] + 0.02)
        top[k] = pitch[k] >= pitch[sounding].max() - 1e-9
    rest3 = rest2 & ~top
    low = np.zeros(n, bool)
    if top.sum() >= 1:
        ref = np.median(pitch[top]) - 12
        low = rest3 & (pitch < ref)
    assigned = chord | short | top | low
    voices = []

    def add(name, mask, rule):
        if mask.sum() >= min_notes:
            voices.append((name, rule % int(mask.sum()), notes[mask]))
        else:
            assigned[mask] = False

    add("chords", chord, "other stem; chords or pad: notes starting 3 or more together within 40 ms, or notes longer "
        "than 1.5 beats overlapping another note by over 50 ms (%d notes)")
    add("short", short, "other stem; short notes: notes under 0.2 s that are not in a chord group (%d notes)")
    add("top", top, "other stem; top line: highest sounding note of the stem at its onset, not chord, not short (%d notes)")
    add("lowmid", low, "other stem; low-mid line: not the top note, more than 12 semitones under the median top-line "
        "pitch (%d notes)")
    return voices, notes[~assigned]


def split_drums(x, sr, ws, we, min_notes=6):
    """Drum voices low/mid/high from window_report.drum_onsets: list of (name, rule, note_array(onset, onset+0.1, 0, 1))."""
    import window_report as wr
    ons = [(t, c) for t, c in wr.drum_onsets(x, sr, ws, we) if ws <= t < we]
    groups = {"low": [], "mid": [], "high": []}
    for t, c in ons:
        if "L" in c:
            groups["low"].append(t)
        if "H" in c:
            groups["high"].append(t)
        if "M" in c:
            groups["mid"].append(t)
    out = []
    rules = {"low": "low band: onset energy gain under 150 Hz is over half of the gain (kick-like)",
             "mid": "mid band: 150 Hz to 2 kHz gain leads (snare, clap, tom-like)",
             "high": "high band: above 5 kHz gain leads, or at least 0.35 alongside a low onset (hat, shaker-like)"}
    for k in ("low", "mid", "high"):
        if len(groups[k]) >= min_notes:
            a = np.array([[t, t + 0.1, 0.0, 1.0] for t in groups[k]])
            out.append(("drums_" + k, "drums stem; %s (%d onsets)" % (rules[k], len(a)), a))
    return out


def split_all(stem_notes, audio, sr, mix_mono, ws, we, bpm=120.0, min_notes=6, vocals_db=-15.0):
    """Voices of one clip window.
    stem_notes: dict 'bass','other','vocals' -> note arrays (onset, offset, midi, velocity).
    audio: dict stem name -> stereo array [n, 2]; sr: sample rate; mix_mono: mono mix at the same rate.
    Returns a list of dicts: stem, name, rule, notes, kind ('pitched' or 'drum'); plus a dict of notes about what was left out."""
    beat = 60.0 / bpm
    a, b = int(ws * sr), int(we * sr)
    left = {}
    out = []
    bn = window_notes(stem_notes.get("bass", np.zeros((0, 4))), ws, we)
    if len(bn) >= min_notes:
        out.append(dict(stem="bass", name="bass", kind="pitched", notes=bn,
                        rule="bass stem: one voice (%d notes)" % len(bn)))
    else:
        left["bass"] = "only %d notes in the window (need %d)" % (len(bn), min_notes)
    vn = window_notes(stem_notes.get("vocals", np.zeros((0, 4))), ws, we)
    if "vocals" in audio:
        gap = rms_db(audio["vocals"][a:b].mean(axis=1)) - rms_db(mix_mono[a:b])
        if gap >= vocals_db and len(vn) >= min_notes:
            out.append(dict(stem="vocals", name="vocals", kind="pitched", notes=vn,
                            rule="vocals stem: one voice because its RMS is %.1f dB against the mix (threshold %.0f dB%s), %d notes" % (gap, vocals_db, "" if vocals_db == -15.0 else ", overridden from -15", len(vn))))
        elif gap < vocals_db:
            left["vocals"] = "vocals stem RMS is %.1f dB against the mix (more than %.0f dB under): no voice" % (gap, -vocals_db)
        else:
            left["vocals"] = "vocals stem within threshold of the mix (%.1f dB) but only %d notes (need %d)" % (gap, len(vn), min_notes)
    on_ = window_notes(stem_notes.get("other", np.zeros((0, 4))), ws, we)
    vs, un = split_other(on_, beat, min_notes)
    for name, rule, arr in vs:
        out.append(dict(stem="other", name=name, kind="pitched", notes=arr, rule=rule))
    left["other_unassigned"] = "%d of %d other-stem notes in no voice" % (len(un), len(on_))
    if "drums" in audio:
        for name, rule, arr in split_drums(audio["drums"].mean(axis=1), sr, ws, we, min_notes):
            out.append(dict(stem="drums", name=name, kind="drum", notes=arr, rule=rule))
    return out, left


def clip_stem_notes(csv_dir, clip):
    d = {}
    for s in ("bass", "other", "vocals"):
        p = os.path.join(csv_dir, "%s_%s_basic_pitch.csv" % (clip, s))
        d[s] = env.read_notes(p) if os.path.exists(p) else np.zeros((0, 4))
    return d


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--clip", required=True)
    ap.add_argument("--stems-dir", required=True)
    ap.add_argument("--csv-dir", required=True)
    ap.add_argument("--mix", required=True)
    ap.add_argument("--start", type=float, default=0.0)
    ap.add_argument("--end", default="end")
    ap.add_argument("--bpm", type=float, default=120.0)
    ap.add_argument("--min-notes", type=int, default=6)
    a = ap.parse_args()
    audio, sr = {}, None
    for s in ("bass", "drums", "other", "vocals"):
        audio[s], sr = load_stem(os.path.join(a.stems_dir, s + ".wav"))
    mix, msr = env.load_audio(a.mix, sr)
    we = len(mix) / sr if a.end == "end" else float(a.end)
    vs, left = split_all(clip_stem_notes(a.csv_dir, a.clip), audio, sr, mix, a.start, we, a.bpm, a.min_notes)
    for v in vs:
        print("%s\t%s\t%d\t%s" % (v["stem"], v["name"], len(v["notes"]), v["rule"]))
    for k, t in left.items():
        print("# %s: %s" % (k, t))


if __name__ == "__main__":
    main()
