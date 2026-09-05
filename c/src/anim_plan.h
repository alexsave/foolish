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
// re-appears" flicker. Per pending card, decide revert / merge / clear:
//
//   * server already shows one of my cards       -> {} (per-event dedup handles it)
//   * this broadcast is a pickup/cards_to_trash  -> a swept card is CLEARed (it was
//     (has_table_clear)                             accepted then carried off; no
//                                                   revert), an unswept one REVERTs
//   * else the defender can't hold all the        -> the attacks REVERT, covers MERGE
//     attacks (capacity rule, attacks only)
//   * else                                        -> all MERGE (defender can hold them)
//
//   events[n_events]   the broadcast's events (type + cards; the clear set is the
//                      union of pickup/cards_to_trash cards)
//   server_table[...]  the authoritative table cards this broadcast shows
//   fin                the final personalized state's scalars
// Returns ANIM_EOK, or ANIM_EBADARG (NULL out / bad counts).
int anim_resolve_unconfirmed_attack_covers(const AnimPending *pending, int n_pending,
                                           const Card *server_table, int n_server_table,
                                           const AnimEvent *events, int n_events,
                                           const AnimFinalState *fin,
                                           AnimResolve *out);

#endif
