// jbench.c: the journal's state-bytes paths, timed on journal.c's own static functions (included).
// Built against one tree's journal.c (base or new); -DNEW selects the new flush call.
#include "store/journal.c"
#include "awire.h"
#include "legal.h"
#include "../../../shared/c/hex/hex.h"
#include "../../../shared/c/clock.h"
#include <stdio.h>

#define GID "ab12cd"
#define UA "aaaaaaaa-0000-4000-8000-000000000001"
#define UB "aaaaaaaa-0000-4000-8000-000000000002"
#define ARENA_BYTES (512 * 1024)
static Game *g_game; static TableSnaps *g_snaps; static uint8_t *g_arena; static Table g_t; static TableCommit g_c;
static uint8_t g_state[64 * 1024], g_roster[ROSTER_MAX], g_seed[FOOLISH_SEED_LEN];
static int g_state_len, g_roster_len; static uint32_t g_version; static char g_seed_hex[2 * FOOLISH_SEED_LEN + 1]; static bool g_seeded;
static int load(void) {
    table_init(&g_t, g_game, g_snaps);
    int rc = table_load(&g_t, g_state, g_state_len, g_roster, g_roster_len);
    if (rc >= 0 && g_seeded) rc = table_set_deal_seed(&g_t, g_seed_hex, (int)strlen(g_seed_hex));
    return rc;
}
static int commit(void) {
    int rc = table_commit_products(&g_t, GID, 6, g_version + 1, 1760000000000 + g_version, &g_c, g_arena, ARENA_BYTES);
    if (rc < 0) return rc;
    memcpy(g_state, g_arena + g_c.state.off, (size_t)g_c.state.len); g_state_len = g_c.state.len;
    memcpy(g_roster, g_arena + g_c.roster.off, (size_t)g_c.roster.len); g_roster_len = g_c.roster.len;
    g_version++; return 0;
}
static bool pick_move(const char **actor, uint8_t *wire, int *wire_len) {
    static LegalMoves lm;
    for (int seat = 0; seat < 2; seat++) {
        calculate_legal_moves(g_t.g, seat, &lm);
        for (int k = 0; k < lm.n; k++) {
            const LegalMove *m = &lm.moves[k];
            if (m->type == MOVE_WAIT) continue;
            AwireAction a; memset(&a, 0, sizeof a);
            a.kind = m->type; a.n = m->n_cards;
            for (int i = 0; i < m->n_cards; i++) { a.cards[i] = m->cards[i]; a.attacks[i] = m->attack_cards[i]; }
            int n = awire_encode(&a, wire, 64);
            if (n <= 0) continue;
            *actor = seat == 0 ? UA : UB; *wire_len = n; return true;
        }
    }
    return false;
}
static bool play_in(int moves) {
    g_game = calloc(1, sizeof *g_game); g_snaps = calloc(1, sizeof *g_snaps); g_arena = malloc(ARENA_BYTES);
    for (int i = 0; i < FOOLISH_SEED_LEN; i++) g_seed[i] = (uint8_t)(i * 37 + 11);
    hex_encode(g_seed, FOOLISH_SEED_LEN, g_seed_hex, sizeof g_seed_hex);
    table_init(&g_t, g_game, g_snaps);
    if (table_create(&g_t, UA, 36, "alice", 5) < 0 || commit() < 0) return false;
    if (load() < 0 || table_join(&g_t, UB, 36, "bob", 3) < 0 || commit() < 0) return false;
    if (load() < 0 || table_ready(&g_t, UA, 36, g_seed) < 0 || commit() < 0) return false;
    if (load() < 0 || table_ready(&g_t, UB, 36, g_seed) < 0 || commit() < 0) return false;
    g_seeded = true;
    for (int i = 0; i < moves; i++) {
        const char *actor; uint8_t wire[64]; int n;
        if (load() < 0 || !pick_move(&actor, wire, &n)) return false;
        if (table_act(&g_t, actor, 36, wire, n, -1, 0) != TABLE_APPLIED || commit() < 0) return false;
    }
    return load() >= 0;
}

static jgame G; static cb_game_row SRC; static TableResident *RES; static volatile int64_t sink;
static void op_commit(void) { game_set(&G, &SRC, &RES); }   // a commit handing a resident over (the buffers swap)
static void op_flush(void) {
    game_set(&G, &SRC, &RES);
    cb_game_row r;
#ifdef NEW
    if (row_dup_encoded(&r, &G) != CB_OK) abort();
#else
    if (row_refresh(&G) != CB_OK || row_dup(&r, &G.row) != CB_OK) abort();
#endif
    sink += (int64_t)r.state_len; cb_game_row_free(&r);
}
static void op_get_miss(void) {
    game_set(&G, &SRC, &RES);
    cb_game_row r;
    if (row_refresh(&G) != CB_OK || row_dup(&r, &G.row) != CB_OK) abort();
    sink += (int64_t)r.state_len; cb_game_row_free(&r);
}
static void op_get_hit(void) {
    cb_game_row r;
    if (row_refresh(&G) != CB_OK || row_dup(&r, &G.row) != CB_OK) abort();
    sink += (int64_t)r.state_len; cb_game_row_free(&r);
}
// The kernel's two ends of a resident: an operation's adopt of the game's, and the commit's making of one.
static void op_adopt(void) { if (table_adopt(&g_t, G.res, g_roster, g_roster_len) != TABLE_OK) abort(); }
static void op_resident(void) { table_resident(&g_t, NULL, RES); sink += RES->rng_base; }
typedef void (*fn)(void);
static int cmp(const void *a, const void *b) { int64_t x = *(const int64_t *)a, y = *(const int64_t *)b; return x < y ? -1 : x > y; }
static double t_op(fn f) {
    const int64_t k = 200000; int64_t per[31];
    for (int i = 0; i < k; i++) f();
    for (int b = 0; b < 31; b++) { int64_t t0 = host_mono_ns(); for (int64_t i = 0; i < k; i++) f(); per[b] = host_mono_ns() - t0; }
    qsort(per, 31, sizeof *per, cmp); return (double)per[15] / k;
}
int main(int argc, char **argv) {
    if (!play_in(argc > 1 ? atoi(argv[1]) : 8)) { fprintf(stderr, "fixture refused\n"); return 1; }
    static TableResident a, b;
    table_resident(&g_t, NULL, &a); b = a;
    G.res = malloc(sizeof *G.res); *G.res = a; G.res_valid = true;
#ifdef SMALL
    G.res_cap = table_resident_bytes(G.res);   // the game's buffer as the journal records it (res_take)
    fprintf(stderr, "resident bytes %zu of %zu\n", G.res_cap, sizeof *G.res);
#endif
    RES = malloc(sizeof *RES); *RES = b;
    memset(&SRC, 0, sizeof SRC);
    snprintf(SRC.id, sizeof SRC.id, "%s", GID);
    SRC.roster = g_roster; SRC.roster_len = (size_t)g_roster_len; SRC.version = 9;
    cb_game_row r0;
    game_set(&G, &SRC, &RES);
    if (row_refresh(&G) != CB_OK) return 1;
    printf("state_len=%zu (fixture %d) state_cap=%zu\n", G.row.state_len, g_state_len, G.state_cap);
    (void)r0;
    for (int rep = 0; rep < 3; rep++)
        printf("commit %.1f  flush %.1f  get_miss %.1f  get_hit %.1f  adopt %.1f  resident %.1f ns\n", t_op(op_commit), t_op(op_flush), t_op(op_get_miss), t_op(op_get_hit), t_op(op_adopt), t_op(op_resident));
    return 0;
}
