# The README showcase

`foolish-showcase.mp4` is the 56 second clip the top of the repository README plays.
It is cut from a 19m46s iPhone screen recording of one game of Foolish played with Eva
inside Messages.
The source recording is not in the repository; it was 744 MB.

## What it shows

Three takes, each a single continuous stretch of the recording with nothing cut out of
the middle, separated by a fade to black and back.

1. Her three dots, her 6 of diamonds thrown in and animating down the board, you covering
   it with the king of diamonds, your bubble sending, the board expanding to full screen,
   and the pickup sweep and refill. Twenty seven seconds, one take.
2. Her three dots, her ace covering your 7 of spades, you throwing in a 7 of hearts, your
   bubble sending.
3. Her three dots, her cover, you calling Good, and the bout sweeping off the table.

The point of all three is the same and it is the reason they are long: you watch her type,
her move animates on your open board, and then your reply animates and goes back into the
thread.
A still frame cannot show that, and a short beat does not give it time to land.

## Choosing the takes

An exchange worth showing is one where both people move.
Those are findable rather than guessable, because the typing indicator cycles its dots:
during a typing wait every sampled frame carries a little motion, while a genuinely idle
screen gives a hard zero.
Scanning `tighten`'s frame difference for runs of continuous micro-motion locates every
typing wait in the recording, and the ones followed within a few seconds by a second burst
of motion are the two-sided exchanges.

That found five.
Two were dropped because the hand held eight or nine cards, which squeezes them thin enough
that you cannot read the faces.
Every take here has a hand of five or six.

## How it is rendered

Shot boundaries are in `showcase.edl`, in the format `shared/tools/tighten` reads.
The EDL does not reproduce the file on its own, because three things are not expressible
in it:

- **The crop.** `crop=1290:2616:0:180` takes the top 180px off, which is the status bar:
  the clock, the wifi and battery glyphs, and the red screen recording dot.
  180px is the safe area inset on this device, so it removes the status bar and nothing
  else. It is vertical only; the full width is kept.
- **The scale.** `scale=646:1310`, exactly half, which holds the aspect ratio and lands the
  file under 4 MB.
- **The fades.** Each take fades in over 0.3s and out over 0.3s, the first fades in over
  0.45s, and the last holds its final frame for 1.6s before fading out over 0.8s.
  Without that hold the clip reads as though the file were truncated.

Render each EDL line to its own file at 60fps with the crop and scale above, then apply the
fades and concatenate.

## Two things in the source to stay away from

A private conversation sits at the top of the transcript.
It is on screen for the first five seconds, and it comes back whenever the transcript
scrolls far enough: around 64-66s, and again around 847-848s.
None of it is in the clip, and no take should be widened into it.

A "20% Battery, tap to turn on Low Power Mode" banner is on screen from 915.2s to 920.8s.
It hangs below the status bar, so the 180px crop only takes its top half.
That window has to be cut out rather than cropped away.

## Why the README shows a GIF and not the video

GitHub will not play an mp4 in a README unless it is hosted on a very short list of its
own domains, and the reason is a Content-Security-Policy header rather than anything about
the file.
`curl -sI https://github.com/<owner>/<repo>` and read `media-src`; today it is:

```
media-src github.com user-images.githubusercontent.com secured-user-images.githubusercontent.com
          private-user-images.githubusercontent.com
          github-production-user-asset-6210df.s3.amazonaws.com
          gist.github.com github.githubassets.com
```

`raw.githubusercontent.com` and `release-assets.githubusercontent.com` are both in `img-src`
and **neither is in `media-src`**.
So a committed mp4 and a release asset are equally useless to a `<video>` tag: the browser
refuses the request before the content type matters.
Both of those also serve `application/octet-stream` rather than `video/mp4`, which would
have been a second problem had the first not stopped it.

The only URL that works is the one GitHub mints when you drag a file into a comment box,
`https://github.com/user-attachments/assets/...`, because it redirects into the S3 bucket
that is on the list.
There is no API for creating one, so it cannot be scripted.

A GIF has no such restriction: images go through `camo.githubusercontent.com`, they render
inline, and an animated one autoplays and loops without controls.
That is what the README uses, and it is why the clip there is one 12 second exchange rather
than the whole 56 seconds.

`foolish-showcase.gif` is take one, 400px wide at 15fps, with a diff-based palette:

```
ffmpeg -ss 125.80 -t 11.80 -i SOURCE.MP4 -vf \
  "fps=15,crop=1290:2616:0:180,scale=400:-2:flags=lanczos,split[a][b];\
   [a]palettegen=max_colors=256:stats_mode=diff[p];\
   [b][p]paletteuse=dither=bayer:bayer_scale=5:diff_mode=rectangle" \
  -loop 0 foolish-showcase.gif
```

`stats_mode=diff` and `diff_mode=rectangle` are what keep it to 2.3 MB.
The table does not move between frames, so both of them spend the palette and the frame
area on the cards that do.

## The whole game

`tighten` will also render the entire game with the waiting removed, which comes to about
five minutes:

```
shared/tools/tighten/build/tighten cut SOURCE.MP4 -o full-game.mp4 \
    --min 1.4 --crop-top 0.064 --tail 1.2 --height 1310
```

`--min 1.4` is the important flag.
A segment exactly twice `--pad` long is one sample above the threshold, which is a fifth of
a second of flicker rather than a move, and the last four segments of this recording are
all of that kind.
Left in, they make the cut appear to freeze and then stop.
