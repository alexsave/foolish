// anim_plan ("animation core") — the platform-independent half of the animation
// pipeline, moved OUT of TypeScript (src/contexts/AnimationContext.tsx, the pure
// modules under src/state/) and Swift (ios/FoolishKit/Boards/MessageTableView's
// runEventStream/veil/count-freeze) into the one place both already agree they
// want to be. One derivation is the only way to guarantee a card that flies right
// on the phone flies right in the browser (the same argument msg_wire.h and
// evwire.h make for their layers).
//
// WHICH CLIENT IS THE SPEC - CORRECTED, 2026-09-05. This header opened by saying
// the WEB is the spec, because the web had hardened the behaviour through months
// of glitch-fixing while iOS re-derived it second. That is no longer the owner's
// position: "the imessage behavior and layout and animation is slightly
// different from the webs, and I prefer the imessage version." The iMessage
// board has since grown a RICHER model than the one here - beats rather than
// steps, a bout-end hold, an out-collapse that rides its own card motion, and a
// role hand-off this file has no concept of at all.
//
// So the direction of travel is INVERTED. When an iMessage rule lands here it
// REPLACES the rule it meets rather than being reconciled with it, and the web
// becomes the client that re-derives. Do not "fix" an incoming rule to conform
// to what is already here; that is exactly backwards. ConflictModel.swift's
// header states the same thing from the other side.
//
// THE BOUNDARY (agreed with the owner). Only RENDERING is irreducibly
// per-platform: interpolation/springs, view updates, screen coordinates, gesture
// previews. Everything above it is pure data transformation and lives here:
//
//   1. PLAN BUILDING (anim_build_plan). A decoded viewer sequence -> an ordered
//      plan of steps, each carrying {duration_ms, start_ms} plus the COUNT-FREEZE
//      the display obeys (which deck/discard/seat-hand values hold until which
//      step lands) and the VEIL (which real card identities are "not there yet"
//      until their step reveals them). This is exactly what the web's queue +
//      ANIMATION_TIME pacing and the iMessage board's freeze + preHide veil each
//      re-derived, written once. The FREEZE is the iMessage rule, not the web's.
//   2. OPTIMISTIC POLICY (the predicted-move layer): the version gate
//      (anim_should_drop_stale), the release of confirmed optimistic cards
//      (anim_stale_optimistic_on_table), the revert-vs-keep-vs-clear decision for
//      a still-pending attack/cover (anim_resolve_unconfirmed_attack_covers), and
//      the dedup signature (anim_event_key) that lets a confirming broadcast
//      recognise an already-played optimistic card instead of animating it twice.
//   3. TIMING POLICY: ANIM_TIME_MS and the per-event duration rule as C
//      constants the plan emits, so a platform never invents its own pacing.
//   4. THE CONFLICT MODEL (anim_conflict_*): what has to leave a board that no
//      move took off it, and the order it flies back in. This is the iMessage
//      rule, lifted whole - and it is the one incoming rule that does NOT
//      replace what it met, because anim_resolve_unconfirmed_attack_covers in
//      (2) answers a different question against a server verdict. Both are
//      live; the conflict section below says what separates them.
//
// STYLE (msg_wire.h / evwire.h): fixed-size structs, NO allocation, every input
// range-checked, errors as negative defines. The decoded-event inputs BORROW
// their card storage from the caller (like EvwEvent) — no copy. The plan output
// is a fixed struct the caller owns.
//
// WHAT DID NOT MOVE, and why. evwire DECODING (packed bytes -> events + per-step
// snapshots) still lives in the platform decoders (sdk/ts/wire/evwire.ts,
// sdk/swift/EvWire.swift) that the replay-parity tests pin; this layer consumes
// their OUTPUT (AnimPlanEvent, each step's own board included). The React
// queue's setState/timeout machinery and the SwiftUI matchedGeometry flights
// stay per-platform — they are the rendering the boundary leaves behind. See
// docs/ANIMATION_CORE_C.md.
#ifndef CNITRO_ANIM_PLAN_H
#define CNITRO_ANIM_PLAN_H

#include "card.h"
#include "game.h"   // MAX_PLAYERS, MAX_HAND_SIZE, MAX_BATTLES

// ---------- timing policy -------------------------------------------------
// ANIMATION_TIME (src/constants/constants.ts) — every event animates for this
// long. The web's processAnimationQueue then waits ANIM_GAP_MS before creating
// the next event's cards; the two are a matched pair (see the comment there:
// the gap is coupled to the overlay's clear timeout, "don't lower one without
// the other"). iOS's playStep awaits the flight for the same ANIMATION_TIME.
#define ANIM_TIME_MS 500
#define ANIM_GAP_MS  25

// ---------- event types (mirror ANIMATION_EVENT_TYPE / EVW_T_*) -----------
#define ANIM_EVT_MAGIC_TRANSITION 0
#define ANIM_EVT_DEAL             1
#define ANIM_EVT_FLIPPED          2
#define ANIM_EVT_DEFENDER_MOVE    3
#define ANIM_EVT_ATTACK_PASS      4
#define ANIM_EVT_COVER            5
#define ANIM_EVT_PICKUP           6
#define ANIM_EVT_DISCARD          7
#define ANIM_EVT_OUT              8
#define ANIM_EVT_REFILL           9
#define ANIM_EVT_CARDS_TO_TRASH   10
// CLIENT-ONLY: not a kernel event. AnimationContext synthesises this to fly a
// rejected optimistic card back to hand; it has no evwire byte.
#define ANIM_EVT_REVERT           11

// ---------- locations (mirror EVW_LOC_*) ---------------------------------
#define ANIM_LOC_DECK    0
#define ANIM_LOC_HAND    1
#define ANIM_LOC_TABLE   2
#define ANIM_LOC_DISCARD 3
#define ANIM_LOC_FLIPPED 4
#define ANIM_LOC_NONE    0xFF

#define ANIM_SEAT_NONE (-1)

// ---------- caps (fixed, build-variant-independent) ----------------------
// The SAME cap as ANIM_MAX_BEATS, deliberately: a stream the beats accept must
// have a plan too, or a client would animate a sequence it has no count-freeze
// for and open on the settled board. A bubble carries everything its sender
// staged, which is what sizes both.
#define ANIM_MAX_STEPS 128
// One event's card count: a bout-end pickup sweeps the whole table, MAX_BATTLES
// (64 on the wasm build) x2 = 128. Sized for that hostile-but-legal worst.
#define ANIM_MAX_CARDS 128
// Every card a WHOLE sequence names, for a bridge that has to park them
// somewhere. Far above a full deck swept and re-dealt several times over; the
// per-step product (steps x cards) was two orders of magnitude of dead static.
#define ANIM_MAX_CARD_POOL 1024
// The veil pool: every distinct card the whole sequence touches. A table + a
// full refill is the ceiling; 160 covers it with slack.
#define ANIM_MAX_VEIL  160
// The widest card LIST the policy functions accept as input (an authoritative
// table, or the union of a broadcast's named cards): a full table is
// MAX_BATTLES(64)x2 = 128; 160 covers it with slack. Used by the wasm bridge to
// bound its input scratch.
#define ANIM_MAX_TABLE_INPUT 160

// ---------- errors (all negative; 0 is never an error) -------------------
#define ANIM_EOK      0
#define ANIM_ECAP    -1   // an output buffer / cap would be exceeded
#define ANIM_EBADARG -2   // a NULL pointer, a count out of range, a bad enum
#define ANIM_ETRANSPORT -3 // a question that depends on the transport, asked
                           //   before anyone said which one this is

// ---------- the transport ------------------------------------------------
//
// HOW A CLIENT LEARNS THAT ITS OWN OPTIMISTIC CARD SURVIVED. Set once at
// initialization, because it is a property of the app rather than of the
// question being asked - iMessage is the odd one out and every other client
// shares the server's shape.
//
//   CHAIN   every message carries the whole game, totally ordered (iMessage).
//           A newer chain is the complete truth, so doom is knowable locally
//           and immediately.
//   SERVER  a card's confirmation is its own later broadcast (web, iOS, watch,
//           Steam). "The newest news does not mention my card" means the
//           receipt is still in the post, not that the card was rejected, and
//           reading the second as the first is the card-out / card-home-in-red
//           / card-out-again stutter.
//
// It controls exactly one question - the last `if` in anim_conflict_verdict -
// and it has NO DEFAULT: a verdict that reaches that question with nothing set
// returns ANIM_ETRANSPORT rather than guessing which client it is inside.
#define ANIM_TRANSPORT_UNSET  0
#define ANIM_TRANSPORT_CHAIN  1
#define ANIM_TRANSPORT_SERVER 2

// Returns ANIM_EOK, or ANIM_EBADARG for a value that is not one of the three.
int anim_set_transport(int transport);
// What is set, for diagnostics: a wrong-mode bug looks exactly like an
// animation bug, and reading the mode back is the cheap way to tell them apart.
int anim_transport(void);

// One decoded animation event as this layer sees it — the output of the
// platform evwire decoder, one step of a viewer's sequence. `cards` BORROWS the
// caller's storage (valid for the call only), exactly like EvwEvent: the plan
// reads card identities for the veil and never copies them.
typedef struct {
    int         type;      // ANIM_EVT_*
    int         seat;      // acting seat, or ANIM_SEAT_NONE
    int         from, to;  // ANIM_LOC_*
    const Card *cards;     // n_cards entries; may be NULL iff n_cards == 0
    int         n_cards;
    int         mask_cards; // 1 => cards are viewer-masked backs (no identity);
                            //      excluded from the veil (they animate as backs)
} AnimEvent;

// The count-freeze the plan emits. Before the sequence starts the display holds
// these values (the board as it looked BEFORE this move); each step then
// advances to that step's own board as its flight lands. How the freeze is
// derived - and why it is not a walk back from the final board - is
// anim_build_plan's business.
typedef struct {
    int deck;
    int discard;
    int hand[MAX_PLAYERS];   // per-seat hand size
    int n_players;
} AnimCounts;

// One planned step: the event's identity plus its timing and the board counts
// the display jumps to as this step's flight lands.
typedef struct {
    int type;
    int seat;
    int from, to;
    int n_cards;
    int duration_ms;   // ANIM_TIME_MS
    int start_ms;      // cumulative offset: step i starts at i*(ANIM_TIME_MS+ANIM_GAP_MS)
    // Post-step counts (the display advances to these as the flight lands).
    int deck;
    int discard;
    int hand[MAX_PLAYERS];
    // in-flight-from-deck bookkeeping (web inFlightFromDeck / inFlightToFlipped):
    // how many of this step's cards left the deck, and how many of those are
    // bound for the flipped (trump) slot (which does NOT reduce the deck badge).
    int in_flight_from_deck;
    int in_flight_to_flipped;
} AnimPlanStep;

typedef struct {
    int          n_steps;
    AnimPlanStep steps[ANIM_MAX_STEPS];
    AnimCounts   pre;        // the count-freeze (pre-sequence display values)
    int          total_ms;   // wall time of the whole sequence
    // The veil: real card identities the sequence brings into being that are
    // "not there yet" until their step lands (a card in transit — dealt/refilled
    // into a hand, or an attack/cover placed on the table). Packed as dense ids
    // (card_to_id, 0..51). A platform hides these until the revealing step.
    int          n_veil;
    unsigned char veil_ids[ANIM_MAX_VEIL];
} AnimPlan;

// One decoded event as the PLAN sees it: the step, PLUS the board that step
// committed. `cards` and `hand` BORROW the caller's storage for the call only,
// exactly like EvwEvent.
//
// The board is not decoration. Every evwire event carries the state it produced
// (EvwEvent.snap, GameEvent.state), and that snapshot is the only thing that
// makes the count-freeze derivable at all - see anim_build_plan.
typedef struct {
    int         type;      // ANIM_EVT_*
    int         seat;      // acting seat, or ANIM_SEAT_NONE
    int         from, to;  // ANIM_LOC_*
    const Card *cards;     // n_cards entries; may be NULL iff n_cards == 0
    int         n_cards;
    int         mask_cards; // 1 => cards are viewer-masked backs (no identity);
                            //      excluded from the veil (they animate as backs)
    int         has_counts; // 1 => deck/discard/hand are THIS step's own board
    int         deck, discard;
    const int  *hand;      // n_players entries; NULL iff has_counts == 0
} AnimPlanEvent;

// ---- timing policy: the one place a duration is decided -------------------
// Every event currently paces at ANIM_TIME_MS. A dedicated function (rather than
// inlining the constant) is the seam a per-event-type rule would land in — a
// platform asks here instead of inventing pacing.
int anim_step_duration_ms(int event_type);

// ---- plan building --------------------------------------------------------
// Build the timed plan for a decoded viewer sequence. `final_deck`,
// `final_discard`, `final_hand` (length n_players) are the FINAL committed
// board's counts - the state the platform renders immediately; the plan freezes
// the DISPLAY back to the pre-sequence values and reveals forward per step.
//
// THE FREEZE ANCHORS ON THE FIRST EVENT'S OWN BOARD AND UNDOES EXACTLY ONE
// EVENT. events[0]'s snapshot IS the board one event in, so one undo reaches
// the board before it. That is not a shortcut for undoing all of them; it is
// the only version that is right. A REFILL hands out cards the deck count never
// held - the flipped trump lies UNDER the deck and is dealt last without ever
// being counted - so undoing a refill by putting every card back overshoots the
// deck by one, and the badge opens a card too high near the end of a game.
// Round 16, the owner: "I sometimes saw the deck suddenly go to 5 cards, then
// deal, and now I have 6 cards? Is it a problem with the flipped card?" It was.
//
// A stream never LEADS with a refill - a refill is always some bout end's
// consequence, so a pickup, a trash or a magic transition comes first - which is
// what makes one undo safe where n are not. Undoing all n survives only as the
// fallback for a stream carrying no boards at all, which the packed evwire never
// produces (every event carries one).
//
// Each step's POST counts are that step's OWN board, not a forward derivation
// of it: committing the step's snapshot as its flight lands is what every client
// actually does (iOS GameEvent.state, the web's updateGameState). A step with no
// board of its own carries the derivation forward instead.
//
// Returns ANIM_EOK, or ANIM_ECAP (n_events > ANIM_MAX_STEPS, or the veil
// overflows) / ANIM_EBADARG (NULL out/final_hand, NULL events with n_events > 0,
// n_players out of range).
int anim_build_plan(const AnimPlanEvent *events, int n_events, int n_players,
                    int final_deck, int final_discard, const int *final_hand,
                    AnimPlan *out);

// ---- beats: the SHAPE a sequence plays in ---------------------------------
//
// A stream's events are not its beats. The kernel spends one COVER event per
// card, so a defender who covered two attacks in ONE move arrives as two events
// and must still fly as one movement; consecutive covers by the same seat are
// therefore one beat and nothing else ever merges. On top of that grouping sit
// the rules that decide what a beat carries with it:
//
//   the HOLD    a cover that ended its bout rests before the sweep takes the
//               table away, so the covered table is readable for a moment.
//   the OUTS    an `out` is a notice - no cards, no flight, no time - so a beat
//               that MOVED something adopts the out notices trailing it and
//               collapses those badges with its own card motion instead of a
//               beat later.
//   the PLACED  the identities a beat, and the whole stream, puts DOWN on the
//               table. A later sweep is drawn from these.
//   the BADGE   whose hand count drops as the cards LEAVE rather than as they
//               land (a badge reading 6 with two of that seat's cards in the air
//               is claiming eight).
//
// One entry answers all of them together, because a client that asked for a
// beat's grouping and its hold separately could be told two different things.

// The SAME cap as ANIM_MAX_STEPS, so a stream the beats accept always has a
// plan: a bubble carries everything its sender staged. A stream over the cap is
// REFUSED rather than truncated (half a sequence played as a whole one is worse
// than none).
#define ANIM_MAX_BEATS 128

// "this step carries no board of its own", for good_mask below.
#define ANIM_NO_MASK (-1)

// One event as the beat rules see it. `cards` BORROWS the caller's storage like
// AnimEvent; only real identities count, so a masked back names nothing.
typedef struct {
    int         type;        // ANIM_EVT_*
    int         seat;        // acting seat, or ANIM_SEAT_NONE
    const Card *cards;       // n_cards entries; may be NULL iff n_cards == 0
    int         n_cards;
    int         mask_cards;  // 1 => card backs, no identity
    int         good_mask;   // good_players_mask of THIS step's own board, or
                             //   ANIM_NO_MASK
} AnimBeatEvent;

// Beat flags.
#define ANIM_BEAT_HOLDS  1   // the sequence rests after this beat
#define ANIM_BEAT_MOVED  2   // this beat moved a card at all
#define ANIM_BEAT_PLACED 4   // this beat put a card down on the table
#define ANIM_BEAT_DROPS  8   // the acting badge drops as these cards LEAVE

typedef struct {
    int      first;             // index of the beat's first event
    int      n_events;
    int      type;              // the lead event's type
    int      seat;              // the lead event's seat
    int      flags;             // ANIM_BEAT_*
    unsigned outs_mask;         // seats that go out WITH this beat's motion
    unsigned attack_pass_seats; // seats that laid cards via ATTACK_PASS here;
                                //   the defender among them made a transfer
    uint64_t placed_ids;        // dense card ids this beat puts on the table
    int      good_mask;         // the LAST event's good mask, or ANIM_NO_MASK
} AnimBeat;

typedef struct {
    int      n_beats;
    AnimBeat beats[ANIM_MAX_BEATS];
    uint64_t placed_ids;        // every card the whole stream puts down
    int      first_good_mask;   // event 0's good mask, or ANIM_NO_MASK
} AnimBeats;

// Group a stream into beats and answer every rule above for each one.
// Returns the beat count, or ANIM_EBADARG / ANIM_ECAP.
int anim_build_beats(const AnimBeatEvent *events, int n_events, AnimBeats *out);

// Does a step of this kind take cards OUT of the acting seat's hand? (The
// ANIM_BEAT_DROPS flag as a question, for a caller holding one event.) An
// unrecognised type moves nobody's hand.
int anim_badge_drops_as_cards_leave(int type);

// ---- the role beat --------------------------------------------------------
//
// WHICH marks change at WHICH point of a sequence, which is three different
// timings and not one:
//   a good being SET leads the stream - it is somebody's move, and the
//     transition, the discard and the deal behind it are its consequences;
//   a good being CLEARED runs parallel with the throw-in that cleared it, since
//     the card and the marks are one event and neither leads;
//   a PASS hands the shield over WITH the transfer card.
// Everything else waits for the closing beat at the end of the sequence.
//
// `shown` is what the badges are WEARING, which is not the live board: a
// sequence freezes the marks and walks them forward, so a rule that read the
// resident game would answer about a position nobody is looking at. It crosses
// as an argument, and so does `final_defender`: a pass is snapshotted BEFORE the
// hand-over and writes no event for it, so the new defender appears nowhere in
// the stream - only on the bubble's final board.
typedef struct { int defender; int first_attacker; int good_mask; } AnimRoles;

// Each returns 1 and fills `out` when the marks change, 0 when nothing does.
// A good mask of ANIM_NO_MASK ("this step carried no board") is always 0.
// Only the named bits move; the seats are carried over untouched, so nothing
// flies for a goods change and only the shield flies for a hand-off.
int anim_goods_opening(AnimRoles shown, int first_good_mask, AnimRoles *out);
int anim_goods_cleared(AnimRoles shown, int step_good_mask, AnimRoles *out);
// A transfer is told from an attack by the RULES, not by the wire: the two are
// the same event type, and a defender may not attack, so cards laid by the seat
// currently wearing the shield can only be a pass.
int anim_pass_hand_off(AnimRoles shown, unsigned attack_pass_seats,
                       int final_defender, AnimRoles *out);

// ---- the pre-bout table ---------------------------------------------------
//
// THE TABLE A BOUT END SWEEPS, as it stood the instant before the sweep took
// it. A board opening on a pickup or a discard has to lay that table out to fly
// each card from where it actually sat; the settled board it holds is already
// empty, so without this every swept card starts from one shared centre point.
//
// The pairing is the whole difficulty. A pickup crosses the wire as a FLAT card
// list, so a table that really held two battles with one of them covered reads
// back as three single-card cells - a different grid, which the board animates
// every card into before anything flies off it (round 12, the owner: "they did
// not animate directly from their table positions, but seemed to spread out to
// an evenly spaced row, AND THEN fly to the hand"). A board carries its battles
// with the attack/defence pairing intact, so the answer is to find the step
// whose board still HELD the table rather than to infer the pairing at all.
//
// The rule: walk back from the sweep step to the last board that still had
// cards on it, and for a pickup accept it only if it accounts for exactly the
// cards the pickup takes - a board holding more, or fewer, is describing some
// other moment. Then the board this whole stream OPENED on, under the same
// test. Only then the flat reading.
//
// THE FLAT READING SURVIVES, and the output says so rather than passing it off
// as a table. A pickup that leads its stream carries no earlier board - the
// pickup step's own board is the emptied table - so for a single-action pickup
// turn the pairing lives ONLY in the prior board, and a caller that has no
// prior has nothing better than one cell per card. Measured over 5810 pickups
// (2-6 players, 30 deals, every viewer): 0 recoverable from the stream alone,
// 5653 from the prior board, the remaining 157 needing a prior the caller does
// not have. The flat shape differs from the real table in 3027 of them, which
// is why "is this a real pairing" is an output and not an implementation note.

// 2 bytes per battle - the attack, then its cover or ANIM_TABLE_NONE. The same
// table layout PlayBoard takes (legal.h): one shape for a table in this
// codebase, not a third one for this answer.
#define ANIM_TABLE_NONE 0xFE
// A flat reading lays every card of a pickup in its own cell, so the widest
// answer is one battle per card the wire can name.
#define ANIM_MAX_PRE_BATTLES ANIM_MAX_CARDS
// "this step carried no board of its own", for AnimPreEvent.n_battles and for
// the prior board. A board with an EMPTY table says the same thing to this rule
// - there is no table on it to sweep - so 0 and ANIM_NO_BOARD are one case.
#define ANIM_NO_BOARD (-1)

// One event as this rule sees it: what KIND of step it was, the board it
// committed, and the cards it moved. `battles` and `cards` BORROW the caller's
// storage for the call only, like every other input here.
typedef struct {
    int type;                      // ANIM_EVT_*
    int n_battles;                 // this step's own board, or ANIM_NO_BOARD
    const unsigned char *battles;  // 2 x n_battles bytes
    int n_cards;                   // the step's cards - a pickup's ARE the table
    const unsigned char *cards;    // n_cards dense ids (card_to_id)
} AnimPreEvent;

typedef struct {
    int n_battles;
    unsigned char battles[2 * ANIM_MAX_PRE_BATTLES];
    // 1 when the pairing came off a real board, 0 when it is the flat reading -
    // the same cards in a shape nobody vouched for. A caller choosing between
    // two tables must not treat the second as a table.
    int paired;
} AnimPreTable;

// The table above. `prior` is the board the stream opened on (2 bytes per
// battle), or n_prior == ANIM_NO_BOARD for none. Returns the battle count (0
// when the stream ends no bout), or ANIM_EBADARG / ANIM_ECAP.
int anim_pre_bout_table(const AnimPreEvent *events, int n_events,
                        int n_prior, const unsigned char *prior,
                        AnimPreTable *out);

// ---- optimistic policy ----------------------------------------------------

// Canonical dedup key — the C twin of createCardEventString
// (src/utils/animationUtils.ts). Two events collide iff they name the same
// (type, card, from, to, seat). The web keys on a JSON string that includes the
// player_id; a plan is per-viewer, so the only actor whose optimistic key can
// collide with a confirming broadcast is the local seat, and we carry the seat
// in place of the uuid. Packs into a u64 so a set of keys needs no allocation.
uint64_t anim_event_key(int type, Card card, int from, int to, int seat);

// The card-identity key getCardKey uses (`${suit}-${value}`), as a small int —
// suit*16 + value. Compares two cards for "same card on the table" without the
// event fields. (Distinct from anim_event_key: getCardKey ignores type/from/to.)
static inline int anim_card_key(Card c) { return ((int)c.suit << 5) | ((int)c.value & 31); }

// Live broadcast ordering gate — clientReconcile.shouldDropStaleSequence. A
// broadcast carries the committed games.version; drop any at or below the newest
// already applied (strictly superseded — each sequence carries the full state).
// `has_last`/`has_incoming` model TS null: a replay sequence has no version
// (has_incoming == 0) and is never dropped. Returns 1 (drop) or 0 (apply).
int anim_should_drop_stale(int has_last, int last_version,
                           int has_incoming, int incoming_version);

// The version gate's optimistic release — optimisticAnimation.staleOptimisticKeysOnTable.
// When an authoritative broadcast lands, release the local player's optimistic
// entries for any of its cards the server's table now shows BUT whose own
// confirming event this broadcast does NOT name (their broadcast was dropped by
// the gate). Cards this broadcast DOES name are left for the per-event dedup —
// releasing them here first makes their own confirming event look un-optimistic
// and animate a SECOND time (the double-play bug).
//
//   opt_cards[n_opt]     the local player's pending optimistic cards
//   table_cards[n_table] the authoritative table cards this broadcast shows
//   named_cards[n_named] the union of this broadcast's events' cards
//   out_release[cap]     receives the INDICES into opt_cards to release
// Returns the release count, or ANIM_ECAP if it would exceed `cap`.
int anim_stale_optimistic_on_table(const Card *opt_cards, int n_opt,
                                   const Card *table_cards, int n_table,
                                   const Card *named_cards, int n_named,
                                   int *out_release, int cap);

// A pending optimistic attack/cover, for the revert decision below.
typedef struct {
    Card card;
    int  is_cover;   // 1 if a COVER (the defender's own play — excluded from the
                     //   attack-capacity rule; see optimisticConflicts.ts)
} AnimPending;

// The final personalized state fields the capacity rule reads (a whole game is
// not needed — only these scalars).
typedef struct {
    int defender;                 // seat, or ANIM_SEAT_NONE if undefined
    int n_players;
    int hand_length[MAX_PLAYERS];
    int final_uncovered_attacks;  // table_battles with no defense, in the final state
} AnimFinalState;

// The three-way verdict of anim_resolve_unconfirmed_attack_covers, each a subset
// of the pending cards. Indices into the caller's `pending` array.
typedef struct {
    int n_revert; int revert[ANIM_MAX_CARDS]; // fly back to hand (never accepted)
    int n_merge;  int merge[ANIM_MAX_CARDS];  // keep + merge (not yet confirmed)
    int n_clear;  int clear[ANIM_MAX_CARDS];  // was accepted then swept off; drop
                                              //   tracking, NO revert animation
} AnimResolve;

// The "my optimistically-played attack/cover is not (yet) on the authoritative
// table" decision — optimisticConflicts.resolveUnconfirmedAttackCovers. This is
// the hardened fix for the "card jumps to the table, snaps back to my hand, then
// re-appears" flicker.
//
// THE DECISION IS anim_conflict_verdict's, under ANIM_TRANSPORT_SERVER. What is
// left here is marshalling: the broadcast's cards become the same facts an
// arriving chain builds (the swept set is what the stream MOVES, the
// authoritative table is what it VOUCHES for), the capacity scalars become an
// AnimServerHope, and the three verdicts become the three index sets this
// caller speaks. A KEEP the server table already shows is in NO set - it needs
// no merging, and the per-event dedup upstream owns it.
//
//   events[n_events]   the broadcast's events (type + cards; the clear set is the
//                      union of pickup/cards_to_trash cards)
//   server_table[...]  the authoritative table cards this broadcast shows
//   fin                the final personalized state's scalars
// Returns ANIM_EOK, ANIM_EBADARG (NULL out / bad counts / a pending card with no
// identity), or ANIM_ETRANSPORT if no transport has been set.
int anim_resolve_unconfirmed_attack_covers(const AnimPending *pending, int n_pending,
                                           const Card *server_table, int n_server_table,
                                           const AnimEvent *events, int n_events,
                                           const AnimFinalState *fin,
                                           AnimResolve *out);

// ---- the conflict model ---------------------------------------------------
//
// WHAT HAS TO LEAVE A BOARD THAT NO MOVE TOOK OFF IT: a staged move an arrival
// overrides, a sequence a newer arrival supersedes. The board REVERSES those
// motions before it plays anything else - the cards fly back the way they came,
// tinted red - and only when it stands at a state the newest chain vouches for
// does that chain animate forward. Never a cut, never a snap.
//
// The verdict is per card and asks one thing: does the arriving chain account
// for the card being where the doomed motion put it?
//
//   CLEAR   the chain's own stream moves the card. Its forward replay animates
//           it, so a red flight first is the "I put a card down, someone picked
//           it up, and it flew back to my hand" flicker.
//   KEEP    the card stands at its post spot on the board the chain OPENS on
//           and the chain does not move it. The chain itself is its
//           confirmation; flying it home only for the incoming board to snap it
//           back is that same flicker one board later.
//   REVERT  nothing in the newest truth accounts for it. Unsent staged cards
//           are the canonical case - no other device ever saw them.
//
// PRECEDENCE IS THE RULE, not an implementation detail: CLEAR is tested BEFORE
// the standing sets, because a card the incoming stream moves may also stand on
// its opening table - a pickup's cards do by definition - and a red flight
// first is the flicker.
//
// A masked back is KEPT: it has no identity to conflict on and no per-card view
// to fly back from, having landed INTO a badge. So is anything that went to a
// POOL (the discard pile, the deck, an opponent's badge), because conjuring a
// ghost back OUT of a pile is how the "deal from the pile onto the table" class
// of bug happens.
//
// ONE RULE, TWO TRANSPORTS. anim_resolve_unconfirmed_attack_covers below is no
// longer a second rule: it marshals a server broadcast's inputs into these same
// facts and buckets these same verdicts. Everything above - the precedence, the
// standing sets, the pool and masked-back rules, the reversal's order - is
// identical in both transports. What differs is one question, asked only after
// both tests have failed: is "not accounted for" CONCLUSIVE? A chain is
// complete and totally ordered, so yes. A server broadcast is a partial delta,
// so no, and the server asks its own extra question (AnimServerHope) before
// concluding. See docs/ANIMATION_CORE_C.md.

// The verdict.
#define ANIM_CONFLICT_REVERT 0
#define ANIM_CONFLICT_KEEP   1
#define ANIM_CONFLICT_CLEAR  2

// Where a doomed motion PUT its card - which side of the arriving board the
// standing check reads. POOL is a destination with no persistent per-card view.
#define ANIM_DEST_TABLE   0
#define ANIM_DEST_MY_HAND 1
#define ANIM_DEST_POOL    2

// A motion whose card has no identity (a viewer-masked back), and the same
// sentinel in a card-id list.
#define ANIM_CARD_NONE (-1)

// The three sets a verdict reads, as dense-id bitsets (card_to_id, 0..51 - a
// deck fits a u64, so the sets need no allocation and no hashing).
typedef struct {
    uint64_t incoming_moved;    // identities the arriving stream itself moves
    uint64_t table_at_open;     // identities standing on its opening table
    uint64_t my_hand_at_open;   // identities in MY hand on that board
} AnimConflictFacts;

// THE ARRIVING STREAM'S SWEEP, derived from its own events: which identities it
// moves, and whether it took the table away.
//
// A pickup or a trash names the cards it carries off, and those are exactly the
// ones a revert would fly home out of somebody else's hand; anything else moves
// nothing this rule cares about. That "pickup or trash" test IS the rule - it
// decides table_cleared, which short-circuits the server transport's hope - so
// it is stated once, here, and every caller derives its facts through it rather
// than restating the two event types.
//
// `moved_out` receives up to `cap` dense ids (a masked back names nothing and is
// dropped). Returns the number written, or ANIM_ECAP / ANIM_EBADARG.
int anim_conflict_sweep(const AnimEvent *events, int n_events,
                        int *moved_out, int cap, int *table_cleared_out);

// Build the facts from what an arrival already carries. `moved_ids` are the
// stream's cards (ANIM_CARD_NONE entries are masked backs, which name nothing
// and are dropped); `open_table` is the opening board's table in the 2-bytes-
// per-battle layout every table in this codebase uses (attack, then its cover
// or ANIM_TABLE_NONE) and BOTH sides stand on it, so a cover is in the set;
// `my_hand_ids` is my hand there. A chain that could not be read passes zero of
// everything, which makes every card REVERT - the honest default, since a chain
// nobody can read vouches for nothing.
// Returns ANIM_EOK or ANIM_EBADARG.
int anim_conflict_facts(const int *moved_ids, int n_moved,
                        const unsigned char *open_table, int n_open_battles,
                        const unsigned char *my_hand_ids, int n_my_hand,
                        AnimConflictFacts *out);

// THE SERVER TRANSPORT'S EXTRA QUESTION, as the scalars that answer it: after
// the shared tests have failed, could this card still be accepted? Only read
// under ANIM_TRANSPORT_SERVER, where it is REQUIRED - a NULL there is
// ANIM_EBADARG, because a broadcast that cannot say is not a broadcast that
// says no. Ignored entirely under ANIM_TRANSPORT_CHAIN.
typedef struct {
    int is_cover;         // the defender's own play - the capacity rule is an
                          //   ATTACK rule (game.c handle_attack) and excludes it
    int table_cleared;    // a pickup or trash took the table away; a card the
                          //   sweep did not name never reached it
    int pending_attacks;  // my still-unconfirmed attacks, covers excluded
    int defender_hand;    // the defender's hand in the final state
    int final_uncovered;  // uncovered attacks the final state shows
} AnimServerHope;

// The verdict for one card. `card_id` is a dense id, or ANIM_CARD_NONE for a
// masked back. `hope` is the server transport's extra inputs (see above); pass
// NULL under the chain transport. Returns ANIM_CONFLICT_*, ANIM_EBADARG, or
// ANIM_ETRANSPORT when no transport has been set.
int anim_conflict_verdict(int card_id, int dest, const AnimConflictFacts *facts,
                          const AnimServerHope *hope);

// Which KIND of place an event's cards went, for the verdict above. Placements
// land on the table; my own draws and pickups land in my hand; everything else -
// an opponent's draw or pickup, a discard sweep, the no-flight notices - lands
// in a pool. An unrecognised type is a pool.
int anim_conflict_dest(int event_type, int seat, int my_seat);

// One motion a doomed sequence actually made, as this rule sees it: which card
// it moved and what kind of place it put it. The FLIGHT is the caller's - rects
// and angles are rendering.
typedef struct {
    int card_id;   // dense id, or ANIM_CARD_NONE for a masked back
    int dest;      // ANIM_DEST_*
} AnimConflictMotion;

// A superseded sequence's motions arrive grouped as it flew them (one group per
// parallel step). Sized like the streams that produce them: ANIM_MAX_BEATS
// groups, and no more motions than a whole sequence's cards.
#define ANIM_MAX_CONFLICT_GROUPS  ANIM_MAX_BEATS
#define ANIM_MAX_CONFLICT_MOTIONS 256

typedef struct {
    // Per input motion, in input order.
    int           n_verdicts;
    unsigned char verdicts[ANIM_MAX_CONFLICT_MOTIONS];
    // The reversal: which motions fly back, in which order, in which steps.
    // `order` holds motion indices with the steps laid end to end, and
    // step i owns step_count[i] of them starting at the running sum.
    int n_steps;
    int step_count[ANIM_MAX_CONFLICT_GROUPS];
    int n_order;
    int order[ANIM_MAX_CONFLICT_MOTIONS];
} AnimConflictPlan;

// THE REVERSAL'S SHAPE. Chain transport only - it reverses motions the caller
// already knows are doomed, which is a thing only a total order can know, so it
// passes no AnimServerHope and a SERVER-transport call is ANIM_EBADARG.
// `group_sizes` slices `motions` in the order they flew.
// Steps come back in REVERSE group order - the cards travel back the way they
// came, last motion first - each group staying one parallel step, and a group
// no motion reverts is DROPPED rather than played as a beat of silence.
// Returns the step count, or ANIM_EBADARG / ANIM_ECAP.
int anim_conflict_reversal(const AnimConflictMotion *motions, int n_motions,
                           const int *group_sizes, int n_groups,
                           const AnimConflictFacts *facts,
                           AnimConflictPlan *out);

// ---- the board's own sets and small rules ---------------------------------
//
// THE VEIL, THE HAND, THE TABLE AND THE END SCREEN, lifted whole out of
// ios/FoolishKit/Boards/MessageTableView.swift where they were the last pure
// statics on the view. iMessage is the spec, so these ARE the rule; a client
// that draws the same board on another screen asks here rather than re-deriving.
//
// CARD SETS ARE u64 BITSETS over dense ids (card_to_id, 0..51), the
// representation AnimConflictFacts already uses: a deck fits in 64 bits, so
// membership is a shift and an and, and there is nothing to allocate. A card
// that is not in the deck - a viewer-masked back - has no bit, which is the
// same thing the conflict model says about it: a back has no identity to veil.
//
// ORDERED lists of cards cross as arrays of those same ids, because the fan
// places cards by INDEX and order is the whole point of the rule.

// ---- the veil ----
//
// Four sets are what the veil answers, and everything else about it is state
// (upstream) or a view (downstream).
//
//   veiled            every card the board must render as not-yet-there
//   flying            the ones whose flight is playing THIS INSTANT
//   hand_slot_deferred which veiled hand cards reserve no fan width yet
//   fan               which veiled cards the fan draws nothing for
//
// THREE SOURCES, UNIONED, deliberately not "whichever is up": the animator's
// own hidden set, an arriving replay the board has not begun to animate, and
// the cards THIS live move just put in my hand. `has_my_hand` false takes the
// third source out entirely - that is a spectator, or a board with no view yet,
// and neither has a fan to veil.
uint64_t anim_veil_veiled(uint64_t hidden, uint64_t pending_open,
                          int has_hand_before, uint64_t hand_before,
                          int has_my_hand, uint64_t my_hand);

// "IN THE AIR RIGHT NOW": hidden \ pre_hidden. A pre-hide puts a card in both
// sets and the opening of its own step takes it out of pre_hidden alone, so the
// difference is exactly the flights that are playing.
uint64_t anim_veil_flying(uint64_t hidden, uint64_t pre_hidden);

// Which veiled hand cards reserve NO fan width yet: everything veiled except
// the card whose flight is playing (it needs a real landing frame) and except a
// HELD-BACK card, which is the one veiled card the fan does draw. Without the
// holdback term the fan drops the very cards the holdback exists to keep on
// screen and the hand renders closed.
uint64_t anim_veil_hand_slot_deferred(uint64_t veiled, uint64_t flying,
                                      uint64_t holdback);

// Which veiled cards the fan draws nothing for. A held-back card is veiled so
// its TABLE copy stays invisible until its ghost lands; the hand must un-veil
// its own copy or the fan reserves a slot and draws a gap. INVARIANT:
// anim_veil_hand_slot_deferred is a subset of this - the fan never withholds a
// slot from a card it is drawing.
uint64_t anim_veil_fan(uint64_t veiled, uint64_t holdback);

// What the battle grid is told, once the caller knows WHICH table it is
// drawing. The two branches answer with different state rather than a different
// filter over one, and that is the point: a picked-up card lives in the hand
// AND on the sweeping grid in the same paint, so honouring the hand veil there
// would take the table copy away before its own flight ever lifted it.
void anim_veil_grid(int sweeping, uint64_t veiled,
                    uint64_t swept_flown, uint64_t sweep_unplaced,
                    uint64_t sweep_arriving, uint64_t flying,
                    uint64_t *out_hidden, uint64_t *out_flying);

// WHAT A FINISHING SEQUENCE OWES THE BOARD. The newest sequence is the last one
// standing, so it reveals its own opens and every orphan handed to it. A
// SUPERSEDED one reveals nothing - its replacement has pre-hidden cards of its
// own it has not flown yet - but must pass its opens ON, because opening a slot
// takes a card out of the pre-hidden set and the blanket net can no longer
// reach it.
void anim_veil_teardown(uint64_t opened, uint64_t orphaned, int is_newest,
                        uint64_t *out_reveal, uint64_t *out_carry);

// WHAT A NEW PLAY OWES THE ONE IT REPLACES. The pending-placement ledger holds
// one slot, so a play that finds an earlier one still standing must take that
// veil down before raising its own; nothing else ever will. The cards the new
// play is placing are the exception - revealing those first would flash them
// back into the fan.
void anim_veil_handover(uint64_t standing, uint64_t placing,
                        uint64_t *out_reveal, uint64_t *out_veil);

// IS THERE A REPLAY THE BOARD HAS NOT STARTED? The window the veil is up for.
// Both halves matter: a pending flag with no events veils nothing, and events
// with the flag down belong to a replay already running, whose veil is the
// animator's. Ignoring the flag makes the veil never lift.
int anim_veil_unstarted_replay(int replay_pending, int n_events);

// THE HOLDBACK'S RESCUE. A teardown owns the holdback only if the holdback was
// armed no later than the veil that teardown raised; one armed AFTER belongs to
// the sequence that replaced this one and must survive. Equal epochs ARE the
// same sequence, so this is <= and not <.
int anim_holdback_is_mine(int armed_at, int teardown_at);

// THE SELECTION MAY ONLY EVER NAME CARDS THAT ARE IN MY HAND. A plain toggle
// was true by accident until a played card stayed DRAWN in the fan while its
// replay flew: tapping it inserted an identity the kernel hand no longer held,
// the move was never playable, and the identity stayed for the life of the
// board with the action bar gated off it. Stale ids are swept whatever the tap
// was for, so the bad state is not representable at all.
uint64_t anim_selection_after_tap(uint64_t selection, int card_id, uint64_t hand);

// ---- the hand's layout ----

// Does this step put a card from a hand onto the table?
int anim_is_placement(int event_type);

// ...and is it MINE? The seed for the holdback: a placement by any other seat,
// and every non-placement step, moves nothing out of my hand. A seatless viewer
// (a spectator, my_seat < 0) places nothing - the kernel spends -1 on "no
// particular player" too, so the comparison has to be asked and not assumed.
int anim_is_my_placement(int event_type, int seat, int my_seat);

// WHAT THE FAN IS ASKED TO LAY OUT: my hand, plus whatever an open replay is
// still holding back. A held-back card appears ONCE, and a card the kernel hand
// already contains is never doubled by it - the fan places by index, so a
// duplicate identity is two cards in one slot. Writes at most n_hand + n_held
// ids and returns the count, or ANIM_ECAP / ANIM_EBADARG.
int anim_fan_cards(const unsigned char *hand, int n_hand,
                   const unsigned char *held, int n_held,
                   unsigned char *out, int cap);

// How many cards the fan LAYS OUT: the hand it is really given, minus the deals
// still deferring their slot. Returns the count or ANIM_EBADARG.
int anim_laid_count(const unsigned char *hand, int n_hand,
                    const unsigned char *held, int n_held, uint64_t deferred);

// THE ARRAY THE FAN IS ACTUALLY GIVEN, in the order it draws. `deferred` drops
// out first; then the local arrangement decides: ids `order` knows keep their
// relative order from it, ids it does not know append in kernel order, and
// stale or repeated entries in `order` fall out by construction. `order` is a
// grow-only memory of where cards have sat, so it names cards that are not in
// the hand at all. Returns the count written, or ANIM_ECAP / ANIM_EBADARG.
int anim_hand_laid_out(const unsigned char *cards, int n_cards, uint64_t deferred,
                       const unsigned char *order, int n_order,
                       unsigned char *out, int cap);

// ---- the table under the sweep ----

// A cell holding a card the caller cannot NAME - not an empty cell, which is
// ANIM_TABLE_NONE, but a card that is there and has no dense id. It contributes
// no bit, and on the INNER side of the subset test below it refuses outright: a
// card nothing can name is a card no table can be shown to account for, and
// this rule exists to guarantee a sweep never drops one.
#define ANIM_TABLE_UNKNOWN 0xFF
// Both sentinels must stay OFF the deck: anim_table_card_ids reads every cell
// through the same id-to-bit step and lets the range check answer for them, so
// a sentinel that ever landed inside 0..51 would name a real card.
_Static_assert(ANIM_TABLE_NONE >= 52 && ANIM_TABLE_UNKNOWN >= 52,
               "a table sentinel must not be a card");

// The identities a battle table holds - each attack, and its cover where it has
// one. `table` is the 2-bytes-per-battle layout every table here uses (attack,
// then its cover or ANIM_TABLE_NONE).
uint64_t anim_table_card_ids(const unsigned char *table, int n_battles);

// Does `outer` account for every card on `inner`? The one subset test two
// table choices rest on, so "accounts for every card on it" cannot come to mean
// two different things. An ANIM_TABLE_UNKNOWN cell on the inner table is never
// accounted for.
int anim_table_covers(const unsigned char *outer, int n_outer,
                      const unsigned char *inner, int n_inner);

// Should a bout-ending cover sweep off the KERNEL's covered table rather than
// the one the board already holds? Only when that table is a real board the
// kernel had (`paired`), is not empty, and accounts for everything the live
// sweep already holds. The live sweep is the real prior view and is never wrong
// about which cards were on the table; the swap is only ever earned by ADDING
// the cover to it. The `paired` test is not redundant with the subset one: a
// pickup reconstructed flat, one cell per card, names exactly the same cards in
// a shape nobody vouched for, and the grid animates every card into its new
// cell before anything flies off it.
int anim_covered_sweep_accepts(int paired,
                               const unsigned char *pre, int n_pre,
                               const unsigned char *cur, int n_cur);

// WHICH TABLE THE GRID PAINTS, and whether it is a sweep. Three sources in
// falling order of authority: the live table, the sweep a move of my own
// captured synchronously, and the pre-bout table of an open replay not started
// yet (which exists only because an arrival publishes its view a paint before
// anything sets the sweep). The answer turns on emptiness alone, so the tables
// themselves never cross. Returns ANIM_SHOWN_*; `out_sweeping` may be NULL.
#define ANIM_SHOWN_NONE    (-1)
#define ANIM_SHOWN_LIVE      0
#define ANIM_SHOWN_SWEEP     1
#define ANIM_SHOWN_PENDING   2
int anim_shown_table(int n_live, int n_sweep, int n_pending, int *out_sweeping);

// ---- the end screen ----

// THE FINISH ORDER: rank 1 is the first player out, counting up to the fool
// last. `elimination` is first-out first and holds everyone except the fool;
// `game_over` is the one seat still holding cards, given the last place, or
// negative while the game is still running. `total` on every row is the seat
// count, so a row knows it is the fool without consulting the list.
typedef struct {
    int place;    // 1..total
    int seat;
    int is_you;   // seat == my_seat; a spectator (my_seat < 0) owns no row
} AnimFinishRow;

int anim_finish_rows(const unsigned char *elimination, int n_elim,
                     int game_over, int n_players, int my_seat,
                     AnimFinishRow *out, int cap);

// ---- who may say what the badges are showing ----
//
// The shown deck/discard/hand counts, the out badges and the role marks belong
// to whoever is ANIMATING: a running sequence freezes them to the board before
// its move and walks them forward one step per landing flight. A caller that is
// not that sequence must not write them, or every badge snaps to a value the
// cards on screen have not earned and the stream's next step puts it back.
// Only a bystander ever stands down; the other three claims are the owner in
// one of its three shapes, and guarding the owner's own advance would freeze
// every badge for the life of the board.
#define ANIM_CLAIM_SEQUENCE   0   // the running sequence, advancing its own ledger
#define ANIM_CLAIM_ARMING     1   // seeding a ledger for a sequence about to start
#define ANIM_CLAIM_HAND_OFF   2   // a role hand-off, which flies what it changes
#define ANIM_CLAIM_BYSTANDER  3   // everybody else
int anim_shown_ledger_allows(int claim, int sequencing);

#endif
