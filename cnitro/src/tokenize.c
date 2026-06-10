// Tokenizer port of nitro_nn.ts `tokenize`. The order of section emission and
// the per-section sort (by rotated suit, then value) must match TS exactly,
// otherwise the trained weights become useless.

#include "tokenize.h"
#include <string.h>
#include <stdlib.h>

// Card encoding: 13 values (1..A) × 4 suits, ordered by trump-relative
// rotation. Values 1..4 only appear in 6+ player games (full deck); 2..5
// player games leave those slots unused. Same encoding handles both.
int card_token_id(int suit, int value, int trump_suit) {
    int rot = (suit - trump_suit + 4) % 4;
    int v = value - MIN_VALUE_LARGE;     // 1..A → 0..12
    if (v < 0) v = 0;
    if (v > 12) v = 12;
    return TOK_CARD_BASE + rot * 13 + v;
}

int card_action_id(int suit, int value, int trump_suit) {
    int rot = (suit - trump_suit + 4) % 4;
    int v = value - MIN_VALUE_LARGE;
    if (v < 0) v = 0;
    if (v > 12) v = 12;
    return rot * 13 + v;
}

void action_id_to_card(int id, int trump_suit, Card *out) {
    int rot = id / 13;
    int value = MIN_VALUE_LARGE + (id % 13);
    int suit = (rot + trump_suit) % 4;
    out->suit = (int8_t)suit;
    out->value = (int8_t)value;
}

int opponent_seat(const Game *g, int bot_idx, int opp_idx) {
    if (opp_idx < 0 || opp_idx == bot_idx) return 0;
    if (g->players[opp_idx].status == PLAYER_STATUS_OUT) return 0;
    int seat = 0;
    for (int k = 1; k < g->num_players; k++) {
        int cur = (bot_idx + k) % g->num_players;
        if (g->players[cur].status == PLAYER_STATUS_OUT) continue;
        seat++;
        if (cur == opp_idx) return seat <= MAX_OPPONENTS ? seat : 0;
    }
    return 0;
}

// Coarse size bucket reused for deck length, discard-pile length, and per-opp
// hand_count. The thresholds are deck-tuned (deck starts at 23 in 2p) but
// give the model usable signal across all three contexts; the section header
// disambiguates what's being measured.
static int size_bucket(int n) {
    if (n <= 0) return TOK_SIZE_EMPTY;
    if (n < 8)  return TOK_SIZE_LOW;
    if (n < 18) return TOK_SIZE_MED;
    return TOK_SIZE_FULL;
}

static int seat_token(int seat) {
    // seat is 1..MAX_OPPONENTS; map to TOK_OPP_SEAT_1..7. Out-of-range
    // (shouldn't happen with current MAX_PLAYERS but defends 3+ player ports)
    // collapses to the last seat token rather than emitting a stray id.
    if (seat < 1) seat = 1;
    if (seat > MAX_OPPONENTS) seat = MAX_OPPONENTS;
    return TOK_OPP_SEAT_1 + (seat - 1);
}

static int s_trump;
static int sort_by_rank(const void *a, const void *b) {
    const Card *ca = a; const Card *cb = b;
    int ar = (ca->suit - s_trump + 4) % 4;
    int br = (cb->suit - s_trump + 4) % 4;
    if (ar != br) return ar - br;
    return ca->value - cb->value;
}

static void push_tok(Tokenized *o, int t) {
    if (o->n_tokens < MAX_SEQ_LEN) o->tokens[o->n_tokens++] = t;
}

void tokenize(const Game *g, int bot_idx, const InProgress *ip, Tokenized *out) {
    int trump = g->power_suit;
    s_trump = trump;
    const Player *me = &g->players[bot_idx];
    bool is_def = (bot_idx == g->defender);
    bool is_first_attack = (g->num_battles == 0);

    out->n_tokens = 0;
    push_tok(out, TOK_CLS);

    // -- History (filtered to move logs, latest MAX_HISTORY_EVENTS) ----
    int move_log_idx[MAX_LOGS]; int n_logs = 0;
    for (int i = 0; i < g->num_logs; i++) {
        int t = g->logs[i].log_type;
        if (t == LOG_ATTACK || t == LOG_COVER || t == LOG_PASS
            || t == LOG_PICKUP || t == LOG_GOOD
            || t == LOG_DISCARD || t == LOG_DRAW
            || t == LOG_PLAYER_OUT) {
            move_log_idx[n_logs++] = i;
        }
    }
    int start = n_logs > MAX_HISTORY_EVENTS ? n_logs - MAX_HISTORY_EVENTS : 0;
    if (n_logs > start) {
        push_tok(out, TOK_SEC_HISTORY);
        for (int li = start; li < n_logs; li++) {
            const GameLog *log = &g->logs[move_log_idx[li]];
            if (log->player_idx >= 0) {
                if (log->player_idx == bot_idx) {
                    push_tok(out, TOK_PLAYER_SELF);
                } else {
                    int seat = opponent_seat(g, bot_idx, log->player_idx);
                    // Eliminated mid-game: status is OUT now but they DID play
                    // this move when active. Use the seat they would have had
                    // had they still been in (next-after-self in ring order).
                    if (seat == 0) {
                        int hop = 0;
                        for (int k = 1; k < g->num_players; k++) {
                            int cur = (bot_idx + k) % g->num_players;
                            hop++;
                            if (cur == log->player_idx) { seat = hop; break; }
                        }
                    }
                    push_tok(out, seat_token(seat));
                }
            }
            int mt;
            switch (log->log_type) {
                case LOG_ATTACK:     mt = TOK_MOVE_ATTACK; break;
                case LOG_COVER:      mt = TOK_MOVE_COVER; break;
                case LOG_PASS:       mt = TOK_MOVE_PASS; break;
                case LOG_PICKUP:     mt = TOK_MOVE_PICKUP; break;
                case LOG_GOOD:       mt = TOK_MOVE_GOOD; break;
                case LOG_DISCARD:    mt = TOK_MOVE_DISCARD; break;
                case LOG_DRAW:       mt = TOK_MOVE_DRAW; break;
                case LOG_PLAYER_OUT: mt = TOK_MOVE_OUT; break;
                default: continue;
            }
            push_tok(out, mt);
            for (int p = 0; p < log->num_pairs; p++) {
                Card primary = log->pairs[p].primary;
                if (primary.suit >= 0) push_tok(out, card_token_id(primary.suit, primary.value, trump));
                if (log->pairs[p].has_target) {
                    Card tgt = log->pairs[p].target;
                    if (tgt.suit >= 0) {
                        push_tok(out, TOK_COVER_TARGET);
                        push_tok(out, card_token_id(tgt.suit, tgt.value, trump));
                    }
                }
            }
            if (out->n_tokens >= MAX_SEQ_LEN - 30) break;
        }
    }

    // -- Role + deck bucket --------------------------------------------
    if (is_first_attack && !is_def) push_tok(out, TOK_ROLE_FIRST);
    else if (is_def) push_tok(out, TOK_ROLE_DEF);
    else push_tok(out, TOK_ROLE_ATK);

    int deck_left = g->deck_count + (g->has_flipped ? 1 : 0);
    push_tok(out, size_bucket(deck_left));

    // -- Flipped trump card (visible to all players) -------------------
    if (g->has_flipped && g->flipped.suit >= 0) {
        push_tok(out, TOK_SEC_FLIPPED);
        push_tok(out, card_token_id(g->flipped.suit, g->flipped.value, trump));
    }

    // -- Hand (minus cards already chosen this turn) -------------------
    Card live[MAX_HAND_SIZE]; int ln = 0;
    for (int i = 0; i < me->hand_count; i++) {
        bool chosen = false;
        for (int j = 0; j < ip->n_cards_chosen; j++) {
            if (card_eq(me->hand[i], ip->cards_chosen[j])) { chosen = true; break; }
        }
        if (!chosen) live[ln++] = me->hand[i];
    }
    qsort(live, ln, sizeof(Card), sort_by_rank);
    push_tok(out, TOK_SEC_HAND);
    for (int i = 0; i < ln; i++) push_tok(out, card_token_id(live[i].suit, live[i].value, trump));

    // -- Per-opponent hand-size summary --------------------------------
    // No card identities — just a coarse size bucket per active opponent,
    // emitted in seat order. Walk the ring and skip OUT players; this gives
    // the same seat numbering used in history attribution (TOK_OPP_SEAT_k).
    bool any_opp = false;
    for (int k = 1; k < g->num_players; k++) {
        int cur = (bot_idx + k) % g->num_players;
        if (g->players[cur].status == PLAYER_STATUS_OUT) continue;
        if (!any_opp) { push_tok(out, TOK_SEC_OPP_SIZES); any_opp = true; }
        int seat = opponent_seat(g, bot_idx, cur);
        if (seat <= 0) continue;
        push_tok(out, seat_token(seat));
        push_tok(out, size_bucket(g->players[cur].hand_count));
    }

    // -- Discard pile length only (cards themselves are hidden) --------
    int discard_len = g->discard_pile_length;
    if (discard_len > 0) {
        push_tok(out, TOK_SEC_DISCARD_LEN);
        push_tok(out, size_bucket(discard_len));
    }

    // -- Table battles -------------------------------------------------
    if (g->num_battles > 0) {
        push_tok(out, TOK_SEC_TABLE);
        for (int i = 0; i < g->num_battles; i++) {
            if (i > 0) push_tok(out, TOK_BATTLE_NEXT);
            const Battle *b = &g->table_battles[i];
            push_tok(out, card_token_id(b->attack.suit, b->attack.value, trump));
            if (b->has_defense) {
                push_tok(out, TOK_BATTLE_COVER);
                push_tok(out, card_token_id(b->defense.suit, b->defense.value, trump));
            }
        }
    }

    // -- In-progress chosen cards --------------------------------------
    if (ip->n_cards_chosen > 0) {
        Card prog[8]; int pn = ip->n_cards_chosen;
        for (int i = 0; i < pn; i++) prog[i] = ip->cards_chosen[i];
        qsort(prog, pn, sizeof(Card), sort_by_rank);
        push_tok(out, TOK_SEC_PROGRESS);
        for (int i = 0; i < pn; i++) push_tok(out, card_token_id(prog[i].suit, prog[i].value, trump));
    }
}
