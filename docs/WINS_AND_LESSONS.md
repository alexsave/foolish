# Wins and lessons

*Written 14 September 2026, the day 1.1 (69) went into App Store review.
It covers the 54 days from 23 July to 14 September: 629 commits, 64 builds,
1.0(1) through 1.1(69).*

This file exists because the lessons below were living in one machine's agent
memory, and a lesson that only survives on one laptop is not a lesson the
project has learned.
Everything here is checkable against a commit.

---

## The wins

### One rig, in the repo

`9ccb6da5`, 11 September.

Five rigs existed before it.
Two were tracked, three lived in a Downloads folder.
Each knew something the others did not, and each was one machine failure away
from being lost.
`ios/Tools/rig` replaced all five, and it does something none of them did: it
drives the **real** FoolishMessages extension inside Apple's **real** Messages
app, deterministically.

The store shoot, the caption investigation and the submission all depended on
this.
A board state can now be posed in milliseconds rather than played to by hand.

The durable part is not the code, it is the README.
Twelve numbered traps, each written the day it cost a run.
The transcript is in memory only, so restarting Messages wipes it.
`idb ui describe-all` returns the last foreground app's tree, so call `front`
first.
A chain under five entries photographs a caption twice.
Those sentences are why the second occurrence of each trap cost minutes instead
of an afternoon.

### The kernel convergence

6 September, on a day of 123 commits and 29 merges.

**3,239 lines of TypeScript deleted in exchange for one new kernel export.**

The web and the app had been holding two opinions about the same rules.
The July audit had already found three live divergences of exactly that kind:
offline cordite ran at arena knobs, iOS picked the first eligible seat instead
of shuffling, and offline "Handwritten" was a different bot entirely.

This is the highest-leverage change of the 54 days, because it converts "the
bots feel the same everywhere" from a hope into a compile-time property.

### The double Band-Aid pass

4 and 5 September, 115 commits, almost no features.

The instruction was to find every place where a second fix had been layered over
a first, and remove both.

What went out: a game-record cache that had stored nothing since round 7, a
`prevPayload` that was nil at its only origin, a hand-fan crop that did not
exist.
What came in: one owner for the shown counts with the compiler enforcing it,
one rule for which bytes went out proved against the two it replaced, and one
command for the Mac-only suite with its mtime landmine named in writing.

Deliberately spending two days deleting things is easy to defer forever.
The two days after it were the fastest of the project.

### Searching for the state instead of shooting for it

`--goodwait` and `--passable` in `c/tests/msg_wire_test.c`.

The store shoot began as: play until the board looks right, screenshot, repeat.
It ended with C searchers that find a photographable state - an attacker holding
a legal pair while the others are already good, or a defender who may legally
pass - in milliseconds, print every seat's hand with suits, and hand back a seed.

Choosing a frame became reading text rather than looking at pictures.
That is a categorical speedup, not an incremental one, and the same trick
applies anywhere a test needs a specific game position.

### 6.5 MB down to 2.9 MB

8 September.

The commit note carries the finding: *it was never the network stack*.
Two replay caps halved the extension's dirty pages, three scratch `Game`s in the
iMessage bridge became one, and 118 KB came off the shipped bundle - which also
got the Release archive building again.

### The arrival chain, broken in a day

10 September, builds 1.1(57) through 1.1(68).

Twelve builds, each one a narrower claim than the last:

1. An arrival landing mid-reload was being diffed against nothing.
2. The surface had to remember what it last showed.
3. An arriving bubble **does** become the selection, so every arrival was a cold
   reload.
4. 61 was playing every stream twice, because two handlers were bound to one
   arrival.

Twelve builds sounds like thrashing.
It was not.
Each build tested exactly one hypothesis, and the hypotheses were nested.

---

## The lessons

### 1. Green against the wrong artifact

This is the through-line of every testing failure in 54 days, and not one of
them looked like a wrong assertion.
The claim always looked right.
The **subject** was wrong.

- A test claiming to catch attack/pass collapsing never asserted that passes
  existed, so collapsing them passed it.
- A no-trump invariant searched with a different seed derivation than it played
  with, so it tested a different game and passed against the exact bug it was
  written for.
- `rules_wasm.ts` was a stale embed: 39 of 39 green against a kernel that did
  not contain the change under test.
- The double-cover replay bug took four agents across two days, and the answer
  was that **the fixture was lying, not the kernel**.
- `CoverTiltTests` tested a function with no caller.
- Two rig oracles had baselines that were hiding the defects they existed to
  catch.

**The practice:** write the mutation, run it, watch it go red.
Not "does this test pass" but "can this test fail, against this artifact, in
this runtime".
Six instances in one project is not bad luck.

### 2. A go-ahead answers the nearest question

On build 69 three verified defects were flagged (U2, U11, U12) and the reply was
"let's fix that and get the bump and upload out".
That was read as a waiver, and an archive shipped without any of them.

A short affirmative attaches to the nearest question in a message, not to every
caveat attached to it.

**The practice:** verified defects go in unless the owner names them and says
skip.
If shipping without one is genuinely right, say so as a question with the cost
attached, and wait.
"Pre-existing, not a regression" is triage reasoning, not a reason the owner
accepts.

### 3. Measure the thing, do not infer it from a picture

Three incarnations, in one week:

- A hand-rolled rank table in a new printer made four **correct** product
  summaries look wrong for an afternoon.
  In this repo value 1 is a two and the ace is 13.
  Copy the repo's own table.
- A screenshot gate read a wrapped twelve-card hand as six, because it only
  measured the lowest band.
- Half of the `plain/` screenshot folder was light-mode all along, and four of
  the files being paired as "light twins" were **byte-identical** to their
  supposed dark partners.
  The alternation was a no-op across half the set.

The fix in every case was a measurement of two lines.
For appearance: the mean brightness of the top 120 rows is about 8 in dark mode
and about 156 in light, and dark mode draws card faces black where light draws
them white.

**The practice:** never trust a folder name, a filename or your reading of a
thumbnail.
When a program knows the answer, ask the program.
Screenshots produced three wrong conclusions in one session, because two
independent errors can cancel.

### 4. One rule, one place, and the place is C

Every rule that lives in a host is a rule that will drift.
Given a fork between "keep it in TypeScript or Swift" and "push it into the C
kernel", pick C, **even when it slightly changes behaviour**.

The only counterweight worth weighing is module size against Supabase cold
starts, because edge functions may re-initialise on every invocation.
Never correctness.

Leave the host doing only what the kernel cannot: database reads, secrets,
timers, rendering.

### 5. Fix the cause, not the symptom's neighbour

The collapsed drawer height is 584pt, always, and it is never to be corrected
inside the tween.
The tween is where the wrong number becomes visible, which makes it the most
tempting and the most wrong place to change it.

The same shape appears in the first-paint veil family: three separate bugs, all
of them *derive in the body, not in `onChange`*, and it took the third before
the family had a name.

**The practice:** when a fix lands in the place where the symptom renders rather
than the place where the value is computed, that is a signal, not a solution.

### 6. A defect you cannot fix still has to be understood

Build 70 was gated on the **cause** of the own-caption fallback, not on a
workaround and not on "it corrects itself when Messages is relaunched".

That gate forced a disassembly of IMCore and produced a real answer.
A collapsed caption is built two ways.
A message read back from the store is a type-3 item whose status text is the
body, which carries our `summaryText`, and is correct.
A message still held in memory is converted by
`-[IMTranscriptChatItemRules _fixBreadcrumbs:]`, whose status text is the
**private** `MSMessage.statusText` that no app can set, so it falls back to
"Alex sent Foolish message".

The bug still ships.
But shipping a known defect you can name and attribute is a different act from
shipping one you hope nobody notices.

### 7. Write the trap where the code is

The rig's README and the inline notes are why the second occurrence of a trap
cost minutes.
`rig: the grab handle is the LOWEST one, and say so where boxHeight lives` is
the pattern: the finding goes next to the number it explains, not into a
document that will not be re-read.

### 8. The highest-value days produce the fewest features

5 September: 89 commits, essentially all tests and proofs.
4 September: 26 commits of deletions.
Those two days are why 6 to 11 September could move as fast as they did.

The inverse also holds.
The twelve-build day, the four-agent double-cover hunt and the afternoon lost to
a rank table were all cases where something unverified was built on.
The cost showed up days later, in a different file, wearing a different symptom.

---

## Working agreements

These are about how the work gets done rather than what the code does.
They were earned the same way.

**Open results immediately.**
Run `open` on a folder or sheet the moment it lands.
The screen change is the notification; a path in a chat message is not, and it
costs a copy-paste to act on.
A partial folder that can be judged now beats a complete one later.
Lay folders out for Finder as the viewer: one folder per decision with nothing
else in it, filenames that sort meaningfully and carry the key parameter, and
full resolution - never downscale a comparison of visual quality.

**Clarify shape, not detail.**
When a spec has a nontrivial design ambiguity - an action space, an encoding, a
wire format - lay out two or three concrete options with their trade-offs,
recommend one, and wait.
Do not do this for variable names, file layout or test framework choice.
Plow ahead on those.

**Stage the integration, never funnel it.**
In a multi-agent build, integrate small related clusters in parallel first, then
compose the bigger parts in parallel, then let one agent tie the whole thing
together at the end.
A single integrator becomes a serial debugging bottleneck and a single point of
failure, which is the opposite of why the fan-out exists.

**No em dashes in shipped strings.**
Use a plain hyphen.
This covers every localized string and user-facing label, including the
`ios.msg.*` table in `FStrings.swift`.
When touching a strings file, sweep the existing entries too.

---

## Where the evidence lives

| Subject | File |
|---|---|
| The rig, and its twelve traps | `ios/Tools/rig/README.md` |
| The listing frames and their rules | `docs/appstore/screenshots/README.md` |
| The deal-order break and its format retirement | `docs/DEAL_ORDER.md` |
| The animation catalogue | `docs/ANIMATION_CATALOGUE.md` |
| App review notes, both passes | `docs/APP_REVIEW_NOTES.md` |
| The full system architecture | `docs/ARCHITECTURE.html` |
