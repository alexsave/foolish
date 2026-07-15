// See bot_drive.h.

#include "bot_drive.h"

#include "bot_roster.h"
#include "game.h"
#include "legal.h"

// ---------- pacing ---------------------------------------------------------

// The server's values, adopted verbatim as the one table (owner decision, July
// 2026): 3000ms is its tuned inter-bot pace with a human watching (its own note
// records 4500ms as sluggish and 1500ms as too fast to follow), 300ms is the
// bots-only pace nobody watches live.
#define PACE_MS_WITH_HUMANS 3000
#define PACE_MS_BOTS_ONLY    300

int bot_pacing_ms(int pacing_class, int humans_present) {
    switch (pacing_class) {
        case BOT_PACE_MOVE:
        case BOT_PACE_ROUND_TRANSITION:
            return humans_present ? PACE_MS_WITH_HUMANS : PACE_MS_BOTS_ONLY;
        // Nothing became visible, so there is nothing to give the eye time to
        // follow. NOTE this is deliberately stricter than the server's current
        // rule, which skips the wait only in bots-only games and otherwise
        // pauses 3000ms even for a cycle that changed nothing on screen — i.e.
        // it pads a game with silent passives. Bundling exists precisely so
        // those coalesce and cost nothing.
        case BOT_PACE_BUNDLED_PASSIVE:
        case BOT_PACE_NONE:
        default:
            return 0;
    }
}

// ---------- eligibility ----------------------------------------------------

// LegalMoves is far too big for the wasm module's 22KiB shadow stack (the same
// reason robusta/blackpowder hoist their MC scratch off the stack). One shared
// static is safe: bot_drive is never re-entered, and the MC bots it invokes
// keep their own scratch.
static LegalMoves g_scratch;

static int seat_can_act(const Game *g, int seat, uint32_t human_mask, LegalMoves *lm) {
    if (human_mask & (1u << seat)) return 0;
    if (!should_bot_act(g, seat)) return 0;
    calculate_legal_moves(g, seat, lm);
    return lm->n > 0;
}

uint32_t bot_drive_eligible_mask(const Game *g, uint32_t human_mask) {
    if (!g || game_done(g) >= 0) return 0;
    uint32_t mask = 0;
    for (int seat = 0; seat < g->num_players; seat++)
        if (seat_can_act(g, seat, human_mask, &g_scratch)) mask |= (1u << seat);
    return mask;
}

// ---------- fair selection -------------------------------------------------

// A shuffle seed derived from PUBLIC game state only. Deterministic (so a
// replay or a test reproduces the same bot order), varies every action (so no
// seat holds a systematic tempo advantage), and consumes no RNG — drawing from
// the game stream here would shift every subsequent refill and break the
// "reproducible from the deal seed" property.
static uint32_t drive_seed(const Game *g) {
    uint32_t h = 0x9E3779B9u;
#define MIX(v) do { h ^= (uint32_t)(v); h *= 0x85EBCA77u; h ^= h >> 13; } while (0)
    MIX(g->num_logs);
    MIX(g->num_battles);
    MIX(g->deck_count);
    MIX(g->discard_pile_length);
    MIX(g->defender);
    MIX(g->first_attacker);
    MIX(g->good_players_mask);
    for (int i = 0; i < g->num_players; i++) MIX(g->players[i].hand_count * 31 + i);
#undef MIX
    return h ? h : 1;
}

static uint32_t xs32(uint32_t *s) {
    uint32_t x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return *s = x;
}

// ---------- applying -------------------------------------------------------

static int apply_move(Game *g, int seat, const LegalMove *m) {
    switch (m->type) {
        case MOVE_ATTACK: return handle_attack(g, seat, m->cards, m->n_cards) ? 1 : 0;
        case MOVE_COVER:  return handle_cover(g, seat, m->cards, m->attack_cards, m->n_cards) ? 1 : 0;
        case MOVE_PASS:   return handle_pass(g, seat, m->cards, m->n_cards) ? 1 : 0;
        case MOVE_PICKUP: return handle_pickup(g, seat) ? 1 : 0;
        case MOVE_GOOD:   return handle_good(g, seat) ? 1 : 0;
        default:          return 0;
    }
}

// The few scalars that tell a silent `good` from a round-transitioning one.
// Snapshotting the whole Game would put ~100KB (logs[] included) on the stack,
// which the wasm shadow stack cannot hold.
typedef struct { int16_t discard; int8_t defender; int8_t battles; } BoardMark;

static BoardMark mark(const Game *g) {
    BoardMark m;
    m.discard  = g->discard_pile_length;
    m.defender = g->defender;
    m.battles  = g->num_battles;
    return m;
}

// Did applying this move put anything on screen?
//
// This mirrors the server's test — `isPassive && moveEvents === 0` — without
// building the event stream: only `good` (and the never-enumerated `wait`) can
// be silent, and the one thing that makes a `good` visible is the round
// transition it can trigger (handle_good runs execute_round_transition itself
// once everyone is good and everything is covered), which moves the table to
// the discard, changes the defender and refills hands.
static int classify(int move_type, const BoardMark *before, const Game *after) {
    if (move_type != MOVE_GOOD && move_type != MOVE_WAIT) return BOT_PACE_MOVE;
    if (after->discard_pile_length != before->discard
        || after->defender != before->defender
        || after->num_battles != before->battles) return BOT_PACE_ROUND_TRANSITION;
    return BOT_PACE_BUNDLED_PASSIVE;
}

// ---------- the cycle ------------------------------------------------------

void (*bot_drive_pre_action_hook)(const Game *g, int seat, int phase) = 0;

// Choose with the snapshot hook OFF.
//
// A strategy's deliberation is not the board. The Monte-Carlo bots search by
// running real handle_* calls over scratch games (world/trial), and
// engine_snap_hook is global — so a cycle that left it installed across the
// choose would hand the host an animation plan built from a rollout's imaginary
// cards, and fill MAX_SNAPS with them before the real move even landed.
//
// Hosts that choose and apply in SEPARATE calls never see this: they reset the
// snapshot buffer when they open the apply, which is after the choose (the wasm
// bridge's begin_action). A cycle does both in one call, so it must say so.
// Restores rather than clears: whether snapshots are wanted at all is the
// host's call, and a bundle's earlier actions have already recorded theirs.
static int choose_move(const Game *g, int seat, const LegalMoves *moves) {
    void (*saved)(const Game *, int, int) = engine_snap_hook;
    engine_snap_hook = 0;
    // Seats carry a STRAT_* id by kernel-wide convention (the kernel itself
    // reads it — espresso_prod checks strategy_key == STRAT_RANDOM), so the
    // roster entry is resolved back from the brain. That mapping is 1:1 and the
    // roster tests pin it that way.
    int ridx = bot_roster_find_by_strat(g->players[seat].strategy_key);
    int idx = bot_roster_choose(ridx, g, seat, moves);
    engine_snap_hook = saved;
    return idx;
}

// Same move? Compares what the kernel's enumerator produced, so this is an
// exact structural match (type + cards + cover targets), not a heuristic.
static int same_move(const LegalMove *a, const LegalMove *b) {
    if (a->type != b->type || a->n_cards != b->n_cards) return 0;
    for (int i = 0; i < a->n_cards; i++) {
        if (a->cards[i].suit != b->cards[i].suit || a->cards[i].value != b->cards[i].value) return 0;
        if (a->type == MOVE_COVER
            && (a->attack_cards[i].suit != b->attack_cards[i].suit
                || a->attack_cards[i].value != b->attack_cards[i].value)) return 0;
    }
    return 1;
}

// The seat's preferred move, but only if the CURRENT menu still contains it —
// legality is never taken on the host's word. Returns its index in `moves`, or
// -1 to mean "search normally".
static int pref_index(const BotDrivePref *pref, int n_pref, int seat,
                      const LegalMoves *moves) {
    if (!pref) return -1;
    for (int p = 0; p < n_pref; p++) {
        if (pref[p].seat != seat) continue;
        for (int i = 0; i < moves->n; i++)
            if (same_move(&moves->moves[i], &pref[p].move)) return i;
        return -1;   // stale: the reloaded state took it away — re-choose
    }
    return -1;
}

int bot_drive(Game *g, uint32_t human_mask, int max_actions,
              const BotDrivePref *pref, int n_pref, BotDriveOut *out) {
    if (!g || !out) return -1;
    out->n = 0;
    out->stop = BOT_STOP_NO_ELIGIBLE;
    out->ended = -1;

    if (max_actions <= 0 || max_actions > BOT_DRIVE_MAX_ACTIONS)
        max_actions = BOT_DRIVE_MAX_ACTIONS;

    out->ended = game_done(g);
    if (out->ended >= 0) { out->stop = BOT_STOP_ENDED; return 0; }

    // Collect the eligible bot seats, then shuffle ONCE and walk that order —
    // exactly the server's cycle. Re-collecting after each bundled passive
    // would be equivalent (a silent `good` only ever clears the ACTOR's own bit
    // in good_players_mask; anything that changes another seat's eligibility is
    // a round transition, which is not silent and therefore ends the cycle) but
    // it would also reorder the bundle, and the products must stay comparable
    // to the TS cycle byte-for-byte.
    int order[MAX_PLAYERS];
    int n_order = 0;
    for (int seat = 0; seat < g->num_players; seat++)
        if (seat_can_act(g, seat, human_mask, &g_scratch)) order[n_order++] = seat;

    if (n_order == 0) { out->stop = BOT_STOP_NO_ELIGIBLE; return 0; }

    uint32_t rs = drive_seed(g);
    for (int i = n_order - 1; i > 0; i--) {
        int j = (int)(xs32(&rs) % (uint32_t)(i + 1));
        int t = order[i]; order[i] = order[j]; order[j] = t;
    }

    for (int i = 0; i < n_order && out->n < max_actions; i++) {
        int seat = order[i];
        // Re-verify: an earlier action in this same cycle may have taken this
        // seat's move away.
        if (!seat_can_act(g, seat, human_mask, &g_scratch)) continue;

        // A move this seat already chose in a failed CAS attempt, if the
        // reloaded state still allows it — reusing it skips the search, which
        // is the entire point (see BotDrivePref). No CHOOSE phase then: there
        // is no search to seed, exactly as on the host's own replay path.
        int idx = pref_index(pref, n_pref, seat, &g_scratch);
        if (idx < 0) {
            if (bot_drive_pre_action_hook)
                bot_drive_pre_action_hook(g, seat, BOT_DRIVE_PHASE_CHOOSE);
            idx = choose_move(g, seat, &g_scratch);
        }
        if (idx < 0 || idx >= g_scratch.n) continue;

        LegalMove move = g_scratch.moves[idx];
        BoardMark before = mark(g);
        // Per-DECISION, not per-cycle, and after the search: a strategy's
        // rollouts refill scratch games off the draw stream, so this both
        // undoes that consumption and is the point a one-move-per-call host
        // seeds at. Choosing cannot change the real board, so the seed is the
        // same value it would have been before the search.
        if (bot_drive_pre_action_hook)
            bot_drive_pre_action_hook(g, seat, BOT_DRIVE_PHASE_APPLY);
        if (!apply_move(g, seat, &move)) continue;   // rejected: try the next bot

        BotDriveAction *a = &out->actions[out->n++];
        a->seat = (int8_t)seat;
        a->move = move;
        a->pacing_class = (uint8_t)classify(move.type, &before, g);

        out->ended = game_done(g);
        if (out->ended >= 0) { out->stop = BOT_STOP_ENDED; return out->n; }

        // Silent actions bundle: keep going and let the next bot ride the same
        // cycle. Anything visible ends the cycle so the host can render it.
        if (a->pacing_class != BOT_PACE_BUNDLED_PASSIVE) {
            out->stop = BOT_STOP_EVENTS;
            return out->n;
        }
    }

    out->stop = (out->n >= max_actions) ? BOT_STOP_MAX : BOT_STOP_NO_ELIGIBLE;
    return out->n;
}
