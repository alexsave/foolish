# The rig

One tool that drives the **real FoolishMessages extension inside Apple's real
Messages app** on a simulator, deterministically: QA takes, animation film, and
App Store photography.

It replaces five rigs that had drifted apart and lived outside the repo -
`ios/Tools/msgrig.sh`, `~/Downloads/foolish-store-reshot/rig/*`, and three
scratchpad scripts.
Each knew something the others did not, and each was one machine away from being
lost.

```
export FOOLISH_SIM=<udid>          # rig.sh newsim prints one
ios/Tools/rig/rig.sh doctor        # what is missing, and how to get it
ios/Tools/rig/rig.sh               # the full command list
```

## A shoot, start to finish

```bash
eval "$(ios/Tools/rig/rig.sh newsim FoolishShoot)"   # 6.9" iPhone, 1320x2868
ios/Tools/rig/rig.sh build                           # kernel -> app -> install
ios/Tools/rig/rig.sh stage dark                      # 9:41, appearance, Apple's sheets
ios/Tools/rig/rig.sh session 12                      # the chat behind every frame
ios/Tools/rig/rig.sh setname Alex                    # once per fresh simulator
ios/Tools/rig/rig.sh collapse && ios/Tools/rig/rig.sh clearstage
FOOLISH_OUT=~/Downloads/shots ios/Tools/rig/rig.sh batch ios/Tools/rig/shots/store.txt
ios/Tools/rig/rig.sh sheet dir ~/Downloads/shots/10_dark_exp sheet.png --title "expanded"
```

A shot line is `name|mode|args|seat|table|lang|appearance|view`.
An empty `mode` means "do not seed" (for lobby, settings and rules frames, which
are reached by tapping); an empty `table`/`lang`/`appearance` means "leave it as
it is", so a list only names a setting on the line where it changes.
`seat` is empty for the defender's chair, a number for a specific seat, or `atk`
for an attacker's.

## The eight things that are not obvious

**1. Do not restart Messages mid-shoot.**
The simulator's Messages keeps its conversations **in memory**.
A fresh device has no `Library/SMS/sms.db` at all; a message sent from the UI
lands in no file anywhere on disk; rows injected into `sms.db` are never read.
So the transcript that `session` types is gone the moment the app is relaunched.
Everything here therefore re-enters the **drawer** rather than the app - which
is also what makes a re-seed take, because leaving the drawer kills the appex
and `claimSeededPayload()` is once per appex process.
`front` re-*activates* Messages without restarting it, and is safe.

**2. The transcript has two sides, and it is a trick.**
The runtime ships two stub conversations, and a message sent in one of them
arrives in the other as an **incoming** message.
Alternating between the two builds a real back-and-forth in whichever thread
gets photographed.
Both sides are still ours, and the bubbles are SMS green, because the simulator
has no iMessage account - that is the platform's ceiling, not a choice.
`lib/transcript.py` holds the `sms.db` route that does *not* work; it is kept
because the finding is worth more than the code.

**3. Apple's chrome is found by LABEL; our board is found by COLOUR.**
Inside a thread the accessibility tree names the back chevron (`Messages`), the
`add` button, the `Message` field and, once there is text, `Send`; the `+` menu
names every app, so `Foolish` is found rather than guessed; and the first-run
sheets name `OK` / `Continue`.
Our extension is a separate process and reports **nothing** - an expanded drawer
shows up as one zero-height "Activate to dismiss pop-up window" - so everything
inside the board comes from `lib/ui.py`.
Two traps, both of which cost a run:

- Match labels **exactly**. `Message` is a substring of `Messages`, the chevron
  is the smaller element, and `find` returns the smallest match - so a loose
  lookup for the compose field taps *back out of the thread*, silently.
- `idb ui describe-all` answers with the **last foreground app's** tree even
  when something else is on screen. A stale tree looks exactly like a live one,
  and every tap derived from it misses. Call `front` before navigating.

**4. Preferences: the plist is not the store of record; cfprefsd is.**
Writing `.../Library/Preferences/<domain>.plist` by hand does nothing that
reaches the app - the daemon serves its own cached copy and overwrites the file
from it.
`prefs` goes through `simctl spawn <sim> defaults`, which does reach it.
Verify a pref by setting it to a value it does **not** already have; asking for
felt when it is already felt looks exactly like success.
The one exception is the App Group suite (`fmsg.nickname`), which
`simctl spawn defaults` cannot see at all ("Domain ... does not exist") - that
one is a direct plist edit, and its key contains a dot, which `plutil` reads as
a path separator unless escaped.

**5. One simulator per task. Never share one.**
A batch and a hand-driven experiment on the same device destroy each other, and
the damage is silent: a modal opened by hand (a New Contact sheet, an
onboarding card) survives a Messages relaunch, swallows every later tap, and
the batch keeps going and writes frames. A whole four-take run came back as the
same grey contact editor. `rig.sh newsim` costs seconds - use one per
concurrent run, and never touch a device another run is driving. `stage` now
clears leftover modals first, which helps after the fact but is not a licence
to share.

**6. A chain of bubbles collapses only if each send TAPS the last one.**
Messages renders every message of an `MSSession` except the newest as a caption
LINE, and a send inherits its session from `conversation.selectedMessage` - the
bubble the sender tapped.
Opening the extension through the `+` menu leaves `selectedMessage` nil, so
every send starts its own session and a six-move chain photographs as six full
bubbles stacked down the screen, which reads as six games at once.
`tapopen` opens it the way a real game does, by tapping the newest bubble, and
the same chain then photographs as caption / caption / caption / one bubble.
Both routes show the same seeded board, because `claimSeededPayload()` runs
before the `selectedMessage` payload path.
Messages caps the collapsed lines at three, so a longer chain does not grow the
stack - it only buys the two-way traffic that clears the Report Spam banner.

**7. DerivedData is per simulator, because worktrees share `/tmp`.**
A concurrent agent building a *worktree* into the same derived-data path writes
its own `ios_api.h` there, and every later build in this checkout dies with
`file ... has been modified since the module file was built: size changed`,
naming a header this checkout never touched.
`DD` therefore keys on `FOOLISH_SIM`. This is rule 5 applied to the build.

**8. Never `simctl uninstall`.**
It destroys the App Group container (`dev.fatboard`, `dev.seat`) *and* the
appex's Preferences container, and both come back with fresh UUIDs.
Install over the old build instead.
`build` does.

## Smaller ones, each of which produced a wrong frame

- **`FOOLISH_NAMES` is not indexed by absolute seat.** Written as `Kate,Alex`,
  the seat-0 mover rendered as *Alex*. Verified on the device; a chain driver
  that assumes the obvious mapping puts our own moves under the opponent's
  name, which reads as a game bug rather than a rig one.

- **A staged bubble must match the board underneath it.**
  Opening the extension onto a lobby stages one, and it then rides along in the
  compose field over every later frame - a draft the board on screen could not
  possibly have produced. `clearstage` removes it.
- **A downward drag must not start at the top of the screen.**
  An expanded drawer's top edge is within a few points of the status bar, and a
  downward swipe from there is Notification Centre - which leaves the simulator
  on the lock screen. `pull_y` clamps it.
- **Colour thresholds are per-appearance.** The wooden buttons are `(62, 18, 0)`
  in dark and `(171, 120, 84)` in light; a threshold calibrated in light mode
  finds *no buttons at all* in dark, which reads as "the lobby has no controls".
- **An SMS bubble is green and so is the felt** - `(49, 213, 90)` against
  `(19, 55, 39)`. Without a brightness ceiling every text bubble in the chat
  reads as table, and the drawer's top edge is reported as the top of the
  conversation.
- **Debug, not Release.** `dev.fatboard` seeding is `#if DEBUG`. That is also
  why the lobby carries the DEBUG-only "Add player (testing)" button - never
  photograph a lobby state showing it.
- **zsh does not word-split unquoted variables.** Every driver here has a
  `#!/bin/bash` shebang for that reason.
- **`xcodegen generate` blanks the entitlements files.** `build` restores them
  from git; without that the extension loses its App Group and every seed
  silently does nothing.
- **`local a=$1 b=$((a*2))` does not work in bash** - every word on a `local`
  line is expanded before any of them is assigned.

## Output rules

App Store frames are **1320x2868** (6.9"), PNG, **no alpha**.
`simctl io screenshot` writes RGBA even though the image is fully opaque, so
`shot` flattens every frame and prints its size; the uploader validates on drop,
so a refusal is instant and means re-shoot, not resize.

## Where the seat names come from

`fixture_name()` in `c/tests/msg_wire_test.c`, the single seam every seeded
board flows through.
The cast matches the Messages contacts on purpose - a bubble from Kate should
sit above a board whose seat says Kate.
`FOOLISH_NAMES` overrides it at run time and `lib/seed.py` uses that to swap the
local player into whichever seat the device occupies, so `Alex` is always the
player and the rest keep a fixed order across every frame.

The fixture also takes `nopass` (seal a **podkidnoy** board instead of the
default perevodnoy - the two render differently, and a lane that only ever shot
the default has never looked at half the product) and `preroll` (play that many
whole bouts first, the only way to reach a drained deck, a player already out,
or a hand big enough to wrap two rows).
