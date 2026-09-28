// Component side: implements the two WIT exports over the same KGame the
// baseline uses, plus the three libc symbols wit-bindgen's glue needs.
#include "kstate.h"
#include "kernel.h"

KGame g_game;

// ---- allocator shim -------------------------------------------------------
// The canonical ABI lowers a `game` argument by calling the guest's exported
// cabi_realloc for the record itself and for every list inside it (deck,
// battles, players, each hand, elimination) BEFORE import-state runs. There is
// no way around a guest allocator for list params, so it is a fixed static
// bump arena, reset once import-state has copied everything out.
static uint8_t g_arena[16384];
static size_t g_arena_top;

void *realloc(void *ptr, size_t n) {
    // cabi_realloc only ever grows from NULL here (jco never reallocs).
    if (ptr) __builtin_trap();
    size_t top = (g_arena_top + 7) & ~(size_t)7;
    if (top + n > sizeof g_arena) __builtin_trap();
    g_arena_top = top + n;
    return g_arena + top;
}
void free(void *ptr) { (void)ptr; }
_Noreturn void abort(void) { __builtin_trap(); }

// export-state returns pointers into static storage; nothing to free.
__attribute__((export_name("cabi_post_foolish:kernel/state@0.1.0#export-state")))
void __wasm_export_exports_foolish_kernel_state_export_state_post_return(uint8_t *arg0) { (void)arg0; }

typedef exports_foolish_kernel_state_game_t WGame;

static int clamp(int v, int hi) { return v < 0 ? 0 : v > hi ? hi : v; }

void exports_foolish_kernel_state_import_state(WGame *w) {
    KGame *g = &g_game;
    g->status = (int8_t)w->status;
    g->num_players = (int8_t)clamp((int)w->players.len, MAX_PLAYERS);
    g->power_suit = (int8_t)w->trump_suit;
    g->first_attacker = w->first_attacker;
    g->defender = w->defender;
    g->discard = (int16_t)w->discard;
    g->has_flipped = w->flipped.is_some;
    g->flipped = w->flipped.is_some ? w->flipped.val : 0;
    g->good_mask = w->good_mask;
    g->has_good_ts = w->has_good_ts;
    g->deck_count = (int16_t)clamp((int)w->deck.len, MAX_DECK);
    for (int i = 0; i < g->deck_count; i++) g->deck[i] = w->deck.ptr[i];
    g->num_battles = (int8_t)clamp((int)w->battles.len, MAX_BATTLES);
    for (int i = 0; i < g->num_battles; i++) {
        g->battles[i].attack = w->battles.ptr[i].attack;
        g->battles[i].defense = w->battles.ptr[i].defense.is_some ? w->battles.ptr[i].defense.val : WIRE_NONE;
    }
    for (int i = 0; i < g->num_players; i++) {
        KPlayer *p = &g->players[i];
        exports_foolish_kernel_state_player_t *wp = &w->players.ptr[i];
        p->status = (int8_t)wp->status;
        p->awaiting = wp->awaiting;
        p->hand_count = (int8_t)clamp((int)wp->hand.len, MAX_HAND_SIZE);
        for (int j = 0; j < p->hand_count; j++) p->hand[j] = wp->hand.ptr[j];
    }
    g->num_eliminated = (int8_t)clamp((int)w->elimination.len, MAX_PLAYERS);
    for (int i = 0; i < g->num_eliminated; i++) g->elimination[i] = (int8_t)w->elimination.ptr[i];
    g_arena_top = 0;  // everything the host lowered is now dead
}

static exports_foolish_kernel_state_battle_t g_out_battles[MAX_BATTLES];
static exports_foolish_kernel_state_player_t g_out_players[MAX_PLAYERS];

void exports_foolish_kernel_state_export_state(WGame *ret) {
    KGame *g = &g_game;
    ret->status = (uint8_t)g->status;
    ret->num_players = (uint8_t)g->num_players;
    ret->trump_suit = (uint8_t)g->power_suit;
    ret->first_attacker = g->first_attacker;
    ret->defender = g->defender;
    ret->discard = (uint16_t)g->discard;
    ret->flipped.is_some = g->has_flipped;
    ret->flipped.val = g->flipped;
    ret->good_mask = g->good_mask;
    ret->has_good_ts = g->has_good_ts;
    ret->deck.ptr = g->deck;  // zero-copy: the host reads straight out of KGame
    ret->deck.len = (size_t)g->deck_count;
    for (int i = 0; i < g->num_battles; i++) {
        g_out_battles[i].attack = g->battles[i].attack;
        g_out_battles[i].defense.is_some = g->battles[i].defense != WIRE_NONE;
        g_out_battles[i].defense.val = g->battles[i].defense;
    }
    ret->battles.ptr = g_out_battles;
    ret->battles.len = (size_t)g->num_battles;
    for (int i = 0; i < g->num_players; i++) {
        g_out_players[i].status = (uint8_t)g->players[i].status;
        g_out_players[i].awaiting = g->players[i].awaiting;
        g_out_players[i].hand.ptr = g->players[i].hand;
        g_out_players[i].hand.len = (size_t)g->players[i].hand_count;
    }
    ret->players.ptr = g_out_players;
    ret->players.len = (size_t)g->num_players;
    ret->elimination.ptr = (uint8_t *)g->elimination;
    ret->elimination.len = (size_t)g->num_eliminated;
}
