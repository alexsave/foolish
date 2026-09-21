# tighten

Cut the dead time out of a screen recording.

A twenty minute recording of a turn-based game is mostly a still image.
You play, you wait for the other person, you play again.
`tighten` finds the parts where something moves, keeps those, and throws the waiting away.

The real recording this tool was written against is nineteen minutes and forty six seconds long.
At the defaults it tightens to five minutes and thirty two seconds, which is 28% of it.
The other 72% is a phone screen holding perfectly still.

## Build

```
make
```

One C file and libc.
Everything that touches video is `ffmpeg`, run as a child process, so there is nothing to link and nothing to install but ffmpeg itself.
`ffmpeg` and `ffprobe` have to be on `PATH`.

## Why the arithmetic is not in C

The motion metric is a frame difference: take each sampled frame, subtract the one before it, and average the absolute value over the picture.
A still frame scores zero, a card sliding across the table scores a few units, a full screen transition scores tens.

None of that is computed here.
The scan pass is one ffmpeg invocation whose filtergraph does the whole job:

```
fps=5,crop=...,scale=72:-,format=gray,tblend=all_mode=difference,signalstats,metadata=print:key=lavfi.signalstats.YAVG:file=-
```

`tblend=all_mode=difference` is |a-b| per pixel, `signalstats` averages it, and `metadata=print` writes the average out as text.
What crosses into this program is one number per sampled frame and nothing else.

That is the whole design decision.
Pulling raw frames into C would mean linking libavcodec, owning a decode loop, and carrying pixel format handling for whatever a phone happens to record this year, all to compute a mean that ffmpeg already computes correctly and fast.
The tool is therefore a scheduler for three ffmpeg invocations and the segment algebra between them, which is the part ffmpeg has no opinion about.

The diff pass runs on a 72 pixel wide grayscale copy at 5 frames a second.
That is small enough that a twenty minute source is bound by decoding rather than by the filter, and still wide enough that one card moving registers.

## The workflow

Three commands, in this order, and the middle one has a human in it.

### 1. sheet: find out what is in the recording

```
tighten sheet IN.mp4 --from 0 --to 120 --step 2 -o sheet.jpg
```

A contact sheet of a time window.
The step is fixed, so tile `i`, counted across the rows from zero, is at `from + i * step` seconds.
Every time you point at a tile you are naming a timestamp, and those timestamps are what you will type into the EDL later.

### 2. scan: get the segments and an EDL

```
tighten scan IN.mp4 -o cut.edl
```

Prints every segment it found, and the total it would keep:

```
    0   0:04.40 -> 0:14.00     9.60s
    1   0:41.60 -> 0:43.20     1.60s
    ...
   74  19:40.40 -> 19:41.60     1.20s

  75 segments, 5:32.00 kept of 19:46.00 (28%)
```

Read the segment list before you read the summary.
The summary tells you the knobs are roughly right; the list tells you whether they are actually right.

### 3. edit the EDL

This is the seam between "dead time removed" and "a showcase".
The EDL is plain text, two numbers a line.
Delete the segments you do not want, reorder the ones you keep, trim the numbers by hand if a shot starts half a second late.

### 4. cut: render it

```
tighten cut IN.mp4 --edl cut.edl -o OUT.mp4
```

Renders the segments the EDL names, in the order the EDL names them.
`tighten cut IN.mp4 -o OUT.mp4` with no `--edl` scans and renders in one go, which is the right thing when all you want is the dead time gone.

## The scan knobs, and how to tell when one is wrong

```
--thr F        motion threshold, mean absolute pixel difference 0-255 (0.30)
--gap F        bridge stretches of stillness shorter than this, seconds (3.00)
--pad F        stillness kept on each side of a segment, seconds (0.60)
--min F        drop segments shorter than this, seconds (1.00)
--skip-top F   ignore this fraction of the height at the top (0.05)
--probe-fps F  sampling rate of the diff pass (5)
```

### --thr

The threshold every sampled frame is compared against.

Too low and the scan keeps time that is not motion.
A phone status bar clock ticking over, a wifi glyph redrawing, and plain compression noise on a static frame all score somewhere above zero, and at a low enough threshold they read as action.
You can see this in the output before you see it in the video: the segment count climbs into the hundreds, the kept fraction climbs past half, and segments appear at moments when you know nothing happened.

Too high and the quiet moves vanish.
A card sliding in from the edge of a dark table is a small fraction of the picture changing, and it is exactly the shot worth keeping.
The tell is a cut that jumps from one board state to a different one with nothing in between.

`--skip-top` exists so that the threshold does not have to be raised just to survive the status bar.
It crops the top of the picture out of the scan only.
The picture itself is untouched unless you also ask for `--crop-top`.

### --gap

How long a stillness has to be before it counts as dead time rather than a beat inside one action.

Too small and the tool cuts through the middle of a single move.
A card lands, nothing moves for a moment, the hand re-sorts itself: with a small gap that is two segments with a splice in it, and the splice lands exactly where a viewer is still looking at the card.
The tell is a pair of segments a second or two apart that obviously belong together, and a cut that feels like it stutters.

Too large and the dead time comes back, because two real actions a long way apart get bridged into one segment with the waiting still inside it.
The tell is a segment far longer than any action in the game takes.

### --pad

Stillness deliberately kept on each side of a segment.

This is the knob that stops a cut landing on the first moving frame.
Without it every shot begins with the card already in flight and ends the instant it settles, which reads as a machine cutting rather than an edit.
The padding also merges segments that padding makes touch, which is `--gap` bridging by another route.

If a cut feels abrupt at the joins, `--pad` is the first thing to raise.
If the kept fraction is high and every segment is suspiciously similar in length, it is the first thing to look at, for the reason below.

### --min, and the 2 x --pad tell

Drop segments shorter than this.

There is one number to watch for here, and it is `2 * --pad`.
A segment whose length comes out at exactly twice the padding is a segment with nothing in the middle: one single sample crossed the threshold, and everything else in it is padding.
One sample at the default 5 frames a second is a fifth of a second of change, which is never a real action.
It is one frame of flicker: a typing indicator blinking, a badge repainting, a cursor, an avatar loading.

At the defaults that length is 1.20s, and the real nineteen minute recording has twelve segments of exactly 1.20s out of seventy five.
Worse, its last four segments are all of them, one after another.
A run of those at the end of a recording is what makes a cut look like it freezes and then stops, because the last thing the viewer is given is four separate one second holds on a still screen.

So when the source has that kind of idle chatter in it, set `--min` above `2 * --pad`.
With the defaults that means `--min 1.4` or higher, and every segment that survives is then one that contains at least two moving samples.
On that recording it drops seventeen of the seventy five and takes the flicker at the end with them.

### --probe-fps

How often the diff pass samples.

Raising it costs scan time and buys resolution at the edges of a segment.
Lowering it risks missing a short move outright, because one that begins and ends between two samples never registers at all.
It also coarsens the 2 x `--pad` tell above, since the length of a one sample segment is the only thing that makes flicker recognisable.
It is the knob least worth touching.

## The render options

```
--height N     scale the output to this height (default: the source size)
--fps F        output frame rate (default: the source rate, capped at 60)
--codec C      h264 (default, plays everywhere) or hevc (VideoToolbox, faster)
--crf N        libx264 quality (22)
--preset P     libx264 preset (medium)
--quality N    VideoToolbox q:v (60)
--crop-top F   crop this fraction off the top of the picture (0)
--tail F       hold the last shot this many seconds longer (0)
--audio        keep the audio track
--dry-run      print the commands, the filtergraph and the concat list,
               and render nothing
```

### --crop-top

Takes a fraction off the top of the output picture, which on a phone recording is the status bar with the clock, the wifi glyph and the battery.

It is the render side twin of `--skip-top`, and the two are independent on purpose.
`--skip-top` decides what the scan looks at, `--crop-top` decides what the viewer sees.
Ignoring the clock when detecting motion but keeping it in the shot is the usual combination, because the clock is honest and the flicker is not.
The reverse is also useful: scan the whole picture, then crop the bar out of the final cut.

The crop happens before any `--height` scale, so the scale sees the picture you are actually shipping and the aspect ratio comes out right.
The cropped height is rounded down to an even number, because 4:2:0 video has no way to express an odd one.

### --tail

Extends the final segment in render order by this many seconds, clamped to the end of the source.

A cut that ends the instant the last pixel stops moving feels truncated.
The eye has not finished the action yet, and a second or two of held frame is how you end a shot.
The tail goes on the last segment in *render* order, which for an edited EDL is the last line of the file rather than the last one in the source.

If there is not that much source left after the final segment, tighten takes what there is and says so:

```
  tail: only 0.50s of the requested 5.00s left before the end of the source
```

### Audio

Audio is dropped by default.
A phone screen recording of a silent app still carries a real AAC track of nothing at all, and shipping it is bytes and a speaker icon for no gain.
`--audio` keeps it, except that a track which is digital silence in every frame is dropped anyway, and tighten says so when it does.

## The EDL format

Plain text.
Blank lines and lines starting with `#` are comments.
Every other line is two numbers, seconds, in and out, and anything after them on the line is ignored, which is where the comment with the duration goes.

```
# tighten edl - in and out points in seconds, one segment a line.
# ...
# source: ScreenRecording.MP4
# scan:   thr=0.30 gap=3.00 pad=0.60 min=1.00 skip-top=0.050
# kept:   75 segments, 332.0s of 1186.0s

    4.40    14.00   #   9.60s
   41.60    43.20   #   1.60s
  129.20   150.80   #  21.60s
```

Three things follow from that format, and they are the point of it:

- Deleting a line drops a shot.
- Moving a line moves the shot, because the cut is rendered in the order written here and not in source order.
- Repeating a line repeats the shot.

The header comments record the scan the EDL came out of, so an EDL you find later still says what produced it.

## A worked example

The recording is a 19:46 iPhone screen recording of a card game, 1290x2796, HEVC, 60fps, 744 MB.

```
$ tighten scan ScreenRecording.MP4 -o cut.edl
  scanning ScreenRecording.MP4 (19:46.00, 1290x2796) ...
    0   0:04.40 -> 0:14.00     9.60s
    ...
   74  19:40.40 -> 19:41.60     1.20s

  75 segments, 5:32.00 kept of 19:46.00 (28%)

  wrote cut.edl
```

Five and a half minutes of a twenty minute recording contain every moment where anything at all happened.
That is `cut.edl`, and rendering it as it stands gives a watchable but shapeless video, because seventy five shots is not an edit.

So the EDL gets opened.
Four segments are kept out of the seventy five, the long rally is moved to the front and the opening deal to the back, and the rest of the file is deleted:

```
# tighten edl - hand trimmed down from the 75 the scan found.
# The long rally first, the opening deal last.
  129.20   150.80   #  21.60s
  901.00   921.60   #  20.60s
  844.20   862.00   #  17.80s
    4.40    14.00   #   9.60s
```

```
$ tighten cut ScreenRecording.MP4 --edl showcase.edl \
      --crop-top 0.05 --tail 1.5 --height 960 -o showcase.mp4
  showcase.edl: 4 segments, 69.6s
  showcase.mp4: 1:11.10  466x960  3.5 MB
```

Seventy one seconds, status bar gone, ending on a held frame rather than on a cut.
The 744 MB source is 3.5 MB, and the render took 27 seconds, because seeking to four segments only ever decodes those four.

## How the cut is rendered

Each segment is seeked to and encoded on its own, and the pieces are joined with ffmpeg's concat demuxer.

The obvious alternative is one `select` filter with an OR of `between()` calls, one decode for the whole cut.
It is a trap.
`select` is a predicate over one pass through the file, so it can only ever emit frames in source order, and a cut that cannot put the tenth shot first is not an edit, it is a shortened recording.

The concat demuxer can nominally do the whole job by itself, with an `inpoint` and `outpoint` per entry and no per-segment pass at all.
Its seek is not frame accurate: it hands back everything from the preceding keyframe, which on a screen recording with a sparse keyframe interval is seconds of dead time in front of every single shot.
That is precisely what this tool exists to remove.

Rendering per segment is also the faster way round on a long source.
A select pass decodes all twenty minutes; seeking decodes only the five that survive.
The join itself is a stream copy, because every piece came out of the same encoder at the same size, so nothing is encoded twice.

`--dry-run` prints every command, the filtergraph and the concat list, and renders nothing.

## Tests

```
./test.sh
```

No fixture is checked in.
The test generates its own source with ffmpeg: 25 seconds of 320x240, three flat colours with two bursts of `testsrc2` spliced in, so that nothing moves except during 5.0s to 8.0s and 15.0s to 19.0s.
Every number it asserts is a number the test chose, which makes the assertions statements about tighten rather than about a recording somebody made once.

It checks that the scan finds those two bursts and where their edges land, that a hand written EDL renders to the right duration and in the EDL's order rather than the source's, that `--crop-top` moves the output height by the right amount and rounds to even, and that `--tail` lengthens the cut and clamps at the end of the source.
The order check works by rendering two differently coloured stills out of source order and reading the colour back out of the rendered frames, because a render that silently produces the right duration in the wrong order still exits 0.
