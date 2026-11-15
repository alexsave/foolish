#!/usr/bin/env python3
"""v7_rng.py - the ONE random source of the v7 long samples. SEED is the only value that decides every draw: fix it to a constant
and every v7 sample is bit for bit the same. Each sample gets its own stream, named by its file name (crc32 of the name spawns
the stream), so adding or removing a sample never changes another one. No other module may use numpy.random or random
(test_extend_v7.py greps for it)."""
import zlib
import numpy as np

SEED = 7_000_003


def stream(name):
    """A fresh numpy Generator for the sample called NAME (file name without folder)."""
    return np.random.Generator(np.random.PCG64(np.random.SeedSequence(SEED, spawn_key=(zlib.crc32(name.encode()),))))
