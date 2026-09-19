// ios_api.c - THE RESIDENT GAME, and the lifecycle that seats it (see
// ios/include/ios_api.h for the Swift-visible contract).
//
// One static Game, no threads inside (the Swift EngineC wrapper serializes every
// call onto a single queue). Everything crosses as the kernel's own packed bytes
// — the same wire the wasm build hands the web — and Swift decodes them
// directly (MaskedView / MoveWire / awire). No Durak rule lives here either:
// this file only marshals to and from game.c / legal.c / view.c / replay.c
// (docs/IOS_APP_DESIGN.md §3, §16.0).
//
// The bridge is WIDE - 123 entry points at about nine lines each - so it is
// split by what each entry is ABOUT rather than left in one file: the client's
// board slot is ios_api_view.c, the move menu and the board's gesture rules are
// ios_api_play.c, the animation surface is ios_api_anim.c, replays and evwire
// are ios_api_replay.c, the roster and its seat/name gates are
// ios_api_identity.c, the iMessage envelope is ios_api_msg.c, and the bot half
// is ios_bots_api.c (which is a LINK split, not a reading one - see
// ios_internal.h). What stays here is the state they all share and the calls
// that establish it.

#include "ios_api.h"
#include "ios_internal.h"

#include "game.h"
#include "view.h"
#include "msg_wire.h"
#include "awire.h"

#include <string.h>

// ---------- the one game --------------------------------------------------
//
// The resident game and the terms it was dealt under, as ONE struct
// (ios_internal.h FioSession) because they are one fact: a file that may change
// this device's game has to be able to see the deal behind it. This file owns
// the storage; every other TU reaches it through fio_session().
//
// THE LOG MARK (msg_base_logs) is the field with the subtlest contract, so it
// is worth stating where it lives. It is g_session.game's log count at the
// moment the resident game was established from a chain (a decode), or from a
// fresh deal. Everything logged since is what this device is about to send, so
// `fio_msg_encode` hands the mark to msg_seal, which asks the encoder how many
// atoms of the body come after it - the bubble delta (msg_wire.h's n_new),
// which is how a receiver knows to animate this move and not the one before it
// as well.
//
// A LOG mark rather than the parent's atom count (which is what this was until
// round 16): the atom stream is re-derived from the whole log every time it is
// encoded, so a pending good stops being an atom the moment anything follows
// it, and two atom counts subtracted lose exactly those. The log only ever
// grows, so a mark into it is stable. -1 = unknown, and every path that makes a
// game resident without a chain to measure from must say so, because a stale
// base would name the WRONG suffix.
//
// It lives here for the same reason ios_api_msg.c's g_msg_round does: the fact
// belongs to the adopted chain, this bridge is the one place that adopts one,
// and asking Swift to carry it back down at seal time would put a rules input
// in app code (and in every other host's app code) for nothing.
//
// msg_base_sent_at is the send clock of that adopted chain (msg_wire.h's
// sent_at). A bubble that adds NOTHING repeats it rather than stamping now: the
// defender's pickup hold measures from when the attack was actually sent, and
// an undo-to-empty re-seal did not re-send that attack. Without this,
// cancelling a staged move handed every recipient a fresh 15 seconds of hold on
// a board nobody touched.
//
// msg_opening is THE FOOL'S PENALTY: the seat this game OPENED on, or
// MSG_NO_OPENING for the ordinary lowest-trump derivation. It belongs to the
// resident game exactly as deal_seed does - it is a term of the deal, not of
// any one bubble - so it is established once (by the Start that resolved it, or
// by decoding a chain that carries it) and then repeated by every seal of that
// game without Swift having to carry it down each time. A fresh deal clears it:
// an ordinary new game punishes nobody.
//
// msg_carry_key / msg_carry_fool are the other half, and the LOBBY's half: the
// rematch carry a WAITING envelope hands forward until someone taps Start.
// Sticky for the same reason - every join re-seals the lobby, and the question
// must survive each re-seal - and cleared by the deal that answers it
// (fio_new_game), so a live game never carries a lobby's question alongside its
// own answer.
//
// msg_rules are THE RULES THE RESIDENT GAME IS PLAYED UNDER, kept beside the
// seed for the same reason: a re-deal (fio_reseat_game, the rematch Start)
// rebuilds the Game from the locked seed and would otherwise drop them, and the
// lobby that CHOSE them is several bubbles back by then. Set by decoding a
// chain (which states them), or by fio_set_passing before a lobby's first seal;
// repeated by every seal of that game. A fresh deal restores the default, which
// is the classic passing game (game.h GAME_RULE_NO_PASS is what a variant
// costs, not what a default does).
//
// The sentinels are named in the initialiser rather than assumed to be zero,
// because two of them are not.
static FioSession g_session = {
    .msg_base_logs  = -1,
    .msg_opening    = MSG_NO_OPENING,
    .msg_carry_fool = MSG_NO_FOOL,
};

// ---------- THE ONE SCRATCH GAME ------------------------------------------
//
// A Game is 136,328 B at the shipped caps, and this bridge used to hold THREE
// of them: the resident game above, plus a private static scratch inside each
// of fio_legal_from_packed and fio_msg_encode. The resident one has to be
// resident — its log IS the session history the FMSG encoder reads back. The
// other two never were: each is filled from the caller's own bytes at the top
// of a call, consumed before that call returns, and read by nobody afterwards.
// They were static only to keep a 133 KB frame off the stack, which one shared
// slot does just as well.
//
// FoolishKit is linked by the iMessage extension, which is memory-capped and
// has a history of being killed, so a whole resident Game that exists only to
// avoid a stack frame is worth deleting. This changes no computed value
// anywhere: same engine, same inputs, same bytes out.
//
// SAFE BECAUSE THE TWO USES CANNOT OVERLAP. This bridge has no threads inside
// (see the file header: the Swift EngineC wrapper serializes every call onto a
// single queue), neither user keeps state across its own return, and neither
// call path re-enters the other — the kernel never calls back out into fio_*.
// A future entry point may borrow this slot under exactly those terms: fill it
// before you read it, and do not hold it across a return. It is now shared
// ACROSS translation units (fio_scratch_game), which widens the blast radius of
// breaking that rule but not the rule itself.
static Game  g_scratch_game;
static int   g_last_reject = 0;

// ---------- the layout this library was compiled for ------------------------
//
// `make ios-lib` bakes in the structgen hash of the structs the Swift bindings
// read (tools/structgen/specs/ios_layout.args, c/Makefile "the iOS layout").
// The generated module carries the same number, and KernelLayout compares them
// at startup, so a library and a binding built from different headers refuse to
// run instead of reading the right fields at the wrong offsets.
//
// A build that did not stamp one answers 0, which matches no generated module:
// the smoke, golden and archive targets build these sources with the host
// compiler and never meet the Swift side, so they need no libclang.
#ifndef SG_LAYOUT_HASH
#define SG_LAYOUT_HASH 0u
#endif
uint32_t fio_layout_hash(void) { return (uint32_t)SG_LAYOUT_HASH; }

// ---------- what the other translation units may reach ----------------------
//
// Accessors rather than exported statics, so this file stays the owner and "is
// there a game" keeps one answer. ios_internal.h states the terms.

FioSession *fio_session(void) { return &g_session; }

// The resident game, for the bridge's other translation units - the domain
// files beside this one, and ios_bots_api.c across the link split.
Game *fio_resident_game(void) {
    return g_session.has_game ? &g_session.game : 0;
}

Game *fio_scratch_game(void) { return &g_scratch_game; }

// ---------- packed emitters -------------------------------------------------
//
// Client and server are kernel-to-kernel, so these hand out the SAME bytes the
// wasm build does and Swift decodes them directly (MaskedView / MoveWire). The
// menu's emitter is ios_api_play.c's; this is the board's.

// The resident game's masked view for `viewer`, as the packed state wire
// (view.c state_put). Swift's MaskedView decoder reads it.
int fio_state_packed(int viewer, char *out, int cap) {
    if (!g_session.has_game) return FIO_ENOGAME;
    if (cap < 1024) return FIO_ECAP;   // state_put is unbounded; guard the buffer
    return state_put(&g_session.game, viewer, (unsigned char *)out);
}

// ---------- applying a move (§4.4 / A3) -------------------------------------
//
// Nothing is captured on the APPLY path. This bridge used to keep 48 Game-prefix
// snapshot slots and arm engine_snap_hook around every fio_apply_awire so it
// could render the move's events on demand; the app never asked. It animates
// from the chain instead (fio_replay_last_events_packed, which replays through
// replay_steps.c and arms its own hook), so the slots were filled on every move
// and read by nobody.

// The client sends the awire action bytes — fio_apply_awire is the one apply
// entry, and the FMSG rebase path (fio_msg_rebase_awire) reads the same frame.
int fio_apply_awire(int actor_seat, const uint8_t *buf, int len) {
    if (!g_session.has_game) return FIO_ENOGAME;
    if (!buf || len <= 0) return FIO_EBADARG;
    if (actor_seat < 0 || actor_seat >= g_session.game.num_players) return FIO_EBADARG;
    AwireAction a;
    if (!awire_decode(buf, len, &a)) return FIO_EPARSE;

    engine_last_reject = ENGINE_REJECT_NONE;
    // the kernel owns the switch
    bool ok = awire_apply(&g_session.game, actor_seat, &a);
    if (!ok) { g_last_reject = engine_last_reject; return FIO_EREJECT; }
    g_last_reject = 0;
    return FIO_EOK;
}

int fio_last_reject(void) { return g_last_reject; }

// ---------- lifecycle & turn queries --------------------------------------

int fio_new_game(const uint8_t *seed, int seed_len, int n_players) {
    if (n_players < 2 || n_players > MAX_PLAYERS) return FIO_EBADARG;

    // Deal RNG: wide (ChaCha) mode when 32+ seed bytes are supplied — the whole
    // deal space, reproducible on any platform (deal_rng.h). Otherwise fall back
    // to the legacy 32-bit LCG seed from the first 4 bytes (golden fixtures).
    g_session.has_deal_seed = 0;
    if (seed && seed_len >= FOOLISH_SEED_LEN) {
        game_set_deal_seed_bytes(seed, seed_len);
        // Keep it: it is what makes this game's replay v6 (exact hands) instead
        // of v5 (retrodicted). Held here rather than handed back to Swift at
        // share time so the app never has to know a replay needs a deal seed —
        // fio_replay_encode_v6_b32 takes no arguments.
        memcpy(g_session.deal_seed, seed, FOOLISH_SEED_LEN);
        g_session.has_deal_seed = 1;
    } else {
        uint32_t s = 0;
        if (seed) for (int i = 0; i < seed_len && i < 4; i++) s |= ((uint32_t)seed[i]) << (8 * i);
        game_set_seed(s);
        random_strategy_set_seed(s);
    }

    Game *g = &g_session.game;
    memset(g, 0, sizeof *g);
    // Identity (player_id) and the host-side roster mirror are ours to set; the
    // seat count, per-seat kind, and the deal are the kernel's (game_seat_and_deal).
    int8_t strategies[MAX_PLAYERS];
    for (int i = 0; i < n_players; i++) {
        strategies[i] = STRATEGY_KEY_HUMAN;  // all human until fio_set_seat_strategy
    }
    game_seat_and_deal(g, strategies, n_players);
    // A GENUINELY fresh game is the classic one: a variant is chosen for a
    // table, and this deal is not that table (fio_reseat_game, which re-derives
    // the SAME locked seed when a lobby starts, carries the rules across
    // itself). Without this a lobby played podkidnoy would leave the next local
    // game podkidnoy too, in a process that never restarts.
    g_session.msg_rules = 0;
    g->rules = g_session.msg_rules;
    g_session.has_game = 1;
    g_last_reject = 0;
    // A fresh deal continues nothing: every atom after this is new, and nobody
    // has sent it anywhere.
    g_session.msg_base_logs = g->num_logs;
    g_session.msg_base_sent_at = 0;
    // The pin is consumed by the deal above, never left standing: a later
    // ordinary game must not inherit a penalty that was owed to someone else.
    // fio_msg_start_rematch is the one caller that sets both, in that order.
    g_session.msg_opening = MSG_NO_OPENING;
    g_session.msg_carry_key = 0;
    g_session.msg_carry_fool = MSG_NO_FOOL;
    game_open_at_seat(-1);
    return FIO_EOK;
}

// Re-deal the CURRENT resident game's own LOCKED seed at a different player
// count — the iMessage lobby's "Start" action (docs/IMESSAGE_LOBBY_V3.md): a
// group lobby is created OPEN (fio_new_game with the wire's max capacity, 8,
// §5.2) so seats stay free to fill; when the joined players decide to start,
// this re-derives the SAME seed's deal at the ACTUAL joined count (seats are
// claimed lowest-first, so it is always a contiguous 0..<n) — never a new
// random seed, which is the "locked at create" guarantee the lobby promises.
//
// Just fio_new_game fed the session's own seed back to itself: the seed already
// lives in the session (kept there from whichever call last dealt or decoded it
// — fio_new_game or fio_msg_decode), so it never has to cross back out to Swift
// and back in, mirroring the same "the kernel keeps the seed, the app never
// touches it" discipline fio_replay_encode_v6_b32 already relies on. Returns
// FIO_ENOSEED if no wide seed is resident (nothing to re-derive from — a lobby
// is always created wide-seeded, so this is only reachable by calling it out of
// order), or whatever fio_new_game returns for a bad n_players.
int fio_reseat_game(int n_players) {
    if (!g_session.has_deal_seed) return FIO_ENOSEED;
    uint8_t seed[FOOLISH_SEED_LEN];
    // Copy the seed OUT first: fio_new_game overwrites it. …and the same for
    // the RULES. This is the same table dealt again, not a new one, so the
    // variant the lobby chose crosses the re-deal (fio_new_game clears it,
    // deliberately, for the fresh-game case it also serves).
    memcpy(seed, g_session.deal_seed, FOOLISH_SEED_LEN);
    const int8_t rules = g_session.msg_rules;
    const int rc = fio_new_game(seed, FOOLISH_SEED_LEN, n_players);
    if (rc == FIO_EOK) {
        g_session.msg_rules = rules;
        g_session.game.rules = rules;
    }
    return rc;
}

// SET THE TABLE'S RULES before a lobby is sealed - the iMessage lobby's passing
// checkbox, and the only way this variant is ever chosen. `passing` is 1 for
// perevodnoy (the transfer, the default) and 0 for podkidnoy.
//
// It writes both the resident game and the sticky copy, because the two answer
// different questions: the game's own rules decide what is LEGAL right now (and
// what the body of the next seal is coded against), and the sticky copy is what
// survives the re-deal Start performs. A caller sets this AFTER adopting the
// lobby it is changing, and the very next seal states it on the wire.
int fio_set_passing(int passing) {
    g_session.msg_rules = passing ? 0 : (int8_t)GAME_RULE_NO_PASS;
    if (g_session.has_game) g_session.game.rules = g_session.msg_rules;
    return FIO_EOK;
}

// The resident game's rules, as the same 1/0 fio_set_passing takes. 1 with no
// game resident: nothing has said otherwise, and the classic game is what a
// fresh one would be.
int fio_passing_allowed(void) {
    return (g_session.msg_rules & GAME_RULE_NO_PASS) ? 0 : 1;
}

int fio_has_game(void) { return g_session.has_game; }

int fio_actor_mask(void) {
    if (!g_session.has_game) return FIO_ENOGAME;
    const Game *g = &g_session.game;
    int mask = 0;
    for (int i = 0; i < g->num_players; i++) if (should_bot_act(g, i)) mask |= (1 << i);
    return mask;
}

int fio_game_over(void) {
    if (!g_session.has_game) return FIO_ENOGAME;
    return game_done(&g_session.game);
}
