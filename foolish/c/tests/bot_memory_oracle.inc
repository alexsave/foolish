// The log scans BotMemory replaced, verbatim from the tree before it
// (renamed, their research switches at their defaults): the equivalence oracle.
// GENERATED once from that tree; never edited to agree with the memory.

static void old_rb_pinned(const Game *g, int bot_idx, OldPool *u) {
    for (int p = 0; p < MAX_PLAYERS; p++) u->pinned_n[p] = 0;
    for (int i = 0; i < g->num_logs; i++) {
        const GameLog *L = &g->logs[i];
        if (L->log_type == LOG_PICKUP) {
            int p = L->player_idx;
            if (p < 0 || p >= g->num_players || p == bot_idx) continue;
            // Pickup adds the (attack) cards onto p's hand. Engine stores
            // them as `primary` in card_pairs.
            for (int k = 0; k < L->num_pairs && u->pinned_n[p] < MAX_HAND_SIZE; k++) {
                u->pinned[p][u->pinned_n[p]++] = L->pairs[k].primary;
            }
        } else if (L->log_type == LOG_ATTACK
                   || L->log_type == LOG_COVER
                   || L->log_type == LOG_PASS) {
            int p = L->player_idx;
            if (p < 0 || p >= g->num_players || p == bot_idx) continue;
            // Cards leave p's hand. Remove them from pinned if present.
            for (int k = 0; k < L->num_pairs; k++) {
                Card c = L->pairs[k].primary;
                for (int q = 0; q < u->pinned_n[p]; q++) {
                    if (card_eq(u->pinned[p][q], c)) {
                        u->pinned[p][q] = u->pinned[p][u->pinned_n[p] - 1];
                        u->pinned_n[p]--;
                        break;
                    }
                }
            }
        }
    }
    // Drop pinned cards for players who are no longer IN (their hand cleared).
    for (int p = 0; p < g->num_players; p++) {
        if (g->players[p].status != PLAYER_STATUS_IN) u->pinned_n[p] = 0;
    }
}

static void old_rb_pool(const Game *g, int bot_idx, OldPool *u) {
    old_rb_pinned(g, bot_idx, u);
    // Known cards = my hand + table battles + discard log + flipped
    //             + all pinned cards (specifically located in some opp's hand).
    Card known[160];
    int kn = 0;
    const Player *bot = &g->players[bot_idx];
    for (int j = 0; j < bot->hand_count; j++) known[kn++] = bot->hand[j];
    for (int i = 0; i < g->num_battles; i++) {
        known[kn++] = g->table_battles[i].attack;
        if (!card_is_none(g->table_battles[i].defense)) known[kn++] = g->table_battles[i].defense;
    }
    if (g->has_flipped) known[kn++] = g->flipped;
    for (int i = 0; i < g->num_logs; i++) {
        if (g->logs[i].log_type == LOG_DISCARD) {
            for (int j = 0; j < g->logs[i].num_pairs; j++) {
                known[kn++] = g->logs[i].pairs[j].primary;
            }
        }
    }
    for (int p = 0; p < g->num_players; p++) {
        for (int j = 0; j < u->pinned_n[p] && kn < (int)(sizeof(known)/sizeof(known[0])); j++) {
            known[kn++] = u->pinned[p][j];
        }
    }

    int start_v = min_value_for(g->num_players);
    u->n = 0;
    for (int suit = 0; suit < 4; suit++) {
        for (int v = start_v; v <= ACE_VALUE; v++) {
            Card c = { (int8_t)suit, (int8_t)v };
            if (!bot_set_contains(known, kn, c)) {
                u->pool[u->n++] = c;
            }
        }
    }
}

static void old_bp_belief(const Game *g, int bot_idx, BotBelief *B) {
    memset(B, 0, sizeof(*B));

    // The flipped trump is the last card ever drawn. If the deck (incl.
    // flipped) is exhausted, the final LOG_DRAW event's player took it.
    int flip_log_idx = -1;
    if (g->deck_count == 0 && !g->has_flipped && 1) {
        for (int i = 0; i < g->num_logs; i++) {
            if (g->logs[i].log_type == LOG_DRAW) flip_log_idx = i;
        }
    }

    // Replayed table state. We rebuild the table from ATTACK/COVER/PASS
    // events rather than trusting LOG_PICKUP/LOG_DISCARD card lists: those
    // two can exceed MAX_LOG_PAIRS (a 10-battle pickup is 20+ cards) and get
    // silently truncated, whereas per-move events carry at most 8 cards.
    Card tbl[80];               // every card currently on the table
    int  tbl_n = 0;
    Card unc[MAX_BATTLES * 2];  // uncovered attack cards only
    int  unc_n = 0;
    Card discards[160];         // accumulated discard pile (complete)
    int  disc_n = 0;

    for (int i = 0; i < g->num_logs; i++) {
        const GameLog *L = &g->logs[i];
        int p = L->player_idx;
        switch (L->log_type) {
            case LOG_ATTACK:
            case LOG_PASS:
                for (int k = 0; k < L->num_pairs; k++) {
                    Card c = L->pairs[k].primary;
                    if (unc_n < (int)(sizeof(unc) / sizeof(unc[0]))) unc[unc_n++] = c;
                    if (tbl_n < (int)(sizeof(tbl) / sizeof(tbl[0]))) tbl[tbl_n++] = c;
                    if (p >= 0 && p != bot_idx) bot_pinned_remove(B, p, c);
                }
                break;
            case LOG_COVER:
                for (int k = 0; k < L->num_pairs; k++) {
                    // primary = cover card (leaves p's hand), target = attack.
                    Card c = L->pairs[k].primary;
                    if (tbl_n < (int)(sizeof(tbl) / sizeof(tbl[0]))) tbl[tbl_n++] = c;
                    if (p >= 0 && p != bot_idx) bot_pinned_remove(B, p, c);
                    if (!card_is_none(L->pairs[k].target)) {
                        for (int q = 0; q < unc_n; q++) {
                            if (card_eq(unc[q], L->pairs[k].target)) {
                                unc[q] = unc[--unc_n];
                                break;
                            }
                        }
                    }
                }
                break;
            case LOG_PICKUP:
                if (p >= 0 && p != bot_idx) {
                    // Exactly one uncovered attack => the defender held no
                    // cover for it. (Multi-attack pickups only prove "no
                    // joint full cover" - too weak to use as a hard
                    // constraint, so we skip those.)
                    if (unc_n == 1 && B->void_n[p] < BOT_MAX_VOIDS) {
                        B->voids[p][B->void_n[p]++] = unc[0];
                    }
                    // The whole replayed table enters p's hand.
                    for (int k = 0; k < tbl_n; k++) bot_pinned_add(B, p, tbl[k]);
                }
                tbl_n = 0;
                unc_n = 0;
                break;
            case LOG_DISCARD:
                for (int k = 0; k < tbl_n && disc_n < 160; k++) discards[disc_n++] = tbl[k];
                tbl_n = 0;
                unc_n = 0;
                break;
            case LOG_DRAW:
                if (p >= 0 && p != bot_idx) {
                    B->void_n[p] = 0;  // new unknown cards: constraints expire
                    if (i == flip_log_idx) bot_pinned_add(B, p, g->flipped);
                }
                break;
            default:
                break;
        }
    }

    // Players that left the game hold nothing.
    for (int p = 0; p < g->num_players; p++) {
        if (p == bot_idx) { B->pinned_n[p] = 0; B->void_n[p] = 0; continue; }
        if (g->players[p].status != PLAYER_STATUS_IN) {
            B->pinned_n[p] = 0;
            B->void_n[p] = 0;
        }
        if (B->pinned_n[p] > g->players[p].hand_count) {
            B->pinned_n[p] = g->players[p].hand_count;  // defensive clamp
        }
    }

    // Unseen pool = full deck minus everything publicly located:
    // my hand, table, discards, the face-up flipped card, pinned cards.
    Card known[160];
    int kn = 0;
    const Player *bot = &g->players[bot_idx];
    for (int j = 0; j < bot->hand_count; j++) known[kn++] = bot->hand[j];
    for (int i = 0; i < g->num_battles; i++) {
        known[kn++] = g->table_battles[i].attack;
        if (!card_is_none(g->table_battles[i].defense)) known[kn++] = g->table_battles[i].defense;
    }
    if (g->has_flipped) known[kn++] = g->flipped;
    for (int i = 0; i < disc_n && kn < 160; i++) known[kn++] = discards[i];
    for (int p = 0; p < g->num_players; p++) {
        for (int j = 0; j < B->pinned_n[p] && kn < 160; j++) {
            known[kn++] = B->pinned[p][j];
        }
    }

    int start_v = min_value_for(g->num_players);
    B->n = 0;
    for (int suit = 0; suit < 4; suit++) {
        for (int v = start_v; v <= ACE_VALUE; v++) {
            Card c = { (int8_t)suit, (int8_t)v };
            if (!bot_set_contains(known, kn, c)) B->pool[B->n++] = c;
        }
    }

    // Sanity: if a constraint leaves fewer allowed pool cards than the
    // player's unknown count, the opponent didn't play "cover-if-you-can"
    // (e.g. the random bot). Drop their constraints rather than skewing
    // every sampled world.
    for (int p = 0; p < g->num_players; p++) {
        if (B->void_n[p] == 0) continue;
        int unknown = g->players[p].hand_count - B->pinned_n[p];
        if (unknown <= 0) continue;
        int allowed = 0;
        for (int i = 0; i < B->n; i++) {
            if (!bot_void_forbidden(B, g, p, B->pool[i])) allowed++;
        }
        if (allowed < unknown) B->void_n[p] = 0;
    }
}

static bool old_cd_drawn_trump(const Game *g, const GameLog *L, Card *out) {
    if (card_in_range(g->flipped, 1, ACE_VALUE)) { *out = g->flipped; return true; }
    for (int k = 0; k < L->num_pairs; k++) {
        if (card_in_range(L->pairs[k].primary, 1, ACE_VALUE)) { *out = L->pairs[k].primary; return true; }
    }
    return false;
}

static void old_cd_belief(const Game *g, int bot_idx, BotBelief *B) {
    memset(B, 0, sizeof(*B));

    // Last draw event: holds the flipped trump if the deck is exhausted, and
    // marks the moment the deck died (for "deck alive at log i" tests).
    int last_draw_idx = -1;
    for (int i = 0; i < g->num_logs; i++) {
        if (g->logs[i].log_type == LOG_DRAW) last_draw_idx = i;
    }
    bool deck_alive_now = (g->deck_count > 0 || g->has_flipped);
    int flip_log_idx = (!deck_alive_now && 1) ? last_draw_idx : -1;

    int trump_viol[MAX_PLAYERS] = {0};
    int in_now = g->num_players;

    // Replayed table state (per-move events only; PICKUP/DISCARD card lists
    // silently truncate at MAX_LOG_PAIRS=16 and must not be trusted).
    Card tbl[80];
    int  tbl_n = 0;
    Card unc[MAX_BATTLES * 2];  // uncovered attack cards only
    int  unc_n = 0;
    Card discards[160];
    int  disc_n = 0;

    for (int i = 0; i < g->num_logs; i++) {
        const GameLog *L = &g->logs[i];
        int p = L->player_idx;
        bool deck_alive_at = deck_alive_now || i <= last_draw_idx;
        switch (L->log_type) {
            case LOG_ATTACK:
            case LOG_PASS: {
                bool first_attack = (L->log_type == LOG_ATTACK && tbl_n == 0);
                bool any_trump = false;
                for (int k = 0; k < L->num_pairs; k++) {
                    Card c = L->pairs[k].primary;
                    if (c.suit == g->power_suit) any_trump = true;
                    if (unc_n < (int)(sizeof(unc) / sizeof(unc[0]))) unc[unc_n++] = c;
                    if (tbl_n < (int)(sizeof(tbl) / sizeof(tbl[0]))) tbl[tbl_n++] = c;
                    if (p >= 0 && p != bot_idx) {
                        bot_floor_check(B, g, p, c);
                        bot_pinned_remove(B, p, c);
                    }
                }
                if (p >= 0 && p != bot_idx && L->log_type == LOG_ATTACK) {
                    if (any_trump && deck_alive_at) trump_viol[p]++;
                    if (first_attack && L->num_pairs == 1 && !any_trump
                        && in_now > 2 && 1) {
                        B->floor_v[p] = L->pairs[0].primary.value;
                    }
                }
                break;
            }
            case LOG_COVER:
                for (int k = 0; k < L->num_pairs; k++) {
                    Card c = L->pairs[k].primary;
                    if (tbl_n < (int)(sizeof(tbl) / sizeof(tbl[0]))) tbl[tbl_n++] = c;
                    if (p >= 0 && p != bot_idx) {
                        bot_floor_check(B, g, p, c);
                        bot_pinned_remove(B, p, c);
                    }
                    if (!card_is_none(L->pairs[k].target)) {
                        for (int q = 0; q < unc_n; q++) {
                            if (card_eq(unc[q], L->pairs[k].target)) {
                                unc[q] = unc[--unc_n];
                                break;
                            }
                        }
                    }
                }
                break;
            case LOG_PICKUP:
                if (p >= 0 && p != bot_idx) {
                    // Exactly one uncovered attack => defender held no cover.
                    if (unc_n == 1 && B->void_n[p] < BOT_MAX_VOIDS) {
                        B->voids[p][B->void_n[p]++] = unc[0];
                    }
                    for (int k = 0; k < tbl_n; k++) bot_pinned_add(B, p, tbl[k]);
                    B->floor_v[p] = 0;   // hand gained cards
                }
                tbl_n = 0;
                unc_n = 0;
                break;
            case LOG_DISCARD:
                for (int k = 0; k < tbl_n && disc_n < 160; k++) discards[disc_n++] = tbl[k];
                tbl_n = 0;
                unc_n = 0;
                break;
            case LOG_DRAW:
                if (p >= 0 && p != bot_idx) {
                    B->void_n[p] = 0;    // new unknown cards: constraints expire
                    B->floor_v[p] = 0;
                    Card trump;
                    if (i == flip_log_idx && old_cd_drawn_trump(g, L, &trump)) bot_pinned_add(B, p, trump);
                }
                break;
            case LOG_PLAYER_OUT:
                in_now--;
                break;
            default:
                break;
        }
    }

    // Behavior-based trust: lowest-first attackers almost never lead trump
    // while the deck is alive, so one strike kills floors. Voids are NOT
    // gated this way - a trump lead can be forced (all-trump hand), and the
    // 1-in-4 unconstrained world mixture already absorbs void violators.
    for (int p = 0; p < g->num_players; p++) {
        if (trump_viol[p] >= 1) { B->distrust_floor[p] = true; B->floor_v[p] = 0; }
    }

    // Players that left the game hold nothing.
    for (int p = 0; p < g->num_players; p++) {
        if (p == bot_idx) { B->pinned_n[p] = 0; B->void_n[p] = 0; B->floor_v[p] = 0; continue; }
        if (g->players[p].status != PLAYER_STATUS_IN) {
            B->pinned_n[p] = 0;
            B->void_n[p] = 0;
            B->floor_v[p] = 0;
        }
        if (B->pinned_n[p] > g->players[p].hand_count) {
            B->pinned_n[p] = g->players[p].hand_count;  // defensive clamp
        }
    }

    // Unseen pool = full deck minus everything publicly located.
    Card known[160];
    int kn = 0;
    const Player *bot = &g->players[bot_idx];
    for (int j = 0; j < bot->hand_count; j++) known[kn++] = bot->hand[j];
    for (int i = 0; i < g->num_battles; i++) {
        known[kn++] = g->table_battles[i].attack;
        if (!card_is_none(g->table_battles[i].defense)) known[kn++] = g->table_battles[i].defense;
    }
    if (g->has_flipped) known[kn++] = g->flipped;
    for (int i = 0; i < disc_n && kn < 160; i++) known[kn++] = discards[i];
    for (int p = 0; p < g->num_players; p++) {
        for (int j = 0; j < B->pinned_n[p] && kn < 160; j++) {
            known[kn++] = B->pinned[p][j];
        }
    }

    int start_v = min_value_for(g->num_players);
    B->n = 0;
    for (int suit = 0; suit < 4; suit++) {
        for (int v = start_v; v <= ACE_VALUE; v++) {
            Card c = { (int8_t)suit, (int8_t)v };
            if (!bot_set_contains(known, kn, c)) B->pool[B->n++] = c;
        }
    }

    // Feasibility: if constraints leave fewer allowed pool cards than the
    // player's unknown count, relax floors first, then voids.
    for (int p = 0; p < g->num_players; p++) {
        if (B->void_n[p] == 0 && B->floor_v[p] == 0) continue;
        int unknown = g->players[p].hand_count - B->pinned_n[p];
        if (unknown <= 0) continue;
        int allowed = 0;
        for (int i = 0; i < B->n; i++) {
            if (!bot_void_forbidden(B, g, p, B->pool[i])
                && !bot_floor_forbidden(B, g, p, B->pool[i])) allowed++;
        }
        if (allowed < unknown && B->floor_v[p] > 0) {
            B->floor_v[p] = 0;
            allowed = 0;
            for (int i = 0; i < B->n; i++) {
                if (!bot_void_forbidden(B, g, p, B->pool[i])) allowed++;
            }
        }
        if (allowed < unknown) B->void_n[p] = 0;
    }
}

static void old_og_belief(const Game *g, int bot_idx, OgBeliefProbe *B) {
    memset(B, 0, sizeof(*B));

    // Last draw event: holds the flipped trump if the deck is exhausted, and
    // marks the moment the deck died (for "deck alive at log i" tests).
    int last_draw_idx = -1;
    for (int i = 0; i < g->num_logs; i++) {
        if (g->logs[i].log_type == LOG_DRAW) last_draw_idx = i;
    }
    bool deck_alive_now = (g->deck_count > 0 || g->has_flipped);
    int flip_log_idx = (!deck_alive_now && 1) ? last_draw_idx : -1;

    int trump_viol[MAX_PLAYERS] = {0};
    int in_now = g->num_players;

    // Declined-attack tell state: per-seat bitmask of non-trump values the
    // seat said GOOD over (while the defender had spare capacity). A
    // handwritten-family attacker NEVER declines a legal non-trump attack, so
    // later playing a non-trump card of a declined value (before gaining
    // cards) proves the seat attacks strategically - an MC bot or a thinking
    // human. Cleared on draw/pickup (the played card might be a new one).
    uint16_t decl_vals[MAX_PLAYERS] = {0};
    int hand_n[MAX_PLAYERS];
    for (int i = 0; i < g->num_players; i++) hand_n[i] = CARDS_PER_PLAYER;
    int cur_def = -1;

    // Replayed table state (per-move events only; PICKUP/DISCARD card lists
    // silently truncate at MAX_LOG_PAIRS=16 and must not be trusted).
    Card tbl[80];
    int  tbl_n = 0;
    Card unc[MAX_BATTLES * 2];  // uncovered attack cards only
    int  unc_n = 0;
    Card discards[160];
    int  disc_n = 0;

    for (int i = 0; i < g->num_logs; i++) {
        const GameLog *L = &g->logs[i];
        int p = L->player_idx;
        bool deck_alive_at = deck_alive_now || i <= last_draw_idx;
        switch (L->log_type) {
            case LOG_ATTACK:
            case LOG_PASS: {
                bool first_attack = (L->log_type == LOG_ATTACK && tbl_n == 0);
                if (p >= 0) hand_n[p] -= L->num_pairs;
                if (first_attack && cur_def < 0 && p >= 0) {
                    cur_def = (p + 1) % g->num_players;   // pre-change rounds
                }
                bool any_trump = false;
                for (int k = 0; k < L->num_pairs; k++) {
                    Card c = L->pairs[k].primary;
                    if (c.suit == g->power_suit) any_trump = true;
                    if (unc_n < (int)(sizeof(unc) / sizeof(unc[0]))) unc[unc_n++] = c;
                    if (tbl_n < (int)(sizeof(tbl) / sizeof(tbl[0]))) tbl[tbl_n++] = c;
                    if (p >= 0 && p != bot_idx) {
                        if (deck_alive_at) {
                            B->cards_played[p]++;
                            if (c.suit == g->power_suit) B->trumps_played[p]++;
                        }
                        // Playing a card a still-active void says they lacked
                        // (and that wasn't publicly picked up) proves the void
                        // wrong: the seat picked up strategically while
                        // holding cover. Handwritten-family never does that.
                        if (!bot_set_contains(B->b.pinned[p], B->b.pinned_n[p], c)
                            && bot_void_forbidden(&B->b, g, p, c)) B->mc_tell[p] = true;
                        if (c.suit != g->power_suit
                            && (decl_vals[p] & (1u << c.value))) B->mc_tell[p] = true;
                        bot_floor_check(&B->b, g, p, c);
                        bot_pinned_remove(&B->b, p, c);
                    }
                }
                if (p >= 0 && p != bot_idx && L->log_type == LOG_ATTACK) {
                    if (any_trump && deck_alive_at) trump_viol[p]++;
                    if (first_attack && L->num_pairs == 1 && !any_trump
                        && in_now > 2 && 1) {
                        B->b.floor_v[p] = L->pairs[0].primary.value;
                    }
                }
                break;
            }
            case LOG_COVER:
                if (p >= 0) hand_n[p] -= L->num_pairs;
                for (int k = 0; k < L->num_pairs; k++) {
                    Card c = L->pairs[k].primary;
                    if (tbl_n < (int)(sizeof(tbl) / sizeof(tbl[0]))) tbl[tbl_n++] = c;
                    if (p >= 0 && p != bot_idx) {
                        if (deck_alive_at) {
                            B->cards_played[p]++;
                            if (c.suit == g->power_suit) B->trumps_played[p]++;
                        }
                        if (!bot_set_contains(B->b.pinned[p], B->b.pinned_n[p], c)
                            && bot_void_forbidden(&B->b, g, p, c)) B->mc_tell[p] = true;
                        if (c.suit != g->power_suit
                            && (decl_vals[p] & (1u << c.value))) B->mc_tell[p] = true;
                        bot_floor_check(&B->b, g, p, c);
                        bot_pinned_remove(&B->b, p, c);
                    }
                    if (!card_is_none(L->pairs[k].target)) {
                        for (int q = 0; q < unc_n; q++) {
                            if (card_eq(unc[q], L->pairs[k].target)) {
                                unc[q] = unc[--unc_n];
                                break;
                            }
                        }
                    }
                }
                break;
            case LOG_GOOD:
                if (p >= 0 && p != bot_idx && in_now > 2 && cur_def >= 0
                    && hand_n[cur_def] - unc_n >= 1) {
                    for (int k = 0; k < tbl_n; k++) {
                        if (tbl[k].suit != g->power_suit)
                            decl_vals[p] |= (uint16_t)(1u << tbl[k].value);
                    }
                }
                break;
            case LOG_DEFENDER_CHANGE:
                cur_def = L->defender_index;
                break;
            case LOG_PICKUP:
                if (p >= 0) { hand_n[p] += tbl_n; decl_vals[p] = 0; }
                if (p >= 0 && p != bot_idx) {
                    // Exactly one uncovered attack => defender held no cover.
                    if (unc_n == 1 && B->b.void_n[p] < BOT_MAX_VOIDS) {
                        B->b.voids[p][B->b.void_n[p]++] = unc[0];
                    }
                    for (int k = 0; k < tbl_n; k++) bot_pinned_add(&B->b, p, tbl[k]);
                    B->b.floor_v[p] = 0;   // hand gained cards
                }
                tbl_n = 0;
                unc_n = 0;
                break;
            case LOG_DISCARD:
                for (int k = 0; k < tbl_n && disc_n < 160; k++) discards[disc_n++] = tbl[k];
                tbl_n = 0;
                unc_n = 0;
                break;
            case LOG_DRAW:
                if (p >= 0) { hand_n[p] += L->num_pairs; decl_vals[p] = 0; }
                if (p >= 0 && p != bot_idx) {
                    B->b.void_n[p] = 0;    // new unknown cards: constraints expire
                    B->b.floor_v[p] = 0;
                    if (i == flip_log_idx) {
                        // Deck just ran dry: this seat picked up the face-up
                        // trump, watched all game, so pin it. Its identity is the
                        // LAST card of this record (drawn once the deck emptied)
                        // and is the one REVEALED draw in the masked belief stream
                        // (other draws arrive {-1,-1}). Read it from the LOG, not
                        // g->flipped: the wire serializer zeroes g->flipped to
                        // {0,0} once has_flipped goes false (view.c), so the
                        // resident kernel state no longer carries the real card -
                        // pinning g->flipped there phantom-pins {0,0}.
                        if (L->num_pairs > 0) {
                            Card fc = L->pairs[L->num_pairs - 1].primary;
                            if (fc.value >= 0 && fc.suit >= 0) bot_pinned_add(&B->b, p, fc);
                        }
                    }
                }
                break;
            case LOG_PLAYER_OUT:
                in_now--;
                break;
            default:
                break;
        }
    }

    // Behavior-based trust: lowest-first attackers almost never lead trump
    // while the deck is alive, so one strike kills floors. Voids are NOT
    // gated this way - a trump lead can be forced (all-trump hand), and the
    // 1-in-4 unconstrained world mixture already absorbs void violators.
    for (int p = 0; p < g->num_players; p++) {
        B->trump_leads[p] = trump_viol[p];
        if (trump_viol[p] >= 1) { B->b.distrust_floor[p] = true; B->b.floor_v[p] = 0; }
    }

    // Semtex adaptivity: a proven void contradiction means the seat picks up
    // strategically while holding cover - an MC bot (cordite family) or a
    // thinking human. Both floor and void inference model heuristic-family
    // opponents and are wrong for such seats; drop them for the rest of the
    // game (no draw-expiry reset - the tell is about the PLAYER, not the hand).
    if (1) {
        for (int p = 0; p < g->num_players; p++) {
            if (!B->mc_tell[p]) continue;
            B->b.void_n[p] = 0;
            B->b.floor_v[p] = 0;
            B->b.distrust_void[p] = true;
            B->b.distrust_floor[p] = true;
        }
    }

    // Semtex weak-seat profile (fulminate's lever, conservatively gated): a
    // seat that burns trumps while the deck is alive at a rate no strong or
    // heuristic bot exhibits is rolled out with the LOOSE model instead of
    // handwritten. Evidence: deck-alive trump share ramped over [0.40, 0.60]
    // (needs >= 14 observed cards), plus repeated deck-alive trump attacks.
    if (old_og_profile) {
        for (int p = 0; p < g->num_players; p++) {
            if (p == bot_idx) continue;
            double conf = 0.0;
            if (B->cards_played[p] >= 14) {
                double r = (double)B->trumps_played[p] / (double)B->cards_played[p];
                double ramp = (r - 0.40) / 0.20;
                if (ramp < 0) ramp = 0;
                if (ramp > 1) ramp = 1;
                conf += ramp;
            }
            int leads = B->trump_leads[p] > 3 ? 3 : B->trump_leads[p];
            conf += 0.25 * leads;
            if (conf >= 0.70) {
                B->loose[p] = true;
                B->b.void_n[p] = 0;    // weak seats don't obey cover-if-you-can
                B->b.floor_v[p] = 0;   // ...or lowest-first attacks
            }
        }
    }

    // Players that left the game hold nothing.
    for (int p = 0; p < g->num_players; p++) {
        if (p == bot_idx) { B->b.pinned_n[p] = 0; B->b.void_n[p] = 0; B->b.floor_v[p] = 0; continue; }
        if (g->players[p].status != PLAYER_STATUS_IN) {
            B->b.pinned_n[p] = 0;
            B->b.void_n[p] = 0;
            B->b.floor_v[p] = 0;
        }
        if (B->b.pinned_n[p] > g->players[p].hand_count) {
            B->b.pinned_n[p] = g->players[p].hand_count;  // defensive clamp
        }
    }

    // Unseen pool = full deck minus everything publicly located.
    Card known[160];
    int kn = 0;
    const Player *bot = &g->players[bot_idx];
    for (int j = 0; j < bot->hand_count; j++) known[kn++] = bot->hand[j];
    for (int i = 0; i < g->num_battles; i++) {
        known[kn++] = g->table_battles[i].attack;
        if (!card_is_none(g->table_battles[i].defense)) known[kn++] = g->table_battles[i].defense;
    }
    if (g->has_flipped) known[kn++] = g->flipped;
    for (int i = 0; i < disc_n && kn < 160; i++) known[kn++] = discards[i];
    for (int p = 0; p < g->num_players; p++) {
        for (int j = 0; j < B->b.pinned_n[p] && kn < 160; j++) {
            known[kn++] = B->b.pinned[p][j];
        }
    }

    int start_v = min_value_for(g->num_players);
    B->b.n = 0;
    for (int suit = 0; suit < 4; suit++) {
        for (int v = start_v; v <= ACE_VALUE; v++) {
            Card c = { (int8_t)suit, (int8_t)v };
            if (!bot_set_contains(known, kn, c)) B->b.pool[B->b.n++] = c;
        }
    }

    // Every hidden slot a world deals must get a pool card: og_sample_world
    // leaves a slot it cannot fill holding the board's own card, and on a
    // masked board (the Oracle's) that is card.h's CARD_HIDDEN, which has no
    // card id for the rollouts' bitboards. A pool shorter than the slots means
    // the log located cards the board does not agree with, so drop what the log
    // located and keep what the board shows; game_validate's masked bound
    // (hidden cards never outnumber what the deck has left) makes that pool
    // cover every slot. A consistent log never gets here: one test per decision.
    {
        int slots = g->deck_count;
        for (int p = 0; p < g->num_players; p++)
            if (p != bot_idx) slots += g->players[p].hand_count - B->b.pinned_n[p];
        if (B->b.n < slots) {
            int board_kn = bot->hand_count + g->num_battles + (g->has_flipped ? 1 : 0);
            for (int i = 0; i < g->num_battles; i++)
                board_kn += !card_is_none(g->table_battles[i].defense);
            for (int p = 0; p < g->num_players; p++) B->b.pinned_n[p] = 0;
            B->b.n = 0;
            for (int suit = 0; suit < 4; suit++) {
                for (int v = start_v; v <= ACE_VALUE; v++) {
                    Card c = { (int8_t)suit, (int8_t)v };
                    if (!bot_set_contains(known, board_kn, c)) B->b.pool[B->b.n++] = c;
                }
            }
        }
    }

    // Feasibility: if constraints leave fewer allowed pool cards than the
    // player's unknown count, relax floors first, then voids.
    for (int p = 0; p < g->num_players; p++) {
        if (B->b.void_n[p] == 0 && B->b.floor_v[p] == 0) continue;
        int unknown = g->players[p].hand_count - B->b.pinned_n[p];
        if (unknown <= 0) continue;
        int allowed = 0;
        for (int i = 0; i < B->b.n; i++) {
            if (!bot_void_forbidden(&B->b, g, p, B->b.pool[i])
                && !bot_floor_forbidden(&B->b, g, p, B->b.pool[i])) allowed++;
        }
        if (allowed < unknown && B->b.floor_v[p] > 0) {
            B->b.floor_v[p] = 0;
            allowed = 0;
            for (int i = 0; i < B->b.n; i++) {
                if (!bot_void_forbidden(&B->b, g, p, B->b.pool[i])) allowed++;
            }
        }
        if (allowed < unknown) B->b.void_n[p] = 0;
    }
}
