# UI state flows: iMessage vs web

How the *visible* board can change on each client, drawn as state machines.
Two clients, two truths: on iMessage the thread's newest bubble is the game and
nothing is authoritative until a human presses Send; on the web the server's
`games.version` is the game and every local move is a prediction the server
will confirm, contradict, or sweep away. Both clients animate optimistically
and both revert in red, but *why* a card flies home differs, and that is the
last section.

Every state and edge below is read from the code on 2026-09-07. Where a design
doc disagrees with the code, the code wins. Anchors are `file:line`.

---

## Part 1: iMessage

### 1.1 Surfaces

The extension has no screen enum. `GameSurface.expandedContent` picks the first
screen whose guard holds, in this order
(`ios/FoolishKit/Messages/MessagesRootView.swift:920-1092`): board, lobby, name
gate, setup, seat picker (DEBUG), spectator, damaged, blank wool. Routing comes
from `MessageSurfaceRouter.resolve`
(`ios/FoolishKit/Messages/MessageSurfaceRouter.swift:52-65`).

Compact and expanded are **not** branches of this machine. One `GameSurface`
renders both presentation styles, sized by live geometry
(`MessagesRootView.swift:296-345`); the only things that read the height are
the send-hint and the opponent-ring radius via `collapseFraction`
(`ios/FoolishKit/Boards/MessageTableView.swift:1412`).

```mermaid
stateDiagram-v2
    direction TB
    [*] --> Load : bubble tapped, New game, or chat change

    state Load <<choice>>
    Load --> Setup : no payload, or New game
    Load --> Damaged : decode fails
    Load --> Lobby : phase WAITING
    Load --> SeatResolve : phase LIVE or FINISHED

    state SeatResolve <<choice>>
    SeatResolve --> Board : seat known
    SeatResolve --> NameGate : seat known, no nickname stored
    SeatResolve --> Spectator : seat unknown or ambiguous

    NameGate --> Board : name entered

    Setup --> Lobby : Create game, stages WAITING bubble, seat 0 cached
    Lobby --> Lobby : Join, Exit, passing tick, Invite (each re-stages)
    Lobby --> Board : Start, seals LIVE handoff, fresh controller
    Board --> Results : terminal move sent, gameOverHold 1s
    Results --> Lobby : New game with full named roster (rematch)
    Spectator --> Spectator : finished game, replayable board at seat minus one

    Board --> Board : arrival wins Rule P, adopt in place
    Lobby --> Lobby : arrival wins Rule P, adopt
    Board --> Superseded : chain behind high-water mark
    Superseded --> Board : tap Open newest

    Board --> Blank : loadKey changes, controller kept
    Blank --> Load
```

Anchors: setup to lobby `MessagesRootView.swift:1388-1450`; join/exit/passing
`:1487-1584`, `:1629-1681`; start `:1742-1758`; rematch `:1349-1375`; adopt
`:718-762`; superseded bar and Open newest `:649-681`; name gate `:1843-1846`,
`:1955-1961`; seat ambiguity `:1850-1902`; reload keeps the controller
`:1115-1128`; results after `settleResults` `MessageTableView.swift:3307-3313`.

Two host-level transitions ride over the top of this machine:

- **Auto-collapse after staging.** From expanded, a staged move waits 250 ms,
  then `BoardAnimator.waitForSettle` (so the flight lands first), then 500 ms,
  then requests compact and inserts the bubble only after `didTransition`
  (`ios/FoolishMessages/MessagesViewController.swift:565-603`). The tween
  itself is 0.38 s (`MessagesRootView.swift:253-267`) and blocks flight aiming
  while it runs (`MessageTableView.swift:3356-3383`).
- **Send from expanded dismisses the extension; send from compact keeps the
  drawer open** (`MessagesViewController.swift:237`).

### 1.2 The board: one move, from tap to thread

There is no mode enum on the board either. The visible state is the product of
the controller's flags, `pending` (staged moves), `sending`, `settlementHeld`,
`conflictRetracting`, `superseded`, `pickupHold`
(`ios/FoolishKit/Messages/MessageTurnController.swift:67-76`, `:323`, `:429`,
`:481`, `:643`), and the board's animator ownership (`sequenceDepth`,
`animSequenceToken`, `arrivalEpoch`, `MessageTableView.swift:435-506`).

The diagram below is the lifecycle of a single local move. The three animation
channels named on the edges are the catalogue's
(`docs/ANIMATION_CATALOGUE.md`): **A** plays the action half on the tap, **B**
plays the withheld settlement half on Send, and undo plays A backwards.

```mermaid
stateDiagram-v2
    direction TB
    state "Idle: buttons from kernel legal set" as Idle
    state "Refused: haptic, white flash, veil released" as Refused
    state "Staged: card flown (channel A), Undo pill, bubble in compose field" as Staged
    state "Settlement held: bout end, discard and refills withheld, legal set empty" as Held
    state "Undoing: exact reverse flight" as Undoing
    state "Sending: Undo pill on the tap, board rebases onto sent bytes" as Sending
    state "Released: settlement plays now (channel B)" as Released
    state "Conflict retracting: base republished with pending empty, red reverse flight" as Retract
    state "Superseded: read only, bar offers Open newest" as Sup

    [*] --> Idle
    Idle --> Idle : pickup hold ticks 15s, Take pill appears
    Idle --> Refused : admit fails
    Refused --> Idle
    Idle --> Staged : drop or tap, admit ok, preHide and fly
    Staged --> Held : the move closes the bout
    Staged --> Staged : another legal move, chain grows, bubble re-staged
    Staged --> Undoing : Undo
    Held --> Undoing : Undo, hold dropped not released
    Undoing --> Staged : shorter chain re-staged
    Undoing --> Idle : undo to empty, base resealed as carrying nothing
    Staged --> Idle : bubble deleted in compose field, didCancelSending
    Staged --> Sending : Send tapped, didStartSending
    Held --> Sending : Send tapped
    Sending --> Released : markSent, base and parent8 rebased
    Released --> Idle : discard, serial refills, role handoff land
    Released --> Results : terminal move, gameOverHold 1s
    Staged --> Retract : arrival wins Rule P (TurnWire says retract)
    Held --> Retract : arrival wins Rule P
    Retract --> Idle : reversal lands, finishConflictAdopt, arrival replays forward
    Retract --> Idle : failsafe 3s, adopt anyway
    Idle --> Sup : own chain behind the high water mark
    Sup --> Idle : Open newest adopted
```

What each edge does on screen:

- **Idle to Staged.** `playAt` snapshots takeoff rects, pre-hides the played
  cards and shows a resting ghost in the same frame
  (`MessageTableView.swift:5116-5211`), then `play` freezes the counts and,
  for pickup or good, keeps the table drawn as a sweep so it never blinks empty
  (`:4838-4862`). `controller.apply` admits through `TurnWire.admit`, appends to
  `pending`, and `captureSettlement` cuts a bout-ending turn at the kernel's
  settlement boundary (`MessageTurnController.swift:1109-1142`, `:390-416`).
  The board's `onChange(of: controller.view)` flies the placement
  (`MessageTableView.swift:2134-2323`). Staging seals base plus pending into a
  bubble and inserts it; insert only stages, it never sends
  (`MessageTurnController.swift:1561`, `MessagesViewController.swift:447-603`).
- **Staged to Sending to Released.** `didStartSending` commits the cache
  synchronously (just-sent marker, seat, high-water chain,
  `MessagesViewController.swift:187`, `:636-657`) and bumps `sentToken`; the
  surface calls `markSending` at once (the Undo pill goes on the tap, not after
  the rebase) then `markSent`, which rebases base, parent8, joins and boundary
  and releases the held settlement (`MessagesRootView.swift:572-586`,
  `MessageTurnController.swift:1211-1297`, `:1456-1478`). Nothing re-animates
  the move itself; only the withheld discard, refills and role flights play
  (`MessageTableView.swift:2289-2292`). A send handed the wrong bytes still
  releases the hold and clears `sending`, but rebases nothing
  (`MessageTurnController.swift:1359-1455`).
- **Undo.** `undo` drops the last pending move in one assignment, drops the
  hold, sets `lastChangeWasUndo` (`MessageTurnController.swift:1487-1519`).
  The board routes that into `flyUndoReturn` (table to hand) or
  `flyUndoRelease` (hand back to the table, for an undone pickup)
  (`MessageTableView.swift:2263-2281`, `:3491`, `:3622`). Undo to empty
  re-seals the base with `MSG_NEW_NOTHING` (`:5011-5018`).
- **Refused.** `rejectTick` plays a haptic and white flash and
  `releaseLivePlayVeil` reveals the pre-hidden cards again
  (`MessageTableView.swift:783-798`, `:4936-4978`).
- **Conflict retract.** See 1.3.

### 1.3 Arrivals: a bubble lands while the board is open

`didReceive` drops my own chain coming back (`StagedBubbleRouting.isMine`,
`MessagesViewController.swift:173`), otherwise hands the bytes to the surface
through `incomingToken`; Apple never moves `selectedMessage`, so this is the
only channel a live arrival has (`MessagesRootView.swift:594`, `:697-717`).

```mermaid
flowchart TD
    R([didReceive]) --> M{my own chain?}
    M -- yes --> D0[drop]
    M -- no --> S{same bytes as shown?}
    S -- yes --> D1[nothing]
    S -- no --> P{Rule P prefers it?}
    P -- no --> D2[nothing on screen, stale or older]
    P -- yes --> DEC{decodes?}
    DEC -- no --> D3[nothing]
    DEC -- yes --> TW{TurnWire.arrival}
    TW -- skip, duplicate of my chain --> D4[nothing, staged move survives]
    TW -- latch, already retracting --> L[replace the one slot latch, newest wins]
    TW -- retract, a move is staged --> RT[publish base with pending empty]
    RT --> RED[red reverse flight of staged cards, last motion first]
    RED --> FIN[finishConflictAdopt]
    L -.-> FIN
    TW -- adopt --> AD{canAdopt into live controller?}
    FIN --> AD
    AD -- same game, seat, ready --> FOLD[fold in, no teardown, no flash]
    AD -- otherwise --> FRESH[fresh controller]
    FOLD --> RP[replayPending raised with the new view]
    FRESH --> RP
    RP --> ANIM{sequence in flight?}
    ANIM -- no --> PLAY[replay the arriving turn as a cold open]
    ANIM -- yes --> EP[bump arrivalEpoch, let the in-air step land]
    EP --> DEBT[drain superseded sequence's flown motions]
    DEBT --> V{kernel verdict per motion}
    V -- revert --> VR[fly back red]
    V -- keep --> VK[stays, arriving board shows it there]
    V -- clear --> VC[leave it, the forward replay moves it]
    VR --> PLAY
    VK --> PLAY
    VC --> PLAY
    PLAY --> RES{game over in the chain?}
    RES -- yes --> OVER[results after gameOverHold]
    RES -- no --> IDLE[idle at the new base]

    style RED stroke:#DC2626,stroke-width:2px
    style VR stroke:#DC2626,stroke-width:2px
```

Anchors: `maybeAdoptIncoming` `MessagesRootView.swift:718-762`; `adopt` and
seat resolution `:1773-1940`; `canAdopt` `MessageTurnController.swift:631`;
`offerArrival` verdicts and the retraction `:697-800`; `finishConflictAdopt`
and the 3 s failsafe `:668`, `:785`; `publish` raising `replayPending`
`:1040-1058`; board picks it up at `MessageTableView.swift:2142`, `:2284`;
mid-animation epoch and reversal `:4622-4639`, `:3722-3758`; the verdicts as
flights `ios/FoolishKit/Boards/ConflictModel.swift:49-64`; the red tint on the
ghost only `ios/FoolishKit/Boards/BoardFlight.swift:465-492`.

Three rules worth stating in words:

- **Bursts are not a queue.** Several arrivals in quick succession reverse
  whatever is animating and play only the last one; intermediate boards are
  never shown (`MessageTableView.swift:4622`).
- **The board never renders new base with old staged moves, not for one
  frame.** That is why the retraction is published against the *old* base and
  the arrival is adopted only after the red flight lands
  (`MessageTurnController.swift:690-696`).
- **Clear beats revert.** If the arriving chain already contains my staged
  card (their cover or pickup took it), it must not fly home first; the forward
  replay owns it. This is the "put a card down, someone picked it up, it flew
  back to my hand" flicker, avoided on both clients.

The five lagging display values (deck, discard, per-seat counts, out set, role
marks) live in `ShownLedger` and can only be written by the sequence that owns
the animator (`ios/FoolishKit/Boards/ShownLedger.swift:56-118`, `:244-253`).
`FlightRecorder` writes breadcrumbs for every adopt, send, rebase and conflict
to the App Group and raises the health banner on the next launch if the last
run ended badly (`ios/FoolishKit/Messages/FlightRecorder.swift:81-118`).

### 1.4 Timers

| Timer | Where | What moves |
| --- | --- | --- |
| Post-stage collapse: 250 ms, waitForSettle (8 s cap), 500 ms | `MessagesViewController.swift:565-567` | drawer down to Messages' Send |
| Collapse tween 0.38 s, released after 1.2 s | `MessagesRootView.swift:259-294` | box height |
| Pickup hold 15 s, ticks once a second | `MessageTurnController.swift:1071-1084` | Take pill appears by itself |
| Flight 0.5 s plus 25 ms gap, boutEndHold 1.5 s, gameOverHold 1.0 s | `BoardFlight.swift:21-69` | every card motion |
| Conflict failsafe 3 s | `MessageTurnController.swift:668` | adopts the latch if the board dies mid-flight |
| Send-hint arrow 3 s fuse | `MessageTableView.swift:5513-5518` | hint fades |

No bots run inside the extension; auto-play exists only under DEBUG harness
flags (`MessagesRootView.swift:1263`).

---

## Part 2: Web

### 2.1 Surfaces

The `/:game_id` route renders a replay directly for a long base32 segment and
otherwise mounts the protected provider tree
(`src/app/[game_id]/page.tsx:16-26`). Inside it, `GameView` picks a screen
from the loaded game's `status` (`src/components/GameView.tsx:40-62`).

```mermaid
stateDiagram-v2
    direction TB
    [*] --> Route
    state Route <<choice>>
    Route --> Replay : long path segment
    Route --> AuthPending : short game code
    AuthPending --> Welcome : no user, router.replace to root
    AuthPending --> Loading : user resolved, providers mount
    state "Loading skeleton (lobby shaped)" as Loading
    Loading --> NotFound : both view caches miss
    NotFound --> Dashboard : redirect
    Loading --> Lobby : status WAITING
    Loading --> KernelWarmup : status PLAYING, guards.wasm not ready
    KernelWarmup --> Board : kernel ready
    Loading --> Board : status PLAYING
    Loading --> Win : status GAME_OVER
    Lobby --> Lobby : no self and room, auto join
    Lobby --> Board : start broadcast flips status
    Board --> Win : final broadcast commits at end of animation queue
    Win --> Lobby : continue, optimistic resetToLobby before the round trip
    Lobby --> Win : continue fails, rollback
    Board --> Spectator : exit self, or no self on load
    Spectator --> Win : final broadcast on the game channel
    Board --> Error : render throw
    Lobby --> Error : render throw
    Error --> Board : Try Again
```

Anchors: auth gate `src/components/ProtectedRoute.tsx:20-52`; load and the
not-found throw `src/contexts/ServerContext.tsx:150-169`, `:623-661`; auto join
and spectator subscription `:690-717`; game over committed from the queue's
completion callback `src/contexts/AnimationContext.tsx:991-1023`; optimistic
continue and rollback `src/components/WinScreen.tsx:120-126`,
`ServerContext.tsx:1133-1153`, mirror in `src/state/clientReconcile.ts:19-41`;
exit to spectator `ServerContext.tsx:517-552`; error boundary
`src/components/ErrorBoundary.tsx:142-191`.

Spectating is a mode of the board, not a screen: `game.self` is null, the hand
and buttons are gone and `ActionButtons` says Spectating
(`src/components/GameDisplay/ActionButtons.tsx:54-56`, `:179-181`).

### 2.2 The board: four orthogonal machines

The board (`src/components/GameBoard.tsx:74-109`) is one composition whose look
is the product of four independent machines. They compose: you can be
mid-drag while the animation queue is draining, and a pressed button can be
hidden while a revert flies.

```mermaid
stateDiagram-v2
    direction LR
    state "Animation (AnimationContext)" as A {
        direction TB
        AIdle : queue empty, overlay null
        AFly : real card hidden, clone flies 500ms in fixed overlay
        ARevert : same flight, red border, red glow, pink face
        AIdle --> AFly : queue non empty
        AFly --> AFly : commit event game_state at 500ms, 25ms gap, next event
        AFly --> ARevert : next event is_revert
        ARevert --> AFly
        AFly --> AIdle : queue empty, commit final message.game
        ARevert --> AIdle : queue empty
    }
    state "Input (DragContext)" as I {
        direction TB
        IIdle
        Pressed : no visual change
        Rearrange : dragged card at 0.3 opacity, hand swaps live
        ForAction : DragShadow with Attack, Cover or Pass badge, drop zones lit
        IIdle --> Pressed : pointer down on a hand card
        Pressed --> IIdle : release under 150ms, toggles selection
        Pressed --> Rearrange : moved over 10px
        Rearrange --> ForAction : cursor leaves the hand strip
        ForAction --> Rearrange : cursor returns
        Rearrange --> IIdle : release, rearrange persisted after 5s debounce
        ForAction --> IIdle : release, attack, cover, multicover, pass or no op
    }
    state "Keyboard (KeyboardPlayMode)" as K {
        direction TB
        none --> cards : left or right
        cards --> target : up as defender, ambiguous cover or pass
        target --> cards : down, or up fires cover or pass
        cards --> cards : up as attacker fires attack, down fires pickup or good
    }
    state "Pressed actions (GameContext)" as P {
        direction TB
        Shown --> Hidden : button clicked or key fired
        Hidden --> Shown : predicate rising edge, or rejection
    }
```

Anchors: animation machine `AnimationContext.tsx:1237-1362`, overlay
`src/components/GameDisplay/AnimationOverlay.tsx:245-579`, card hidden while
flying `src/components/GameDisplay/CardFace.tsx:112-117`; drag machine
`src/contexts/DragContext.tsx:165-382`, badge
`src/components/GameDisplay/DragShadow.tsx:28-59`, drop zones
`src/components/GameDisplay/TableBattles.tsx:35-68`; keyboard
`src/components/GameDisplay/KeyboardPlayMode.tsx:218-304`, `:355-363`; pressed
actions `src/contexts/GameContext.tsx:24-26`, `ActionButtons.tsx:160-194`.

### 2.3 The optimistic path: one move, from tap to verdict

Every input (drop, click, key) funnels into `AnimationContext.attack`, `pass`,
`pickup` or `cover` (`AnimationContext.tsx:1451`, `:1529`, `:1605`, `:1671`).
The POST is fired first, stamped with the last applied authoritative version
as `intent_version` (`ServerContext.tsx:1209-1217`,
`src/state/authoritativeVersion.ts:12-27`), then the same bytes are validated
locally against `guards.wasm`; only a locally valid move animates.

```mermaid
stateDiagram-v2
    direction TB
    state "Idle" as Idle
    state "In flight: card flies now, button hidden, POST in flight" as Fly
    state "Pending: card rests at its optimistic spot, tracked by (type, card, from, to, player)" as Pend
    state "Confirmed silently: server event dedup'd, no second flight" as Conf
    state "Released: table shows it, this broadcast did not name it" as Rel
    state "Kept: merged into every incoming game_state until its own broadcast" as Keep
    state "Cleared: tracking dropped, the sweep animates it off" as Clr
    state "Reverting: red flight home, spliced before the replacing event" as Rev
    state "Rejected: red flight from last visual spot, toast if stale round" as Rej

    [*] --> Idle
    Idle --> Idle : local validation fails, nothing moves, server reject is ignored
    Idle --> Fly : valid, triggerOptimisticAnimation, state patch scheduled at 500ms
    Fly --> Pend : flight lands, hand and table mutate
    Pend --> Conf : confirming broadcast names the card
    Conf --> Idle
    Pend --> Rel : broadcast shows it on the table but does not name it
    Rel --> Idle
    Pend --> Keep : broadcast lacks it, kernel says keep
    Keep --> Conf : its own broadcast arrives
    Pend --> Clr : broadcast lacks it, kernel says clear
    Clr --> Idle
    Pend --> Rev : broadcast lacks it, kernel says revert
    Pend --> Rev : pickup verdict revert or clear (both fly back)
    Rev --> Idle : lands in hand, sticky slot restored
    Pend --> Rej : HTTP reject, not already reverting
    Fly --> Rej : HTTP reject
    Rej --> Idle : button reappears
    Pend --> Idle : GC, entry older than 30s
    Pend --> Pend : reconnect resync re-applies the card via optimisticOverlay
```

The edges in detail:

- **Idle to In flight.** `triggerOptimisticAnimation` queues a client event
  that flies hand to table (or table to hand for pickup) exactly like a server
  event, and records the card in `optimisticAnimations` and
  `optimisticCardPositions` (`AnimationContext.tsx:1405-1442`). A pass also
  records the predicted defender rotation (`:1554-1561`). `ServerContext`
  applies the state mutation at `ANIMATION_TIME` so the hand and table change
  as the card lands (`ServerContext.tsx:768-912`). The rendered hand is a
  sticky-slot selector, so a card that leaves and comes back keeps its place
  (`clientReconcile.ts:127-147`).
- **Confirmed silently.** When the confirming broadcast arrives, each server
  event whose every card matches an optimistic entry under the same
  `(type, card, from, to, player)` key is dropped from the queue and untracked;
  if all events were optimistic the state is committed with no animation at
  all (`AnimationContext.tsx:932-969`). This is the "never animate my own
  card twice" rule.
- **Released.** Cards the authoritative table shows but this broadcast does
  not name were confirmed by a broadcast the version gate dropped; they are
  released without animating (`:879-899`, `src/state/optimisticAnimation.ts`,
  delegating to `anim_stale_optimistic_on_table` in C).
- **Kept, cleared, reverted.** `resolveOptimisticConflicts`
  (`AnimationContext.tsx:330-757`) asks the C animation core
  (`anim_conflict_verdict`, marshalled by `src/state/optimisticConflicts.ts`)
  for a verdict per pending card. Keep merges the card into every `game_state`
  in the message (`:678-753`); clear drops tracking and lets the broadcast's
  pickup or trash sweep move it (`:640-645`); revert queues a red flight home.
  Pickup verdicts revert *and* clear both fly back, because the card is in my
  hand and the sweep runs from the table (`:565-619`).
- **Revert ordering.** Revert events are spliced immediately before the
  replacing attack, pickup or `magic_transition` so the retraction and the
  replacement read as one beat (`:1189-1227`), and each revert carries a
  synthetic `game_state` that still shows the cards where they were so nothing
  teleports (`:1071-1169`).
- **Rejected.** An HTTP rejection queues a revert from the card's last visual
  location unless conflict detection already reverted or cleared it
  (`:1484-1526` and siblings). `REJECT_STALE_ROUND` also raises the 4 s toast
  (`ServerContext.tsx:1231-1239`, `src/components/GameDisplay.tsx:22-33`).

### 2.4 Remote arrivals

```mermaid
flowchart TD
    B([broadcast on gu channel, or masked game channel for spectators]) --> DEC{decodes with the local roster?}
    DEC -- no --> DROP1[drop, refetch authoritative state]
    DEC -- yes --> VER{version above last applied?}
    VER -- no --> DROP2[drop, superseded]
    VER -- yes --> DUP{seen sequence_id or content signature?}
    DUP -- yes --> DROP3[drop]
    DUP -- no --> REL[release stale optimistic entries the table already shows]
    REL --> CONF[resolve optimistic conflicts: revert, keep, clear]
    CONF --> DEDUP[drop events that were my own optimistic flights]
    DEDUP --> ALL{anything left to animate?}
    ALL -- no --> COMMIT[commit final game silently]
    ALL -- yes --> Q[append to the animation queue, reverts spliced before their replacement]
    Q --> DRAIN{queue draining?}
    DRAIN -- no --> START[start the drain]
    DRAIN -- yes --> TAIL[plays after the current sequence, never preempts]
    START --> STEP[fly one event, commit its game_state at 500ms, 25ms gap]
    TAIL --> STEP
    STEP --> MORE{more events?}
    MORE -- yes --> STEP
    MORE -- no --> FINAL[commit final message.game from the completion callback]
    FINAL --> OVER{status GAME_OVER?}
    OVER -- yes --> WIN[win screen]
    OVER -- no --> IDLE[idle]
    RS([channel re-subscribed]) --> LOAD[loadGame resync, optimistic cards re-applied]
    LOAD --> VER
    BUMP([no bot motion for 5s and a bot can move]) --> POST[POST bump]
    POST --> B

    style CONF stroke:#DC2626,stroke-width:2px
```

Anchors: materialisation and refetch `AnimationContext.tsx:766-852`; version
gate `:869-874` via `clientReconcile.ts:55-56` and C
`anim_should_drop_stale`; dedup `:93-110`, `:905-922`; queue drain
`:1237-1370`; the authoritative final state is held until the queue empties
`:991-1024`, `:1249-1254`; dashboard pushes skip the on-screen game for the
same reason `ServerContext.tsx:351-356`; resync on resubscribe
`src/state/RealtimeAnimationFeed.tsx:92-104` and the overlay re-apply
`ServerContext.tsx:639-645`, `clientReconcile.ts:155-168`; bot bump
`AnimationContext.tsx:241-291`.

What an arrival does to the other machines:

- **While dragging:** nothing interrupts the drag. The hand under it can
  shrink, so `reorderHand` and `isHandPermutation` no-op on out-of-range
  indices and the keyboard cursor clamps (`clientReconcile.ts:95-125`,
  `KeyboardPlayMode.tsx:101-103`).
- **While a queue drains:** the new sequence is appended. There is no
  preemption and no "newest wins"; every intermediate board is shown.
- **Out of order or skipped versions:** a lower version is dropped, a gap is
  simply jumped, because every sequence carries its full resulting state.

### 2.5 Timers

| Timer | Where | What moves |
| --- | --- | --- |
| Flight 500 ms plus 25 ms gap | `AnimationContext.tsx:1308-1362`, `constants.ts:2` | one card, then its state commit |
| Optimistic state patch at 500 ms | `ServerContext.tsx:768` | hand and table mutate as the card lands |
| Bot bump every 5 s | `AnimationContext.tsx:241-291` | a bot move animates |
| Optimistic GC every 5 s, entries older than 30 s | `AnimationContext.tsx:294-314` | orphaned pending cards stop suppressing server events |
| Channel retry 500 ms to 5 s backoff, resync on re-subscribe | `RealtimeAnimationFeed.tsx:113-134` | a catch-up jump |
| Rearrange debounce 5 s | `DragContext.tsx:375-381` | server persist, rollback on error |
| Stale-round toast 4 s, chat bubble 8 s | `ServerContext.tsx:93`, `PlayerRing.tsx:124-132` | notices |

No polling fallback and no auto-pass timer exist on the web client.

---

## Part 3: The same red flight, two different reasons

Both clients use the same three-way verdict from the C animation core
(`c/src/anim_plan.h`, revert / keep / clear), the same rule that a retraction
is a reversed motion and never a cut, and the same red tint
(`rgb(220,38,38)`). What differs is the source of truth that triggers it.

| | iMessage | Web |
| --- | --- | --- |
| What is authoritative | the newest bubble the device can see, chosen by Rule P | `games.version`, monotonic, chosen by the server's CAS commit |
| When a local move becomes real | when the human presses Send in Messages | when the server commits the POST |
| What optimism means | the move is applied locally and *staged*; the bubble sits in the compose field until Send | the move is applied locally and the POST is already in flight |
| Bout-ending move | action half plays now, settlement half is *withheld* until Send (the human could still undo) | everything plays now; the server's own settlement events are dedup'd against the optimistic ones |
| Undo | a first-class action: reverse flight, shorter chain re-staged | none; the only way back is a rejection or a conflict |
| Why a card flies home red | an arriving chain is a sibling or descendant that does not contain my staged move (retract), or a superseded sequence's motion the newest chain disowns | the confirming broadcast does not show my card and the kernel says revert, or the server rejected the POST |
| Rebase | the board rebases onto the chain it adopts, so a cleared card never needs a flight | no rebase, so a cleared optimistic *pickup* must fly back to the table before the sweep |
| Several updates at once | reverse what is animating, play only the last (no queue) | append, never preempt, show every intermediate board |
| Stale intent across a round boundary | Rule P prefers the higher round; the losing move is superseded and the player is told | `intent_version` on the POST, `REJECT_STALE_ROUND`, toast |
| Duplicate of what is shown | no conflict, staged move survives | dropped by sequence id or content signature |
| Recovery from a missed update | the next bubble carries the whole chain; nothing to recover | REST resync on re-subscribe, with the optimistic overlay re-applied |
