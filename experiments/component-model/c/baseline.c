// The repo's way: a static IO buffer the host writes the compact wire into,
// parsed by C (same layout and clamps as c/src/view.c state_get/state_put).
// Freestanding, exported memory, no allocator, no imports.
#include "kstate.h"

KGame g_game;
static uint8_t g_io[4096];

__attribute__((export_name("wasm_io_ptr")))
uint8_t *wasm_io_ptr(void) { return g_io; }

static int clamp(int v, int hi) { return v < 0 ? 0 : v > hi ? hi : v; }

__attribute__((export_name("wasm_import_state")))
void wasm_import_state(void) {
    KGame *g = &g_game;
    const uint8_t *q = g_io;
    g->status = (int8_t)*q++;
    g->num_players = (int8_t)clamp((int8_t)*q++, MAX_PLAYERS);
    g->power_suit = (int8_t)*q++;
    g->first_attacker = (int8_t)*q++;
    g->defender = (int8_t)*q++;
    g->discard = (int16_t)(q[0] | (q[1] << 8)); q += 2;
    g->has_flipped = *q++ != 0;
    g->flipped = *q++;
    g->good_mask = (uint32_t)q[0] | ((uint32_t)q[1] << 8) | ((uint32_t)q[2] << 16) | ((uint32_t)q[3] << 24);
    q += 4;
    g->has_good_ts = *q++ != 0;
    g->deck_count = (int16_t)clamp((int16_t)(q[0] | (q[1] << 8)), MAX_DECK); q += 2;
    for (int i = 0; i < g->deck_count; i++) g->deck[i] = *q++;
    g->num_battles = (int8_t)clamp((int8_t)*q++, MAX_BATTLES);
    for (int i = 0; i < g->num_battles; i++) {
        g->battles[i].attack = *q++;
        g->battles[i].defense = *q++;
    }
    for (int i = 0; i < g->num_players; i++) {
        KPlayer *p = &g->players[i];
        p->status = (int8_t)*q++;
        p->awaiting = *q++ != 0;
        p->hand_count = (int8_t)clamp((int8_t)*q++, MAX_HAND_SIZE);
        for (int j = 0; j < p->hand_count; j++) p->hand[j] = *q++;
    }
    g->num_eliminated = (int8_t)clamp((int8_t)*q++, MAX_PLAYERS);
    for (int i = 0; i < g->num_eliminated; i++) g->elimination[i] = (int8_t)*q++;
}

__attribute__((export_name("wasm_export_state")))
int wasm_export_state(void) {
    const KGame *g = &g_game;
    uint8_t *q = g_io;
    *q++ = (uint8_t)g->status;
    *q++ = (uint8_t)g->num_players;
    *q++ = (uint8_t)g->power_suit;
    *q++ = (uint8_t)g->first_attacker;
    *q++ = (uint8_t)g->defender;
    *q++ = (uint8_t)(g->discard & 0xff);
    *q++ = (uint8_t)((g->discard >> 8) & 0xff);
    *q++ = g->has_flipped;
    *q++ = g->flipped;
    *q++ = (uint8_t)g->good_mask;
    *q++ = (uint8_t)(g->good_mask >> 8);
    *q++ = (uint8_t)(g->good_mask >> 16);
    *q++ = (uint8_t)(g->good_mask >> 24);
    *q++ = g->has_good_ts;
    *q++ = (uint8_t)(g->deck_count & 0xff);
    *q++ = (uint8_t)((g->deck_count >> 8) & 0xff);
    for (int i = 0; i < g->deck_count; i++) *q++ = g->deck[i];
    *q++ = (uint8_t)g->num_battles;
    for (int i = 0; i < g->num_battles; i++) {
        *q++ = g->battles[i].attack;
        *q++ = g->battles[i].defense;
    }
    for (int i = 0; i < g->num_players; i++) {
        const KPlayer *p = &g->players[i];
        *q++ = (uint8_t)p->status;
        *q++ = p->awaiting;
        *q++ = (uint8_t)p->hand_count;
        for (int j = 0; j < p->hand_count; j++) *q++ = p->hand[j];
    }
    *q++ = (uint8_t)g->num_eliminated;
    for (int i = 0; i < g->num_eliminated; i++) *q++ = (uint8_t)g->elimination[i];
    return (int)(q - g_io);
}
