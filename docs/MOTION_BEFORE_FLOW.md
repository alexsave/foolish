# Draw it in HTML, and decide how it moves before you decide what it looks like

Two findings from building foolish's iMessage board and then designing
werewolf's without writing any of it.
Both are about cost, and both are things the next product should start with
rather than arrive at.

Neither is a style opinion.
They are the two reasons foolish reached **build 74**.

## 1. The loop, not the work, is the cost

Foolish's surface was designed in Swift.
Every question - is that text too small, does that row fit at eight seats, is
that colour doing anything - was answered by editing Swift, rebuilding in
Xcode, bumping `CURRENT_PROJECT_VERSION`, uploading to App Store Connect,
waiting for processing, downloading to the device and playing a game to reach
the screen in question.

Call it fifteen minutes when nothing goes wrong, and something usually does.

Werewolf's surface was designed as **one HTML file** - `werewolf/docs/UI.html`,
opened in a browser, devices drawn at 390pt with the real type and the real
palettes.
Roughly forty rounds of "make the moon less cartoonish", "the shield roster
should be centred", "that gradient is too horizontal", each answered in
seconds.

Twenty seconds against fifteen minutes is not an efficiency.
It is the difference between asking a question and not bothering.
**Most of the 74 builds were questions that were not worth asking at fifteen
minutes each**, which means most of them were never asked, which is why they
were still being found at build 60.

What the HTML honestly cannot answer, so do not pretend otherwise:

- Touch target sizes against a real thumb, and the 46pt threshold that turned
  out to be unreachable on every iPhone.
- Real text metrics for a script you did not think to test. Put the scripts in
  the mockup - `UI.html` has a twelve-script roster for exactly this.
- Anything about motion. See below, which is the other half of this document.
- Anything about the host. Messages gives one truncating line of caption and an
  extension cannot send; both were learned on device and belong in the mockup
  as constraints, not discoveries.

The mockup is not a prototype and should never become one.
It is a drawing that can be argued with.

**It needs a checker.** `werewolf/docs/check_ui_doc.py` exists because the same
scoped-CSS bug shipped three times: a device using the wrong structure keeps
its headings and silently loses its grid, so it looks like a layout bug and is
a selector bug. A big single-file mockup earns a structural test the same way
code does.

## 2. Motion is a grid, and it has to be filled in first

Foolish has two of these and they are both good: `docs/ANIMATION_CATALOGUE.md`
(every shape, with an honest status column) and `docs/UI_STATE_FLOWS.md` (the
state machines, as seven mermaid diagrams - the move lifecycle `Idle / Refused
/ Staged / Held / Undoing / Sending / Released / Retract / Sup`, arrivals on an
open board, and the same red flight for two different reasons).
The catalogue was written at **1.0(24)**, walked with the owner at **1.0(28)**,
and audited for timing later still.
Every shape in it was discovered by shipping something that felt wrong.

Its structure is the thing that should have existed on day one,
because it is not specific to Durak at all.
**The same axes apply to any game that lives in a transcript.**

### The channels

A move does not have one animation. It has one per channel it can arrive
through, and they are different animations for the same event.

| Channel | The situation | foolish's name |
|---|---|---|
| **A** | My own move, staged, before Send | `stagedAnimation` |
| **B** | The same move, at Send | `releasedSettlement` |
| **C** | Reopening my OWN bubble cold - a replay | `openReplayEvents` |
| **D** | Opening SOMEBODY ELSE'S bubble cold | `openReplayEvents`, masked |
| **E** | A move arriving on a board that is already open | arrival |
| **Undo** | Tapping x on a staged bubble, or the Undo pill | `flyUndoReturn` |
| **Conflict** | A race I lost - the move I staged is no longer legal | `anim_conflict_*` |

Seven, for one move.
A and B are two animations for a single action, and getting that split wrong is
what "some moves animate when you stage and then again when you send" means.
C and D differ because a replay of my own move should not surprise me and a
stranger's should.
E is the only one that interrupts something the player is already looking at.
Conflict is the only one that has to say *no* without feeling like a bug.

And A/B is not the only split inside one move. Foolish cuts a move's event
stream into an **action** half and a **settlement** half, and the cut is the
kernel's (`evw_is_settlement`), not the board's - A plays the action, B plays
the settlement only if the move ended a bout. Decide where your equivalent cut
is before you decide what either half looks like.

Undo has a subtlety worth stealing: **the x on a staged bubble is not the same
event as an Undo pill.** By the time the board hears about the x, Messages has
already removed the bubble, so it cannot be refused - only the pill asks
permission (`UndoGate`).

Fill this table in before drawing a screen.
A cell can legitimately say "nothing moves" - that is a decision. An empty cell
is not.

### The anchor split

When the board auto-collapses, everything moves at once, and it will look wrong
unless every element has been assigned an edge to hold:

- **Top-anchored** - holds its distance from the top and slides with it.
- **Bottom-anchored** - holds its distance from the bottom, which during a
  collapse means it must move *against* the container.
- **Centre-anchored** - keeps its proportion, which usually reads as the least
  correct of the three and is the right answer surprisingly often.

foolish spells this as a type, `CollapseRide` in `CollapseLayer.swift` -
"where a view sits in its box as the box shrinks" - either `fraction(CGFloat)`
or `path(rest:y:)`. One attribute per element.

Foolish found this by measuring: `docs/COLLAPSE_MSE.md`.
The box's bottom edge is meant to travel **6.7pt** during an auto-collapse and
was leaving that band for ~400ms, because the box was top-glued to a descending
edge while the thing inside it wanted to be bottom-glued.
That is an anchor decision, made accidentally, that cost a measurement rig, a
sweep harness and several builds.

**Assign every element an anchor in the mockup.** It is one attribute per box
and it is free there.

### The vocabulary

Keep the list small and name them in the kernel, so a spec can say which one.
Foolish's surface transitions are exactly three (`anim_plan.h`):

```c
#define ANIM_TRANSITION_SNAP 0   // it is simply true now; no motion at all
#define ANIM_TRANSITION_TURN 1   // the control that changed rotates out and back
#define ANIM_TRANSITION_FADE 2   // one whole surface cross-fades into another
```

and its role marks have five gestures (`FRoleMotion.swift`): `flip`,
`rotateOut`, `rotateIn`, `restore`, `none`.
Cards get **fly** and **fly+rotate** - the one that reads as a physical object
being dealt or returned - plus **hold**, a deliberate pause that is
load-bearing: `boutEndHold` is `flightTime * 3` and `gameOverHold` is
`flightTime * 2`, off a `flightTime` of 0.5s.

There is also a composition default worth copying verbatim:
**within one kernel move, everything that moves goes at once; between moves,
one after another.**

And a duration that is not a duration: `settle_ms` on a surface plan is how
long a surface must be **on screen before it may be put away**, which is what
stops a lobby's fade from being eaten by the collapse that follows it.

Combinations are allowed and should be written as combinations.
"Undoing a pickup flies the cards back out of my hand onto the table" is a
spec. "It animates back" is not.

### Timing is part of the spec

Give every cell a duration and a curve, and expect to be wrong about them.
`docs/ANIM_TIMING_AUDIT.md` exists because the rig was reporting tween times
~315ms late, so every timing judgement made before PR #162 was made against
numbers that were not real.
If motion is going to be tuned, the instrument gets checked first.

## The order to work in

1. The kernel decides what can happen.
2. **The channel grid** - every event, every channel, what moves and what
   anchors it. Before any screen.
3. The surface, in HTML, at device size, with real type and real scripts.
4. Argue with it until it is boring.
5. Then Swift.

Werewolf did 1 and 3 and is paused before 5, which is the right place to be
paused. See `werewolf/HANDOFF.md`.

## Where foolish put it

Worth knowing that the channels are not just a document - the code is split
along the same seams, which is what a grid decided up front buys you:

```
ios/FoolishKit/Boards/
  BoardSpring.swift              the curves
  BoardFlight.swift              fly + rotate
  ConflictModel.swift            the race I lost
  MessageTableView+Undo.swift    x on a staged bubble
  UndoFlightSource.swift         where the cards fly back FROM
  MessageTableView+OpenReplay.swift   channels C and D
  MessageTableView+Sequence.swift     ordering
  MessageTableView+BoutEnd.swift      the one-second hold
  FRoleMotion.swift              the role marks
  AnimLog.swift                  what the rig reads to check any of it
```

`AnimLog.swift` is the part people skip. **Motion that nothing can observe
cannot be regression-tested**, and every timing number in this repo comes from
the rig reading that log, not from watching a screen.

## The files this is drawn from

- `docs/ANIMATION_CATALOGUE.md` - the channels, the conflict model, the honest
  status column that says what nothing tests.
- `docs/UI_STATE_FLOWS.md` - the same thing as state machines, in mermaid.
- `c/src/anim_plan.h` - the transitions, beats, veil, conflict verdicts and
  surface plans, as C the whole product shares.
- `docs/ANIMATION_CORE_C.md` - where the animation plan lives in C.
- `docs/ANIM_TIMING_AUDIT.md` - the instrument being wrong.
- `docs/COLLAPSE_MSE.md` - the anchor problem, measured.
- `werewolf/docs/UI.html` + `werewolf/docs/check_ui_doc.py` - the method.
