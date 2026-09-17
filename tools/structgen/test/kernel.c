// Test kernel for the structgen parity test and bench. It is the HEAD kernel's
// state codec (view.c: the legacy byte wire the hand-written TS marshal feeds)
// next to the in-place path the generated accessors feed, over ONE resident
// Game - wired exactly as c/wasm/wasm_api.c wires wasm_import_state /
// wasm_export_state / wasm_import_strategy_keys. Built by test/harness.sh from
// a `git archive HEAD` copy of c/src + c/wasm, so it does not move under
// concurrent kernel edits.
#include "game.h"
#include "view.h"

void *memcpy(void *dst, const void *src, size_t n) { __builtin_memcpy(dst, src, n); return dst; }
void *memset(void *dst, int c, size_t n) { __builtin_memset(dst, c, n); return dst; }
void *memmove(void *dst, const void *src, size_t n) { __builtin_memmove(dst, src, n); return dst; }

static Game g_game;
static unsigned char g_io[1 << 16];

Game *k_game(void) { return &g_game; }
unsigned char *k_io(void) { return g_io; }

// The layout this module was compiled with: structgen --print-hash for the
// same header + flags, passed in as -DSG_LAYOUT_HASH=0x...
unsigned k_layout_hash(void) { return SG_LAYOUT_HASH; }

// ---- legacy: the byte wire ----------------------------------------------------
void k_import(void) { state_get(&g_game, g_io, 0); g_game.deterministic_deck = false; }
void k_import_keys(void) { for (int i = 0; i < g_game.num_players; i++) g_game.players[i].strategy_key = (int8_t)g_io[i]; }
int k_export(void) { return state_put(&g_game, VIEW_UNMASKED, g_io); }
void k_set_deterministic_deck(int on) { g_game.deterministic_deck = on != 0; }

// ---- generated: the host wrote g_game's fields in place -----------------------
// Everything state_get did besides moving bytes. Counts only: memory safety.
// TODO: adopt must call the shared kernel validator (game_validate) once it lands.
void k_adopt(void) {
    Game *g = &g_game;
#define CLAMP(x, hi) do { if ((x) < 0) (x) = 0; if ((x) > (hi)) (x) = (hi); } while (0)
    CLAMP(g->num_players, MAX_PLAYERS);
    CLAMP(g->deck_count, MAX_DECK);
    CLAMP(g->num_battles, MAX_BATTLES);
    CLAMP(g->num_eliminated, MAX_PLAYERS);
    for (int i = 0; i < g->num_players; i++) CLAMP(g->players[i].hand_count, MAX_HAND_SIZE);
#undef CLAMP
    if (!g->has_flipped) { g->flipped.suit = 0; g->flipped.value = 0; }
    g->num_logs = 0;
    g->log_cap = 0;
    g->log_virt = 0;
}

// ---- kernel moves, so the parity test also reads states the kernel produced ----
int k_human_mask(void) { return (int)game_human_mask(&g_game); }
int k_pickup(int seat) { return handle_pickup(&g_game, seat); }
void k_transition(void) { engine_run_round_transition(&g_game); }
void k_refill(void) { engine_run_refill(&g_game); }
