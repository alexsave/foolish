#include "replay_steps.h"
#include "card.h"
#include "view.h"
#include <string.h>

// Two passes, because a deck has to exist before the deal that consumes it:
// pass 1 decodes the code for its atoms (the hands, the draw order, the moves);
// pass 2 builds the deck those atoms imply and plays the moves through the
// engine. The decode is where the cost is and it happens once.

#define RS_MAX_ACTIONS 4096
#define RS_MAX_SNAPS   64

typedef struct {
    int  kind;
    int  seat;
    Card cards[REPLAY_MAX_PAIRS];
    int  n_cards;
    Card target;
} RsAction;

typedef struct {
    Card hands[MAX_PLAYERS][CARDS_PER_PLAYER];
    int  dealt[MAX_PLAYERS];        // cards recorded per seat
    int  n_dealt_seats;
    Card draws[MAX_DECK];
    int  n_draws;
    RsAction acts[RS_MAX_ACTIONS];
    int  n_acts;
    int  overflow;                  // latched; a truncated stream is not replayable
} RsCollect;

static void rs_collect(void *ctx, const ReplayAtom *a) {
    RsCollect *c = (RsCollect *)ctx;
    switch (a->kind) {
        case REPLAY_ATOM_DEAL:
            if (a->seat < 0 || a->seat >= MAX_PLAYERS ||
                a->n_cards != CARDS_PER_PLAYER) { c->overflow = 1; return; }
            for (int i = 0; i < CARDS_PER_PLAYER; i++) c->hands[a->seat][i] = a->cards[i];
            c->dealt[a->seat] = a->n_cards;
            if (a->seat + 1 > c->n_dealt_seats) c->n_dealt_seats = a->seat + 1;
            return;
        case REPLAY_ATOM_DRAW:
            for (int i = 0; i < a->n_cards; i++) {
                if (c->n_draws >= MAX_DECK) { c->overflow = 1; return; }
                c->draws[c->n_draws++] = a->cards[i];
            }
            return;
        default: {
            if (c->n_acts >= RS_MAX_ACTIONS) { c->overflow = 1; return; }
            RsAction *r = &c->acts[c->n_acts++];
            r->kind = a->kind;
            r->seat = a->seat;
            r->n_cards = a->n_cards;
            for (int i = 0; i < a->n_cards; i++) r->cards[i] = a->cards[i];
            r->target = a->target;
            return;
        }
    }
}

/* ---------------------------- snapshot capture ---------------------------- */
// Same trick as the iOS bridge: state_put/put_state read only the Game prefix,
// so a prefix copy can stand in for a Game when an event is rendered.

#define RS_GAME_PREFIX (__builtin_offsetof(Game, num_logs))
typedef struct { _Alignas(8) unsigned char bytes[RS_GAME_PREFIX]; } RsSnapSlot;

static RsSnapSlot g_rs_snaps[RS_MAX_SNAPS];
static int        g_rs_tags[RS_MAX_SNAPS];
static int        g_rs_aux[RS_MAX_SNAPS];
static int        g_rs_n;

static void rs_snap_cb(const Game *g, int tag, int aux) {
    if (g_rs_n >= RS_MAX_SNAPS) return;
    memcpy(g_rs_snaps[g_rs_n].bytes, g, RS_GAME_PREFIX);
    g_rs_tags[g_rs_n] = tag;
    g_rs_aux[g_rs_n] = aux;
    g_rs_n++;
}

// Walk what the last engine call produced, then forget it. The logs are sliced
// from 0 because rs_step clears them per action: unlike the iOS bridge's
// resident Game (whose log is the whole history the encoder reads back), this
// game is a scratch playback and nothing reads its past.
static void rs_walk(const Game *g, int viewer, EvwSink sink, void *ctx) {
    EvSnap refs[RS_MAX_SNAPS];
    for (int i = 0; i < g_rs_n; i++) {
        refs[i].g = (const Game *)(const void *)g_rs_snaps[i].bytes;
        refs[i].tag = g_rs_tags[i];
        refs[i].aux = g_rs_aux[i];
    }
    evwire_walk(refs, g_rs_n, g->logs, g->num_logs, viewer, sink, ctx);
}

/* ------------------------------ deck rebuild ------------------------------ */

// hands (seat-major, as dealt: deal_initial takes all CARDS_PER_PLAYER for one
// seat before the next) ++ the flip ++ every stock draw in pop order ++ the
// cards that never came out.
//
// The tail matters even though it is never drawn: deck_count is on screen. Its
// identities do not — nothing reveals them, by construction — but its SIZE is
// the difference between a truthful deck and a wrong one.
//
// The trump is skipped when copying draws: the engine holds the flip outside
// the deck (has_flipped) and hands it over when the stock runs dry, so the
// decoder's last DRAW — the flipped_held branch — is already accounted for.
// Copying it too would deal the trump twice.
static int rs_build_deck(const RsCollect *c, int n, int trump_id, Card *deck) {
    const int min_v = min_value_for(n);
    int pos = 0;

    for (int s = 0; s < n; s++)
        for (int k = 0; k < CARDS_PER_PLAYER; k++) deck[pos++] = c->hands[s][k];

    deck[pos++] = card_of_id(trump_id);

    for (int i = 0; i < c->n_draws; i++) {
        if (card_to_id(c->draws[i]) == trump_id) continue;
        if (pos >= MAX_DECK) return -1;
        deck[pos++] = c->draws[i];
    }

    unsigned long long seen = 0;
    for (int i = 0; i < pos; i++) seen |= 1ull << card_to_id(deck[i]);
    for (int suit = 0; suit < NUM_SUITS; suit++) {
        for (int v = min_v; v <= ACE_VALUE; v++) {
            Card t; t.suit = (int8_t)suit; t.value = (int8_t)v;
            if (seen & (1ull << card_to_id(t))) continue;
            if (pos >= MAX_DECK) return -1;
            deck[pos++] = t;
        }
    }
    return pos;
}

/* -------------------------------- playback -------------------------------- */

// One action: capture its hooks, then walk them into events. Logs are cleared
// first so the walk sees this action's records and no others.
#define RS_STEP_BEGIN(g) do { g_rs_n = 0; (g)->num_logs = 0; } while (0)

static void rs_apply(Game *g, const RsAction *a) {
    switch (a->kind) {
        case REPLAY_ATOM_ATTACK:
            handle_attack(g, a->seat, a->cards, a->n_cards);
            break;
        case REPLAY_ATOM_COVER:
            handle_cover(g, a->seat, a->cards, &a->target, 1);
            break;
        case REPLAY_ATOM_PASS:
            handle_pass(g, a->seat, a->cards, a->n_cards);
            break;
        case REPLAY_ATOM_PICKUP:
            handle_pickup(g, a->seat);
            break;
        case REPLAY_ATOM_ROUND_END:
            // The live path: every IN attacker says good, and the last one to
            // do it triggers the transition (handle_good). A round whose
            // attackers are ALL out says nothing and would hang here, so the
            // transition is run directly — the same branch apply_round_end has
            // for it, and the reason the decoder reports ROUND_END as an atom
            // instead of leaving it to be inferred from a GOOD that never came.
            for (int s = 0; s < g->num_players; s++) {
                if (s == g->defender) continue;
                if (g->players[s].status != PLAYER_STATUS_IN) continue;
                handle_good(g, s);
            }
            if (g->num_battles > 0) engine_run_round_transition(g);
            break;
        default:
            break;
    }
}

int replay_steps_v6(const unsigned char *code, int code_len, int viewer,
                    ReplayHeader *hdr_out, EvwSink sink, void *ctx) {
    static RsCollect col;
    static Game g;

    memset(&col, 0, sizeof col);
    ReplayHeader hdr;
    int r = replay_decode_atoms_v6(code, code_len, &hdr, rs_collect, &col);
    if (r < 0) return r;
    if (hdr_out) *hdr_out = hdr;
    if (col.overflow) return -REPLAY_ECAP;

    const int n = hdr.n;
    if (n < 2 || n > MAX_PLAYERS) return -REPLAY_EHEADER;
    // Every seat must have shown a full opening hand, or the deck below is
    // built from a hole and the deal silently shifts.
    if (col.n_dealt_seats != n) return -REPLAY_EINPUT;
    for (int s = 0; s < n; s++)
        if (col.dealt[s] != CARDS_PER_PLAYER) return -REPLAY_EINPUT;

    Card deck[MAX_DECK];
    int n_deck = rs_build_deck(&col, n, hdr.trump_id, deck);
    if (n_deck < 0) return -REPLAY_ECAP;

    memset(&g, 0, sizeof g);
    g.num_players = (int8_t)n;

    // The deal and the opening flip are events too — a replay opens with the
    // same deal animation live play does, for free, because it IS the deal.
    void (*saved_hook)(const Game *, int, int) = engine_snap_hook;
    engine_snap_hook = rs_snap_cb;

    RS_STEP_BEGIN(&g);
    start_game_with_deck(&g, deck, n_deck);
    rs_walk(&g, viewer, sink, ctx);

    // The rebuilt deal must be the one the code describes. first_attacker is
    // the check with teeth: the engine derives it from the lowest trump
    // (determine_lowest_power_index) while the header carries what was
    // recorded, so agreement means the hands really came back.
    if (g.first_attacker != (int8_t)hdr.first_attacker) {
        engine_snap_hook = saved_hook;
        return -REPLAY_EHEADER;
    }

    for (int i = 0; i < col.n_acts; i++) {
        RS_STEP_BEGIN(&g);
        rs_apply(&g, &col.acts[i]);
        rs_walk(&g, viewer, sink, ctx);
    }

    engine_snap_hook = saved_hook;
    return REPLAY_EOK;
}
