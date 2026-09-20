#!/usr/bin/env python3
"""Every frame `rig.sh tween` measures is placed at its own time in the movie.

    python3 lib/test_window.py

The movie is made to look like a `simctl io recordVideo` take - VARIABLE rate,
and with a run of frames missing where the screen was still - and every frame
draws its own index in binary, so a frame read back out says which one it is
and the time the rig gave it can be checked against the time it really had.
Nothing in it is trusted from ffprobe: the truth is the index in the pixels.
"""
import glob, os, shutil, subprocess, sys, tempfile, unittest
import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
# window.sh is shared now (it knows nothing about any game); tween is not.
SHLIB = os.path.join(HERE, "..", "..", "..", "..", "shared", "rig", "lib")
sys.path.insert(0, HERE)
import tween  # noqa: E402

BITS, COL, W, H = 10, 60, 600, 40   # wider than the rig's 544px crop
FPS, SECS = 60, 5.0
GAP = (150, 179)                     # frames the "still screen" never sent


def pts_of(n):
    """When frame `n` is shown: 60fps with an uneven beat, like a real take."""
    return (n + 0.35 * (n % 3)) / FPS


class WindowTimes(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.dir = tempfile.mkdtemp(prefix="rigwin")
        cls.movie = os.path.join(cls.dir, "take.mp4")
        # Column k is white when bit k of the frame's index is set.
        lum = "255*mod(floor(N/pow(2\\,floor(X/%d)))\\,2)" % COL
        vf = ",".join([
            "geq=lum='%s':cb=128:cr=128" % lum,
            "settb=1/60000",
            "setpts='(N+0.35*mod(N\\,3))/(%d*TB)'" % FPS,
            "select='not(between(n\\,%d\\,%d))'" % GAP,
        ])
        subprocess.run(
            ["ffmpeg", "-v", "error", "-f", "lavfi",
             "-i", "color=black:s=%dx%d:r=%d:d=%s" % (W, H, FPS, SECS),
             # The source's 1/60 timebase, in the filter AND the encoder, would
             # round the uneven beat back onto a 60fps grid.
             "-vf", vf, "-fps_mode", "passthrough", "-enc_time_base", "1/60000",
             "-video_track_timescale", "60000",
             "-c:v", "libx264", "-qp", "0", "-pix_fmt", "yuv444p", cls.movie],
            check=True)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.dir, ignore_errors=True)

    def take(self, ss, t):
        d = tempfile.mkdtemp(dir=self.dir)
        subprocess.run([os.path.join(SHLIB, "window.sh"), self.movie, d, str(ss), str(t), "544"],
                       check=True)
        frames = sorted(glob.glob(os.path.join(d, "f*.ppm")))
        return frames, tween.frame_times(d, len(frames))

    def index_of(self, path):
        a = np.asarray(Image.open(path).convert("L")).astype(int)
        row = a[a.shape[0] // 2]
        return sum(1 << k for k in range(BITS) if (k + 0.5) * COL < row.shape[0]
                   and row[int((k + 0.5) * COL)] > 128)

    def check(self, ss, t):
        frames, times = self.take(ss, t)
        self.assertGreater(len(frames), 10, "the window extracted almost nothing")
        self.assertEqual(len(times), len(frames), "one time per frame")
        wrong = []
        for i, (f, got) in enumerate(zip(frames, times)):
            n = self.index_of(f)
            if abs(got - pts_of(n)) > 0.001:
                wrong.append("frame %d is movie frame %d at %.3fs, given %.3fs"
                             % (i + 1, n, pts_of(n), got))
        self.assertEqual(wrong, [], "%d of %d frames mistimed, e.g. %s"
                         % (len(wrong), len(frames), wrong[:3]))

    def test_the_rigs_window(self):
        """The shipping window: skips the lead, stops well before the end."""
        self.check(1.3, 2.2)

    def test_a_window_across_the_still_gap(self):
        """No frames for half a second in the middle of the window."""
        self.check(2.0, 1.5)

    def test_a_window_that_starts_while_the_screen_is_still(self):
        """How a real take starts: in the lead, where frames are sparse."""
        self.check(2.7, 1.0)

    def test_a_window_that_runs_to_the_end(self):
        self.check(4.0, 2.2)

    def test_the_whole_movie(self):
        self.check(0, 10)


class Mismatch(unittest.TestCase):
    def test_times_that_do_not_match_the_frames_are_refused(self):
        """A whole movie's times beside a window's frames has no right answer."""
        d = tempfile.mkdtemp(prefix="rigwin")
        try:
            with open(os.path.join(d, "times.txt"), "w") as fh:
                fh.write("".join("%.6f\n" % (i / 60) for i in range(200)))
            with self.assertRaises(SystemExit):
                tween.frame_times(d, 133)
        finally:
            shutil.rmtree(d, ignore_errors=True)


if __name__ == "__main__":
    unittest.main(verbosity=2)
