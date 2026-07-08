// Game engine — direct port of supabase/functions/_shared/{common_utils,
// actions/*}. We keep the same semantics around defender rotation, draw
// order, elimination order, good_players reset, etc.
//
// Where TS does `Math.random()` we call game_random(); where the random
// strategy uses its own seed we call random_strategy_random(). The split
// matches what nitro_collect.ts does (`Math.random = seededRandom` and
// `setRandomSeed(seed)`).

#include "game.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// The wide, reproducible deal (ChaCha) lives ONLY in builds that actually deal:
// the rules kernel (server deal + replay) and native tools/tests. The client
// guards module never deals — its optimistic draws are placeholder cards and it
// never learns the seed — so it is compiled with -DDEAL_RNG_DISABLED and never
// links deal_rng. See the Makefile guards flags.
#ifndef DEAL_RNG_DISABLED
#include "deal_rng.h"
#endif

// ---------- RNG (two independent LCGs, same recurrence as TS) ----------
//
// Both LCGs are _Thread_local so multiple worker threads can each run their
// own game(s) without racing. Default seeds are non-zero so a thread that
// never calls game_set_seed/random_strategy_set_seed still produces a
// well-defined sequence. In DEBUG builds (-DGRPO_RNG_DEBUG) we additionally
// require that the seed was explicitly set in the current thread before any
// game_random() call — catches missing initialization in worker code.

static _Thread_local uint32_t g_seed = 1237;
static _Thread_local uint32_t g_rand_seed = 1;

// Deterministic-deck mode. When on, the deck is a full ChaCha shuffle from the
// stored seed and every draw pops the top — so the whole game (deal AND every
// mid-game refill) is a pure function of the seed, with the persisted deck
// order carrying the determinism between kernel calls. Off by default: the deal
// and draws use the 32-bit LCG exactly as before, so every pinned LCG stream
// (the C suite, the e2e seedSource hooks, in-flight legacy games) is unchanged.
// game_set_seed() always turns it off.
static _Thread_local int g_deal_wide = 0;
#ifndef DEAL_RNG_DISABLED
static _Thread_local DealRng g_deal_rng;

// Fisher-Yates over the whole deck, driven by the ChaCha stream. Called once at
// the deal; afterwards draws just pop, so this is the only shuffle in a game.
static void deal_shuffle(Game *g) {
    for (int i = g->deck_count - 1; i > 0; i--) {
        int j = (int)deal_rng_bounded(&g_deal_rng, (uint32_t)(i + 1));
        Card t = g->deck[i]; g->deck[i] = g->deck[j]; g->deck[j] = t;
    }
}
#endif

// Random index in [0, n) for a DECK DRAW. Deterministic mode pops the top of the
// pre-shuffled deck (0); legacy consumes one game_random() and clamps, byte-for-
// byte as the original inline draw code did (including on a 1-card deck, so the
// pinned LCG stream never desyncs). Deterministic mode is signalled two ways,
// both meaning "the deck is pre-shuffled, pop it": g_deal_wide during the deal
// itself, and the per-game deterministic_deck flag mid-game (restored from the
// durable blob, since the thread-local does not survive between kernel calls).
static int draw_index(const Game *g, int n) {
    if (g_deal_wide || g->deterministic_deck) return 0;
    int idx = (int)(game_random() * n);
    if (idx < 0) idx = 0;
    if (idx >= n) idx = n - 1;
    return idx;
}

// Random index in [0, n) for the first-attacker fallback (pick among players
// when nobody holds a trump). Deterministic mode draws it, unbiased, from the
// same ChaCha stream (so it too is reproducible); legacy uses the LCG.
static int deal_index(int n) {
#ifndef DEAL_RNG_DISABLED
    if (g_deal_wide) return (n <= 1) ? 0 : (int)deal_rng_bounded(&g_deal_rng, (uint32_t)n);
#endif
    int idx = (int)(game_random() * n);
    if (idx < 0) idx = 0;
    if (idx >= n) idx = n - 1;
    return idx;
}

// Observation hook + rejection reason (see game.h). Both are no-cost when
// unused: the hook is NULL by default and the reason is a plain store.
void (*engine_snap_hook)(const Game *g, int tag, int aux) = 0;
int engine_last_reject = ENGINE_REJECT_NONE;

#define SNAP(g, tag, aux) do { if (engine_snap_hook) engine_snap_hook((g), (tag), (aux)); } while (0)
#define REJECT(code) do { engine_last_reject = (code); return false; } while (0)

#ifdef GRPO_RNG_DEBUG
static _Thread_local int g_seed_set = 0;
static _Thread_local int g_rand_seed_set = 0;
#endif

void game_set_seed(uint32_t s) {
    g_seed = s ? s : 1;
    g_deal_wide = 0;   // revert the deal to the legacy 32-bit LCG path
#ifdef GRPO_RNG_DEBUG
    g_seed_set = 1;
#endif
}

#ifndef DEAL_RNG_DISABLED
void game_set_deal_seed_bytes(const uint8_t *seed, int len) {
    if (!seed || len < 32) return;   // too little entropy: leave wide mode off
    deal_rng_seed(&g_deal_rng, seed);
    g_deal_wide = 1;                 // start_game will shuffle; draws then pop
#ifdef GRPO_RNG_DEBUG
    g_seed_set = 1;                  // a wide seed also satisfies the init guard
#endif
}
#else
void game_set_deal_seed_bytes(const uint8_t *seed, int len) { (void)seed; (void)len; }
#endif

int game_deal_seed_active(void) { return g_deal_wide; }
uint32_t game_random_u32(void) {
#ifdef GRPO_RNG_DEBUG
    if (!g_seed_set) {
        fprintf(stderr, "game_random_u32: seed not set in this thread\n");
        abort();
    }
#endif
    g_seed = g_seed * 1664525u + 1013904223u;
    return g_seed;
}
double game_random(void) {
    uint32_t v = game_random_u32();
    return (double)v / 4294967296.0;
}

uint32_t game_rng_get(void) { return g_seed; }
void     game_rng_set(uint32_t s) {
    g_seed = s ? s : 1;
#ifdef GRPO_RNG_DEBUG
    g_seed_set = 1;
#endif
}

void random_strategy_set_seed(uint32_t s) {
    g_rand_seed = s ? s : 1;
#ifdef GRPO_RNG_DEBUG
    g_rand_seed_set = 1;
#endif
}
double random_strategy_random(void) {
#ifdef GRPO_RNG_DEBUG
    if (!g_rand_seed_set) {
        fprintf(stderr, "random_strategy_random: seed not set in this thread\n");
        abort();
    }
#endif
    g_rand_seed = g_rand_seed * 1664525u + 1013904223u;
    return (double)g_rand_seed / 4294967296.0;
}

// ---------- Helpers ----------------------------------------------------

bool can_cover(Card attack, Card defense, int power_suit) {
    if (defense.suit != attack.suit) {
        return defense.suit == power_suit && attack.suit != power_suit;
    }
    return defense.value > attack.value;
}

int get_next_player_index(const Game *g, int current) {
    int n = g->num_players;
    // TS guard: with one (or zero) players still IN the rotation is
    // meaningless — return the caller's seat unchanged. This only fires in
    // the endgame (game_done is imminent) but it decides the final stored
    // first_attacker/defender, so it must match.
    int in_count = 0;
    for (int i = 0; i < n; i++) if (g->players[i].status == PLAYER_STATUS_IN) in_count++;
    if (in_count <= 1) return current;
    int next = (current + 1) % n;
    while (g->players[next].status == PLAYER_STATUS_OUT) {
        next = (next + 1) % n;
    }
    return next;
}

int game_done(const Game *g) {
    int in_count = 0, out_count = 0, last_in = -1;
    for (int i = 0; i < g->num_players; i++) {
        if (g->players[i].status == PLAYER_STATUS_IN) { in_count++; last_in = i; }
        else if (g->players[i].status == PLAYER_STATUS_OUT) { out_count++; }
    }
    if (in_count == 1 && out_count == g->num_players - 1) return last_in;
    return -1;
}

// ---------- Logs -------------------------------------------------------

static GameLog *log_alloc(Game *g, int log_type, int player_idx) {
    bool drop;
    if (g->log_cap > 0) {
        // Short-log instance (sampled-world slot, see game.h): keep only
        // LOG_DISCARD entries, and only while a full-size instance would
        // still have kept the append (log_virt mirrors what num_logs would
        // be if every append had landed) — so espresso's discard memory
        // sees the exact same discard set as before.
        int virt = g->log_virt;
        if (virt < MAX_LOGS) g->log_virt = (int16_t)(virt + 1);
        drop = virt >= MAX_LOGS || log_type != LOG_DISCARD
            || g->num_logs >= g->log_cap;
    } else {
        // Should never happen at MAX_LOGS=512 for sane games. Drop silently.
        drop = g->num_logs >= MAX_LOGS;
    }
    if (drop) {
        static GameLog scratch;
        memset(&scratch, 0, sizeof(scratch));
        scratch.log_type = log_type;
        scratch.player_idx = player_idx;
        scratch.defender_index = -1;
        return &scratch;
    }
    GameLog *l = &g->logs[g->num_logs++];
    l->log_type = log_type;
    l->player_idx = player_idx;
    l->defender_index = -1;
    l->num_pairs = 0;
    return l;
}

static void log_add_card(GameLog *l, Card c) {
    if (l->num_pairs >= MAX_LOG_PAIRS) return;
    LogPair *p = &l->pairs[l->num_pairs++];
    p->primary = c;
    p->target = CARD_NONE;
}

static void log_add_pair(GameLog *l, Card primary, Card target) {
    if (l->num_pairs >= MAX_LOG_PAIRS) return;
    LogPair *p = &l->pairs[l->num_pairs++];
    p->primary = primary;
    p->target = target;
}

// ---------- Hand ops ---------------------------------------------------

static void hand_remove_card(Player *p, Card c) {
    for (int i = 0; i < p->hand_count; i++) {
        if (card_eq(p->hand[i], c)) {
            for (int j = i + 1; j < p->hand_count; j++) p->hand[j - 1] = p->hand[j];
            p->hand_count--;
            return;
        }
    }
}

static bool hand_contains(const Player *p, Card c) {
    for (int i = 0; i < p->hand_count; i++) if (card_eq(p->hand[i], c)) return true;
    return false;
}

// ---------- Deck / draw ------------------------------------------------

static void refill_deck(Game *g) {
    int idx = 0;
    int min_v = min_value_for(g->num_players);
    for (int suit = 0; suit < NUM_SUITS; suit++) {
        for (int v = min_v; v <= ACE_VALUE; v++) {
            g->deck[idx].suit = (int8_t)suit;
            g->deck[idx].value = (int8_t)v;
            idx++;
        }
    }
    g->deck_count = (int16_t)idx;
}

// Draw one card. Mirrors common_utils.ts `draw`: picks a random card from the
// deck (Math.random()) and splices it; if the deck is empty, returns the
// flipped card if any.
static bool draw_card(Game *g, Card *out) {
    if (g->deck_count == 0) {
        if (!g->has_flipped) return false;
        *out = g->flipped;
        g->has_flipped = false;
        return true;
    }
    int idx = draw_index(g, g->deck_count);
    *out = g->deck[idx];
    for (int i = idx + 1; i < g->deck_count; i++) g->deck[i - 1] = g->deck[i];
    g->deck_count--;
    return true;
}

static void deal_initial(Game *g) {
    // Player-major deal, mirroring the TS start_game: each player draws all
    // CARDS_PER_PLAYER cards before the next player starts, and a snapshot
    // hook fires per player (that's the per-player DEAL animation event, with
    // the deck draining 36 → 30 → 24 → ... between snapshots).
    for (int j = 0; j < g->num_players; j++) g->players[j].hand_count = 0;
    for (int j = 0; j < g->num_players; j++) {
        for (int i = 0; i < CARDS_PER_PLAYER; i++) {
            Card c;
            if (!draw_card(g, &c)) break;
            g->players[j].hand[g->players[j].hand_count++] = c;
        }
        SNAP(g, ENGINE_HOOK_DEAL, j);
    }
}

static int determine_lowest_power_index(Game *g) {
    int lowest_v = ACE_VALUE + 1;
    int lowest_p = -1;
    for (int i = 0; i < g->num_players; i++) {
        for (int j = 0; j < g->players[i].hand_count; j++) {
            Card c = g->players[i].hand[j];
            if (c.suit == g->power_suit && c.value < lowest_v) {
                lowest_v = c.value;
                lowest_p = i;
            }
        }
    }
    if (lowest_p == -1) {
        lowest_p = deal_index(g->num_players);
    }
    return lowest_p;
}

void start_game(Game *g) {
    g->status = GAME_STATUS_PLAYING;
    g->num_battles = 0;
    g->num_eliminated = 0;
    g->num_logs = 0;
    g->discard_pile_length = 0;
    g->good_players_mask = 0;
    g->has_good_timestamp = false;

    // Game start log (system event, no player_idx).
    log_alloc(g, LOG_GAME_START, -1);

    for (int i = 0; i < g->num_players; i++) {
        g->players[i].status = PLAYER_STATUS_IN;
    }

    refill_deck(g);
    // Record how this game draws, so mid-game kernel calls (which restore this
    // from the durable blob, not the thread-local) keep popping the pre-shuffled
    // deck. Legacy deals leave it false and draw at random, exactly as before.
    g->deterministic_deck = g_deal_wide ? true : false;
#ifndef DEAL_RNG_DISABLED
    // Seed-dealt game: shuffle the whole deck once from the ChaCha stream, then
    // every draw below (and every mid-game refill) pops the top — the full deal
    // and game are reproducible from the seed.
    if (g_deal_wide) deal_shuffle(g);
#endif
    // TS emits its opening MAGIC_TRANSITION here: PLAYING status, full deck,
    // hands still empty from the lobby.
    SNAP(g, ENGINE_HOOK_START_MAGIC, -1);
    deal_initial(g);

    // Flip a non-Ace.
    Card f;
    while (true) {
        if (!draw_card(g, &f)) break;
        if (f.value == ACE_VALUE) {
            // push back to deck; same as TS (no shuffle, but draw picks random)
            g->deck[g->deck_count++] = f;
            continue;
        }
        break;
    }
    g->flipped = f;
    g->has_flipped = true;
    g->power_suit = f.suit;
    SNAP(g, ENGINE_HOOK_FLIPPED, -1);

    int lowest = determine_lowest_power_index(g);
    g->first_attacker = (int8_t)lowest;
    g->defender = (int8_t)((lowest + 1) % g->num_players);
    SNAP(g, ENGINE_HOOK_START_DEFENDER, g->defender);
}

// Refill phase: defender first if their hand is empty, then around starting
// from first_attacker, mirroring refillPlayerHandsWithEvents.
static bool no_cards_left(const Game *g) {
    return g->deck_count == 0 && !g->has_flipped;
}

static void refill_player_hands(Game *g) {
    if (no_cards_left(g)) {
        for (int i = 0; i < g->num_players; i++) {
            if (g->players[i].hand_count == 0 && g->players[i].status == PLAYER_STATUS_IN) {
                g->players[i].status = PLAYER_STATUS_OUT;
                g->players[i].awaiting_attack = false;
                g->elimination_order[g->num_eliminated++] = (int8_t)i;
            }
        }
        return;
    }

    // Defender draws first if their hand is empty.
    int defender = g->defender;
    if (g->players[defender].hand_count == 0) {
        Card drawn[CARDS_PER_PLAYER];
        int n_drawn = 0;
        while (g->players[defender].hand_count < CARDS_PER_PLAYER) {
            Card c;
            if (!draw_card(g, &c)) break;
            g->players[defender].hand[g->players[defender].hand_count++] = c;
            drawn[n_drawn++] = c;
        }
        if (n_drawn > 0) {
            GameLog *l = log_alloc(g, LOG_DRAW, defender);
            for (int i = 0; i < n_drawn; i++) log_add_card(l, drawn[i]);
            SNAP(g, ENGINE_HOOK_DRAW, defender);
        }
    }

    int p_idx = g->first_attacker;
    bool visited[MAX_PLAYERS] = { false };
    do {
        if (visited[p_idx]) break;
        visited[p_idx] = true;
        Card drawn[CARDS_PER_PLAYER];
        int n_drawn = 0;
        while (g->players[p_idx].hand_count < CARDS_PER_PLAYER) {
            Card c;
            if (!draw_card(g, &c)) break;
            g->players[p_idx].hand[g->players[p_idx].hand_count++] = c;
            drawn[n_drawn++] = c;
        }
        if (n_drawn > 0) {
            GameLog *l = log_alloc(g, LOG_DRAW, p_idx);
            for (int i = 0; i < n_drawn; i++) log_add_card(l, drawn[i]);
            // TS pushes the refill event (and its snapshot) BEFORE the
            // zero-hand OUT check below, so the hook fires here.
            SNAP(g, ENGINE_HOOK_DRAW, p_idx);
        }
        if (g->players[p_idx].hand_count == 0
            && g->players[p_idx].status == PLAYER_STATUS_IN) {
            g->players[p_idx].status = PLAYER_STATUS_OUT;
            g->players[p_idx].awaiting_attack = false;
            g->elimination_order[g->num_eliminated++] = (int8_t)p_idx;
        }
        p_idx = get_next_player_index(g, p_idx);
    } while (p_idx != g->first_attacker);
}

// ---------- Action: attack --------------------------------------------

static int count_uncovered(const Game *g) {
    int n = 0;
    for (int i = 0; i < g->num_battles; i++) if (!!card_is_none(g->table_battles[i].defense)) n++;
    return n;
}

static bool table_has_value(const Game *g, int v) {
    for (int i = 0; i < g->num_battles; i++) {
        if (g->table_battles[i].attack.value == v) return true;
        if (!card_is_none(g->table_battles[i].defense) && g->table_battles[i].defense.value == v) return true;
    }
    return false;
}

// Shared move-validation predicates. Pure (no state mutation, no reject
// side-effect): each handler keeps its own REJECT at its own call site, so
// the per-handler reject-code ORDER — a tested parity contract vs the old TS
// validators — is untouched. Factored out so attack/cover/pass don't each
// inline their own copy of these sweeps (-Oz keeps them as one shared func).
static bool all_in_hand(const Player *p, const Card *c, int n) {
    for (int i = 0; i < n; i++) if (!hand_contains(p, c[i])) return false;
    return true;
}
static bool has_dup(const Card *c, int n) {
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++) if (card_eq(c[i], c[j])) return true;
    return false;
}
static bool all_same_value(const Card *c, int n) {
    for (int i = 1; i < n; i++) if (c[i].value != c[0].value) return false;
    return true;
}

bool handle_attack(Game *g, int player_idx, const Card *cards, int n_cards) {
    engine_last_reject = ENGINE_REJECT_NONE;
    if (n_cards <= 0) REJECT(ENGINE_REJECT_EMPTY);
    if (g->status != GAME_STATUS_PLAYING) REJECT(ENGINE_REJECT_NOT_PLAYING);
    if (player_idx == g->defender) REJECT(ENGINE_REJECT_IS_DEFENDER);

    // Validation ordering mirrors TS validateAttack: full in-hand sweep
    // first, then the duplicate sweep, so multi-fault inputs reject for the
    // same reason on both engines.
    Player *p = &g->players[player_idx];
    if (!all_in_hand(p, cards, n_cards)) REJECT(ENGINE_REJECT_NOT_IN_HAND);
    if (has_dup(cards, n_cards)) REJECT(ENGINE_REJECT_DUPLICATES);

    bool first_attack = (g->num_battles == 0);
    if (first_attack) {
        if (!all_same_value(cards, n_cards)) REJECT(ENGINE_REJECT_NOT_SAME_VALUE);
        if (player_idx != g->first_attacker) REJECT(ENGINE_REJECT_NOT_FIRST_ATTACKER);
    } else {
        for (int i = 0; i < n_cards; i++) {
            if (!table_has_value(g, cards[i].value)) REJECT(ENGINE_REJECT_VALUE_NOT_ON_TABLE);
        }
    }

    int uncovered = count_uncovered(g);
    int defender_cards = g->players[g->defender].hand_count;
    if (defender_cards < uncovered + n_cards) REJECT(ENGINE_REJECT_DEFENDER_CAPACITY);

    // Apply.
    for (int i = 0; i < n_cards; i++) {
        hand_remove_card(p, cards[i]);
        Battle *b = &g->table_battles[g->num_battles++];
        b->attack = cards[i];
        b->defense = CARD_NONE;
    }

    GameLog *l = log_alloc(g, LOG_ATTACK, player_idx);
    for (int i = 0; i < n_cards; i++) log_add_card(l, cards[i]);

    g->good_players_mask = 0;
    g->has_good_timestamp = false;
    SNAP(g, ENGINE_HOOK_ATTACK, player_idx);

    // Attackers only LEAVE the game when the stock is exhausted too — with
    // cards still in the deck they sit out the bout and refill at round end
    // (mirrors the TS no_cards_left guard; without it, dumping a whole hand
    // as throw-ins "won" instantly with 20+ cards still in the stock).
    if (p->hand_count == 0 && no_cards_left(g)) {
        p->status = PLAYER_STATUS_OUT;
        p->awaiting_attack = false;
        g->elimination_order[g->num_eliminated++] = (int8_t)player_idx;
        log_alloc(g, LOG_PLAYER_OUT, player_idx);
        SNAP(g, ENGINE_HOOK_OUT, player_idx);
        return true;
    }

    bool was_first = (g->num_battles == n_cards);
    if (was_first) {
        for (int i = 0; i < g->num_players; i++) if (i != g->defender) g->players[i].awaiting_attack = true;
    } else {
        p->awaiting_attack = true;
    }
    return true;
}

// ---------- Action: cover ---------------------------------------------

bool handle_cover(Game *g, int player_idx,
                  const Card *cover_cards, const Card *attack_cards, int n) {
    engine_last_reject = ENGINE_REJECT_NONE;
    if (g->status != GAME_STATUS_PLAYING) REJECT(ENGINE_REJECT_NOT_PLAYING);
    if (n <= 0) REJECT(ENGINE_REJECT_EMPTY);

    // TS validateCover checks for uncovered attacks BEFORE the defender
    // check; keep that priority.
    int uncovered = count_uncovered(g);
    if (uncovered == 0) REJECT(ENGINE_REJECT_NO_UNCOVERED);
    if (player_idx != g->defender) REJECT(ENGINE_REJECT_NOT_DEFENDER);

    Player *def = &g->players[player_idx];
    if (!all_in_hand(def, cover_cards, n)) REJECT(ENGINE_REJECT_NOT_IN_HAND);
    if (has_dup(cover_cards, n)) REJECT(ENGINE_REJECT_DUPLICATES);

    // Each attack card must be on the table & uncovered — matched by EXACT
    // card (suit+value), not just value. The value-only lookup let a request
    // naming an already-covered card slip past validation when another
    // same-rank attack was still uncovered (the defender double-tap bug the
    // TS side fixed in validateCover).
    for (int i = 0; i < n; i++) {
        bool found = false;
        for (int j = 0; j < g->num_battles; j++) {
            if (!!card_is_none(g->table_battles[j].defense)
                && card_eq(g->table_battles[j].attack, attack_cards[i])) {
                found = true; break;
            }
        }
        if (!found) REJECT(ENGINE_REJECT_ATTACK_NOT_ON_TABLE);
    }
    if (has_dup(attack_cards, n)) REJECT(ENGINE_REJECT_DUPLICATES);
    for (int i = 0; i < n; i++) {
        if (!can_cover(attack_cards[i], cover_cards[i], g->power_suit)) REJECT(ENGINE_REJECT_CANNOT_COVER);
    }

    // Apply each cover (with logging) and record discards if defender clears
    // their hand.
    for (int i = 0; i < n; i++) {
        Card cover_card = cover_cards[i];
        Card attack_card = attack_cards[i];
        int idx = -1;
        for (int j = 0; j < g->num_battles; j++) {
            if (!!card_is_none(g->table_battles[j].defense)
                && card_eq(g->table_battles[j].attack, attack_card)) {
                idx = j; break;
            }
        }
        if (idx < 0) REJECT(ENGINE_REJECT_ATTACK_NOT_ON_TABLE);
        g->table_battles[idx].defense = cover_card;
        hand_remove_card(def, cover_card);

        GameLog *l = log_alloc(g, LOG_COVER, player_idx);
        log_add_pair(l, cover_card, attack_card);
        SNAP(g, ENGINE_HOOK_COVER, idx);
    }

    if (def->hand_count == 0) {
        // Discard all table cards, refill, advance defender. Mirrors the TS
        // `executeCover` end-of-round branch.
        int discarded = g->num_battles * 2;
        g->discard_pile_length += discarded;

        GameLog *l = log_alloc(g, LOG_DISCARD, -1);
        for (int i = 0; i < g->num_battles; i++) {
            log_add_card(l, g->table_battles[i].attack);
            if (!card_is_none(g->table_battles[i].defense)) log_add_card(l, g->table_battles[i].defense);
        }

        g->num_battles = 0;
        SNAP(g, ENGINE_HOOK_DISCARD, -1);

        refill_player_hands(g);

        g->first_attacker = (int8_t)g->defender;
        g->good_players_mask = 0;
        g->has_good_timestamp = false;

        if (def->hand_count == 0) {
            // Defender still empty after refill — they win.
            bool was_in = (g->players[g->first_attacker].status == PLAYER_STATUS_IN);
            g->players[g->first_attacker].status = PLAYER_STATUS_OUT;
            g->players[g->first_attacker].awaiting_attack = false;
            if (was_in) g->elimination_order[g->num_eliminated++] = g->first_attacker;
            log_alloc(g, LOG_PLAYER_OUT, g->first_attacker);
            SNAP(g, ENGINE_HOOK_OUT, g->first_attacker);
            g->first_attacker = (int8_t)get_next_player_index(g, g->first_attacker);
        }
        g->defender = (int8_t)get_next_player_index(g, g->first_attacker);

        GameLog *dc = log_alloc(g, LOG_DEFENDER_CHANGE, -1);
        dc->defender_index = g->defender;
        SNAP(g, ENGINE_HOOK_DEFENDER_MOVE, g->defender);
        return true;
    }

    g->good_players_mask = 0;
    g->has_good_timestamp = false;

    bool all_covered = (g->num_battles > 0);
    for (int i = 0; i < g->num_battles; i++) {
        if (!!card_is_none(g->table_battles[i].defense)) { all_covered = false; break; }
    }
    if (all_covered) {
        g->has_good_timestamp = true; // matches `game.good_timestamp = Date.now()`
        for (int i = 0; i < g->num_players; i++) {
            if (i != g->defender && g->players[i].status == PLAYER_STATUS_IN) {
                g->players[i].awaiting_attack = true;
            }
        }
    }
    return true;
}

// ---------- Action: pass ----------------------------------------------

bool handle_pass(Game *g, int player_idx, const Card *cards, int n_cards) {
    engine_last_reject = ENGINE_REJECT_NONE;
    if (g->status != GAME_STATUS_PLAYING) REJECT(ENGINE_REJECT_NOT_PLAYING);
    if (n_cards <= 0) REJECT(ENGINE_REJECT_EMPTY);

    // TS validatePass priority: same-value → duplicates → defender →
    // in-hand → table-nonempty → cover-present → values-match → capacity.
    int v = cards[0].value;
    if (!all_same_value(cards, n_cards)) REJECT(ENGINE_REJECT_NOT_SAME_VALUE);
    if (has_dup(cards, n_cards)) REJECT(ENGINE_REJECT_DUPLICATES);
    if (player_idx != g->defender) REJECT(ENGINE_REJECT_NOT_DEFENDER);

    Player *def = &g->players[player_idx];
    if (!all_in_hand(def, cards, n_cards)) REJECT(ENGINE_REJECT_NOT_IN_HAND);
    if (g->num_battles == 0) REJECT(ENGINE_REJECT_NO_TABLE_CARDS);
    for (int i = 0; i < g->num_battles; i++) if (!card_is_none(g->table_battles[i].defense)) REJECT(ENGINE_REJECT_COVER_PRESENT);
    for (int i = 0; i < g->num_battles; i++) if (g->table_battles[i].attack.value != v) REJECT(ENGINE_REJECT_PASS_VALUES);

    int next = get_next_player_index(g, g->defender);
    if (g->players[next].hand_count < n_cards + g->num_battles) REJECT(ENGINE_REJECT_PASS_CAPACITY);

    for (int i = 0; i < n_cards; i++) {
        hand_remove_card(def, cards[i]);
        Battle *b = &g->table_battles[g->num_battles++];
        b->attack = cards[i];
        b->defense = CARD_NONE;
    }

    GameLog *l = log_alloc(g, LOG_PASS, player_idx);
    for (int i = 0; i < n_cards; i++) log_add_card(l, cards[i]);

    g->good_players_mask = 0;
    g->has_good_timestamp = false;
    SNAP(g, ENGINE_HOOK_PASS, player_idx);

    if (no_cards_left(g) && def->hand_count == 0) {
        def->status = PLAYER_STATUS_OUT;
        def->awaiting_attack = false;
        g->elimination_order[g->num_eliminated++] = (int8_t)player_idx;
        log_alloc(g, LOG_PLAYER_OUT, player_idx);
        SNAP(g, ENGINE_HOOK_OUT, player_idx);
    }

    g->defender = (int8_t)next;
    GameLog *dc = log_alloc(g, LOG_DEFENDER_CHANGE, -1);
    dc->defender_index = g->defender;

    int uncovered = count_uncovered(g);
    int defender_cards = g->players[g->defender].hand_count;
    if (uncovered > defender_cards) {
        // TS throws here (post-mutation, so the move never commits). We
        // treat as fatal — abort the game — and report the reason.
        g->status = GAME_STATUS_GAME_OVER;
        REJECT(ENGINE_REJECT_PASS_OVERFLOW);
    }
    return true;
}

// ---------- Action: pickup --------------------------------------------

bool handle_pickup(Game *g, int player_idx) {
    engine_last_reject = ENGINE_REJECT_NONE;
    if (g->status != GAME_STATUS_PLAYING) REJECT(ENGINE_REJECT_NOT_PLAYING);
    if (player_idx != g->defender) REJECT(ENGINE_REJECT_NOT_DEFENDER);
    if (g->num_battles == 0) REJECT(ENGINE_REJECT_NO_TABLE_CARDS);

    Player *def = &g->players[player_idx];
    GameLog *l = log_alloc(g, LOG_PICKUP, player_idx);

    // TS pickup logs table cards attack-major (all attacks' cards in battle
    // order, defense right after its attack) — same interleaving here.
    for (int i = 0; i < g->num_battles; i++) {
        Battle *b = &g->table_battles[i];
        log_add_card(l, b->attack);
        def->hand[def->hand_count++] = b->attack;
        if (!card_is_none(b->defense)) {
            log_add_card(l, b->defense);
            def->hand[def->hand_count++] = b->defense;
        }
    }
    g->num_battles = 0;
    SNAP(g, ENGINE_HOOK_PICKUP, player_idx);

    refill_player_hands(g);

    g->first_attacker = (int8_t)get_next_player_index(g, g->defender);
    g->defender = (int8_t)get_next_player_index(g, g->first_attacker);

    GameLog *dc = log_alloc(g, LOG_DEFENDER_CHANGE, -1);
    dc->defender_index = g->defender;
    g->good_players_mask = 0;
    g->has_good_timestamp = false;
    return true;
}

// ---------- Action: good ----------------------------------------------

static void execute_round_transition(Game *g) {
    // TS emits a MAGIC_TRANSITION event with the pre-discard state.
    SNAP(g, ENGINE_HOOK_MAGIC_TRANSITION, -1);

    int discarded = g->num_battles * 2;
    g->discard_pile_length += discarded;

    GameLog *l = log_alloc(g, LOG_DISCARD, -1);
    for (int i = 0; i < g->num_battles; i++) {
        log_add_card(l, g->table_battles[i].attack);
        if (!card_is_none(g->table_battles[i].defense)) log_add_card(l, g->table_battles[i].defense);
    }
    g->num_battles = 0;
    SNAP(g, ENGINE_HOOK_TRASH, -1);

    refill_player_hands(g);

    g->first_attacker = (int8_t)g->defender;
    g->defender = (int8_t)get_next_player_index(g, g->first_attacker);

    GameLog *dc = log_alloc(g, LOG_DEFENDER_CHANGE, -1);
    dc->defender_index = g->defender;

    g->good_players_mask = 0;
    g->has_good_timestamp = false;
}

bool handle_good(Game *g, int player_idx) {
    engine_last_reject = ENGINE_REJECT_NONE;
    if (g->status != GAME_STATUS_PLAYING) REJECT(ENGINE_REJECT_NOT_PLAYING);
    if (g->players[player_idx].status != PLAYER_STATUS_IN) REJECT(ENGINE_REJECT_NOT_IN_STATUS);
    if (player_idx == g->defender) REJECT(ENGINE_REJECT_IS_DEFENDER);
    if (g->num_battles == 0 && player_idx == g->first_attacker) REJECT(ENGINE_REJECT_FIRST_MUST_ATTACK);
    if (g->good_players_mask & (1u << player_idx)) REJECT(ENGINE_REJECT_ALREADY_GOOD);

    g->good_players_mask |= (1u << player_idx);
    log_alloc(g, LOG_GOOD, player_idx);
    g->players[player_idx].awaiting_attack = false;

    // Count attackers and check all_attackers_good.
    int n_attackers = 0;
    bool all_good = true;
    for (int i = 0; i < g->num_players; i++) {
        if (i != g->defender && g->players[i].status == PLAYER_STATUS_IN) {
            n_attackers++;
            if (!(g->good_players_mask & (1u << i))) all_good = false;
        }
    }
    if (n_attackers == 0) all_good = false;

    bool all_covered = (g->num_battles > 0);
    for (int i = 0; i < g->num_battles; i++) {
        if (!!card_is_none(g->table_battles[i].defense)) { all_covered = false; break; }
    }
    if (all_good && all_covered) execute_round_transition(g);
    return true;
}

// Standalone entries mirroring the TS exports (see game.h).
void engine_run_round_transition(Game *g) { execute_round_transition(g); }
void engine_run_refill(Game *g) { refill_player_hands(g); }

// ---------- should_bot_act --------------------------------------------

bool should_bot_act(const Game *g, int bot_idx) {
    if (g->status != GAME_STATUS_PLAYING) return false;
    if (g->players[bot_idx].status != PLAYER_STATUS_IN) return false;
    bool first_attack = (g->num_battles == 0);
    bool is_def = (bot_idx == g->defender);
    bool all_covered = (g->num_battles > 0);
    for (int i = 0; i < g->num_battles; i++) {
        if (!!card_is_none(g->table_battles[i].defense)) { all_covered = false; break; }
    }
    if (first_attack) return bot_idx == g->first_attacker;
    if (is_def) return !all_covered;
    return !(g->good_players_mask & (1u << bot_idx));
}

// ---------- Clone -----------------------------------------------------

void game_clone(Game *dst, const Game *src) {
    memcpy(dst, src, sizeof(Game));
}
