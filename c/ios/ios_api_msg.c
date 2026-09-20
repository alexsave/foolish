// ios_api_msg.c - THE iMESSAGE ENVELOPE, AND THE SURFACE THAT SITS ON IT (see
// ios/include/ios_api.h).
//
// Split out of ios_api.c by domain, unchanged. The widest family in the bridge
// and the one with the most state behind it: decode adopts a chain into the
// resident game, and seal reads it back out. Both go through the session
// (fio_session, ios_internal.h), which is where every term of that chain the
// wire repeats is kept.
//
// The roster the seal writes is read by ios_api_identity.c's gates through the
// same fio_read_joins this file seals with.

#include "ios_api.h"
#include "ios_internal.h"

#include "game.h"
#include "replay.h"
#include "msg_wire.h"
#include "msg_expand.h"
#include "anim_plan.h"
#include "awire.h"
#include "../../shared/c/sha256.h"

#include <string.h>

// ---------- FMSG: the iMessage envelope (src/msg_wire.h) -------------------
//
// The phone's door onto the SAME envelope the web reads. Nothing here decides
// anything: decode/seal/Rule P/rebase are all msg_wire.c, and this file only
// marshals. That is what makes a phone and a browser agree on a payload by
// construction rather than by two implementations staying in step —
// e2e/msg_wire.test.ts pins the wasm side against fixtures the NATIVE kernel
// sealed, and ios_api_smoke drives these against the same bytes.

static int g_last_msg_error = 0;
static int g_msg_round = -1;      // the adopted chain's round — Rule R's guard input

int fio_last_msg_error(void) { return g_last_msg_error; }

// 1.0(6) DIAGNOSTIC: the replay codec version (5/6/7) of the body the last
// fio_msg_decode replayed, or -1 for an empty-body message. Set through
// msg_last_body_version (msg_wire.c).
int fio_msg_last_body_version(void) { return msg_last_body_version; }

// THE HEADER, AS A STRUCT (msg_wire.h MsgHeader), which is how the web has
// taken it since Phase 1 (wasm_msg_header_ptr). It used to be a packed blob
// this file wrote and MessageEnvelope.swift read back field by field: 65 bytes
// of fixed layout plus a join tail, stated twice, in two languages, kept in
// step by hand and by the comment that used to be here explaining which byte
// went where.
//
// Nothing about the ENVELOPE changed - the wire is msg_wire.c's, as it always
// was. What changed is that its reader is generated from the declaration
// (sdk/swift/gen/kernel.ios.swift) instead of written out again.
//
// `actions` is dropped on the way, exactly as the wasm twin drops it: the body
// BORROWS the caller's payload, and a host holding a pointer into bytes the
// next call may overwrite is worse than a host holding no body at all.
static MsgHeader g_msg_header;

const void *fio_msg_header_ptr(void) { return &g_msg_header; }

static void fio_msg_header_set(const MsgEnvelope *e, const uint8_t *digest) {
    g_msg_header.e = *e;
    g_msg_header.e.actions = 0;
    g_msg_header.e.actions_len = 0;
    g_msg_header.e.n_actions = 0;
    memcpy(g_msg_header.digest, digest, SHA256_DIGEST_LEN);
}

// THE RULES, as the one question a UI ever asks of them: may the defender
// transfer (1) or not (0)? Answered here rather than read off `variant`, so no
// host has to know which envelope formats predate the rules byte and are the
// passing game by definition (msg_pass_allowed).
int fio_msg_passing(void) { return msg_pass_allowed(&g_msg_header.e) ? 1 : 0; }

// READ a payload's header and CHANGE NOTHING: the same header as
// fio_msg_decode, without the replay and without touching one byte of
// the resident game or of the base a later seal measures its bubble against.
//
// ROUND 16 - because a decode is not a read. The composer decodes the payload
// it has just sealed, purely to read the joins and the summary out of it, and
// that decode used to ADOPT: it told the kernel "the chain up to and including
// my staged move is history somebody else made", so the NEXT action of the
// same turn measured its delta from the middle of its own bubble. A bubble
// carrying two actions then claimed one, and its caption and its recipient's
// animation both dropped everything but the last (owner: the bubble caption
// naming the wrong span of a turn). The base belongs to the chain this device
// ADOPTED - the bubble it opened, or its own bubble once sent - so composing
// one must not move it, and now it cannot.
//
// A peek can be asked of ANY payload, including one this device could not
// replay: nothing here validates the body, so the fields are the sender's
// claims. Use `fio_msg_decode` for a chain that is about to be PLAYED -
// there validation is the replay, and the replay is the point.
int fio_msg_peek(const uint8_t *payload, int len) {
    if (!payload) return FIO_EBADARG;
    g_last_msg_error = 0;

    MsgEnvelope e;
    const int rc = msg_decode(payload, len, &e);
    if (rc != MSG_EOK) { g_last_msg_error = rc; return FIO_EMSG; }

    uint8_t digest[SHA256_DIGEST_LEN];
    msg_digest(payload, len, digest);
    fio_msg_header_set(&e, digest);
    return FIO_EOK;
}

int fio_msg_decode(const uint8_t *payload, int len) {
    if (!payload) return FIO_EBADARG;
    g_last_msg_error = 0;
    msg_last_body_version = -1;   // 1.0(6) diagnostic reset

    MsgEnvelope e;
    int rc = msg_decode(payload, len, &e);
    if (rc != MSG_EOK) { g_last_msg_error = rc; return FIO_EMSG; }

    // Digest BEFORE anything reuses the bytes — it is the child's parent8 and
    // Rule P's tiebreak.
    uint8_t digest[SHA256_DIGEST_LEN];
    msg_digest(payload, len, digest);

    FioSession *s = fio_session();
    rc = msg_replay(&e, &s->game);   // validation IS replay
    if (rc != MSG_EOK) { g_last_msg_error = rc; return FIO_EMSG; }

    s->has_game = 1;
    g_msg_round = e.round;
    // This chain is now the base every later seal measures its bubble against -
    // the log mark it ends at (which is also what says the game has not moved
    // past it yet) and the clock a bubble that adds nothing must repeat.
    s->msg_base_logs = s->game.num_logs;
    s->msg_base_sent_at = e.sent_at;
    // …and this chain's opening seat is now the resident game's, so every seal
    // of it repeats the term of the deal the chain arrived with.
    s->msg_opening = e.opening;
    s->msg_carry_key = e.carry_key;
    s->msg_carry_fool = e.carry_fool;
    // …and so are its RULES: msg_replay has already stamped them onto the game
    // it dealt, and this is the copy that survives the re-deal at Start.
    s->msg_rules = msg_pass_allowed(&e) ? 0 : (int8_t)GAME_RULE_NO_PASS;
    memcpy(s->deal_seed, e.seed, FOOLISH_SEED_LEN);
    s->has_deal_seed = 1;

    fio_msg_header_set(&e, digest);
    return FIO_EOK;
}

int fio_msg_encode(int phase, int last_actor_seat, uint64_t game_id,
                   const uint8_t parent8[8], const uint8_t *joins, int joins_len,
                   int sent_at, uint8_t *out, int cap) {
    FioSession *s = fio_session();
    if (!s->has_game) return FIO_ENOGAME;
    if (!out || cap <= 0 || !joins || joins_len < 1) return FIO_EBADARG;
    if (!s->has_deal_seed) return FIO_ENOSEED;   // no seed, no serverless game
    g_last_msg_error = 0;

    MsgEnvelope e;
    msg_envelope_init(&e);   // NOT memset: the rematch fields have sentinels
    e.format = MSG_FORMAT_V6;
    e.flags = 0;
    e.phase = (uint8_t)phase;
    e.game_id = game_id;
    e.last_actor_seat = (uint8_t)last_actor_seat;
    e.n_players = (uint8_t)s->game.num_players;
    // The rules are stamped by msg_seal, off the game itself - a host does not
    // get to state them independently of what it is sealing.
    // ROUND 16: the caller's clock, unix seconds mod 65536, or 0 for "do not
    // stamp this one" - which seals a format-2 envelope exactly as before. The
    // time is passed IN rather than read here on purpose: a kernel that called
    // time() would answer differently on two devices holding the same bytes.
    e.sent_at = (uint16_t)(sent_at & 0xffff);
    // …EXCEPT on the bubble that adds nothing (§10's undo-to-empty re-seal),
    // which repeats the adopted chain's stamp instead. `sent_at` is not "when
    // these bytes were made", it is when the move in them was played, and this
    // bubble carries no move: stamping now would restart the defender's pickup
    // hold on an attack that was sent minutes ago, every time somebody changed
    // their mind. See the session's msg_base_sent_at (ios_internal.h).
    const int seal_base = msg_seal_base(&s->game, s->msg_base_logs);
    if (seal_base == MSG_BASE_NOTHING) e.sent_at = s->msg_base_sent_at;
    // The resident game's opening seat, repeated (see ios_api.c). Not a
    // caller's argument: a host that could choose it per bubble could re-point
    // the deal mid-chain, and msg_replay would reject the result anyway.
    e.opening = s->msg_opening;
    e.carry_key = s->msg_carry_key;
    e.carry_fool = s->msg_carry_fool;
    if (parent8) memcpy(e.parent8, parent8, MSG_PARENT_LEN);
    memcpy(e.seed, s->deal_seed, FOOLISH_SEED_LEN);

    const int jrc = fio_read_joins(joins, joins_len, &e);
    if (jrc != FIO_EOK) return jrc;

    static unsigned char body[1024];   // a v6 body measures ~68 B at 8 players
    Game *scratch = fio_scratch_game();   // the shared slot; see ios_api.c
    // ROUND 16: everything played since the resident game was established is
    // what this bubble adds, so the base is the delta msg_seal writes as n_new
    // - or MSG_BASE_NOTHING when nothing was played at all (msg_seal_base).
    const int rc = msg_seal(&e, &s->game, seal_base, body, (int)sizeof body,
                            scratch);
    if (rc != MSG_EOK) { g_last_msg_error = rc; return FIO_EMSG; }
    const int n = msg_encode(&e, out, cap);
    if (n < 0) { g_last_msg_error = n; return n == MSG_ECAP ? FIO_ECAP : FIO_EMSG; }
    return n;
}

// Where THIS DEVICE's own staged run starts in the resident game's atom stream
// - the same question msg_seal answers for the bubble delta, asked for the
// animation instead of for the wire, and answered from the same log mark.
//
// It has to be the mark. A board animating its own move used to pass the atom
// count of the chain it ADOPTED, on the reasoning that everything past it is
// mine; but the atom stream is re-derived from the whole log on every encode
// (see the session's msg_base_logs, ios_internal.h), so that count can be
// HIGHER than the number of atoms the same history now encodes to. Passed as a
// starting point it lands
// past the end of the stream, and the kernel dutifully reports that this turn
// added nothing - the sender's own bout end animating not at all, and (round
// 16) its settlement not being recognised as one to withhold.
int fio_msg_staged_atoms_before(void) {
    const FioSession *s = fio_session();
    if (s->msg_base_logs < 0) return -1;
    return replay_atoms_before_log(s->game.logs, s->game.num_logs,
                                   s->msg_base_logs);
}

// ---------- the chain layer's gates (msg_wire.h) ---------------------------
//
// Nothing here decides anything: each entry hands msg_wire.c's rule its
// arguments. The gates that need a ROSTER to answer are in ios_api_identity.c,
// reading it through the same fio_read_joins this file seals with.

int fio_msg_chain_is_ahead(int a_phase, int a_round, int a_turn,
                           int b_phase, int b_round, int b_turn) {
    return msg_chain_is_ahead(a_phase, a_round, a_turn, b_phase, b_round, b_turn);
}

// ROUND 16 — the pickup hold, on the resident game. Pure relay: the rule is
// msg_pickup_hold_remaining (msg_wire.c) and this only supplies the game.
int fio_msg_pickup_hold(int seat, int sent_at, int now) {
    Game *g = fio_resident_game();
    if (!g) return FIO_ENOGAME;
    return msg_pickup_hold_remaining(g, seat,
                                     (uint16_t)(sent_at & 0xffff),
                                     (uint16_t)(now & 0xffff));
}

int fio_msg_rule_p(const uint8_t *a, int a_len, const uint8_t *b, int b_len) {
    if (!a || !b) return FIO_EBADARG;
    g_last_msg_error = 0;
    MsgChainKey ka, kb;
    int rc = msg_chain_key(a, a_len, &ka);
    if (rc != MSG_EOK) { g_last_msg_error = rc; return FIO_EMSG; }
    rc = msg_chain_key(b, b_len, &kb);
    if (rc != MSG_EOK) { g_last_msg_error = rc; return FIO_EMSG; }
    return msg_rule_p(&ka, &kb);
}

// 1.1(56): the two rules that answer "what does this arriving chain do to the
// lobby on screen" - msg_surface_delta (what changed, and in what order) and
// anim_surface_plan (the beats and their timing) - joined here, which is the
// only place that has both headers. Decodes exactly as fio_msg_rule_p does: two
// header reads, no resident game touched, so a surface may ask this about a
// chain it has not adopted.
// The bridge's names for the controls must BE the kernel's, not merely agree
// with them today: these functions forward `msg_lobby_offered`'s answer through
// unchanged, so a value that drifted would silently relabel every control on the
// lobby. A compile error is the only honest guard for a pair of headers that
// cannot include each other.
_Static_assert(FIO_LOBBY_START   == MSG_LOBBY_START,   "lobby control drift: START");
_Static_assert(FIO_LOBBY_INVITE  == MSG_LOBBY_INVITE,  "lobby control drift: INVITE");
_Static_assert(FIO_LOBBY_WAITING == MSG_LOBBY_WAITING, "lobby control drift: WAITING");
_Static_assert(FIO_LOBBY_JOIN    == MSG_LOBBY_JOIN,    "lobby control drift: JOIN");
_Static_assert(FIO_LOBBY_FULL    == MSG_LOBBY_FULL,    "lobby control drift: FULL");

int fio_msg_lobby_offered(int my_seat, int joined, int capacity,
                          int i_sent_the_newest, int i_changed_the_rules) {
    return msg_lobby_offered(my_seat, joined, capacity,
                             i_sent_the_newest, i_changed_the_rules);
}

int fio_msg_lobby_can_exit(int my_seat, int joined) {
    return msg_lobby_can_exit(my_seat, joined);
}

int fio_msg_lobby_can_set_rules(int my_seat) {
    return msg_lobby_can_set_rules(my_seat);
}

int fio_msg_lobby_rules_changed(int have_baseline, int baseline, int current, int mine) {
    return msg_lobby_rules_changed(have_baseline, baseline, current, mine);
}

int fio_anim_surface_beat_ms(void) { return ANIM_TIME_MS; }

// THE SURFACE PLAN ITSELF (anim_plan.h AnimSurfacePlan), where it lies. It used
// to be flattened into a block of int32 words here and read back word by word
// in SurfacePlan.swift - a beat's kind, idiom, rules, controls and timing, one
// stride apart, stated twice.
static AnimSurfacePlan g_surface_plan;

const void *fio_surface_plan_ptr(void) { return &g_surface_plan; }

int fio_anim_surface_swap(int passing) {
    anim_surface_swap(passing, &g_surface_plan);
    return FIO_EOK;
}

int fio_msg_surface_plan(const uint8_t *showing, int showing_len,
                         const uint8_t *arriving, int arriving_len) {
    if (!showing || !arriving) return FIO_EBADARG;
    g_last_msg_error = 0;
    static MsgEnvelope a, b;   // ~1.3KB each - too big for this frame
    int rc = msg_decode(showing, showing_len, &a);
    if (rc != MSG_EOK) { g_last_msg_error = rc; return FIO_EMSG; }
    rc = msg_decode(arriving, arriving_len, &b);
    if (rc != MSG_EOK) { g_last_msg_error = rc; return FIO_EMSG; }

    MsgSurfaceDelta d;
    msg_surface_delta(&a, &b, &d);
    const int n = anim_surface_plan(d.on_a_lobby, d.roster_moved,
                                    d.passing_before, d.passing_after, d.started,
                                    d.ended, &g_surface_plan);
    if (n < 0) return FIO_EMSG;
    // A PLAN WITH NO BEATS IS STILL AN ANSWER. `settle_ms` answers a question
    // the beats cannot carry - how long before this surface may be put AWAY -
    // and the caller who needs it most is the one holding an empty plan (a lone
    // roster snap, which is folded to no beats and still has to be read before
    // the drawer collapses over it). See anim_plan.h.
    return FIO_EOK;
}

// Rule R over the AWIRE frame - the one rebase entry (the phone stages moves as
// awire and the pending ledger holds them the same way). Same contract as
// wasm_msg_rebase: decode the
// action, then msg_rebase_one against the adopted chain's round (g_msg_round, set
// by the last fio_msg_decode). Returns MSG_REBASE_* (0 re-applied and
// APPLIED to the resident game, 1 discarded by the round guard, 2 discarded as
// illegal), or a negative MSG_E*.
int fio_msg_rebase_awire(int pending_round, int seat, const uint8_t *buf, int len) {
    Game *g = fio_resident_game();
    if (!g) return FIO_ENOGAME;
    if (!buf || len <= 0) return FIO_EBADARG;
    if (g_msg_round < 0) return FIO_ENOGAME;   // nothing adopted to rebase onto
    if (seat < 0 || seat >= g->num_players) return FIO_EBADARG;

    AwireAction a;
    if (!awire_decode(buf, len, &a)) return FIO_EPARSE;
    return msg_rebase_one(g, g_msg_round, pending_round, seat, &a);
}

// ---------- the turn controller, as a transition function -------------------
//
// The chain layer's decisions ACROSS its own suspension points (msg_wire.c's
// msg_turn_*). These marshal and nothing more: no resident game, no static, so
// they are safe to ask from a render pass the way the board rules already are.
//
// The bridge's names are the kernel's values, asserted rather than trusted - a
// bit or a verdict that drifted between the two headers would be a silent wrong
// answer, which is the whole class of bug this section exists to prevent.
_Static_assert(FIO_TURN_STAGED         == MSG_TURN_STAGED,         "turn bits diverged");
_Static_assert(FIO_TURN_SENDING        == MSG_TURN_SENDING,        "turn bits diverged");
_Static_assert(FIO_TURN_READY          == MSG_TURN_READY,          "turn bits diverged");
_Static_assert(FIO_TURN_SUPERSEDED     == MSG_TURN_SUPERSEDED,     "turn bits diverged");
_Static_assert(FIO_TURN_RETRACTING     == MSG_TURN_RETRACTING,     "turn bits diverged");
_Static_assert(FIO_TURN_BOARD_WATCHING == MSG_TURN_BOARD_WATCHING, "turn bits diverged");
_Static_assert(FIO_TURN_HELD           == MSG_TURN_HELD,           "turn bits diverged");
_Static_assert(FIO_TURN_GENESIS        == MSG_TURN_GENESIS,        "turn bits diverged");
_Static_assert(FIO_TURN_CANCEL_NOOP    == MSG_TURN_CANCEL_NOOP,    "cancel diverged");
_Static_assert(FIO_TURN_CANCEL_RESTAGE == MSG_TURN_CANCEL_RESTAGE, "cancel diverged");
_Static_assert(FIO_TURN_CANCEL_CLEAR   == MSG_TURN_CANCEL_CLEAR,   "cancel diverged");
_Static_assert(FIO_TURN_ADMIT_RETRACTING  == MSG_TURN_ADMIT_RETRACTING,  "admit diverged");
_Static_assert(FIO_TURN_ADMIT_SUPERSEDED  == MSG_TURN_ADMIT_SUPERSEDED,  "admit diverged");
_Static_assert(FIO_TURN_ADMIT_HELD_PICKUP == MSG_TURN_ADMIT_HELD_PICKUP, "admit diverged");
_Static_assert(FIO_TURN_ARRIVE_SKIP    == MSG_TURN_ARRIVE_SKIP,    "arrival diverged");
_Static_assert(FIO_TURN_ARRIVE_LATCH   == MSG_TURN_ARRIVE_LATCH,   "arrival diverged");
_Static_assert(FIO_TURN_ARRIVE_ADOPT   == MSG_TURN_ARRIVE_ADOPT,   "arrival diverged");
_Static_assert(FIO_TURN_ARRIVE_RETRACT == MSG_TURN_ARRIVE_RETRACT, "arrival diverged");
_Static_assert(FIO_TURN_BYTES_HOST     == MSG_TURN_BYTES_HOST,     "send picker diverged");
_Static_assert(FIO_TURN_BYTES_SEALED   == MSG_TURN_BYTES_SEALED,   "send picker diverged");
_Static_assert(FIO_TURN_SEND_FOREIGN    == MSG_TURN_SEND_FOREIGN,    "send verdict diverged");
_Static_assert(FIO_TURN_SEND_NOOP       == MSG_TURN_SEND_NOOP,       "send verdict diverged");
_Static_assert(FIO_TURN_SEND_BLIND      == MSG_TURN_SEND_BLIND,      "send verdict diverged");
_Static_assert(FIO_TURN_SEND_DECODE     == MSG_TURN_SEND_DECODE,     "send verdict diverged");
_Static_assert(FIO_TURN_SEND_UNREADABLE == MSG_TURN_SEND_UNREADABLE, "send verdict diverged");
_Static_assert(FIO_TURN_SEND_REBASE     == MSG_TURN_SEND_REBASE,     "send verdict diverged");
_Static_assert(FIO_TURN_SEND_OTHERGAME  == MSG_TURN_SEND_OTHERGAME,  "send verdict diverged");

int fio_msg_turn_can_send(int state) { return msg_turn_can_send(state); }

int fio_msg_turn_can_act(int state, int n_human_moves) {
    return msg_turn_can_act(state, n_human_moves);
}

int fio_msg_turn_can_stage(int state, int n_human_moves) {
    return msg_turn_can_stage(state, n_human_moves);
}

int fio_msg_turn_cancel(int state, int n_pending) {
    return msg_turn_cancel(state, n_pending);
}

int fio_msg_turn_admit(int state, int move_type, int pickup_hold) {
    return msg_turn_admit(state, move_type, pickup_hold);
}

int fio_msg_turn_arrival(int state, int same_chain) {
    return msg_turn_arrival(state, same_chain);
}

int fio_msg_turn_adopt_duplicate(int state, int same_chain) {
    return msg_turn_adopt_duplicate(state, same_chain);
}

int fio_msg_turn_sent_source(int staged, int have_host, int have_sealed) {
    return msg_turn_sent_source(staged, have_host, have_sealed);
}

int fio_msg_turn_send_verdict(int staged, int have_host, int have_sealed,
                              int host_is_sealed, int decoded, int same_game) {
    return msg_turn_send_verdict(staged, have_host, have_sealed, host_is_sealed,
                                 decoded, same_game);
}

int fio_msg_turn_hold_state(int n_events, int cut) {
    return msg_turn_hold_state(n_events, cut);
}

void fio_msg_turn_publish(int state, int base_atoms_before, int staged_atoms_before,
                          int n_open_replay, int view_would_change,
                          int *out_show_held_view, int *out_empty_menu,
                          int *out_anim_atoms_before, int *out_raise_veil) {
    MsgTurnRead in;
    MsgTurnPublished out;
    in.state = state;
    in.base_atoms_before = base_atoms_before;
    in.staged_atoms_before = staged_atoms_before;
    in.n_open_replay = n_open_replay;
    in.view_would_change = view_would_change;
    msg_turn_publish(&in, &out);
    if (out_show_held_view)    *out_show_held_view = out.show_held_view;
    if (out_empty_menu)        *out_empty_menu = out.empty_menu;
    if (out_anim_atoms_before) *out_anim_atoms_before = out.anim_atoms_before;
    if (out_raise_veil)        *out_raise_veil = out.raise_veil;
}

/* ---------------------------------------------------------------------------
 * THE NAME-ENTRY DRAWER. c/src/msg_expand.c decides; MessagesViewController
 * performs the effect and owns the callbacks. Kept as a self-contained block at
 * the end of this file so it merges past anything else landing here.
 * ------------------------------------------------------------------------- */

_Static_assert(FIO_EXPAND_WANTED   == MSG_EXPAND_WANTED,   "expand events diverged");
_Static_assert(FIO_EXPAND_COMPACT  == MSG_EXPAND_COMPACT,  "expand events diverged");
_Static_assert(FIO_EXPAND_EXPANDED == MSG_EXPAND_EXPANDED, "expand events diverged");

int fio_msg_expand_note(int event, double now,
                        int *io_pending, int *io_retries, double *io_wanted_at) {
    MsgExpand st;
    st.pending   = io_pending   ? *io_pending   : 0;
    st.retries   = io_retries   ? *io_retries   : 0;
    st.wanted_at = io_wanted_at ? *io_wanted_at : 0.0;
    const int issue = msg_expand_note(&st, event, now);
    if (io_pending)   *io_pending   = st.pending;
    if (io_retries)   *io_retries   = st.retries;
    if (io_wanted_at) *io_wanted_at = st.wanted_at;
    return issue;
}
