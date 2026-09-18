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

## The eleven things that are not obvious

**1. Do not restart Messages mid-shoot.**
The simulator's Messages keeps its conversations **in memory**.
A fresh device has no `Library/SMS/sms.db` at all; a message sent from the UI
lands in no file anywhere on disk; rows injected into `sms.db` are never read.
So the transcript that `session` types is gone the moment the app is relaunched.
Everything here therefore re-enters the **drawer** rather than the app - which
is also what makes a re-seed take, because leaving the drawer kills the appex
and `claimSeededPayload()` is once per appex process.
`front` re-*activates* Messages without restarting it, and is safe.

**2. The transcript has two sides, and the mirror runs the other way.**
The runtime ships two stub conversations, and alternating between them builds a
real back-and-forth in whichever thread gets photographed.
The direction is the opposite of the obvious one, and it cost a whole set of
frames: a message sent in a thread appears **in that same thread as INCOMING**,
and its outgoing twin lands in the other one.
So to put our own moves on the RIGHT of the photographed thread, they are sent
from the OTHER thread.
Sending them from the photographed one puts every one of them on the left, under
the opponent's side of the conversation - which reads as the opponent having
made our moves.
The unambiguous check is the result card: it prints "(You)" from `dev.seat`
beside the name it resolved for that seat, so one frame showing "Alex (You)"
with Alex's captions on the left settles the direction.
Both sides are still ours, and the bubbles are SMS green, because the simulator
has no iMessage account - that is the platform's ceiling, not a choice.
`lib/transcript.py` holds the `sms.db` route that does *not* work; it is kept
because the finding is worth more than the code.

**3. Apple's chrome is found by LABEL; our board is found by COLOUR.**
Inside a thread the accessibility tree names the back chevron (`Messages`
through iOS 26, `Back` on iOS 27 - `tap_back` asks for both, and it is the only
label in the rig with a version split), the
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

**7. Apple's unknown-sender banner cannot be switched off, only scrolled.**
The simulator has no iMessage account, so every thread is unverified and
Messages glues two lines of grey text and a "Report Spam" pill under the newest
message - which in a collapsed frame is exactly between the bubble and the
drawer.
Both filtering prefs (`sForceUnknownFilteringCompleted`, `FilterUnknownSenders`)
leave it precisely where it was, and adding the sender to Contacts does not help
either: the avatar resolves (Kate Bell is `(555) 564-8583`, John Appleseed is
`888-555-1212`) and the banner stays.
It also cannot be scrolled on its own, because a transcript that FITS does not
scroll - it rubber-bands straight back.
`FOOLISH_CHAIN_PREFACE=1` plays a previous, FINISHED game into the thread first;
that is what makes the transcript taller than its viewport, and `nudge` then
scrolls in small steps until the banner is behind the compose bar.
Small steps matter: the pill goes behind a full line before the text above it
does, so a scroll that watched only the pill stopped with "may be spam" still on
screen and reported success.

**8. DerivedData is per simulator, because worktrees share `/tmp`.**
A concurrent agent building a *worktree* into the same derived-data path writes
its own `ios_api.h` there, and every later build in this checkout dies with
`file ... has been modified since the module file was built: size changed`,
naming a header this checkout never touched.
`DD` therefore keys on `FOOLISH_SIM`. This is rule 5 applied to the build.

**9. Never `simctl uninstall`.**
It destroys the App Group container (`dev.fatboard`, `dev.seat`) *and* the
appex's Preferences container, and both come back with fresh UUIDs.
Install over the old build instead.
`build` does.

**10. A seed that is not CLAIMED photographs as a product bug.**
`MessageDevBoard.claimSeededPayload()` is once per appex *process*, and the only
thing that ends that process is leaving the thread.
When a `back` does not take, the extension re-opens the seed it already claimed,
auto-stages *that*, and every bubble in the chain comes out one move behind -
silently, because `chain` used to run `cmd_back … || true` and throw away the
one signal that says so.

Nothing in the frames gives it away.
Each board is a real legal state; the caption row under it names the defender,
and a defender does not change within a bout, so that row reads correctly for
the bubble **and** for its predecessor.
The only visible tell is the summary line, which reads as "each caption
describes the previous message" - and on that evidence the lag was filed as a
caption bug in the shipping product three times.
`ios/FoolishTests/MessageCaptionActorTests.swift` settles both halves offline:
the product's line names its own actor over a played chain, and the defender row
is provably blind to a one-bubble lag.

So the rig stops inferring.
`claimSeededPayload` writes the hex it claimed to `dev.claimed`; `seed_open`
deletes that receipt, seeds, leaves, opens and **compares** before anything is
sent, retrying the leave up to three times and failing the shoot rather than
photographing a stale board.
`rig.sh claimed` prints the receipt by hand.
No sleeps are involved: a claim either happened in a fresh process or it did
not, and the answer is a file.

**12. A chain under FIVE entries photographs a duplicated caption.**
Messages draws only the **last three** messages of a session as caption lines,
and the simulator corrupts the summary of the **first two** in the session -
they come out carrying the NEWEST message's text.
So whether the defect is in frame is purely a function of chain length: at four
entries the caption window is indices 1-3 and the corrupted index 1 is visible,
so the frame shows one caption twice - the same card played twice, which is an
impossible transcript and exactly the kind of thing the owner catches. At five
the window is 2-4 and the corrupted pair falls outside it.
This is deterministic, not a flake: the four-entry chain reproduces the
identical wrong caption on every run, and `hero_v7` was clean only because it
happened to be five.
`chain` refuses fewer than five now (`FOOLISH_CHAIN_SHORT=1` overrides).
It is the same family as Apple's 2016 report - previous summaries taking the
newest one's text, simulator only, correct on device
(<https://developer.apple.com/forums/thread/64452>) - so do not go looking for
it in the product.

**THE TRANSCRIPT FRAME, in full.** The owner-approved shape is `hero_v7.png`:
two players, our captions on the RIGHT named Alex, the opponent's on the LEFT
named Kate, the drawer agreeing with both, no preface pill, banner clear.
The board is kept SPARSE on purpose - it clutters fast at bubble size - and the
spec is **two attacks on the table with one covered**.
Pick the depth by reading the generator rather than by shooting and looking:
`--chain` prints `atk= cov= hand=` for every entry, so a depth whose LAST entry
reads `atk=2 cov=1` is the one that produces the specced board.

  FOOLISH_CHAIN_PREFACE=0 rig.sh chain <name> 5 3 2

Verify every frame against `/tmp/rig_chain.log`, which names each entry's real
move: the caption lines must match it seat for seat and card for card. Do not
read the cards off a deck table - value 1 is a TWO and the ace is 13, and a
hand-rolled table cost an afternoon of treating correct summaries as a bug.

**11. The compact drawer has TWO heights, and Messages' compose field picks
which.**
The drawer is 388.7pt tall when Messages' own text field holds no first
responder and 372pt when it does - the whole input stack drops 17pt with it, and
our surface just fills whatever it is handed (the extension's own AnimLog reads
`follow geo=...->340` and `...->323` for the two).
So on a 6.9" phone the drawer's top edge is 567 or 584, and **nothing in the
extension chooses**: an `open` taken after a tap anywhere in the compose area
measures 16pt shorter than an `open` taken after leaving the thread and coming
back.
`clearstage` is such a tap - it presses the staged bubble's X - which is why it
now ends with a `back`.
This cost a whole investigation.
Four films of the same create read as "the create path settles at a different
height on identical code"; all four were really reading the state their
prologue had left behind, and `create_X_before` / `create_X_before2` prove it -
their *first* frames, before either take does anything, already sit at 584 and
567.
Measure the drawer with `ui.py hostedge` (Messages' own grab handle) rather
than `top` (the top of OUR felt, which a dark panel of ours splits - the New
game screen's black name field reports 739 for a drawer whose edge is 584).

## Smaller ones, each of which produced a wrong frame

- **`FOOLISH_NAMES` IS indexed by absolute seat** - `fixture_name` returns
  `slots[seat]`. An earlier note here claimed the opposite, on evidence that was
  really the mirror direction above: our own captions were appearing on the
  wrong side, and renaming the seats made them *read* right while leaving the
  sides wrong. Put "Alex" at the seat we occupy and fix the sides separately.

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
- **Stop `recordVideo` with SIGINT, always.** A recorder killed any other way
  leaves the simulator answering "Host recording is already in progress" until it
  is rebooted. Scripts that record trap EXIT and send INT.
- **`ui.py`'s hand and table finders are tuned to the COMPACT drawer.** Expanded,
  with one card in hand, `hand_y` returned the opponent's fan (101pt) and a cover
  tap raised "That move isn't allowed." `lib/board.py` reads the hand off the
  ruler's green bar and the table off the pairs' squares, in either presentation.
- **Prove a move by the flight log, not by the screen.** Undo is hidden until the
  animations and the collapse are over, so "a plank appeared" is late or absent,
  and a SELECTED card lifts out of the hand finder's band, so "one card fewer"
  reads true for a tap that played nothing. The flight log line is
  `12.00 fly 45.5 ^45.6 place-3-11 from=... to=...` - the id comes after two
  memory columns.
- **Unit tests on the rig's simulator read the rig's dev files.** The test host
  shares the App Group, so `dev.ruler` (and any other `dev.*`) reaches the views
  the tests render: MemoryProfileTests went red 2 runs in 3 with the ruler on and
  green 3 in 3 with it off. Set the ruler aside for a test run, or test on
  another simulator.
- **The animation reel.** `shots/anim_reel.sh` films every move and its Undo in
  one take (throw in, bout-ending Good, first attack, cover, pickup, pass by drag,
  the cover that ends the bout, 8 seats, compact) plus replayed arrivals, and
  `lib/tablesquares.py` reports every jump of a table pair and every card that
  left the table before its flight existed, named by scenario.
  Local only, never CI; run it now and then. Positions are measured inside the
  drawer (from its red top bar), because Messages' own drawer slide moves the
  whole table and is not ours. `FOOLISH_FLAGS='table.slide=0'` is how the reel
  was shown to bite: it reports exactly the six throw-in and undo moments.
  `ONLY='pass|8p'` plays just the matching scenarios. Every card in my hand now
  carries a BLUE square too, so a dragged card can be followed from the fan;
  `lib/squareplot.py REEL "label" out.png --offset S` draws every square's x and
  y over one scenario - the picture that showed the pass pairs reversing three
  times, which the 12pt jump rule never flags.
- **A reel board is set up in place.** `clearstage stay` removes the Undo's
  draft without leaving the thread, and the appex is killed and reopened
  through the + menu - leaving and re-entering was ~5s a scenario, and waiting
  for a plank on a board that never gets one (an attacker's empty table) burned
  its whole ~14s ceiling.
- **Where a pass is dropped decides what it looks like.** A drop 110pt above the
  table landed the card at the top centre and flew it DOWN into its slot; a
  person drops it on the slot, and the reel now does (a smooth `--delta 6` drag).
- **A mark's width is not the log's.** The log said every coin flipped on an
  8-seat Undo; the film said the check came up at 30 of 34px with no collapse.
  Measure a badge's ink per frame (white sword, green check) before believing a
  gesture played.
- **A window's frames need the window's times.** `tween` measures a slice of
  the movie, and lining the whole movie's timestamps up with it from the end put
  every frame ~315ms late and stretched a 0.77s collapse to 1.0s. `lib/window.sh`
  logs each frame's time from the run that writes it, and `tween.py` refuses a
  `times.txt` that does not match its frames. `python3 lib/test_window.py`
  checks it against a movie whose frames draw their own index.

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
