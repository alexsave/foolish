# The auto-collapse's bottom edge

The box's bottom edge is not supposed to move.
It rests at 921.7pt before an auto-collapse and settles at 915.0pt after, both on a 6.9" phone, and the 6.7pt between them is the only travel the design asks for.
What it actually does is leave that band for ~400ms and come back.

This is the measurement of that, what was tried against it, and what is left.

## The measure

> **STALE, 2026-09-20.** `lib/mse.py`, `lib/msecmp.py`, `shots/mse_run.sh` and
> `shots/mse_sweep.sh` are no longer in the tree - only `lib/tween.py` survives.
> The measurements below stand; the commands do not run. Read this as the
> record of what was found, not as a runbook.

`ios/Tools/rig/lib/mse.py` scored a set of filmed collapses.
Every frame is scored by how far the bottom edge is outside the band, zero inside it; the takes are averaged first and the result squared and summed, so noise that is not repeatable cancels and only the excursion the design really has survives.

```
FOOLISH_SIM=<udid> ios/Tools/rig/shots/mse_run.sh <name> 20
FOOLISH_KNOBS="lead=0.006" ios/Tools/rig/shots/mse_sweep.sh resp 12 0.300 0.338 0.380
python3 ios/Tools/rig/lib/msecmp.py out.png a.json b.json
```

Twenty takes cost about 3.5 minutes.
Its resolution is worth knowing before reading anything below as precise: the old configuration scored 3322.3 and 3371.3 on two separate runs on two separate builds, but 6ms scored 1297 driven by the debug knob and 725 as the compiled constant - the same configuration, twenty takes each.
Individual takes inside one run range 466 to 3542.
So the measure separates 10ms from 6ms beyond any doubt, and does NOT separate 2ms from 6ms; treat the small-lead rows as one band, not as an ordering.

Two numbers ride alongside it, and both exist because the MSE alone can be read the wrong way.

**Judder**, the mean frame-to-frame movement of the edge, measured per take on the raw ~90Hz frames.
The edge does not overshoot smoothly - it alternates about ±14pt every frame while the top edge slides, which is the whole box translating vertically at the composite rate.
Averaging takes whose phase differs cancels exactly that, so a change that only re-phased the judder would read as a win.

**Cut-off frames**, the frames where the box was drawn taller than the screen and its bottom edge - with the hand sitting on it - went off the bottom.
`tween.py` used to return nothing for those and `mse.py` skipped them, which was backwards in the most dangerous way: the frames that go wrong were the frames that stopped being counted, so the changes that cause them scored as improvements.
A top bar with no bottom bar is now a defect, not missing data.

## The mechanism

The box is top-glued to the drawer's descending edge, so

```
bottom(t) = 921.7 - 6.7·p(t) - 535.4·[ p_ours(t + lead) - p_host(t) ]
```

where `p` is the host's critically damped spring, response 0.338s.
The bracket is velocity times phase error, and it is the entire defect.

There are two phase errors and they are different in kind.

**The lead**, which is deliberate.
`CollapseTween.hostLead` evaluates our curve ahead of the host, so on the frames we draw the box is deliberately short.
At peak drawer velocity that costs 3.6pt per millisecond of lead.

**The staleness**, which is not.
The app renders at ~62Hz against a composite of 86-94Hz, so roughly a third of composited frames show a height we set one interval ago while the drawer has moved on.
`ios/Tools/rig/lib/rate.py` reports both rates off any take.

The lead is a margin against the staleness, and it is paid on every frame to be claimed on a few: a stale frame comes out one interval of drawer travel **too tall**, and too tall is the hand cut off below the screen.
At the shipping 10ms lead against a ~5-10ms stale gap, the frames we drew sat 34pt high and the stale ones landed on the drawer - which is exactly the 887.7 / 915.0 alternation the film shows.

## What moved it

Twenty collapses per point, one build, one session.

| `hostLead` | MSE | peak out of band | frames with the hand cut off |
|---|---|---|---|
| 10ms *(was)* | 3371 | 23.5pt | 0 |
| **6ms** *(is)* | **1297**, and **725** as the compiled constant | 15.1pt / 10.5pt | **0** |
| 4ms | 737 | 10.0pt | 2 |
| 2ms | 369 | 9.7pt | 2 |
| 0ms | 464 | 10.6pt | 9 |

6ms is the smallest lead that still cut nothing off in twenty collapses, and it is 2.6x better than 10ms.

Below it the trade inverts rather than continuing.
The excursion band is ~30pt peak-to-peak at **every** lead; the lead only chooses where that band sits relative to the drawer.
Ten milliseconds puts all of it above the drawer - safe, and visible as the hand floating with wool under it.
Zero puts half of it below, and the half below is the hand clipped off the bottom of the screen.
`msecmp.py`'s plot shows this directly: at 10ms the mean curve is one smooth arc above the band, and at 0ms it is a sawtooth crossing it.

## What did not move it

**`CADisableMinimumFrameDurationOnPhone`** in the extension's Info.plist.
The obvious lever, since the whole defect is frames we did not draw - but the app's redraw rate went 62.6Hz to 61.8Hz, i.e. nowhere, and the MSE differences were inside the noise band.
Reverted rather than shipped: an unverified production plist key is not worth carrying.
The gaps between our renders are 7-25ms wide with a tail past 100ms, which is not the flat 16.7ms a hard cap would give - the app is work-limited, not throttled.

**`driveHz`**, at 60, 90, 120 and 240.
Scored 532, 764, 415 and 871 at six takes each, which is one noise band.
The driver is not what is late; the render is.
120 stays.

**`hostResponse`.**
Already the optimum and bracketed hard: 0.300 scores 14109 (our box too fast, so too short, so a 43pt lift) and 0.380 scores 1513 while cutting the hand off on 19 frames (too slow, so too tall).
0.338 stays.

## What is left

The ~30pt band is the app's render rate against the host's composite rate, times the drawer's peak velocity, and no constant in this file touches it.
Closing it means the render server interpolating the box height itself instead of a 120Hz timer writing `@State` between our 62Hz frames - a different design, not a different number, and a real change to production animation code.
That is the next thing worth trying, and it is the only thing that would take the MSE below a few hundred without clipping the hand.

Second, `hostLead` is one constant for every device and the stale gap is not.
The iPhone SE's collapse is the same shape over a shorter travel, so the same lead buys a different margin there; the lead wants to be derived from the measured gap rather than pinned.
`rate.py` already reports the gap.
