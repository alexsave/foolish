# Cast deltas: can the per-viewer CAST drop its boards?

Read-only trace and design against origin/main at ebeaf94a6 (2026-10-20).
Every path below is a file and line in that tree unless it says otherwise; the server numbers come from APPLY_RECORD_FLOOR.md and OPTIMIZED_DESIGN.md in this directory and keep their COLD and WARM labels.
Nothing was built or run for this document; byte counts are computed from the wire layouts and say so.

## 0. The headline

- A cast carries one viewer-masked board after EVERY event plus the committed final board (evwire.c:28-33 put_snapshot, :54, :274).
  At two humans that is 6 to 12 board encodes per move across the three casts (seat 0, seat 1, spectators), about 400 to 1,200 bytes of payload, and 660 ns WARM / 2.9 us COLD of a 2.9 us WARM / 13.8 us COLD part 1 (APPLY_RECORD_FLOOR.md 1.1 item 10).
- The per-event boards were not added to work around React animation bugs.
  They are the ORIGINAL design of multi-step animation, two days after animation events existed (505796faa, 2025-07-19: `game_state?: Game; // intermediate game state after this event`, a `JSON.parse(JSON.stringify(game))` per event in the Deno handlers), because the TypeScript client of 2025 had no rules engine and could not derive the board between a cover and the discard it caused.
  Every later fix (masking 2025-07-20, the deck-drains-card-by-card snapshots of 2026-05-19, the count freeze lifted into anim_build_plan, the lobby-join plan stall of 2026-10-13) refined WHAT each board must say; none of them is the reason boards are on the wire.
- Today every reader of those boards is the kernel itself, running in the client: client_table.c reads each step's board into the one slot (client_push_next :320-326), and everything downstream reads a TableView the kernel wrote.
  The web's TypeScript reads six things off those views (the version, the game id, the good mask, the seats' ids, the defender, the battle row) and all six are fields of a board the kernel can rebuild.
  iMessage never sees a cast (no network); it consumes the same evwire v1 FRAMES from a kernel-local replay (ios_api_replay.c:168 fio_replay_last_events_packed) and must keep them.
- A client that holds the previous committed masked board CAN rebuild every step's board and the final board from the move's action wire, because awire_apply is deterministic on a masked game once two gaps are filled: the acting seat's played cards must be written into its placeholder hand before the apply (handle_attack checks all_in_hand, game.c:973), and the viewer's own refill draws must be substituted for the placeholder the deterministic deck pops (game.c:530-543, :863-877).
  Those two card lists are the irreducible per-viewer delta: the move's cards (already in the action wire) and the viewer's drawn cards (0 to 6 bytes).
  The deal, the lobby edits, a rearranged hand and the rematch are not actions and keep today's board-carrying cast.
- Design: a second cast format byte (EVW_CAST_DELTA) carrying the commit's move list (the same seat + awire list the journal records, table_ops.c:1050-1052, :1208-1212), the viewer's drawn cards, a game-over flag and a 32-bit check of the viewer's masked final board.
  The kernel's client applies it to the kept board, captures the hook snapshots exactly as table_act does (table.c:130-135), serializes an evwire v1 sequence into module scratch, checks it against the hash, and hands that sequence to the SAME client_push_open the web already walks.
  No TypeScript changes shape; no Swift changes at all; the server's cast cost goes from 6 to 12 board encodes to 0, and the cast bytes from about 400 to 1,200 per 2-player move to about 45 to 75.
- Verdict: do it, as a kernel change with one byte-identity test (the client's rebuilt sequence against table_cast's bytes, every seat, every move, 1,000 random games), and let it REPLACE the "casts from one walk" half of OPTIMIZED_DESIGN.md step 3, which patches the same bytes a delta never sends.
  The risks are the kernel version skew between a deployed web.wasm and the server (a check mismatch per move until the web redeploys; mitigated by the deploy order and by the existing refetch), and the one-slot ClientTable (the previous board must travel with the cast, like the identity does, never be assumed resident).

## 1. What a cast carries today

The server builds a cast per human seat and one for the spectators on every commit (fanout.c:3-7, :105-132; table_ops.c:388-414 section_cast; b3f5ea72e made "every commit" literal on 2026-10-18 so the version never skips).
table_cast (table.c:328-334) writes the 5-byte cast header (format 0xC1, u32 version) and then table_push (table.c:307-326), which is evwire_serialize (evwire.c:250-279) plus the as3 flags byte and, on a roster or rules change, the roster trailer.

evwire_serialize per viewer: the 4-byte header, then for every hook snapshot of the operation one event of 7 fixed bytes + the card bytes + an optional target + an optional battle + a u16 length + the viewer-masked state_put of that snapshot (put_event evwire.c:38-56, put_snapshot :28-33), the game-over MAGIC_TRANSITION with the final board as its snapshot when the game ended (:267-271), and the trailer: the committed final board (:274).

state_put (view.c:14-81) is 16 fixed bytes (status, seats, suit, lead, shield, discard count, flags with the rules, flipped, good mask, good timestamp flag, deck count), then one byte per deck card (all WIRE_CARD_HIDDEN for a masked viewer), 1 + 2 x battles, per seat 3 + hand bytes (the viewer's own real, every other hand a run of WIRE_CARD_HIDDEN), 1 + eliminated.
Computed from that layout: at two seats mid-game (deck 12, 2 battles, hands 6 and 6) a board is 52 bytes; just after the deal (deck 24, 1 battle) 61; at eight seats (deck 4, hands 6 x 8, 2 battles) about 100.

So a plain two-player attack (one event) is 5 + 4 + (7 + 1 + 2 + 55) + (2 + 55) + 1 = about 132 bytes per viewer, about 400 for the three casts; a cover that ends the bout (cover, transition, trash, two draws: 5 events) is about 5 + 4 + 5 x 65 + 57 + 1 = 390 per viewer, about 1,200 for three.
APPLY_RECORD_FLOOR.md 1.1 item 10 prices the three casts at 660 ns WARM (220 each) and 2.9 us COLD per move at two humans, "about 750 B of output", with 6 to 12 state_puts and 3 of the move's 4 evwire_walks; OPTIMIZED_DESIGN.md step 3 prices one cast at about 1 us at two seats and 2 to 3 us at eight on the nano, and hum8 builds nine of them.

## 2. Every reader of a cast's per-event boards and its final board

### 2.1 The kernel's client side

netclient.c (the C transport client every host links):

- cast_over (:1454-1461) reads ONE byte of the final board: fin[0], the status, to tell the quiet-game timer that the game is over and needs no more reads (:1486-1505).
  Derivable: a flag in the cast header replaces it.
- take_push NW_WS_CAST (:1558-1596) reads no board: it gates by version (stale :1584, skipped :1585-1590 -> refetch_queue) and hands the whole payload to the host as NC_EV_CAST.
  Its view branch (:1538-1546) holds back the per-move VIEW for a game held on its own stream and arms a refetch only when the view is newer than the board.

client_table.c (the web's and iMessage's reader; wasm_table_api.c:358-366 and :558-575 are one-line doors to it):

- push_check (:215-228), run by client_push_open (:261): reads each step's snapshot ONLY to measure it whole (state_measure) and to read its seat count (board_seats :211-213, snap[1]), and refuses a push whose boards disagree on seat count or whose event names a seat past them.
  Also the final board's seat count (:263-265) against the roster.
  Derivable: a rebuilt board has the same bytes.
- client_push_next (:302-328): imports each step's board into the slot (board_import :322 -> state_get + game_validate MASKED) and fills the TableView from it (view_fill :326).
  This is THE reader: everything a host sees of a step's board is this TableView (client_table.h:64-107 lists every field: status, num_players, power_suit, first_attacker, defender, num_battles, num_eliminated, my_seat, my_hand_count, fool, deck_count, discard_pile_length, has_flipped, has_good_timestamp, passing, flipped, good_mask, version, battles, seats {status, hand_count, awaiting_attack (own seat only), is_ai, id, name}, my_hand, elimination; rating fields are always unrated on a push).
- client_push_final (:330-337): the same import of the trailer.
- client_plan_step / plan_board (:775-819): off each step's TableView reads deck_count, discard_pile_length, flipped, every seat's hand_count, num_players and the battle row (attack and defense ids, ANIM_TABLE_UNKNOWN for a hidden card).
  client_plan (:821-838) reads the final board's deck, discard, flipped, hand counts and num_players, and takes the WIDEST num_players of the step boards and the screen's board (:829-831, the #287 fix).
  These feed anim_build_plan (anim_plan.c:275-310): the count freeze adopts events[0]'s own counts and undoes exactly one event (adopt_counts :264-270, apply_undo :230-258), and anim_pre_stream_table reads the first step's row.
  anim_plan.h:400-424 says it outright: "The board is not decoration. Every evwire event carries the state it produced, and that snapshot is the only thing that makes the count-freeze derivable at all".
  Derivable: every one of these is a count or a card the engine's own snapshot holds.
- client_push_settle (:940-1090): reads the FINAL board's table (:951-957, a card confirmed by a push the gate dropped), the final board's my_seat and defender (:1025-1026), and the board of the LAST EVENT NOT PLAYED, `open` (:999-1001): its table and the viewer's hand (conflict_verdicts :865-876), the defender's hand count on the final board (:889) and the uncovered count on final or open (:890).
  client_revert_board (:1099-1136) takes the board of the FIRST unplayed event and the board of a leading MAGIC_TRANSITION (:1115-1117) to draw the return flights over.
  client_keep_board (:1138-1154) edits every step's board and the final board in place to keep the viewer's pending cards on them.
  Derivable: all boards, all from the slot's TableViews.

Nothing in the kernel's client reads a hidden card, a card order of another seat, or any byte a rebuilt masked board would not reproduce.

### 2.2 The wasm bridge

wasm_table_api.c passes bytes through: wasm_client_cast_open (:560-563) hands the IO buffer and the identity to client_cast_open; wasm_client_cast_version (:569-575) peeks the header; wasm_client_push_next / push_final (:365-366) step the slot; the plan, settle and revert doors (:397-402, :432-441, :450-461) take TableViews the host writes back through the generated writer.
sdk/ts/table/client_table.ts read() (:149-168) walks push_next and copies each step's PushEvent and TableView out through the generated readers (:159-161), then the final (:163-166), and keeps the identity (:167).
It reads no field itself.

### 2.3 The web (foolish/src)

The feed: RealtimeAnimationFeed.tsx:38-42 republishes each NC_EV_CAST payload (backend.ts:149) into animationFeed; AnimationContext.tsx decodeCast (:286-308) calls readCast and pushToSequence (pushSequence.ts:44-62), which names each step `{type, seat, cards, from_location, to_location, target_card, battle_index, game_state: TableView}` and the final board `game`.
From there the TypeScript reads these fields of a step's board or the final board, and nothing else:

| reader | file:line | fields read | of which board | derivable from the previous board + the move |
|---|---|---|---|---|
| version gate | AnimationContext.tsx:324-329 | version (`read.final.version` at :307) | final | it is the cast header's (client_push_open :277), not the board's |
| the game a push is for | :337-340, :343 | gameId | final | the identity's (client_identity), not the board's |
| the empty-stream good | :378-392 | goodMask | final | yes: handle_good's mask |
| a good leading the stream | :433-439, stepGoodMask :476-477 | goodMask | events[0] | yes |
| a good cleared by a throw-in | :547-549 | goodMask | the opened step | yes |
| a pass handing the shield on | :555-563 | defender (rolesOf(final)) | final | yes: handle_pass's rotation |
| the closing beat | :565-573, sequenceFinalRef :420, :389 | the whole view (syncFromView) | final | yes |
| the landing commit | :519-532 | the whole view (updateGameState) | each step, or the last step of a merged beat (:516) | yes |
| the sequence's settled board | :398-415 | the whole view, with my pending pass laid on it (passedBoard) | final | yes |
| the plan | animPlan.ts planFor :224-233 (board: s.game_state), useAnimationRun.ts:194 | deck, discard, flipped, hand counts, num_players, battles row (via client_plan_step) | each step and the screen's board | yes |
| the piles coming down | animPlan.ts arrivingPiles :164-179 | battles (the index of the arriving attack) | each opened step | yes |
| my unanswered moves | optimisticResolve.ts settlePush :104-138 | battles, my_hand, defender, hand counts (via client_push_settle), and the boards themselves (keptBoard :121-124, revertBoard :127-128) | `open`, `home`, `magic`, final | yes |
| where a flight's seat is on the page | GameDisplay/flightGeometry.ts:672-673 | seats[seat].id (seatKey) | the step's board, else the screen's | the identity's, not the board's |
| the bot bump timer | AnimationContext.tsx:213-216 (canBotMove) | rulesOf(view).botToMove | the screen's board | yes |

Not cast readers but evwire v1 readers the design must leave alone: the replay screen and the tutorial decode kernel-produced replay FRAMES with per-step boards (replay/frames.ts:289-320 commits the previous frame's board when stepping backwards; Tutorial.tsx:427 rewrites each step's game_state), produced by replay_steps_frames_v6, not by a server.
The dashboard's list of games not on screen reads the user-wide VIEW stream (ServerContext.tsx:276-312), and ServerContext.tsx:128-133 says why that stream never touches the game on screen.

No React component reads a step's board directly: AnimationOverlay, TableBattles, PlayerRing and DeckAndFlipped render the screen's TableView plus the frame-loop state (currentAnimation, heldPiles, arrivingPiles, inFlightFromDeck, trumpFlight), and docs/ANIM_TIMING_AUDIT.md:53 and :194 record that the one direct read (`currentAnimation.game_state ?? game` in AnimationOverlay.tsx) was the "live outranks pending" bug and was replaced by the kernel's row.

### 2.4 iMessage and the iOS app (Swift)

iMessage has no network (project.yml, and OPTIMIZED_DESIGN.md section 8) and never receives a cast.
It consumes evwire v1 frames that the KERNEL writes locally: MessageKernel's lastMoveEvents calls fio_replay_last_events_packed (ios_api_replay.c:168-237), which rebuilds the game from the chain's v6 code and emits replay_steps_frames_v6 frames, each a full evwire v1 sequence with a board per event; EvWire.swift:85-104 decodes them through the same client_push_next and copies each step's board out of MaskedView.table(), and :66-82 reads a frame's trailer for "the board the first step committed".
Readers of GameEvent.state (Models.swift:305-310): MessageTableView+Sequence.swift (:122 the prior board for the role marks, :407-416 the battle row written into the ledger over the flight's duration, :423-425 deck count and the trump as cards leave, :470-476 a seat's hand count as a card leaves, :573-575 the trump at departure, :656-665 deck, trump, discard and every other seat's hand count at the landing), and MessageTurnController.swift:428-456, which pins the board to `evs[at].state` while a staged bout end's settlement is withheld (heldView, 368e6667e).
These are the same six count fields plus the row and the trump; iMessage is the richer consumer of per-step boards (project_imessage_is_the_spec.md), and it gets them from a kernel-local replay, so a CAST format change does not reach it as long as evwire v1 stays the FRAME format.

The iOS app (FoolishNet) is a boards host: nc_set_boards makes netclient turn every cast into a refetch (netclient.c:1575-1580) and it adopts VIEW envelopes whole (OPTIMIZED_DESIGN.md section 8: Stream.swift:23-25, OnlineGame.swift:52-86).
It reads no cast byte; the owner has paused it (project_ios_app_not_priority.md).

### 2.5 Field by field

Every field any reader takes off a step's board or the final board is one of: status, num_players, power_suit, first_attacker, defender, the battle row, deck_count, discard_pile_length, has_flipped and flipped, good_mask, has_good_timestamp, the seats' statuses and hand counts, my_hand, my awaiting_attack, num_eliminated and the elimination order, the fool, and the identity (ids, names, title, rules) that is not the board's at all.
Section 4 shows each is a deterministic function of the previous masked board and the move, given the move's cards and the viewer's drawn cards.

## 3. Why the boards are on the wire

### 3.1 The origin

- 2025-07-18, 26ce91ebc "better animation events": per-action animation events exist (attack, cover, good, pass, pickup) with no board.
- 2025-07-19, 505796faa "multi step animations are working": `game_state?: Game; // intermediate game state after this event` appears in the shared types, and every Deno handler deep-copies the game after each step (`gameStateAfterAttack`, `gameStateAfterCover`, `gameStateAfterDiscard`, `gameStateAfterRefill`, `gameStateAfterDefenderMove`, `gameStateForTransition`), while the client's queue commits each event's board as its 500 ms flight ends.
  The client had no engine; a cover that ended a bout was one request whose consequences (discard, refill, rotation) had no other way to reach the screen one step at a time.
- 2025-07-20, 03233abd7: game_state becomes PublicGame / PersonalGame, the per-viewer masking that put_snapshot inherited.
- 2025-11-22, 270570591 "smoother deduping": the optimistic dedup, not about boards.
- 2026-05-19, 7c4f813aa and c0e80f6c1: the deal and the refill had been snapshotting one FINAL board onto several events, so the deck jumped 36 -> 24 in one step; the fix made every event's snapshot honest for its moment, and 1ffd11fc4 dropped the deck count as cards leave.
  These are bugs IN the boards (stale snapshots), fixed by better boards, and the kernel's deal now fires one honest hook per card (game.c:545-560 comment, game.h:357-365).
- 2026-07 (984c7a3f2 and the A3 row of docs/C_CORE_CONSOLIDATION.md:700): the C port keeps `snap` per event as "the counterpart of the web evwire's per-event snap_len payload", and ADDS `GameEvent.state` for iOS "because a bot_drive cycle applies several actions and the intermediate boards were otherwise unreachable without the diff engine F4 cancelled".
  docs/PACKED_WIRE_CUTOVER.md:108 pins the byte: `u16 snap_len | masked put_state bytes (this step's game_state, viewer-masked)`.

So the boards were put on the wire so that a client WITHOUT the rules could show intermediate boards and so that a second client (iOS) need not derive them by diffing.
Both reasons are gone on the web: since 2026-09-17 (9be65211b, 8458a0a85, b42c6df71) the browser links the kernel's client (client_table.c), judges moves on its board (client_validate runs awire_apply on a masked game, :485-499), and edits boards only through the kernel.

### 3.2 The bugs since, and whether today's code prevents them another way

| bug | where it is recorded | what the boards had to do with it | prevented today by |
|---|---|---|---|
| a card snaps back after a pickup swept it (July 2026) | project_optimistic_revert_flicker.md; e2e/optimistic_revert.test.ts (deleted at e3021f473, its scenarios in e2e/optimistic_animation.test.ts and e2e/ui_animation_trace.test.ts) | the verdict needs the push's final table and the board of the last unplayed event | the kernel's client_push_settle (fc706e4b8), which reads those boards from the slot; a rebuilt slot gives the same boards |
| the board jumped 36 -> 24 on the deal, 5 -> 6 cards near the end | 7c4f813aa, c0e80f6c1, anim_plan.h "Round 16" | stale or shared snapshots | honest per-hook snapshots (game.c deal_initial) and the one-undo freeze in anim_build_plan; a rebuilt snapshot is the engine's own, so it cannot be stale |
| the lobby-join plan stall killed every animation (2026-10-13) | project_lobby_join_plan_stall.md, b69eccdf2 (#287) | anim_build_plan refused a step board of 2 seats against a screen board of 1 | client_plan takes the widest board (:829-831); a lobby join is not an action and keeps the board cast anyway |
| one listener throw wedges the animation queue (open) | project_lobby_join_plan_stall.md | not a board problem | still open; a refused delta must fall through to a refetch, not a throw (section 5.4) |
| the view snaps past the animation | ServerContext.tsx:128-133, netclient.c:1538-1546 | the dashboard VIEW is a final board with no steps | held back for a game on its own stream; unchanged |
| a lost or reordered push | netclient.c:1584-1590, unit_netclient.c:958 a_skipped_version_refetches, b3f5ea72e | a cast only applies on the board it follows | the version gate and refetch; a delta makes the same requirement explicit and checkable |
| hand array order differs between two derivations (iMessage) | project_hand_order_replay_divergence.md | a replay reaches a position by a different route than a continuation | not applicable to a delta, which applies the same action to the same previous board, so the hand arrays come out as the server's; the identity test proves it byte for byte |
| the first paint shows the end state (iMessage) | project_first_paint_veil_family.md | SwiftUI onChange timing | not a wire matter |
| the one resident game slot read across an await (iMessage) | project_resident_game_seal.md | one slot, many readers | the web's ClientTable is ONE slot too (wasm_table_api.c:323-333) and adoptEnvelope for a dashboard game overwrites it; section 5.2 therefore never assumes the slot holds the previous board |
| 4-player live arrival overshoots the replay floor (iMessage) | project_imessage_4p_live_arrival.md | pending goods fold into one atom | kernel msg_open_boundary; iMessage only, no casts |
| a replay is the game rebuilt (A5) | project_a5_replay_steps.md, project_a5_web_done.md | the precedent: the kernel rebuilds boards by replaying actions through the real engine and emits the same evwire frames | the delta client is that precedent applied to one move |
| what a gesture means moved into the kernel | project_gesture_rules_to_c.md | "can the inputs be made on the far side of the boundary" | yes: the previous board is already on the far side (the slot imports it), the move is 3 to 7 bytes |

None of the bugs needs the boards to come over the wire; they need the boards to EXIST on the client, which a rebuild gives.

## 4. What a client can derive

### 4.1 The engine is deterministic on a masked game, with two gaps

table_act (table.c:116-141) is: seat lookup, awire_decode, scope_open, game_rng_set(game_state_seed(g, rng_base, SALT)) (:129), awire_apply under the snapshot hook (:130-135), finalize (:104-114: GAME_OVER, bots READY, humans IDLE).
The client's board_game (client_table.c:444-483) already turns a TableView into a masked Game with `deterministic_deck = true`, and client_validate runs awire_apply on it with the hook disabled.
What differs from the server's apply:

1. The RNG. draw_card (game.c:530-543) draws a RANDOM deck index from the server's secret rng_base (draw_index :73-78) unless deterministic_deck, which pops index 0.
   On a masked board every deck card is the same placeholder, so WHICH index is popped changes no public byte; the count is what the viewer sees.
2. The acting seat's cards. handle_attack (:973), handle_cover (:1042) and handle_pickup (:1175) check all_in_hand against the actor's hand, which on the viewer's masked board is placeholders when the actor is another seat.
   The fix is to write the move's cards (they are in the action wire) over that many placeholders before the apply; the server vouched for them.
   The removal (hand_remove_card :497) then shifts the right slots, and the count comes out as the server's.
3. The viewer's own draws. A refill (refill_player_hands :885-918, draw_up_to_six :863-877) appends popped cards to each hand in attacker order, defender last; for every seat but the viewer the appended placeholders are masked again by state_put, but the viewer's own hand must hold the real cards.
   The fix is a reveal list: the viewer's LOG_DRAW primaries for this commit, substituted by draw_up_to_six when `seat == viewer` (a 4-line engine hook, zero cost when the list is NULL), including the flipped trump when the deck is out (draw_card :531-536 hands it over by identity, which the substitution reproduces).
4. The logs. The client slot is a Game prefix (client_table.h:268, offsetof(Game, logs)) and the rules slot keeps one LOG_DISCARD under log_cap (game.h:210-219 says log_cap filters every other type).
   evwire_walk reads LOG_ATTACK, LOG_PASS, LOG_COVER, LOG_DISCARD, LOG_DRAW and LOG_PICKUP of the action (evwire.c:110-248), so the delta slot needs room for one action's logs (a measured worst of 12 hooks, game.h:367-376; at the web's MAX_LOG_PAIRS=64 a GameLog is 132 bytes, so 16 logs are 2.1 KB) and a `log_room` bound that keeps every type up to it, beside log_cap.
5. The snapshots. table_snap copies TABLE_SNAP_BYTES per hook into a TableSnaps of MAX_SNAPS=64 slots (table.c:21-31, game.h:377-379, about 45 KB); the client never applies a deal, so a 16-slot store with ENGINE_SNAP_ROOM's drop-never-corrupt rule (snap_room.h) is enough, and a plain move's worst is 12.

Everything else is a function of public state: the rotation (execute_round_transition :1277-1302, handle_good :1125-1150, handle_pass) reads statuses and the shield; eliminations read hand counts (refill_player_hands :894-902); game_done, finalize and the fool read statuses; the discard count and the battle row are public; the good mask and has_good_timestamp are set by handle_good from public facts; awaiting_attack is written by handle_good and cleared by the refill and READ by nothing in the rules (game.c:1306 game_seat_awaiting is display, and state_put already masks every seat's but the viewer's).
The rules byte rides the state flags (view.c:21-29), so a podkidnoy table rebuilds under podkidnoy.

### 4.2 The hidden information, event by event

| event | what the viewer may see | needs extra revealed data |
|---|---|---|
| ATTACK_PASS, COVER | the cards (public once laid) | no: they are the action wire |
| PICKUP | the table's cards (public) | no |
| DISCARD, CARDS_TO_TRASH | the table's cards | no |
| REFILL to another seat | backs and a count | no |
| REFILL to the viewer | the drawn cards | YES: 0 to 6 card bytes, the viewer's LOG_DRAW primaries in draw order |
| OUT, DEFENDER_MOVE, MAGIC_TRANSITION | seats | no |
| DEAL, FLIPPED, START_MAGIC, START_DEFENDER | one card per seat per round, the trump, the opening seats | the deal is random (ChaCha20 deal_rng) and the first attacker is the lowest trump over ALL hands (determine_lowest_power_index :703), which no masked viewer can compute: the deal keeps the board cast |
| a lobby edit (join, exit, bot, reseat, retitle, rules), the rematch | one MAGIC_TRANSITION on the committed board plus the roster trailer | these are asks, not actions (ask.h:44-56), and change the roster: they keep the board cast |
| a rearranged hand | nothing visible to others; the viewer's own order | an ask (ASK_REARRANGE_HAND); rare; keeps the board cast (or a 6-byte own-hand variant later) |
| game end | the final transition, statuses, the fool | derivable (finalize needs the bot mask, which the identity carries as is_ai); the RATING stamp is never on a cast (client_table.h:90-101, ViewRules.wants_envelope), so nothing changes there; the loser's hand is not revealed by a cast today either |

So for a plain move the irreducible per-viewer delta is the move list and the viewer's drawn cards, and for a spectator just the move list.

### 4.3 A commit is a move LIST

A commit can hold several actions: a bot cycle bundles silent goods ahead of its one visible action (table_bot_drive table.c:987-1008, BotDriveOut.actions), and the journal records up to 9 ReplayMoves per commit (table_ops.c:1050-1052 builds them from the drive, :1208-1212 from a human's wire; recovery replays them through cb_table_replay_move :1055).
The delta is therefore exactly what the record already is: `(seat, awire)` in order, which is why the Table should own that list once (today the server rebuilds it from the drive's actions) and both the journal and the delta read it.

## 5. The design

### 5.1 The wire

A second cast format byte, beside EVW_CAST_FORMAT 0xC1 (evwire.h:277), so a client that reads only 0xC1 refuses the new one at evwire_cast_header_get and falls into today's unreadable-cast refetch:

```
u8  EVW_CAST_DELTA         0xC2
u32 LE version             the committed games.version (as 0xC1)
u8  flags                  bit0 GAME_OVER after this commit (what cast_over read off fin[0])
                           bit1 the final MAGIC_TRANSITION is appended (t->ended, evwire.c:267)
                           bit2 a roster trailer follows the check (never set on a move today; reserved so the two formats stay one family)
u8  n_moves                1..9
per move:
  u8  seat
  u8  wire_len             2..REPLAY_MOVE_WIRE_MAX
  wire_len x u8            the awire frame (awire.h:7-10), the bytes table_act applied
u8  n_reveal               0..CARDS_PER_PLAYER; 0 for the spectators' cast
n_reveal x u8              the viewer's drawn cards this commit, in draw order (wire card ids)
u32 LE check               board_check(final, viewer), below
[roster trailer]           iff bit2
```

A plain attack is 5 + 1 + 1 + (1 + 1 + 3) + 1 + 0 + 4 = 17 bytes; a bout-ending cover with two cards drawn is 5 + 1 + 1 + (1 + 1 + 4) + 1 + 2 + 4 = 20; a bot cycle of two silent goods and an attack about 27.
Three casts per two-player move: about 45 to 75 bytes against about 400 to 1,200 today; nine casts at eight seats: about 180 against about 4,000.

The check: `board_check(const Game *g, int viewer)` in view.c, one function both sides call: FNV-1a 32 over the bytes state_put writes for VIEW_SPECTATOR (the public skeleton, computed ONCE per commit by the server and cached in TableCommit), folded with FNV-1a over the viewer's own hand bytes and awaiting byte (what the masked board adds for that viewer; the server reads them straight off g->players[viewer], about 7 bytes, no encode).
A 32-bit check is not a security property; it is a divergence detector for a deterministic computation, and it is the same strength as the record's checksum (APPLY_RECORD_FLOOR.md 2.2).
CRC32C is available on arm64 and the wasm has no instruction for it, so FNV-1a, which both already link.

Which commits take which format: table_commit_products writes the delta for every commit whose move list is non-empty and that neither dealt, changed the roster or the rules, nor reset the log (exactly the test the journal uses for a plain record, table_ops.c:885 and :914), and the 0xC1 cast otherwise.
The deal, the lobby, the rematch and a rearranged hand therefore keep today's bytes, and a goods-only commit (b3f5ea72e: it has a move, AWIRE_GOOD) is a 17-byte delta with no events, which the web's empty-stream guard (AnimationContext.tsx:352-395) already handles off the final board.

### 5.2 The kernel's client: the delta becomes an evwire v1 sequence before anything reads it

client_cast_open (client_table.c:287-300) gains a branch on the header byte:

1. The previous board.
   The host hands back the committed masked board of this game at version v-1, as opaque bytes the kernel wrote, exactly as it hands back the identity today (client_table.h:24-29, client_identity / client_identity_at; sdk/ts/table/client_table.ts:104-110 keepIdentity and the identities Map).
   A new `client_board_keep(c, out, cap)` writes `u32 version + state_put(c->g, viewer)` (about 60 bytes) after every client_push_final and client_adopt_envelope; the TS keeps it per game id beside the identity and passes both to readCast.
   The slot itself is NOT trusted to hold the previous board: adoptEnvelope for a dashboard game overwrites c->g (project_resident_game_seal.md is the same lesson on the phone).
   A missing board, or one whose version is not v-1, is a new refusal CLIENT_E_BEHIND, and decodeCast's existing `refetchForEnvelope` path (AnimationContext.tsx:297-305) fetches the game whole, which lands as a VIEW envelope and primes the board for the next cast.
   netclient.c's gate (:1584-1590) already makes v-1 the normal case; the kernel check is the belt to its braces.
2. The apply.
   board_import of the previous board into a delta slot of `offsetof(Game, logs) + 16 x sizeof(GameLog)` with `log_room = 16`, `deterministic_deck = true`; for each move: if seat != viewer, overwrite the first n placeholders of that seat's hand with the move's cards (refuse CLIENT_E_PUSH if the hand holds fewer); install the reveal list on the viewer's seat; install engine_snap_hook on a 16-slot client TableSnaps; awire_apply(g, seat, &a), refused (CLIENT_E_DIVERGED) if the engine rejects; after the last move, finalize as table.c:104-114 does with the identity's bot mask.
3. The sequence.
   evwire_serialize(snaps, n, logs, n_logs, g, viewer, actor = last move's seat, append_final = flags bit1, scratch, cap) into a module scratch of 4 KB (a plain move's worst: 12 events x about 110 bytes + the final), then the as3 flags byte 0 (or the roster trailer when bit2).
   Then board_check(g, viewer) against the wire's check: a mismatch is CLIENT_E_DIVERGED and the host refetches (the same path as 1).
4. The read.
   client_push_open(c, scratch, len, as3 = 1, identity, identity_len, version): the SAME walk the web does today, so push_check, client_push_next, client_push_final, the plan, settle, revert and keep are untouched, and so is every TypeScript and Swift line downstream.

The kernel does ONE new thing, in one file pair (client_table.c, with the reveal hook and log_room in game.c), and the evwire v1 format stays the one in-memory and on-disk shape of a sequence: the replay frames, iMessage's local frames, the deal and lobby casts, and the tests that read sequences all keep it.

Linear memory: the delta slot (about 2.9 KB), the snapshot store (16 x TABLE_SNAP_BYTES, about 11 KB) and the scratch (4 KB) add about 18 KB to web.wasm's 26 pages; code: evwire_serialize's writers (sink_packed, put_event, put_snapshot) and evwire_walk are not reached by the browser's export list today (Makefile:1491-1509 names no table or push writer), about 1.5 to 2 KB raw; web.wasm is 67,311 B raw against a 137,000 B pin and 31,219 B gz against 61 KiB (e2e/mem/wasm_memory.test.ts:100-101), so the pin moves by a few percent and the gz by about 1 KB.

### 5.3 How the web animation gets its per-step boards

Unchanged in shape: pushToSequence receives the same `steps[]` with a TableView per step and the same `final`, from the same readers, because the kernel rebuilt the sequence the server used to send.
anim_build_plan freezes on events[0]'s board (anim_plan.c:288-298), client_push_settle judges on `open` and `final`, onLanded commits each step's board, and the closing beat syncs from the final: all off boards the client computed.
The only TypeScript change is in sdk/ts/table/client_table.ts: keep the board bytes per game beside the identity and pass them to readCast, which is the generated-door pattern (no TS knows a byte of them, feedback_no_hand_rolled_kernel_bytes.md).

### 5.4 What happens on a gap, a mismatch, or a lagging kernel

- A skipped version: netclient.c:1585-1590 drops the cast and refetches the game whole, as today; the answer is a VIEW envelope, adopted by ServerContext.tsx:293-306 with the pending cards kept on it, and client_board_keep primes the next delta.
- A kept board that is not v-1 (a cast decoded while a refetch was in flight, a second tab): CLIENT_E_BEHIND, refetch, no animation for that move (the board jumps once), exactly today's behaviour for an unreadable cast.
- A check mismatch: CLIENT_E_DIVERGED, refetch.
  Unlike today, a divergence is DETECTED: a board decoded from the wire today is trusted whole, and a rules difference between client and server shows only as a refused move later.
- A web.wasm older than the server (the deploy window, or a tab open for days): every delta mismatches and the page refetches once per move until it reloads.
  A 1-byte kernel rules version in the header (or the structgen layout hash's low byte) lets the client say CLIENT_E_BEHIND without applying, and the page's existing stale-bundle reload (Next.js chunk failure) is the recovery.
  The tests must include "a client at kernel N-1 against a server at N refetches and never shows a wrong board".
- A thrown listener still wedges the queue (project_lobby_join_plan_stall.md, open): the delta path returns refusals, never throws, and decodeCast already turns null into a refetch; the open defect is independent and worth fixing beside this.

### 5.5 The bytes and cycles saved per move

Server, two humans, from APPLY_RECORD_FLOOR.md 1.1 item 10 (casts: 660 ns WARM, 2.9 us COLD, 6 to 12 state_puts, 3 evwire_walks, about 750 bytes written through put_u8):

| | today | delta | saved |
|---|---|---|---|
| board encodes in casts | 6 to 12 state_puts (about 55 bytes each at 2p) | 1 public state_put for the check, shared by the three casts | 5 to 11 encodes |
| evwire_walks for casts | 3 (plus op_stream's for the clock) | 0 (op_stream stays) | 3 |
| cast bytes written | about 400 to 1,200 | about 45 to 75 | about 90 to 95 percent |
| WARM | 660 ns | about 60 (one public encode) + 30 (its FNV) + 3 x 20 (header, move list memcpy, 7-byte own fold, writes) = about 150 ns | about 500 ns, a sixth of part 1's 2.9 us |
| COLD | 2.9 us | about 0.4 us (the snapshot slots are no longer walked; the arena lines written shrink from about 750 bytes to about 60) | about 2.5 us of part 1's 13.8 us |

Eight humans (hum8, OPTIMIZED_DESIGN.md step 3: nine casts at 2 to 3 us each on the nano, "-14 us per move" for the holes model): the delta writes nine casts of about 20 bytes from one move list and one public encode, about 0.4 us WARM in total against about 2.7 to 5 us, and about 180 bytes against about 4,000.
Downstream, the five copies every cast makes between the arena and the socket (move_path_verdicts.md:50) and the TCP payload shrink in the same ratio; the frame count and the send() count do not change (one send per hearer, with the VIEW riding the same segment).

What it does NOT remove: the per-seat VIEW envelope (630 ns WARM / 2.0 us COLD, item 8), which the web holds back for a game on its own stream; after deltas it is the only board encode left per move for room sockets, and OPTIMIZED_DESIGN.md step 9 (no VIEW for a connection SUBscribed to the room) plus P4 (no spectator envelope) finish the job: a plain move then encodes the board exactly once, for the check.

Client, per move: one board_import (what client_push_next does per step today), one awire_apply (what client_validate does per tap), 1 to 12 snapshot memcpys of about 700 bytes, one evwire_serialize of about 0.1 to 1 KB, then today's walk.
About the same work as today's decode, in wasm, once per cast, well under a frame.

### 5.6 Overlap with the work in flight

OPTIMIZED_DESIGN.md step 3 ("every viewer's cast from one walk": evwire_serialize_holes, table_casts, table_cast_seat, branch nano-logs-casts) patches the per-viewer card spans into a shared copy of today's bytes.
A delta sends neither the spans nor the boards, so the two are alternatives on the cast half of step 3, not a sequence: if the delta lands, the holes model should not, because it would be dead code the day the 0xC1 move cast stops being written (it would still serve the deal and lobby casts, which are not where the cycles are).
Step 3's other two parts (one grown log buffer per game, finalize scratch per thread) and step 8 (the fixed-layout record) are independent, and step 8's record IS the move list the delta carries, which argues for one owner of that list on the Table.
P22 (block-checked writers) still pays for the envelopes and the 0xC1 casts.

### 5.7 The tests that must pass, and the ones to add

Kernel (foolish/c/tests/tests.c):

- The identity test, the one that makes this safe: over 1,000 random games with 2 to 8 seats, bots and humans, every plain commit, every viewer including the spectator: the client's rebuilt evwire sequence from (kept board at v-1, delta) is BYTE-IDENTICAL to table_cast's 0xC1 sequence for that viewer (events, every snapshot, the final), and board_check agrees.
  Mutations that must kill it: drop the actor-hand patch (the apply refuses), drop the reveal list (the viewer's refill shows placeholders), drop the awaiting byte from the check's own fold (a divergence in the viewer's own awaiting passes), swap two reveals (the hand order differs), skip finalize (the statuses differ at game end).
- test_client_reads_every_push_of_a_game (:14168), test_every_push_is_leak_free_and_mirrors_the_commit (:17923), test_every_envelope_and_push_of_whole_games_reads_as_the_table (:18160), test_client_push_steps_and_refusals (:14669), test_client_push_settle (:15827), the client_plan cases (:16116-16255), test_table_cast_carries_its_version (:14460): run as they are on the 0xC1 path and again through the delta path.
- The refusals: a kept board at v-2, a check off by one bit, a reveal list one short, a move whose actor hand is too short, a truncated move list; each a refusal and never a partial board (client_table.h:12-17, "refuse, never clamp").
- The evwire v1 reader and frame cases (:3551, :3663, :3820, :10980, :11019, :11076) are untouched and must stay green: the frame format did not change.

Server (foolish/server/test, c_base): smoke.c cast_gaps (:311) and the listeners that count casts, smoke_matrix.c cast_event (:313-337, which checks board_hides on every snapshot: a delta has no snapshot, so it decodes through the client and checks the rebuilt boards hide every other hand), unit_app.c every_commit_is_cast (:1497) and a_goods_only_commit_is_broadcast (:1398), fuzz/fz_client_table.c and fz_sequence.c (a delta input under the fuzzer: no read past the move list, no apply of a hostile wire that the engine does not refuse), bench/roundtrip.c and bench/cpu.c's byte counts.

Web (foolish/e2e): ui_animation_trace.test.ts is the strongest gate and should need NO golden change, because every board the page commits is byte-identical to today's; a changed golden is a bug in the delta, not a re-record.
optimistic_animation.test.ts, realtime_animation_feed.test.ts, realtime_feed_seating.test.ts, kernel_wire_decode.test.ts, anim_plan_ask.test.ts, helpers/client_read.ts: as today, plus one case that feeds a 0xC2 cast with a kept board and one without (refetch).
replay_frames_web.test.ts and replay_steps_frames.test.ts: unchanged (frames stay v1).
Then one full `npm run test:e2e` pass at the end (feedback_defer_full_e2e.md).

Film: main has no CDP film rig checked in (the showcase tool of 304f44be7 and the fake backend's --latency of e7d981551 are the pieces); the before/after take that matters is the same two-player scenario filmed against a server writing 0xC1 and one writing 0xC2, at normal speed, compared frame by frame (feedback_no_slowmo_record_60fps.md).

iMessage: its animation spec reads kernel-local v1 frames and never a cast; no Swift test changes, and ios-smoke's event-path cases (ios_api_smoke.c:86-124) stay as they are.
If the kernel's reveal hook or log_room touches game.c, the FoolishBots xcframework rebuilds and the replay rebuild difftest (tests/replay_rebuild_difftest.c) must stay at zero.

### 5.8 Risks

- Kernel skew between the deployed web.wasm and the server: a rules change on one side turns every move into a refetch for stale tabs.
  Today the same skew shows as a refused move or a wrong gate; the delta makes it a detected, self-healing refetch, and a kernel version byte makes it cheap.
- The one slot: a delta applied to a board that is not this game's at v-1 produces a plausible wrong board.
  The explicit kept-board round trip and the version check are what prevent it; the design must never fall back to "the slot probably holds it".
- The reveal hook is engine code on the server's hot path too (draw_up_to_six): a NULL check per draw, measured at zero in the nano model, but it must be byte-identical for the server (the identity test covers it, and tests/replay_rebuild_difftest.c covers replays).
- A wrong actor-hand patch on a spectator's or opponent's masked board could leave a real card in a hidden hand: state_put masks every byte of a non-viewer hand, so it cannot leak on the wire, and the identity test catches the count.
- Memory on the web: about 18 KB more linear memory and about 2 KB more code; the raw pin has 2x room.
- The deal and lobby casts stay large; they are a few per game.

### 5.9 Migration

The owner's standing rule is that shipped data is not frozen (feedback_shipped_data_not_frozen.md) and the web redeploys with the server; the only thing to get right is the ORDER, because the web is CI-built and deployed to Vercel separately from the VM (reference_vercel_deploy_model.md, project_codegen_build_artifact.md):

1. Land the kernel change behind a server products flag (TABLE_PRODUCTS_DELTA_CASTS, default off), with the client able to read both 0xC1 and 0xC2.
2. Deploy the web (new web.wasm reads both).
3. Turn the server flag on; an old tab still open reads 0xC2 as an unknown header, refetches once per move and shows every board a move late with no animation until it reloads, which is today's unreadable-cast behaviour and acceptable under the rule above.
4. Remove the flag and the 0xC1 move cast in the next pass; the 0xC1 format stays for the deal, the lobby and the rematch.

No stored data changes: casts are never stored, the journal's record is already the move list, and replays and the iMessage chains keep evwire v1 frames.

## 6. Verdict

The per-event boards were the right design for a client with no rules in 2025 and a second client with no rules in July 2026.
Since September 2026 the web's only reader of them is the kernel's own client, and every field the page or the phone reads off them is a count, a row, a mask or a seat that the engine produces when it applies the move.
The server encodes the board 6 to 12 times per two-player move to tell a client what the client's own kernel can compute from 17 to 27 bytes, and at eight seats it is nine casts of a hundred-byte board apiece.

Do it as described: one kernel change (a delta cast format, a client apply that rebuilds the evwire v1 sequence, a reveal hook and a log_room bound), one byte-identity test that mutation-checks each of its five pieces, no TypeScript shape change, no Swift change, the deal and lobby casts untouched, deployed web first.
It takes the casts from 660 ns to about 150 ns WARM and 2.9 us to about 0.4 us COLD at two humans, about 2.5 to 4.5 us WARM at eight, cuts the cast bytes by about 90 to 95 percent, supersedes the holes model of step 3, and leaves the VIEW envelope as the one board a plain move still encodes, which step 9 and P4 then remove for room sockets.
