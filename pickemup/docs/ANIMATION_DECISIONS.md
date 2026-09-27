# Pick 'Em Up - animation decisions made on the owner's behalf

Every choice in the motion layer (`pickemup/c/src/pk_beats.{h,c}`, the bridge's `pk_api_beats*`, and `pickemup/ios/PickemupKit/Board/BeatPlayer.swift` with the views it drives) that the owner did not dictate and `UI.html` did not settle.
Each one can be vetoed on its own.
Rules are `RULES_AND_KERNEL.md`'s (Dn), visuals `UI_DECISIONS.md`'s (Un), iOS `IOS_DECISIONS.md`'s (In), orchestration `ORCHESTRATION.md`'s (On).

## Where the timeline lives

DECISION A1: the whole timeline is C: `pk_beats_build` lays a plan's events out as beats (start, duration, curve, from and to anchor, card, parts), `pk_beats_frame` answers what the board shows at any millisecond, and `pk_beat_sample` answers one beat's transform (eased progress, scale, rotation, offset, opacity, which face).
Swift samples both every frame from a `TimelineView` and tweens a ghost between two anchors' frames; it holds no duration, no curve and no order.
Alternative: foolish's iMessage shape, a Swift `BoardAnimator` that walks steps with `Task.sleep` and `withAnimation`.
Why: the owner's rule is C over Swift, and a sampler cannot drift from the plan the way a chain of sleeps can; a superseding arrival is one assignment, not a cancelled task tree.
The curves are cubic-bezier control points solved in C, so the host needs none of them.
Confidence: high.

DECISION A2: the board a plan starts from is the kernel's replay of the game to the end of bubble `from` (`pk_beats_pre`), never a view the host remembered.
Alternative: the host hands C the view it was showing.
Why: it makes "clear, not revert" free: a superseding arrival builds a new plan whose start is exact, and nothing the host kept can disagree with it.
Channel A is the one case where the start is the draft's own parent bubble plus the events already played (DONE), for the same reason.
Confidence: high.

DECISION A3: a turn bar moves 25ms after the last motion of its turn (the grid's "Turn moves" prose: "25ms after the flight that caused it", foolish's flightGap), including after a skip's 900ms dim, a reverse's TURN and a penalty's cards.
Alternative: the demos' timing, where `DEMO.skip`, `DEMO.reverse`, `DEMO.draw2` and `DEMO.w4` call `turnTo` with no gap after the effect.
Why: one rule with one owner; the 25ms is below what the eye separates, and the prose is the contract.
The C tests write the demo's number plus 25 and say so.
Confidence: medium.

DECISION A4: where the grid's prose is silent the demo script is the number: 250ms after the deal lands before the start card turns, 300ms before a rejected card is buried and 120ms after, a riffle whose eight layers start 8ms apart (so one riffle is 196ms), a dealt card's bulge 1.05, and the results 1060ms after the last reveal flip starts (the demo's `sleep(60)` loop, then `sleep(T.gameOver)`).
Alternative: drop the rests the grid does not list.
Why: the demos are the reference the brief names; each rest is a constant in `pk_beats.h` with a comment saying where it came from.
Confidence: medium.

DECISION A5: keyframes are sampled the way the Web Animations API samples the demos: the curve eases the progress first, and a keyframe at offset .5 means half of the EASED progress.
So a play's 1.15 bulge peaks before half time, while its back turns face at half TIME (the demo's `sleep(T.flight / 2)`).
Alternative: keyframes on linear time.
Why: the same numbers then give the same motion as the page.
Confidence: high.

## The cut, and what a tap plays

DECISION A6: every plan event is PLAYED, DONE or HELD, read off `half` alone: at stage (A) the action half plays and the settle of the bubble's last turn is held (so the frame keeps the draft as played: no turn bar, no penalty count) until Send (B) plays exactly what was held; a settle half is released the moment a later turn begins in the same bubble (a two-player reverse).
The real-game test proves every event plays exactly once across A and B, the same beats an opened bubble (C) plays.
Confidence: high.

DECISION A7: channel A finds the events a tap added by matching the draft's plan against the plan the previous build saw, in order and by content, not by an index.
Alternative: "the events after index k".
Why: a Last card! or a catch lands at the FRONT of a draft (5.3.3), so an index would replay the draws after it.
The bridge remembers the previous draft plan itself (`pk_api_beats_mark`); a take-back that moves nothing re-marks it.
Confidence: high.

DECISION A8: my own drawn or dealt card flies face down and turns over where it lands (`PK_FLIGHT_BACK`), as the grid's "a back flies deck to the right end of the row ... lands, flips face" says; a seat's played card flies as a back and turns at the midpoint; my played card flies face up.
Confidence: high.

DECISION A9: the pop of the suit tiles is the demo's (each tile scales up where it stands, 30ms apart) rather than the grid prose's "pop out of the card to the compass points"; the collapse moves them back into the card, as both say.
Why: the demo is the reference; the difference is 260ms and the tile positions are the kernel's (`pk_lay_picker`, which also closes IOS_DECISIONS I19).
Confidence: medium.

DECISION A10: a picked suit's ring and the tiles' collapse lead the stage plan of the wild (`PK_BFL_PICKED`), and the wild's own flight is skipped because the picker already put it on the pile (`PK_BFL_WILD_PLACED`): one plan, so the halo and band start as the tiles are gone.
Confidence: medium.

DECISION A11: things with no anchor on this phone keep their time but draw nothing: my own seat has no badge, so an OUT stamp for me still takes its 340ms before the reveal, while a Last card! of mine has no stamp beat at all (the grid's own-view cell is a snap) and my own hand's REVEAL cards are skipped (it is already face up).
Confidence: medium.

DECISION A17: the starter's own Start (or the Join that fills the table) switches to the table and plays the deal at once, as channel A of bubble 0 with the live 16ms lead, where before the lobby stayed up until the bubble was sent.
Alternative: wait for Send, and play the deal as the others see it.
Why: the grid plays bubble 0 "by the starter at Start (A)".
Confidence: medium.

## Deferred, with the reason

DECISION A12: the wild's chosen-suit band snaps with the card; the kernel lays the BAND beat out (220ms, at the landing) but the Swift card draws its band inside its face, so there is nothing to slide yet.
Alternative: a band overlay on the pile that the beat slides up.
Why: the card face is being reworked in parallel (O6); the beat is already in the plan for whoever draws the band as its own layer.
Confidence: medium.

DECISION A13: the lobby's Join and Leave rows are laid out by the kernel (a 220ms fade, and a 320ms close-up on a leave) but not played: the bridge has no lobby entry to beats yet (`pk_api_plan_lobby` feeds nothing), and the lobby screen reports `roster.k` anchors for when it does.
The start bubble's lobby rest (500ms) and FADE play over the felt, because the lobby screen is gone once the game has started.
Confidence: medium.

DECISION A14: uttt's `CollapseSlide` is still not compiled (IOS_DECISIONS I20 stands): the drawer's auto-collapse now waits the kernel's `settle_ms` (250 + the move's own plan + 500) instead of the 750ms literal, but the render-server slide needs a collapse curve in the kernel and can only be judged on a phone in Messages, which could not be launched (ORCHESTRATION B2).
Confidence: medium.

DECISION A15: the shared Send reminder (`SendHint`, IOS_DECISIONS I21) is still not compiled: it needs a kernel word, a fuse number and its place against Messages' own Send button, and none of it can be seen without Messages.
Confidence: medium.

## Budgets, and what they found

DECISION A16: U21's budget ("a five-draw turn still plays in under four seconds") is held two ways: the synthetic five-draw turn with a reshuffle and a play (2618ms) and the 99th percentile of every eight-player arrival in 400 played-out games (about 2.9s).
Two-player bubbles run longer at the tail (p99 about 4.8s, max about 10.7s) because a reverse-as-skip lets one bubble carry several turns (D7); that is several turns, not a slow one, and is not budgeted.
Confidence: medium.

FOUND for the owner: U20 says the clamp "keeps every table between 3.2s and 4.2s end to end"; the upper bound holds for the shuffle and deal at every size, but the lower one does not at two or three players (a two-player shuffle and deal is 2.3s), and counting the start card too, an eight-player deal seen by the dealer is 4.3s.
The numbers are U20's own clamp; only the sentence is off.
