"""wa03_events.py: notes TSV -> replica score.txt of exact-time EVENTS lines.

Input TSV (no header): part (piano|cello|pedal), start_s, end_s, midi_pitch, velocity 0..1.
Output: TEMPO 120 (a beat is exactly 0.5 s, so beat = 2 * seconds and nothing is quantised), one SECTION long enough for the last note,
EVENTS PIANO / CELLO / PEDAL lines of at most 40 events.  Run with python3 -I.
usage: wa03_events.py NOTES.tsv score.txt
"""
import sys

BPM = 120.0


def read_notes(path):
    out = []
    for ln in open(path):
        if not ln.strip() or ln.startswith('#'):
            continue
        p, s, e, m, v = ln.split('\t')[:5]
        out.append((p.strip().lower(), float(s), float(e), float(m), float(v)))
    return out


def to_score(notes):
    k = BPM / 60.0
    end = max(e for _, _, e, _, _ in notes)
    bars = int(end * k / 4) + 2
    lines = ['TEMPO %d' % BPM, 'SECTION CLIP BARS %d KEY C' % bars]
    for part in ('piano', 'cello', 'pedal'):
        ev = sorted((s, e, m, v) for p, s, e, m, v in notes if p == part)
        for i in range(0, len(ev), 40):
            toks = ['%.6f,%.3f,%.6f,%.4f' % (s * k, m, (e - s) * k, v) for s, e, m, v in ev[i:i + 40]]
            lines.append('EVENTS %s: %s' % (part.upper(), ' '.join(toks)))
    return '\n'.join(lines) + '\n'


if __name__ == '__main__':
    open(sys.argv[2], 'w').write(to_score(read_notes(sys.argv[1])))
