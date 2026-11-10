// gate.c - the bot behaviour gate for foolish's bot-memory change (docs/BOT_MEMORY.md H7).
//
// Seeded games played the server's way: every operation runs on a table adopted from the
// previous commit's resident, held as the journal holds it (GATE_NEW: in table_resident_bytes(res)
// bytes, malloc'd at exactly that size; base: the whole struct), with an eviction (a load of the
// row and its session log) every RELOAD_EVERY operations. Bots move by their brains through
// table_bot_drive; human seats by the gate's own seeded draw. Each commit prints the moves it
// applied (seat and action wire), the row's state bytes as hex and, from the held resident, the
// encode (it must be the same bytes). Base and change are run with the same arguments and the
// outputs compared byte for byte.
//
// Randomness: one generator (gate_rng), seeded by GATE_SEED and the game's index only.
#include "table.h"
#include "legal.h"
#include "awire.h"
#include "roster.h"
#include "game.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define GATE_SEED 0x5eed0001u
#define RELOAD_EVERY 7

static uint64_t gate_rng_state;
static void gate_rng_seed(uint64_t s) { gate_rng_state = s * 0x9E3779B97F4A7C15ull + 1; }
static uint32_t gate_rng(uint32_t n) {
    uint64_t x = gate_rng_state;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    gate_rng_state = x;
    return (uint32_t)((x >> 11) % n);
}

static Game game;
static TableSnaps snaps;
static Table tb;
static uint8_t arena[1 << 18];
static LegalMoves moves;
static TableResident made;          // what table_resident writes (the section's whole buffer)
static TableResident *held;         // what the journal holds
static uint8_t roster[ROSTER_MAX];
static int roster_len;
static uint8_t state[8192], logbuf[1 << 17];
static int state_len, log_len;
static uint32_t version;
static long long drive_ns, drives, bot_actions;
static int problems;
static long long holds, light_holds;

static long long now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void hex(const uint8_t *p, int n) {
    for (int i = 0; i < n; i++) printf("%02x", p[i]);
}

static void hold(const BoardClock *clock) {
    free(held);
    holds++;
#ifdef GATE_NEW
    // As the server's section and journal: a buffer of each size, exactly that size, and the one
    // table_resident_into wrote is held.
    TableResident *whole = malloc(sizeof(TableResident)), *small = malloc(TABLE_RESIDENT_SMALL);
    held = table_resident_into(&tb, clock, whole, small);
    if (held == small) { light_holds++; free(whole); } else free(small);
    if (table_resident_bytes(held) != (held == small ? TABLE_RESIDENT_SMALL : sizeof(TableResident))) { printf("  wrong size\n"); problems++; }
#else
    table_resident(&tb, clock, &made);
    held = malloc(sizeof made);
    memcpy(held, &made, sizeof made);
#endif
}

// Commits the table's operation into the row, prints it, and holds the resident.
static int commit(const char *what) {
    TableCommit c;
    const int64_t now = 1700000000000LL + (int64_t)version * 1000;
    const int n = table_commit_products(&tb, "g", 1, version + 1, now, &c, arena, sizeof arena);
    if (n < 0) { printf("  commit %s refused %d\n", what, n); problems++; return 0; }
    memcpy(state, arena + c.state.off, (size_t)c.state.len);
    state_len = c.state.len;
    memcpy(roster, arena + c.roster.off, (size_t)c.roster.len);
    roster_len = c.roster.len;
    if (c.logs_reset) log_len = 0;
    if (log_len + c.logs.len > (int)sizeof logbuf) { printf("  log full\n"); problems++; return 0; }
    memcpy(logbuf + log_len, arena + c.logs.off, (size_t)c.logs.len);
    log_len += c.logs.len;
    version++;
    printf("v%u %s", version, what);
    for (int i = 0; i < tb.n_moves; i++) { printf(" s%d:", tb.moves[i].seat); hex(tb.moves[i].wire, tb.moves[i].wire_len); }
    printf(" state=");
    hex(state, state_len);
    printf("\n");
    hold(&c.clock);
    // The held resident encodes to the row's bytes (the flush's and a read's path).
    uint8_t enc[TABLE_STATE_MAX];
    int el = 0;
    if (table_resident_encode(held, enc, (int)sizeof enc, &el) != TABLE_OK || el != state_len || memcmp(enc, state, (size_t)el) != 0) {
        printf("  the held resident does not encode to the row\n");
        problems++;
    }
    return 1;
}

// The next operation's table: the held resident adopted, or (every RELOAD_EVERY) the row loaded
// with its session log, as an evicted game is.
static int open_table(int op) {
    if (op % RELOAD_EVERY == RELOAD_EVERY - 1) {
        if (table_load(&tb, state, state_len, roster, roster_len) != TABLE_OK) { printf("  load refused\n"); problems++; return 0; }
        if (table_bots_need_logs(&tb) && table_set_session_log(&tb, logbuf, log_len) < 0) { printf("  log refused\n"); problems++; return 0; }
    } else if (table_adopt(&tb, held, roster, roster_len) != TABLE_OK) {
        printf("  adopt refused\n"); problems++; return 0;
    }
    table_set_deal_seed(&tb, "abababababababababababababababab", 32);
    return 1;
}

// A human's move: a draw over every IN human seat's sendable moves.
static int human_move(uint32_t bots) {
    int counts[MAX_PLAYERS], total = 0;
    for (int s = 0; s < game.num_players; s++) {
        counts[s] = 0;
        if (game.players[s].status != PLAYER_STATUS_IN || ((bots >> s) & 1u)) continue;
        calculate_legal_moves(&game, s, &moves);
        for (int i = 0; i < moves.n; i++) counts[s] += moves.moves[i].type <= MOVE_GOOD;
        total += counts[s];
    }
    if (total == 0) return 0;
    int r = (int)gate_rng((uint32_t)total), s = 0;
    while (r >= counts[s]) r -= counts[s++];
    calculate_legal_moves(&game, s, &moves);
    for (int i = 0; i < moves.n; i++) {
        const LegalMove *m = &moves.moves[i];
        if (m->type > MOVE_GOOD || r-- != 0) continue;
        AwireAction a;
        memset(&a, 0, sizeof a);
        a.kind = m->type;
        a.n = m->n_cards;
        for (int k = 0; k < m->n_cards; k++) { a.cards[k] = m->cards[k]; a.attacks[k] = m->attack_cards[k]; }
        uint8_t w[128];
        const int wl = awire_encode(&a, w, sizeof w);
        char id[16];
        snprintf(id, sizeof id, "id-%d", s);
        return wl > 0 && table_act(&tb, id, (int)strlen(id), w, wl, -1, 0) == TABLE_APPLIED;
    }
    return 0;
}

// Plays the dealt game to its end. 1 when it ended.
static int play(uint32_t bots) {
    for (int op = 0; op < 4000; op++) {
        if (!open_table(op)) return 0;
        if (game.status != GAME_STATUS_PLAYING) return 1;
        int drove = 0;
        if (table_bots_due(&tb)) {
            BotDriveOut out;
            const long long t0 = now_ns();
            const int d = table_bot_drive(&tb, 0, 0, 0, &out);
            drive_ns += now_ns() - t0;
            drives++;
            if (d < 0) { printf("  drive refused %d\n", d); problems++; return 0; }
            bot_actions += d;
            drove = d > 0;
        }
        if (!drove && !human_move(bots)) { printf("  no move\n"); problems++; return 0; }
        if (!commit(drove ? "bots" : "human")) return 0;
        if (game.status == GAME_STATUS_GAME_OVER) return 1;
    }
    printf("  too long\n");
    problems++;
    return 0;
}

static void lobby_roster(int np, const char *const *brains, Roster *r) {
    memset(r, 0, sizeof *r);
    roster_set_title(r, "Table", 5);
    for (int i = 0; i < np; i++) {
        char id[16], name[16];
        snprintf(id, sizeof id, "id-%d", i);
        snprintf(name, sizeof name, "Seat %d", i);
        const char *b = brains[i] ? brains[i] : "";
        roster_seat_add(r, id, (int)strlen(id), name, (int)strlen(name), b, (int)strlen(b));
    }
}

static uint32_t bot_mask(int np, const char *const *brains) {
    uint32_t m = 0;
    for (int i = 0; i < np; i++) if (brains[i]) m |= 1u << i;
    return m;
}

static void deal_seed(int gi, int round, uint8_t *seed) {
    for (int i = 0; i < FOOLISH_SEED_LEN; i++) seed[i] = (uint8_t)(i * 13 + gi * 7 + round * 101 + 1);
}

// One table: dealt with `brains` (NULL = human) and played out; then, when `add` names a brain,
// a rematch in which that bot joins the lobby (a table whose memory kind changes), played out.
static void run(int gi, int np, const char *const *brains, const char *add) {
    static Game lobby;
    Roster r;
    uint8_t seed[FOOLISH_SEED_LEN];
    gate_rng_seed((uint64_t)GATE_SEED ^ (uint64_t)gi << 20);
    printf("game %d np %d:", gi, np);
    for (int i = 0; i < np; i++) printf(" %s", brains[i] ? brains[i] : "human");
    if (add) printf(" then +%s", add);
    printf("\n");
    memset(&lobby, 0, sizeof lobby);
    lobby.num_players = (int8_t)np;
    game_reset_to_lobby(&lobby, (1u << np) - 1u);   // every seat READY
    lobby_roster(np, brains, &r);
    table_init(&tb, &game, &snaps);
    if (table_seal(&tb, &lobby, &r, arena, (int)sizeof arena, NULL) <= 0) { printf("  seal refused\n"); problems++; return; }
    deal_seed(gi, 0, seed);
    const int rc = table_ready(&tb, "id-0", 4, seed);
    game_set_seed(1);
    if (rc != TABLE_OK || !tb.dealt_now) { printf("  deal refused %d\n", rc); problems++; return; }
    version = 0;
    log_len = 0;
    if (!commit("deal")) return;
    uint32_t bots = bot_mask(np, brains);
    if (!play(bots)) return;
    if (!add) return;
    // The rematch: back to the lobby, the bot added, every human ready (the last one deals).
    if (!open_table(0) || table_continue(&tb, "id-0", 4) != TABLE_OK || !commit("continue")) { problems++; return; }
    char bid[16];
    snprintf(bid, sizeof bid, "id-%d", np);
    deal_seed(gi, 1, seed);
    if (!open_table(0) || table_add_bot(&tb, "id-0", 4, bid, (int)strlen(bid), "Bot", 3, add, (int)strlen(add), seed) != TABLE_OK
        || !commit("add_bot")) { printf("  add_bot refused\n"); problems++; return; }
    bots |= 1u << np;
    for (int s = 0; s < np + 1 && game.status != GAME_STATUS_PLAYING; s++) {
        if ((bots >> s) & 1u) continue;
        char id[16];
        snprintf(id, sizeof id, "id-%d", s);
        if (!open_table(0)) return;
        const int k = table_ready(&tb, id, (int)strlen(id), seed);
        game_set_seed(1);
        if (k != TABLE_OK && k != TABLE_MOOT) { printf("  ready refused %d\n", k); problems++; return; }
        if (!commit("ready")) return;
    }
    if (game.status != GAME_STATUS_PLAYING) { printf("  the rematch did not deal\n"); problems++; return; }
    play(bots);
}

int main(int argc, char **argv) {
    const int seeds = argc > 1 ? atoi(argv[1]) : 3;
    const char *only = argc > 2 ? argv[2] : 0;
    // Every brain the server seeds (bot_seed.c .seeded = 1), then the others.
    static const char *const all[] = { "random", "robusta", "firecracker", "blackpowder", "cordite", "handwritten",
                                       "octogen", "fusion", "gunpowder", "simple_heuristic", "espresso" };
    const int n_all = (int)(sizeof all / sizeof all[0]);
    int gi = 0;
    for (int k = 0; k < seeds; k++) {
        for (int b = 0; b < n_all; b++) {
            if (only && strcmp(only, all[b]) != 0) { gi += 3; continue; }
            const char *h2[] = { 0, all[b] };
            const char *h3[] = { 0, all[b], all[b] };
            const char *b4[] = { all[b], all[b], all[b], all[b] };
            run(gi++, 2, h2, 0);
            run(gi++, 3, h3, k == 0 ? "octogen" : 0);
            run(gi++, 4, b4, 0);   // bots only
        }
        if (only) continue;
        { const char *t[] = { 0, 0 };                                      run(gi++, 2, t, "octogen"); }
        { const char *t[] = { 0, 0, 0 };                                   run(gi++, 3, t, 0); }
        { const char *t[] = { 0, "handwritten", "random" };                run(gi++, 3, t, "cordite"); }
        { const char *t[] = { 0, "octogen", "random", "handwritten" };     run(gi++, 4, t, 0); }
        { const char *t[] = { 0, "cordite", "robusta", "blackpowder" };    run(gi++, 4, t, 0); }
        { const char *t[] = { "random", "handwritten", "simple_heuristic" }; run(gi++, 3, t, 0); }
        { const char *t[] = { "fusion", "octogen", "firecracker", "espresso" }; run(gi++, 4, t, 0); }
    }
    fprintf(stderr, "gate: %lld holds, %lld of them without the memory block\n", holds, light_holds);
    fprintf(stderr, "gate: %d tables, %lld drives, %lld bot actions, %.1f us per drive, problems %d\n", gi, drives, bot_actions,
            drives ? (double)drive_ns / (double)drives / 1000.0 : 0.0, problems);
    printf("problems %d\n", problems);
    return problems != 0;
}
