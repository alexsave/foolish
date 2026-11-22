#!/usr/bin/env python3
"""citymix_beats.py id in.wav out.txt : madmom downbeats, chords and key of one recording, as plain text.

beat <time> <position in bar> / chord <start> <end> <label> / key <label>.
Analysis only (the madmom models are not shipped).  Run with the venv that has madmom."""
import sys, warnings
warnings.filterwarnings('ignore')
import numpy as np
from madmom.features.downbeats import RNNDownBeatProcessor, DBNDownBeatTrackingProcessor
from madmom.features.chords import CNNChordFeatureProcessor, CRFChordRecognitionProcessor
from madmom.features.key import CNNKeyRecognitionProcessor, key_prediction_to_label
import soundfile as sf
import madmom.features.downbeats as mdb


class RaggedNumpy:
    """madmom 0.16 asks numpy for an array of (path, probability) pairs, which numpy 2 refuses without dtype=object."""
    def __getattr__(self, k):
        return getattr(np, k)

    def asarray(self, a, *r, **k):
        try:
            return np.asarray(a, *r, **k)
        except ValueError:
            return np.asarray(a, dtype=object)


mdb.np = RaggedNumpy()
name, wav, out = sys.argv[1:4]
x, sr = sf.read(wav, dtype='float32')
from scipy.signal import resample_poly
mono = resample_poly(x.mean(axis=1), 147, 160) if sr == 48000 else x.mean(axis=1)
from madmom.audio.signal import Signal
sig = Signal((np.clip(mono, -1, 1) * 32767).astype('int16'), sample_rate=44100, num_channels=1)
act = RNNDownBeatProcessor()(sig)
beats = DBNDownBeatTrackingProcessor(beats_per_bar=[3, 4], min_bpm=55, max_bpm=215, fps=100)(act)
with open(out, 'w') as f:
    f.write('id %s\n' % name)
    for t, p in beats:
        f.write('beat %.3f %d\n' % (t, p))
    for s, e, l in CRFChordRecognitionProcessor()(CNNChordFeatureProcessor()(sig)):
        f.write('chord %.3f %.3f %s\n' % (s, e, l))
    f.write('key %s\n' % key_prediction_to_label(CNNKeyRecognitionProcessor()(sig)))
