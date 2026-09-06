# iMessage: a superseded move is told, not repaired (Rule N)

**Status: DESIGN. Not implemented.** Owner call, 2026-09-02.

This settles the question the lobby has been circling since round 5 — what to
do when two people act on the same bubble at the same moment and one of them
loses — and it settles it the same way for every kind of move, not just joins.
It supersedes the repair-shaped answers considered before it: Rule R's
durable pending ledger (retired, round 9), the Rule M merge (rejected, round
12, `IMESSAGE_GAME_DESIGN.md` §7.7), and the two sketches from the 2026-09-02
review — an automatic re-claim of a lost join, and a "wider re-Start" when a
join loses to a Start. None of those ship.

## The owner's framing, which the design follows literally

> Since we can't work around the fact that iMessage can only see the last
> bubble in the thread, the best we should do is just forget any priority at
> all, DETECT that our move was legally superseded, and INFORM the user with
> specific wording on which move of theirs was superseded, and suggest they
> try again. This can work for joins, pickups, attacks, covers, passes.

## The constraint, stated once

`MSConversation` exposes one message — `selectedMessage` — plus whatever
`didReceive` delivers while the extension happens to be awake. There is no
transcript, no history, no query. Every scheme that REPAIRS a lost move needs
an input the platform does not give: either other people's chains remembered
across events (Rule M — a merged state "exists in no bubble anywhere"), or a
device acting on behalf of a human who is not there (any auto-re-stage still
ends at Messages' Send, which only a human can press — §11.4).

The one thing a device always knows is what it sent itself. That is enough to
DETECT a loss and to SAY what was lost, and it is all this design uses.

What the thread looks like today when a move loses is silence. A lost join
puts the Join button back with no explanation (`SeatIdentity.cacheDisowned
ByJoins`). A lost throw-in leaves the card in your hand as if you never played
it. A lost Start seats you at a different deal. Each one reads as a bug, and
the lobby is where it happens most, because joining is the one move several
people make off the same bubble at once.

## The rule

**Rule N.** When the chain this device is showing is not a descendant of the
chain this device last sent for the same game, and Rule P prefers what is
being shown, the device tells its human which of their moves did not land,
what landed instead, and — when the kernel says the move is legal right now —
that they can make it again. Nothing is re-applied, merged, re-claimed or
re-staged on their behalf.

"Forget any priority" means, precisely:

- **Nothing new is layered on Rule P.** No rebase, no merge, no automatic
  re-claim, no wider re-Start, no "the loser folds themself in". The
  `leaveLobby` note already says why for the lobby: a priority scheme "would
  only be a second opinion about an order the platform has already decided".
  This design extends that stance to every move.
- **Rule P itself stays, unchanged, in its one job**: when two chains are in
  hand, which one is the game. It cannot go — a late-delivered older sibling
  would walk a live board backwards (the rule-4 hole, `msg_wire.h`), and the
  round-20 stale-branch gate needs it as its oracle. But it is never used to
  RESTORE anything. It only ever answers "which".
- **The thread's newest bubble is the game** (round 7). This design changes
  nothing about what renders, ever.

## The one fact remembered: the sent record

Per `(chatKey, gameId)`, the device keeps THE LAST CHAIN IT SENT: the payload
bytes, the seat it sent as, and the wire's `sent_at`. Key `fmsg.sent.v1` in
the App Group store, beside the seat row and the round-20 high-water mark.

- **Written synchronously in `commitPendingStage`** (`MessagesViewController`
  .didStartSending), in the same breath as `markJustSent` and `setLatestChain`.
  Not at stage time: a bubble staged and then deleted from the compose field
  (`didCancelSending`) was never in the thread and must never be reported lost.
- **Overwritten** by the next send in the same game. A retry is a new send
  with a new record; the old one is gone with it.
- **Deleted when resolved**: the shown chain contains it (kept), or the human
  has been told (superseded). One notice per send, never two.
- **Bounded** the way the hand-order rows are: an LRU of a handful of games per
  chat; a finished game's row goes with it.

THE SAFETY PROPERTY, and the sentence that separates this from the caches the
owner removed: **the sent record never decides what renders and never gates an
action. Its only output is a sentence.** Lost (reinstall, second device, a
suite with no App Group) → no sentence, and everything §6/§7 promise still
holds. Round 20's high-water mark was admitted on the same reasoning ("never
renders anything: it gates staging, and offers a button"); this record is one
step weaker still — it does not even gate.

Why the existing notes cannot serve: `justSent` is a single device-wide
one-shot slot, consumed by the first matching adopt, and answers "open my own
bubble quietly"; `latestChain` is overwritten by every adopt that beats it,
so by the time a lost move could be noticed it no longer remembers what was
sent. Both stay as they are. A later cleanup may fold `justSent` into the sent
record (`justSent` ≡ "the sent payload equals the chain being adopted"); it is
not part of this design.

## Detection: the relation between the chain I sent and the chain I see

Inputs: **S**, the sent record's payload; **N**, the chain being adopted —
tapped, arrived (`maybeAdoptIncoming`), or opened by the round-20 "Open the
latest" button. Same `gameId`, or there is nothing to compare.

The wire already carries one hop of ancestry (`parent8` — the first 8 bytes of
the parent's digest) and a body that decodes to the whole atom stream. Between
them the relation is exact, and it is decided in this order:

**Header only (no kernel, no replay):**

| test | meaning | verdict |
| --- | --- | --- |
| `digest(N) == digest(S)` | my own bubble | kept — clear the record |
| `N.parent8 == first8(digest(S))` | N is my direct child | kept — clear |
| `N.parent8 == S.parent8`, N ≠ S | N is my SIBLING: built on the same bubble I built on, so it cannot contain my move | **superseded**, subject to the Rule P guard below |

**Never reported: a bubble that added nothing.** An undo-to-empty reseal
(`MSG_NEW_NOTHING`, `env.addedNothing`) carries no move, so there is
nothing to have lost; a sent record whose payload added nothing is simply
cleared on the next adopt. Likewise a record is only ever compared within
its own `gameId`; a different game is not a relation.

The sibling test is the whole lobby story and most of the board story: two
people acting on the same bubble is, by construction, two children of one
parent. It needs no body decode at all.

**Atoms (kernel, structure only — no `Game` is built):**

For anything deeper, decode both bodies to their atom sequences (the same
`replay_decode_atoms_v6` walk `msg_replay` takes) and compare them as
sequences of `(seat, action)`:

- First strip S's TRAILING pending good(s). `log_atom_kind` (replay.c) makes a
  good an atom only while it is pending; a descendant that acted after it
  re-derives the stream without it, so a naive prefix test would call every
  "good, then somebody threw in" a loss. It is not one.
- S′ is a prefix of N → N descends from S → kept, clear.
- N is a prefix of S′ → N is OLDER than what I sent (a tapped old bubble, a
  stale delivery) → silent, record kept. The round-20 stale gate already
  says "an older move"; the record waits for a chain that can settle it.
- Otherwise they diverge at atom index *f* → **superseded**, subject to the
  guard.

**WAITING chains (phase 0)** have no atoms; the roster is the history, and
seat numbers are not identity (a leave compacts them — `leaveLobby`). After
the header tests, the content test is by NAME and by what my reseal did:

| my reseal was | kept iff |
| --- | --- |
| a join | my name is in `N.joins` |
| a leave | my name is not in `N.joins` |
| a rules change | `N.passingAllowed == S.passingAllowed` and my name is in `N.joins` |
| a Start (S is LIVE at turn 0) | N is LIVE with the same `joins` (rule 3 means a rival Start with a different count is a sibling that already resolved) |

**The Rule P guard.** A diverged or sibling N is reported only if
`msg_rule_p(S, N)` prefers N. If it prefers S, then S is the thread's
best-known state: the arrival path has already refused N
(`maybeAdoptIncoming` adopts only a strictly preferred arrival), a tapped N
gets the stale gate, and nothing is said. This guard is what makes the
verdict independent of delivery order: it is the same comparison every
device makes when it converges, so a notice is shown only for a move the
whole thread will end up without — whether my bubble is delivered before,
after, or never.

**Known limit, accepted.** A lost `good` is recognised through the sibling
test only: after the trailing-good strip, "P + my good" against "P + their
throw-in" reads as a prefix, so beyond one hop the two histories are
indistinguishable — and beyond one hop the board already shows a table that
needs a fresh "good" anyway. Two people acting on the same bubble is the
case that matters, and it is exactly one hop.

**Where it lives: C.** `msg_relation(a, b)` in `msg_wire.c`, exposed as
`fio_msg_relation` (phone) and `wasm_msg_relation` (web), returning the
relation, the fork index *f*, and how many atoms each side carries past it.
"The same bytes must mean the same game on the phone and on the web" applies
to "did my move land" as much as to Rule P; and `e2e/msg_concurrency.test.ts`
is where the web replays the same races.

## Telling: the wording

The sentence has one shape, and its two halves come from the kernel's own
event stream, never re-derived in Swift (§17.16; `MessageSummary`'s rule):

> **{what landed} before {your move} landed. {what you can do}**

- **{your move}** is S's own delta: `MessageKernel.publicRead(payload: S)`
  returns the events for the last `n_new` atoms — the same read that captions
  a bubble — rendered in the second person.
- **{what landed}** is the first action on N's side of the fork:
  `fio_replay_last_events_packed(code, viewer, atoms_before: f)` and take the
  first action, in the existing third-person `ios.msg.mv.*` vocabulary. For a
  sibling, *f* is simply N's own delta boundary.
- **{what you can do}** is offered only when the kernel says the same move is
  legal for my seat on N right now (`residentLegal(seat:)` — N is the
  resident game at adopt) AND the round has not closed
  (`N.round == S.round`, Rule R's own guard: a throw-in that would re-validate
  as next round's opening attack is legal but not what the player chose).
  Otherwise the sentence ends after the fact.

| my move | second person | typical winner | suggestion when legal |
| --- | --- | --- | --- |
| join | your join | "{name} joined" (sibling join) / "{name} started the game" (rule 0) | "Tap Join for the next seat" / none — a started game cannot be joined; "New game" is on screen |
| leave | your leave | "{name} joined" / "{name} started the game" | "Tap Leave again" / none |
| rules change | your rules change | "{name} joined" / "{name} changed the rules" | "Set it again" |
| start | your start with {m} players | "{name} started with {n} players" (rule 3: the fuller table won) | none — "playing theirs"; say it, because the DEAL differs |
| attack / throw-in | your attack with {cards} | "{name} took the cards" (round beat turn) / "{name} covered" / "{name} said good" | "Throw it in again" |
| cover | your cover of {pairs} | "{name} threw in {cards}" / "{name} said good" | "Cover again" |
| pickup | your pickup | rare: a throw-in LOSES to a pickup (round beats turn), so the only sibling that can beat it is one that also closed the bout — the attackers' final good run — and the digest decides | "Pick up again" |
| pass (transfer) | your pass of {cards} | "{name} threw in {cards}" | "Pass again" |
| good | your "good" | "{name} threw in {cards}" | "Say good again" |

Examples, in the owner's lobby case and in §7.5's:

- *Dima joined before your join landed. Tap Join to take the next seat.*
- *The game started before your join landed.* (spectator board; New game)
- *Vera started with 4 players before your start with 3 landed — playing that one.*
- *Sveta took the cards before your attack with 9♣ landed.*
- *Boris threw in 7♠ before your cover of 7♥ with 10♥ landed. Cover again.*

Keys `ios.msg.lost.*` in `FStrings`, in all three languages beside
`ios.msg.mv.*`. A pure `LostMoveWording` type turns the facts into the
string, the way `LobbyControls.offered` turns lobby facts into a control, so
a test can enumerate every kind in every language without a kernel.

### Where it shows

- **Lobby** (expanded-only, so there is room): one line directly above the
  control it explains — above the Join button `cacheDisownedByJoins` already
  brought back, above Leave, above the rules checkbox. It stays until the
  human acts.
- **Board**: the top strip slot the round-20 stale bar uses, in its own colour
  and, unlike that bar, with the board fully LIVE underneath — "try again" is
  the point. Dismissed by tap, and cleared by the next adopt. Not the
  `fFlash` reject toast: that is a half-second flash, too brief for a sentence
  that names cards.
- **Spectator board** (a join that lost to a Start): the same strip, over the
  public table, with the one control available: New game.
- **Compact drawer**: the strip fits the drawer's header band; if a layout
  cannot hold it, it is deferred to the next expand rather than truncated.

### When it fires

At every path by which a chain reaches the surface — a tap
(`present` → `load` → `adopt`), an arrival (`maybeAdoptIncoming` → `adopt`),
"Open the latest" (`openNewest` → `adopt`), and the phase-0 lobby route inside
`adopt` — one relation check against the sent record, then the record is
cleared if it was resolved either way. It is consulted nowhere else.

## The red retraction fires with the notice

The 1.0(28) conflict model (`ANIMATION_CATALOGUE.md`, "The conflict model")
is the animation half of exactly this event: a card of mine leaving the board
because a newer chain disowns it flies home the way it came, tinted red,
before the newer chain plays forward. Today it has two triggers, and neither
is the Rule N case:

- `MessageTurnController.offerArrival` retracts STAGED moves only
  (`guard boardWatching, !pending.isEmpty`). Pressing Send runs `markSent`,
  which empties `pending` and rebases the controller onto the sent chain — so
  by the time a sibling arrives the move lives in `base`, and `offerArrival`
  falls through to the plain `adopt`.
- The board's reversal debt (`reversalDebt`, `drainSupersededForReversal`)
  covers motion a sequence had already flown when a newer arrival superseded
  it MID-ANIMATION. Once the send's own animation has landed the ledger is
  empty. The catalogue says so itself: the REVERT verdict is meant for "a
  fork loser's motion", but "no rig run has ever shown it; a fork arrival
  mid-animation would" — mid-animation is the only road to it.

So for a move that was sent, whose animation has landed, and which a fork
then supersedes — the ordinary Rule N case — the card today goes back to the
hand by the plain adopt's diff: a snap, with nothing to say it was mine or
why it went. That is the same silent swap the conflict model was built to
end, arriving one bubble later.

Rule N supplies the missing trigger, and this design REQUIRES the two to fire
together: the sentence explains the red, the red shows the sentence.

- In `offerArrival`, after the duplicate and mid-retraction checks, when
  `pending` is empty: ask `msg_relation(base, arriving)`. If the arriving
  chain does not descend from the base AND the base's own delta is mine
  (`base.lastActorSeat == mySeat`, `n_new > 0`, not `addedNothing`), the
  base's last `n_new` atoms ARE the retraction. Publish the board BEFORE that
  delta — `openChain(base).prior`, which the kernel already returns — with
  `lastChangeWasUndo` up, latch the arrival, arm the failsafe. The board's
  existing conflict-mode `flyUndoReturn` / `flyUndoRelease` then fly my
  cards home in red from the frames they have been sitting in, and
  `finishConflictAdopt` adopts the arrival when the flight lands. Nothing
  new is choreographed; the retraction is the staged one with a different
  source of "what to retract".
- `ConflictFacts` are peeked from the arriving chain exactly as now, so a
  card the winner covered or picked up is KEEP / CLEAR and never flies home
  first — the clear-flicker rule holds unchanged. The input freeze during a
  retraction (`apply` / `undo` refuse while `conflictRetracting`) holds too.
- The Rule P guard runs before any of this: an arrival the base beats is
  refused by `maybeAdoptIncoming` and never reaches `offerArrival`.
- A good, a join, a leave, a Start: no card of mine is on the table, so the
  retraction changes nothing visible and `offerArrival`'s existing
  equal-view shortcut adopts immediately (as it does for a staged good
  today). The sentence stands alone.
- A cold open (the extension was closed; the human taps the winner's bubble)
  builds a fresh controller on the winner's chain: my lost card was never on
  THIS screen, there is nothing to reverse, and the winner's move plays as a
  cold open would. The sentence stands alone — the reversal is theatre for a
  card the human is looking at.
- The notice is raised when the retraction begins (with nothing to retract,
  at adopt), so it is on screen while the red flight plays, and the sent
  record is cleared on the same path either way.

The retraction never consults Rule P for anything but the guard, never
re-applies anything, and renders only states that were real chains: the
base's parent, then the arrival.

Tests: `ConflictModelTests` gains the sent-then-forked case — a controller
after `markSent`, a sibling offered: `conflictRetracting` goes up, the
published view is the pre-delta board, the latched chain is adopted when the
board reports landing, and a descendant offered the same way retracts
nothing. The rig gains `HARNESS_ARRIVE_FORK=1` (send, then a sibling of the
parent arrives), which finally poses the REVERT verdict outside a unit test —
the catalogue's standing "never seen on a screen" gap.

## Delivery order and slow connections

The claim, checked case by case: **the notice goes only to the loser, only
once the loser can see the winner, and says the same thing regardless of the
order in which anything was delivered.**

1. **Two joins, every other device off.** B and C both join off the creator's
   bubble and both send. Each receives the other's bubble; both are siblings
   at 2 joins; the digest picks one, say C's. On B's device: N is a sibling,
   Rule P prefers N → *"Dima joined before your join landed. Tap Join to take
   the next seat."* The Join button is already back. On C's device: B's
   chain arrives, loses Rule P, is refused — silence, correctly. B taps Join,
   sends a 3-join child of C's chain; rule 3 and rule 4 converge everyone on
   it. Nobody needed the second-to-last message: B needed only its own record
   and the bubble in front of it.
2. **The loser's extension was closed.** No arrival; the notice waits for the
   next open of the thread's newest bubble, which is the first moment B can
   see anything at all. Same sentence.
3. **One-way slow.** B's bubble reached C; C's never reached B. B sits on its
   own chain with no evidence of a fork and sees nothing — correct: nothing
   is visible to B. C's device holds both; if B's wins Rule P, C is told;
   if C's wins, C sees nothing and B will be told when C's bubble eventually
   lands. The notice tracks visibility, not wall-clock.
4. **My send is slow; their move arrives first.** N beats S under Rule P →
   I am told now. When S finally lands everywhere it loses the same
   comparison on every device — the notice was right early, not wrong. If
   instead S would have won (higher turn, higher round), the guard keeps me
   silent, S lands and wins, and THEY are told. Same outcome as the network
   converges to; only the timing moved.
5. **Three-way race.** Two losers, two notices, two retries. A retry can race
   again — and is told again. Rosters only grow toward capacity 8; the loop
   terminates as it always did, now visibly.
6. **A join in flight when someone taps Start.** My join and the Start are
   siblings of the lobby; rule 0 prefers the started chain → *"The game
   started before your join landed."* on the spectator board. No suggestion
   — a dealt game cannot be joined — and no wider re-Start: that would be a
   priority scheme. M9's authorship gate still narrows the window; the
   sentence makes the residual honest instead of silent.
7. **Pickup ∥ throw-in (§7.5), either order.** The pickup closes the bout,
   higher round wins everywhere; the attacker's device is told *"Sveta took
   the cards before your attack with 9♣ landed."* — no suggestion, the round
   guard fired. The defender is told nothing. Reversed delivery: identical.
8. **Double Start.** Rule 3 picks the fuller table; the other starter is told
   *"…before your start with 3 landed — playing that one."* — worth saying out
   loud because the two deals differ, which is precisely the 4-player
   deadlock this thread once had.

## What this deliberately does not do

- **No re-apply.** Rule R stays a kernel capability exercised by the wasm
  bridge; iOS never calls it (round 9 stands).
- **No merge.** §7.7's rejection of Rule M stands, and this design has no
  input Rule M lacked — it reads exactly one foreign chain at a time, the one
  in front of it.
- **No automatic re-claim or re-stage.** The human decides whether to try
  again; any staging needs their Send anyway.
- **No timers, no clocks.** `sent_at` is stored with the record for the
  flight recorder, not consulted for any verdict.
- **No wire change.** `parent8`, `n_new`, the digest and the body already say
  everything the relation needs.
- **No new animation vocabulary.** The red reversal is the 1.0(28) one; this
  design only hands it the trigger it was missing for a SENT move.
- **No new read-only state.** The notice never makes a board read-only; the
  round-20 stale gate remains the only path that does, untouched.

## What stays exactly as it is

Rule P (all five rules); round 7's "render the tapped bubble"; the round-20
high-water mark and its gate; M9; `cacheDisownedByJoins` and the Join button
it restores (the notice is the explanation, not a replacement);
`justSent`; the M9 full-lobby exemption; `leaveLobby`'s accepted race note
(now with a sentence attached).

## Implementation map (for whoever picks this up)

| layer | change |
| --- | --- |
| `c/src/msg_wire.{h,c}` | `msg_relation(a, b, *out)` — header tests, atom-prefix with trailing-good strip, phase-0 roster rules, fork index and tail counts |
| `c/ios/ios_api.{h,c}`, `c/wasm/wasm_api.c` | `fio_msg_relation`, `wasm_msg_relation` |
| `c/tests/msg_wire_test.c` | relation invariants (below) |
| `sdk/swift/MessageEnvelope.swift` (MessageKernel) | `relation(_:_:)` |
| `ios/FoolishKit/Messages/MessageGameStore.swift` | the sent record: `recordSent`, `sentChain`, `resolveSent`; LRU |
| `ios/FoolishMessages/MessagesViewController.swift` | `commitPendingStage` writes the record |
| `ios/FoolishKit/Messages/LostMove.swift` (new) | `LostMove` (facts) and `LostMoveWording` (pure facts → string) |
| `ios/FoolishKit/Messages/MessageTurnController.swift` | `offerArrival`: a sent delta the arrival disowns is retracted (red) exactly like a staged one, from `openChain(base).prior` |
| `ios/FoolishKit/Messages/MessagesRootView.swift` | `adopt` asks the relation, sets `lostMove`; lobby line; board strip |
| `ios/FoolishKit/DesignSystem/FStrings.swift` | `ios.msg.lost.*`, en/ru/ko |

## Tests to pin

**Kernel (`msg_wire_test.c`, and the wasm twin in `e2e/`):** same → same;
child → descends; sibling → sibling; grandchild → descends via atoms; a
pending good followed by another seat's attack in the child → descends (the
strip); the same pair as siblings → sibling; a fork inside history →
diverged with the right *f*; older → ancestor; both arguments swapped give
the mirrored answer; phase-0 join/leave/rules/start rules by name; a
tampered header cannot make an ancestor look like a sibling (`parent8` and
digest disagree → diverged, never kept).

**Swift (`MessageLostMoveTests.swift`):**
- The §7.5 fixture pair (`MessageConcurrencyTests` already builds it): the
  attacker is told, the defender is not, in both delivery orders.
- The Round-5 lobby race fixtures: the losing joiner is told and offered Join;
  the winner is silent; the 9th-player-into-a-full-lobby loser is told with
  no suggestion.
- Join vs Start: told, no suggestion, spectator route.
- Double Start off the 3-join and 4-join views: the 3-player starter is told
  and the sentence names both counts.
- The guard: when S is preferred, nothing is said and the record survives;
  the next preferred chain settles it.
- Record lifecycle: written only on `didStartSending`, not on stage; a
  cancelled stage writes nothing; overwritten by the next send; cleared on
  kept; cleared on told; LRU eviction.
- `LostMoveWording`: every (kind × winner × suggestion) cell in en/ru/ko is
  non-empty and contains the card names the facts carry.

**Harness (`HarnessFlowTests`):** two fake participants join the same lobby
bubble and both send; exactly one of the two sees the line, and one more tap
of Join converges both.

## Open questions for the owner

1. **A lost "good" beyond one hop** is not reported (see the known limit).
   Acceptable, or should a good that vanished be mentioned even when the
   cause cannot be named?
2. **How old is too old?** A move lost twelve turns ago is still true and
   still reported once. Cap it (say, only when `N.round <= S.round + 1`), or
   let the sentence stand?
3. **Strip or toast on the board** — the design says a dismissable strip so
   card names can be read; if the strip crowds the compact drawer, the
   fallback is defer-to-expand rather than a flash.
4. **The suggestion suffix** — keep it kernel-gated as designed, or drop it
   and let the live board speak for itself?
