// Tokenizer port of nitro_nn.ts `tokenize`. The order of section emission and
// the per-section sort (by rotated suit, then value) must match TS exactly,
// otherwise the trained weights become useless.

#include "tokenize.h"
#include <string.h>
#include <stdlib.h>

int card_token_id(int suit, int value, int trump_suit) {
    int rot = (suit - trump_suit + 4) % 4;
    int v = value - 5;
    if (v < 0) v = 0;
    if (v > 9) v = 9;
    return TOK_CARD_BASE + rot * 10 + v;
}

int card_action_id(int suit, int value, int trump_suit) {
    int rot = (suit - trump_suit + 4) % 4;
    int v = value - 5;
    if (v < 0) v = 0;
    if (v > 9) v = 9;
    return rot * 10 + v;
}

void action_id_to_card(int id, int trump_suit, Card *out) {
    int rot = id / 10;
    int value = 5 + (id % 10);
    int suit = (rot + trump_suit) % 4;
    out->suit = (int8_t)suit;
    out->value = (int8_t)value;
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
    int opp_idx = -1;
    for (int i = 0; i < g->num_players; i++) {
        if (i != bot_idx && g->players[i].status == PLAYER_STATUS_IN) { opp_idx = i; break; }
    }
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
            || t == LOG_DISCARD || t == LOG_DRAW) {
            move_log_idx[n_logs++] = i;
        }
    }
    int start = n_logs > MAX_HISTORY_EVENTS ? n_logs - MAX_HISTORY_EVENTS : 0;
    if (n_logs > start) {
        push_tok(out, TOK_SEC_HISTORY);
        for (int li = start; li < n_logs; li++) {
            const GameLog *log = &g->logs[move_log_idx[li]];
            if (log->player_idx >= 0) {
                push_tok(out, log->player_idx == bot_idx ? TOK_PLAYER_SELF : TOK_PLAYER_OPP);
            }
            int mt;
            switch (log->log_type) {
                case LOG_ATTACK:  mt = TOK_MOVE_ATTACK; break;
                case LOG_COVER:   mt = TOK_MOVE_COVER; break;
                case LOG_PASS:    mt = TOK_MOVE_PASS; break;
                case LOG_PICKUP:  mt = TOK_MOVE_PICKUP; break;
                case LOG_GOOD:    mt = TOK_MOVE_GOOD; break;
                case LOG_DISCARD: mt = TOK_MOVE_DISCARD; break;
                case LOG_DRAW:    mt = TOK_MOVE_DRAW; break;
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
    if (deck_left >= 18) push_tok(out, TOK_DECK_FULL);
    else if (deck_left >= 8) push_tok(out, TOK_DECK_MED);
    else if (deck_left >= 1) push_tok(out, TOK_DECK_LOW);
    else push_tok(out, TOK_DECK_EMPTY);

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

    if (opp_idx >= 0) {
        Card opp[MAX_HAND_SIZE]; int on = g->players[opp_idx].hand_count;
        for (int i = 0; i < on; i++) opp[i] = g->players[opp_idx].hand[i];
        qsort(opp, on, sizeof(Card), sort_by_rank);
        push_tok(out, TOK_SEC_OPP);
        for (int i = 0; i < on; i++) push_tok(out, card_token_id(opp[i].suit, opp[i].value, trump));
    }

    // -- Discard memory (from logs) ------------------------------------
    Card seen[160]; int sn = 0;
    for (int i = 0; i < g->num_logs; i++) {
        if (g->logs[i].log_type != LOG_DISCARD) continue;
        for (int p = 0; p < g->logs[i].num_pairs; p++) {
            Card c = g->logs[i].pairs[p].primary;
            bool dup = false;
            for (int j = 0; j < sn; j++) if (card_eq(seen[j], c)) { dup = true; break; }
            if (!dup) seen[sn++] = c;
        }
    }
    if (sn > 0) {
        qsort(seen, sn, sizeof(Card), sort_by_rank);
        push_tok(out, TOK_SEC_DISCARD);
        for (int i = 0; i < sn; i++) push_tok(out, card_token_id(seen[i].suit, seen[i].value, trump));
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
