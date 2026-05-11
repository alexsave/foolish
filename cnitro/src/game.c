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

#ifdef GRPO_RNG_DEBUG
static _Thread_local int g_seed_set = 0;
static _Thread_local int g_rand_seed_set = 0;
#endif

void game_set_seed(uint32_t s) {
    g_seed = s ? s : 1;
#ifdef GRPO_RNG_DEBUG
    g_seed_set = 1;
#endif
}
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
    if (g->num_logs >= MAX_LOGS) {
        // Should never happen at MAX_LOGS=512 for sane games. Drop silently.
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
    p->has_target = false;
}

static void log_add_pair(GameLog *l, Card primary, Card target) {
    if (l->num_pairs >= MAX_LOG_PAIRS) return;
    LogPair *p = &l->pairs[l->num_pairs++];
    p->primary = primary;
    p->target = target;
    p->has_target = true;
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
    int idx = (int)(game_random() * g->deck_count);
    if (idx < 0) idx = 0;
    if (idx >= g->deck_count) idx = g->deck_count - 1;
    *out = g->deck[idx];
    for (int i = idx + 1; i < g->deck_count; i++) g->deck[i - 1] = g->deck[i];
    g->deck_count--;
    return true;
}

static void deal_initial(Game *g) {
    for (int j = 0; j < g->num_players; j++) g->players[j].hand_count = 0;
    for (int i = 0; i < CARDS_PER_PLAYER; i++) {
        for (int j = 0; j < g->num_players; j++) {
            Card c;
            if (!draw_card(g, &c)) return;
            g->players[j].hand[g->players[j].hand_count++] = c;
        }
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
        lowest_p = (int)(game_random() * g->num_players);
        if (lowest_p < 0) lowest_p = 0;
        if (lowest_p >= g->num_players) lowest_p = g->num_players - 1;
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

    int lowest = determine_lowest_power_index(g);
    g->first_attacker = (int8_t)lowest;
    g->defender = (int8_t)((lowest + 1) % g->num_players);
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
    for (int i = 0; i < g->num_battles; i++) if (!g->table_battles[i].has_defense) n++;
    return n;
}

static bool table_has_value(const Game *g, int v) {
    for (int i = 0; i < g->num_battles; i++) {
        if (g->table_battles[i].attack.value == v) return true;
        if (g->table_battles[i].has_defense && g->table_battles[i].defense.value == v) return true;
    }
    return false;
}

bool handle_attack(Game *g, int player_idx, const Card *cards, int n_cards) {
    if (g->status != GAME_STATUS_PLAYING) return false;
    if (n_cards <= 0) return false;
    if (player_idx == g->defender) return false;

    Player *p = &g->players[player_idx];
    for (int i = 0; i < n_cards; i++) {
        if (!hand_contains(p, cards[i])) return false;
        for (int j = i + 1; j < n_cards; j++) if (card_eq(cards[i], cards[j])) return false;
    }

    bool first_attack = (g->num_battles == 0);
    if (first_attack) {
        for (int i = 1; i < n_cards; i++) if (cards[i].value != cards[0].value) return false;
        if (player_idx != g->first_attacker) return false;
    } else {
        for (int i = 0; i < n_cards; i++) {
            if (!table_has_value(g, cards[i].value)) return false;
        }
    }

    int uncovered = count_uncovered(g);
    int defender_cards = g->players[g->defender].hand_count;
    if (defender_cards < uncovered + n_cards) return false;

    // Apply.
    for (int i = 0; i < n_cards; i++) {
        hand_remove_card(p, cards[i]);
        Battle *b = &g->table_battles[g->num_battles++];
        b->attack = cards[i];
        b->has_defense = false;
    }

    GameLog *l = log_alloc(g, LOG_ATTACK, player_idx);
    for (int i = 0; i < n_cards; i++) log_add_card(l, cards[i]);

    g->good_players_mask = 0;
    g->has_good_timestamp = false;

    if (p->hand_count == 0) {
        p->status = PLAYER_STATUS_OUT;
        p->awaiting_attack = false;
        g->elimination_order[g->num_eliminated++] = (int8_t)player_idx;
        log_alloc(g, LOG_PLAYER_OUT, player_idx);
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
    if (g->status != GAME_STATUS_PLAYING) return false;
    if (n <= 0) return false;
    if (player_idx != g->defender) return false;

    int uncovered = count_uncovered(g);
    if (uncovered == 0) return false;

    Player *def = &g->players[player_idx];
    for (int i = 0; i < n; i++) {
        if (!hand_contains(def, cover_cards[i])) return false;
        for (int j = i + 1; j < n; j++) if (card_eq(cover_cards[i], cover_cards[j])) return false;
    }

    // Each attack card must be on the table & uncovered (matches the TS
    // `battle.attack.value === card.value` lookup).
    for (int i = 0; i < n; i++) {
        bool found = false;
        for (int j = 0; j < g->num_battles; j++) {
            if (!g->table_battles[j].has_defense
                && g->table_battles[j].attack.value == attack_cards[i].value) {
                found = true; break;
            }
        }
        if (!found) return false;
    }
    for (int i = 0; i < n; i++) {
        if (!can_cover(attack_cards[i], cover_cards[i], g->power_suit)) return false;
    }

    // Apply each cover (with logging) and record discards if defender clears
    // their hand.
    for (int i = 0; i < n; i++) {
        Card cover_card = cover_cards[i];
        Card attack_card = attack_cards[i];
        int idx = -1;
        for (int j = 0; j < g->num_battles; j++) {
            if (!g->table_battles[j].has_defense
                && card_eq(g->table_battles[j].attack, attack_card)) {
                idx = j; break;
            }
        }
        if (idx < 0) return false;
        g->table_battles[idx].defense = cover_card;
        g->table_battles[idx].has_defense = true;
        hand_remove_card(def, cover_card);

        GameLog *l = log_alloc(g, LOG_COVER, player_idx);
        log_add_pair(l, cover_card, attack_card);
    }

    if (def->hand_count == 0) {
        // Discard all table cards, refill, advance defender. Mirrors the TS
        // `executeCover` end-of-round branch.
        int discarded = g->num_battles * 2;
        g->discard_pile_length += discarded;

        GameLog *l = log_alloc(g, LOG_DISCARD, -1);
        for (int i = 0; i < g->num_battles; i++) {
            log_add_card(l, g->table_battles[i].attack);
            if (g->table_battles[i].has_defense) log_add_card(l, g->table_battles[i].defense);
        }

        g->num_battles = 0;

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
            g->first_attacker = (int8_t)get_next_player_index(g, g->first_attacker);
        }
        g->defender = (int8_t)get_next_player_index(g, g->first_attacker);

        GameLog *dc = log_alloc(g, LOG_DEFENDER_CHANGE, -1);
        dc->defender_index = g->defender;
        return true;
    }

    g->good_players_mask = 0;
    g->has_good_timestamp = false;

    bool all_covered = (g->num_battles > 0);
    for (int i = 0; i < g->num_battles; i++) {
        if (!g->table_battles[i].has_defense) { all_covered = false; break; }
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
    if (g->status != GAME_STATUS_PLAYING) return false;
    if (n_cards <= 0) return false;
    if (player_idx != g->defender) return false;
    if (g->num_battles == 0) return false;
    for (int i = 0; i < g->num_battles; i++) if (g->table_battles[i].has_defense) return false;

    int v = cards[0].value;
    for (int i = 1; i < n_cards; i++) if (cards[i].value != v) return false;
    for (int i = 0; i < g->num_battles; i++) if (g->table_battles[i].attack.value != v) return false;

    Player *def = &g->players[player_idx];
    for (int i = 0; i < n_cards; i++) {
        if (!hand_contains(def, cards[i])) return false;
        for (int j = i + 1; j < n_cards; j++) if (card_eq(cards[i], cards[j])) return false;
    }

    int next = get_next_player_index(g, g->defender);
    if (g->players[next].hand_count < n_cards + g->num_battles) return false;

    for (int i = 0; i < n_cards; i++) {
        hand_remove_card(def, cards[i]);
        Battle *b = &g->table_battles[g->num_battles++];
        b->attack = cards[i];
        b->has_defense = false;
    }

    GameLog *l = log_alloc(g, LOG_PASS, player_idx);
    for (int i = 0; i < n_cards; i++) log_add_card(l, cards[i]);

    g->good_players_mask = 0;
    g->has_good_timestamp = false;

    if (no_cards_left(g) && def->hand_count == 0) {
        def->status = PLAYER_STATUS_OUT;
        def->awaiting_attack = false;
        g->elimination_order[g->num_eliminated++] = (int8_t)player_idx;
        log_alloc(g, LOG_PLAYER_OUT, player_idx);
    }

    g->defender = (int8_t)next;
    GameLog *dc = log_alloc(g, LOG_DEFENDER_CHANGE, -1);
    dc->defender_index = g->defender;

    int uncovered = count_uncovered(g);
    int defender_cards = g->players[g->defender].hand_count;
    if (uncovered > defender_cards) {
        // TS throws here. We treat as fatal — abort the game.
        g->status = GAME_STATUS_GAME_OVER;
        return false;
    }
    return true;
}

// ---------- Action: pickup --------------------------------------------

bool handle_pickup(Game *g, int player_idx) {
    if (g->status != GAME_STATUS_PLAYING) return false;
    if (player_idx != g->defender) return false;
    if (g->num_battles == 0) return false;

    Player *def = &g->players[player_idx];
    GameLog *l = log_alloc(g, LOG_PICKUP, player_idx);

    for (int i = 0; i < g->num_battles; i++) {
        Battle *b = &g->table_battles[i];
        log_add_card(l, b->attack);
        def->hand[def->hand_count++] = b->attack;
        if (b->has_defense) {
            log_add_card(l, b->defense);
            def->hand[def->hand_count++] = b->defense;
        }
    }
    g->num_battles = 0;

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
    int discarded = g->num_battles * 2;
    g->discard_pile_length += discarded;

    GameLog *l = log_alloc(g, LOG_DISCARD, -1);
    for (int i = 0; i < g->num_battles; i++) {
        log_add_card(l, g->table_battles[i].attack);
        if (g->table_battles[i].has_defense) log_add_card(l, g->table_battles[i].defense);
    }
    g->num_battles = 0;

    refill_player_hands(g);

    g->first_attacker = (int8_t)g->defender;
    g->defender = (int8_t)get_next_player_index(g, g->first_attacker);

    GameLog *dc = log_alloc(g, LOG_DEFENDER_CHANGE, -1);
    dc->defender_index = g->defender;

    g->good_players_mask = 0;
    g->has_good_timestamp = false;
}

bool handle_good(Game *g, int player_idx) {
    if (g->status != GAME_STATUS_PLAYING) return false;
    if (g->players[player_idx].status != PLAYER_STATUS_IN) return false;
    if (player_idx == g->defender) return false;
    if (g->num_battles == 0 && player_idx == g->first_attacker) return false;
    if (g->good_players_mask & (1u << player_idx)) return false;

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
        if (!g->table_battles[i].has_defense) { all_covered = false; break; }
    }
    if (all_good && all_covered) execute_round_transition(g);
    return true;
}

// ---------- should_bot_act --------------------------------------------

bool should_bot_act(const Game *g, int bot_idx) {
    if (g->status != GAME_STATUS_PLAYING) return false;
    if (g->players[bot_idx].status != PLAYER_STATUS_IN) return false;
    bool first_attack = (g->num_battles == 0);
    bool is_def = (bot_idx == g->defender);
    bool all_covered = (g->num_battles > 0);
    for (int i = 0; i < g->num_battles; i++) {
        if (!g->table_battles[i].has_defense) { all_covered = false; break; }
    }
    if (first_attack) return bot_idx == g->first_attacker;
    if (is_def) return !all_covered;
    return !(g->good_players_mask & (1u << bot_idx));
}

// ---------- Clone -----------------------------------------------------

void game_clone(Game *dst, const Game *src) {
    memcpy(dst, src, sizeof(Game));
}
