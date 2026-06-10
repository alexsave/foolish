// Encoders for the GRPO policy network. See grpo_encode.h for layout.

#include "grpo_encode.h"
#include <string.h>

// --- public-knowledge derivations from game logs ---------------------------
//
// The engine stores `discard_pile_length` as a count, not the cards, and
// it doesn't track per-player taken cards. Both are derivable from the
// append-only log:
//   * Discard pile = union of cards in LOG_DISCARD events (cards leaving
//     the table when a defense was completed or a defender picked up).
//   * Cards we've publicly seen go into opponent X's hand still there =
//     (cards X picked up via LOG_PICKUP) minus (cards X subsequently
//     played via LOG_ATTACK / LOG_COVER / LOG_PASS).
// LOG_DRAW reveals the cards drawn but is hidden information to other
// players — never used here.

static void compute_discard_set(const Game *g, bool out[MAX_DECK_INDEX]) {
    memset(out, 0, MAX_DECK_INDEX * sizeof(bool));
    for (int i = 0; i < g->num_logs; i++) {
        const GameLog *l = &g->logs[i];
        if (l->log_type != LOG_DISCARD) continue;
        for (int j = 0; j < l->num_pairs; j++) {
            out[CARD_IDX(l->pairs[j].primary)] = true;
        }
    }
}

static void compute_opp_publicly_held(const Game *g, int opp_idx,
                                      bool out[MAX_DECK_INDEX]) {
    memset(out, 0, MAX_DECK_INDEX * sizeof(bool));
    for (int i = 0; i < g->num_logs; i++) {
        const GameLog *l = &g->logs[i];
        if (l->player_idx != opp_idx) continue;
        switch (l->log_type) {
            case LOG_PICKUP:
                for (int j = 0; j < l->num_pairs; j++) {
                    out[CARD_IDX(l->pairs[j].primary)] = true;
                    if (l->pairs[j].has_target) {
                        out[CARD_IDX(l->pairs[j].target)] = true;
                    }
                }
                break;
            case LOG_ATTACK:
            case LOG_COVER:
            case LOG_PASS:
                for (int j = 0; j < l->num_pairs; j++) {
                    out[CARD_IDX(l->pairs[j].primary)] = false;
                }
                break;
            default:
                break;
        }
    }
}

// --- role derivation -------------------------------------------------------

static int derive_role(const Game *g, int player_idx) {
    const Player *p = &g->players[player_idx];
    if (player_idx == g->defender) return ROLE_DEFENDER;
    if (player_idx == g->first_attacker) return ROLE_ATTACKER;
    if (p->status == PLAYER_STATUS_OUT) return ROLE_IDLE;
    if (p->awaiting_attack) return ROLE_CO_ATTACKER;
    return ROLE_IDLE;
}

// Walk seats clockwise from self_idx (exclusive) up to MAX_OPPONENTS.
// IMPORTANT: OUT players are NOT skipped — they keep their physical seat
// slot so the model can attribute past public actions (cards taken, cards
// played) to a stable opponent identity across eliminations. The slot's
// `still_in_game` flag distinguishes IN from OUT.
static int walk_opponents(const Game *g, int self_idx, int out_seats[MAX_OPPONENTS]) {
    int n = 0;
    for (int step = 1; step < g->num_players && n < MAX_OPPONENTS; step++) {
        int seat = (self_idx + step) % g->num_players;
        if (seat == self_idx) break;
        out_seats[n++] = seat;
    }
    return n;
}

// --- state encoder ---------------------------------------------------------

void grpo_encode_state(const Game *g, int self_idx, float *out) {
    memset(out, 0, STATE_DIM * sizeof(float));
    float *p = out;
    const Player *self = &g->players[self_idx];

    // own_hand (52)
    for (int i = 0; i < self->hand_count; i++) {
        p[CARD_IDX(self->hand[i])] = 1.0f;
    }
    p += STATE_OWN_HAND_DIM;

    // trump_suit (4)
    if (g->power_suit >= 0 && g->power_suit < NUM_SUITS) {
        p[g->power_suit] = 1.0f;
    }
    p += STATE_TRUMP_SUIT_DIM;

    // trump_card identity (52) — only revealed while it's still on the
    // table as the flipped bottom card. After someone draws it, the
    // suit is still known via power_suit but the specific rank is not.
    if (g->has_flipped) {
        p[CARD_IDX(g->flipped)] = 1.0f;
    }
    p += STATE_TRUMP_CARD_DIM;

    // discard (52)
    {
        bool disc[MAX_DECK_INDEX];
        compute_discard_set(g, disc);
        for (int i = 0; i < MAX_DECK_INDEX; i++) if (disc[i]) p[i] = 1.0f;
    }
    p += STATE_DISCARD_DIM;

    // table attacks (52) + table defenses (52)
    float *attacks_slot  = p;
    float *defenses_slot = p + STATE_ATTACKS_DIM;
    for (int i = 0; i < g->num_battles; i++) {
        const Battle *b = &g->table_battles[i];
        attacks_slot[CARD_IDX(b->attack)] = 1.0f;
        if (b->has_defense) defenses_slot[CARD_IDX(b->defense)] = 1.0f;
    }
    p += STATE_ATTACKS_DIM + STATE_DEFENSES_DIM;

    // per-opponent block (7 * OPP_FEAT_DIM) — walks physical seats, OUT
    // players preserved with still_in=0.
    int seats[MAX_OPPONENTS];
    int n_opp = walk_opponents(g, self_idx, seats);
    int n_live = 0;
    for (int s = 0; s < n_opp; s++) {
        int opp_idx = seats[s];
        const Player *opp = &g->players[opp_idx];
        bool is_in = (opp->status != PLAYER_STATUS_OUT);
        if (is_in) n_live++;
        float *slot = p + s * OPP_FEAT_DIM;

        // hand_size (1) — normalize by a generous constant.
        slot[0] = (float)opp->hand_count / 16.0f;

        // role one-hot (4) — derive_role returns IDLE for OUT players,
        // which together with still_in=0 fully tags the slot.
        int r = derive_role(g, opp_idx);
        if (r >= 0 && r < N_ROLES) slot[OPP_HANDSIZE_DIM + r] = 1.0f;

        // still_in_game (1)
        slot[OPP_HANDSIZE_DIM + OPP_ROLE_DIM] = is_in ? 1.0f : 0.0f;

        // publicly-held subset (52). For OUT players this is almost always
        // empty (any cards they held were played and discarded), but the
        // computation is the same and we let the data speak.
        bool held[MAX_DECK_INDEX];
        compute_opp_publicly_held(g, opp_idx, held);
        float *taken_slot = slot + OPP_HANDSIZE_DIM + OPP_ROLE_DIM + OPP_STILL_IN_DIM;
        for (int i = 0; i < MAX_DECK_INDEX; i++) if (held[i]) taken_slot[i] = 1.0f;
    }
    p += STATE_OPPS_DIM;

    // num_live_opps (1) — count of still-IN opponents only.
    p[0] = (float)n_live / (float)MAX_OPPONENTS;
    p += STATE_NUM_LIVE_OPPS_DIM;

    // self_role (4)
    {
        int r = derive_role(g, self_idx);
        if (r >= 0 && r < N_ROLES) p[r] = 1.0f;
    }
    p += STATE_SELF_ROLE_DIM;

    // deck_remaining (1) — normalized by max deck size; flipped card
    // counts as in-deck for this signal.
    p[0] = ((float)g->deck_count + (g->has_flipped ? 1.0f : 0.0f)) / 52.0f;
    p += STATE_DECK_REMAINING_DIM;

    // player_count one-hot over {2..8} (7)
    if (g->num_players >= 2 && g->num_players <= 8) {
        p[g->num_players - 2] = 1.0f;
    }
    p += STATE_PLAYER_COUNT_DIM;

    // hidden_trumps_count (1): trumps we haven't observed anywhere public.
    //   total trumps in deck variant
    //     = 9 for 36-card (2-5p, values 5..13)
    //     = 13 for 52-card (6-8p, values 1..13)
    //   subtract observed trumps from: our hand, discard, opp_held bitsets,
    //   trump cards on the table (already in attacks/defenses), and the
    //   flipped trump card (visible to all if has_flipped).
    {
        int trump_suit = g->power_suit;
        int total_trumps = (g->num_players >= 6) ? 13 : 9;
        int observed = 0;
        // Own hand.
        for (int i = 0; i < self->hand_count; i++) {
            if (self->hand[i].suit == trump_suit) observed++;
        }
        // Discard (recompute from logs — cheap).
        {
            bool disc[MAX_DECK_INDEX];
            compute_discard_set(g, disc);
            for (int v = 1; v <= 13; v++) {
                int idx = (v - 1) * 4 + trump_suit;
                if (disc[idx]) observed++;
            }
        }
        // Per-opp public holdings.
        for (int s = 0; s < g->num_players; s++) {
            if (s == self_idx) continue;
            bool held[MAX_DECK_INDEX];
            compute_opp_publicly_held(g, s, held);
            for (int v = 1; v <= 13; v++) {
                int idx = (v - 1) * 4 + trump_suit;
                if (held[idx]) observed++;
            }
        }
        // Cards currently on the table that are trumps.
        for (int i = 0; i < g->num_battles; i++) {
            if (g->table_battles[i].attack.suit == trump_suit) observed++;
            if (g->table_battles[i].has_defense
                && g->table_battles[i].defense.suit == trump_suit) observed++;
        }
        // Flipped bottom trump if still there.
        if (g->has_flipped && g->flipped.suit == trump_suit) observed++;
        int hidden = total_trumps - observed;
        if (hidden < 0) hidden = 0;   // bookkeeping safety
        p[0] = (float)hidden / 13.0f;
    }
    p += STATE_HIDDEN_TRUMPS_DIM;

    // round_phase one-hot (3): early/mid/late based on deck remaining.
    {
        int dc = g->deck_count + (g->has_flipped ? 1 : 0);
        int phase = (dc > 16) ? 0 : (dc > 1 ? 1 : 2);
        p[phase] = 1.0f;
    }
    p += STATE_ROUND_PHASE_DIM;

    // distance_to_defender one-hot (8): clockwise hops from self to the
    // defender, skipping OUT players. 0 = self IS defender, 1 = self is
    // immediately before defender (so I'm next attacker), etc.
    {
        int dist = 0;
        int seat = self_idx;
        int hops = 0;
        if (seat == g->defender) dist = 0;
        else {
            for (int step = 1; step <= g->num_players; step++) {
                seat = (seat + 1) % g->num_players;
                if (g->players[seat].status == PLAYER_STATUS_OUT) continue;
                hops++;
                if (seat == g->defender) { dist = hops; break; }
            }
        }
        if (dist < 0) dist = 0;
        if (dist >= STATE_DIST_TO_DEF_DIM) dist = STATE_DIST_TO_DEF_DIM - 1;
        p[dist] = 1.0f;
    }
    p += STATE_DIST_TO_DEF_DIM;

    (void)p; // sanity: p == out + STATE_DIM
}

// --- move encoder ----------------------------------------------------------

static int move_type_slot(int move_type) {
    switch (move_type) {
        case MOVE_ATTACK: return 0;
        case MOVE_COVER:  return 1;
        case MOVE_PASS:   return 2;
        case MOVE_PICKUP: return 3;
        case MOVE_GOOD:   return 4;
        default:          return -1;
    }
}

void grpo_encode_move(const Game *g, const LegalMove *m, float *out) {
    memset(out, 0, MOVE_FEAT_DIM * sizeof(float));
    float *p = out;

    // action_type one-hot (5)
    int t = move_type_slot(m->type);
    if (t >= 0) p[t] = 1.0f;
    p += MOVE_TYPE_DIM;

    // cards multi-hot (52)
    int max_v = 0;
    bool uses_trump = false;
    for (int i = 0; i < m->n_cards; i++) {
        p[CARD_IDX(m->cards[i])] = 1.0f;
        if (m->cards[i].value > max_v) max_v = m->cards[i].value;
        if (m->cards[i].suit == g->power_suit) uses_trump = true;
    }
    p += MOVE_CARDS_DIM;

    // target attacks multi-hot (52) — populated for MOVE_COVER only.
    if (m->type == MOVE_COVER) {
        for (int i = 0; i < m->n_cards; i++) {
            p[CARD_IDX(m->attack_cards[i])] = 1.0f;
        }
    }
    p += MOVE_TARGETS_DIM;

    // card_count scalar (1) — normalize by MAX_MOVE_CARDS=8.
    p[0] = (float)m->n_cards / 8.0f;
    p += MOVE_NCARDS_DIM;

    // max_value scalar (1) — normalize by ACE_VALUE.
    p[0] = (float)max_v / 13.0f;
    p += MOVE_MAXVAL_DIM;

    // uses_trump flag (1)
    p[0] = uses_trump ? 1.0f : 0.0f;
    p += MOVE_USES_TRUMP_DIM;

    (void)p;
}

void grpo_encode_moves(const Game *g, const LegalMoves *moves, float *out) {
    for (int i = 0; i < moves->n; i++) {
        grpo_encode_move(g, &moves->moves[i], out + (size_t)i * MOVE_FEAT_DIM);
    }
}
