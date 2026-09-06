#include "view.h"
#include "../wasm/wire.h"
#include <string.h>   // memset — the hidden-run fill in state_put (see below)

// Whether the hand of seat `seat` is visible to `viewer`.
static int hand_visible(int viewer, int seat) {
    return viewer == VIEW_UNMASKED || viewer == seat;
}

int state_put(const Game *g, int viewer, unsigned char *out) {
    const int unmasked = (viewer == VIEW_UNMASKED);
    unsigned char *q = out;
    *q++ = (unsigned char)g->status;
    *q++ = (unsigned char)g->num_players;
    *q++ = (unsigned char)g->power_suit;
    *q++ = (unsigned char)g->first_attacker;
    *q++ = (unsigned char)g->defender;
    *q++ = (unsigned char)(g->discard_pile_length & 0xff);
    *q++ = (unsigned char)((g->discard_pile_length >> 8) & 0xff);
    *q++ = (unsigned char)(g->has_flipped ? 1 : 0);
    // Canonical no-flip byte: after the flipped trump is drawn the kernel
    // keeps the stale card in g->flipped (gated by has_flipped) — writing it
    // would make byte-equal states serialize differently depending on
    // whether they came from a marshal round-trip (which normalizes to the
    // {0,0} placeholder -> WIRE_CARD_HIDDEN) or stayed kernel-resident.
    // Decoders ignore this byte when has_flipped is 0.
    *q++ = g->has_flipped ? wire_from_card(g->flipped) : (unsigned char)WIRE_CARD_HIDDEN;
    *q++ = (unsigned char)(g->good_players_mask & 0xff);
    *q++ = (unsigned char)((g->good_players_mask >> 8) & 0xff);
    *q++ = (unsigned char)((g->good_players_mask >> 16) & 0xff);
    *q++ = (unsigned char)((g->good_players_mask >> 24) & 0xff);
    *q++ = (unsigned char)(g->has_good_timestamp ? 1 : 0);
    *q++ = (unsigned char)(g->deck_count & 0xff);
    *q++ = (unsigned char)((g->deck_count >> 8) & 0xff);
    // Hoist the loop-invariant `unmasked` test out of the per-card loop. A
    // masked view (every /ws push) hides the ENTIRE deck, so that collapses to
    // one vectorized memset of WIRE_CARD_HIDDEN instead of a branch + byte
    // store per card. Output is byte-for-byte identical either way. state_put
    // is the server's hottest own-code once bot-thread churn is gone
    // (PROFILE_HOTPATH.md's assembly capture: view.c:34-35 / 49-50).
    if (unmasked) {
        for (int i = 0; i < g->deck_count; i++) *q++ = wire_from_card(g->deck[i]);
    } else if (g->deck_count > 0) {
        memset(q, (unsigned char)WIRE_CARD_HIDDEN, (size_t)g->deck_count);
        q += g->deck_count;
    }
    *q++ = (unsigned char)g->num_battles;
    for (int i = 0; i < g->num_battles; i++) {
        const Battle *b = &g->table_battles[i];
        *q++ = wire_from_card(b->attack);
        *q++ = wire_from_card(b->defense);
    }
    for (int i = 0; i < g->num_players; i++) {
        const Player *pl = &g->players[i];
        const int visible = hand_visible(viewer, i);
        *q++ = (unsigned char)pl->status;
        *q++ = (unsigned char)((visible && pl->awaiting_attack) ? 1 : 0);
        *q++ = (unsigned char)pl->hand_count;
        // Same hoist as the deck loop: a hidden hand (every seat except the
        // viewer, on a masked push) is one memset instead of a per-card branch.
        if (visible) {
            for (int j = 0; j < pl->hand_count; j++) *q++ = wire_from_card(pl->hand[j]);
        } else if (pl->hand_count > 0) {
            memset(q, (unsigned char)WIRE_CARD_HIDDEN, (size_t)pl->hand_count);
            q += pl->hand_count;
        }
    }
    *q++ = (unsigned char)g->num_eliminated;
    for (int i = 0; i < g->num_eliminated; i++) *q++ = (unsigned char)g->elimination_order[i];
    return (int)(q - out);
}

// Masked decode: WIRE_CARD_HIDDEN becomes the {0,1} placeholder the browser
// marshal always used for redacted cards; everything else clamps like the
// legacy path.
static Card card_from_wire_masked(unsigned char b) {
    if (b == WIRE_CARD_HIDDEN) { Card c; c.suit = 0; c.value = 1; return c; }
    return card_from_wire_state(b);
}

void state_get(Game *g, const unsigned char *p, int masked) {
    const unsigned char *q = p;
    // No full-struct memset: the Game is ~200 KB (mostly log capacity) and
    // every read in the kernel is bounded by the counts set below. Every
    // count is clamped to its array capacity — the kernel must never corrupt
    // memory on a malformed/corrupt input (see docs/SECURITY_WASM_BOUNDARY.md).
    g->status = (int8_t)*q++;
    g->num_players = (int8_t)*q++;
    if (g->num_players < 0) g->num_players = 0;
    if (g->num_players > MAX_PLAYERS) g->num_players = MAX_PLAYERS;
    g->power_suit = (int8_t)*q++;
    g->first_attacker = (int8_t)*q++;
    g->defender = (int8_t)*q++;
    g->discard_pile_length = (int16_t)(q[0] | (q[1] << 8)); q += 2;
    g->has_flipped = (*q++ != 0);
    // When there is no flip (TS flipped === null), preserve the exact {0,0}
    // bytes the old 2-byte wire left in g->flipped — semtex-family belief
    // code reads it unguarded and {0,0} acts as a harmless never-matches pin.
    {
        unsigned char fw = *q++;
        if (g->has_flipped) g->flipped = card_from_wire_state(fw);
        else { g->flipped.suit = 0; g->flipped.value = 0; }
    }
    g->good_players_mask = (uint32_t)q[0] | ((uint32_t)q[1] << 8)
        | ((uint32_t)q[2] << 16) | ((uint32_t)q[3] << 24);
    q += 4;
    g->has_good_timestamp = (*q++ != 0);
    g->deck_count = (int16_t)(q[0] | (q[1] << 8)); q += 2;
    if (g->deck_count < 0) g->deck_count = 0;
    if (g->deck_count > MAX_DECK) g->deck_count = MAX_DECK;
    for (int i = 0; i < g->deck_count; i++) {
        unsigned char b = *q++;
        g->deck[i] = masked ? card_from_wire_masked(b) : card_from_wire_state(b);
    }
    g->num_battles = (int8_t)*q++;
    if (g->num_battles < 0) g->num_battles = 0;
    if (g->num_battles > MAX_BATTLES) g->num_battles = MAX_BATTLES;
    for (int i = 0; i < g->num_battles; i++) {
        Battle *b = &g->table_battles[i];
        b->attack = card_from_wire_state(*q++);
        unsigned char db = *q++;
        b->defense = (db == WIRE_CARD_NONE) ? CARD_NONE : card_from_wire_state(db);
    }
    for (int i = 0; i < g->num_players; i++) {
        Player *pl = &g->players[i];
        pl->status = (int8_t)*q++;
        pl->awaiting_attack = (*q++ != 0);
        pl->hand_count = (int8_t)*q++;
        if (pl->hand_count < 0) pl->hand_count = 0;
        if (pl->hand_count > MAX_HAND_SIZE) pl->hand_count = MAX_HAND_SIZE;
        for (int j = 0; j < pl->hand_count; j++) {
            unsigned char b = *q++;
            pl->hand[j] = masked ? card_from_wire_masked(b) : card_from_wire_state(b);
        }
    }
    g->num_eliminated = (int8_t)*q++;
    if (g->num_eliminated < 0) g->num_eliminated = 0;
    if (g->num_eliminated > MAX_PLAYERS) g->num_eliminated = MAX_PLAYERS;
    for (int i = 0; i < g->num_eliminated; i++) g->elimination_order[i] = (int8_t)*q++;
    g->num_logs = 0;
    // The resident game always has a full-size log array; only sampled-world
    // slots ever set log_cap (see game.h). Re-pin defensively per marshal.
    g->log_cap = 0;
    g->log_virt = 0;
}
