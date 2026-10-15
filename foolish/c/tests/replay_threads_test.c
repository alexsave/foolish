// The replay codec, the FMSG envelope and the deal's opening overrides on two
// threads at once.
//
// A server finishes a game on one thread (table_replay_code: encode the game to
// a v6 code, decode it back, verify) while a link unfurl on another thread reads
// an envelope (msg_decode + msg_replay, which re-deals and decodes the v6 body).
// Both run the same codec. Its scratch - the rANS choice log and bignum, the
// public model, the menu, the re-deal slot, the atom sink - and the deal's
// opening overrides in game.c were process globals, so the two calls wrote into
// each other's state: a code that encodes a different game, a decode that fails
// or rebuilds the wrong board, a live deal that opens on the unfurled
// envelope's seat.
//
// Two parts:
//
//   1. Two threads, two different games, many rounds of the whole codec each:
//      encode the game to a v6 code, decode it to a log stream, build its share
//      link with a roster, and read its sealed envelope back into a Game. Every
//      result must equal what the same call returned on one thread before the
//      race began, byte for byte.
//
//      Then the share link alone, many more times, since it is quick.
//
//   2. The opening overrides, made deterministic with a rendezvous instead of
//      hoped for: one thread pins an opening seat (as msg_replay does around its
//      re-deal of a rematch) and holds it while the other deals a fresh game,
//      which must open on the seat its own hands derive. Then each thread deals,
//      both meet, and each must still read its own deal's derived seat. Then one
//      thread forces a no-trump opener (as replay_deal_start does) while the
//      other deals a game with no trump, which must roll its own. Last, each
//      thread has a code refused, both meet, and each must read back its own
//      reason.
//
// Built with real thread-local storage (make replay-threads-test, the analyse
// variant's objects): the host variant compiles _Thread_local away, and there
// this would be measuring nothing.
//
// Mutation-checked: dropping _Thread_local from any one of the codec scratch
// (replay.c), the extras link buffers (replay_extras.c) or the opening
// overrides (game.c) fails the check that names it.
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "bot_drive.h"
#include "game.h"
#include "msg_wire.h"
#include "replay.h"
#include "replay_extras.h"
#include "strategy.h"

static int g_fail;
#define CHECK(c, msg) do { if (!(c)) { g_fail++; fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); } } while (0)

enum { N_THREADS = 2, ROUNDS = 400, LINK_ROUNDS = 20000, CODE_CAP = 8192, DEC_CAP = 1 << 18, LINK_CAP = 2048,
       BODY_CAP = 4096, WIRE_CAP = 8192 };

// One thread's game and everything one thread computed from it before the race.
typedef struct {
    int n_players;
    unsigned char seed[FOOLISH_SEED_LEN];
    Game game;                            // the finished game (table_replay_code's input)
    unsigned char code[CODE_CAP];         // its v6 code
    int code_len;
    unsigned char dec[DEC_CAP];           // the code decoded to a log stream
    int dec_len;
    unsigned char roster[256];            // its share link's names
    int roster_len;
    char link[LINK_CAP];
    int link_len;
    unsigned char wire[WIRE_CAP];         // its sealed envelope (what an unfurl reads)
    int wire_len;
    Game replayed;                        // the envelope read back into a Game
    // the race's own outputs, per thread
    unsigned char r_code[CODE_CAP];
    unsigned char r_dec[DEC_CAP];
    char r_link[LINK_CAP];
    char moves[CODE_CAP * 2];
    Game r_replayed;
    int bad_code, bad_dec, bad_link, bad_msg;
} Lane;

static Lane g_lane[N_THREADS];
static Game g_scratch;                    // msg_seal's, used before any thread starts

static void seed_of(unsigned char *s, int k) {
    for (int i = 0; i < FOOLISH_SEED_LEN; i++) s[i] = (unsigned char)(i * 29 + k * 71 + 5);
}

static void play_out(Lane *L, int k) {
    Game *g = &L->game;
    seed_of(L->seed, k);
    game_set_seed((uint32_t)(k + 1));
    random_strategy_set_seed((uint32_t)(k + 1));
    game_set_deal_seed_bytes(L->seed, FOOLISH_SEED_LEN);
    memset(g, 0, sizeof *g);
    g->num_players = (int8_t)L->n_players;
    for (int i = 0; i < L->n_players; i++) g->players[i].status = PLAYER_STATUS_READY;
    start_game(g);
    for (int i = 0; i < L->n_players; i++) g->players[i].strategy_key = STRAT_HANDWRITTEN_PROD;
    BotDriveOut out;
    for (int c = 0; c < 400; c++)
        if (bot_drive(g, 0, BOT_DRIVE_MAX_ACTIONS, 0, 0, &out) <= 0 || out.ended >= 0) break;
}

static int make_roster(unsigned char *out, int k, int n) {
    int w = 0;
    for (int i = 0; i < n; i++) {
        char name[16];
        const int len = snprintf(name, sizeof name, "%s%d", k ? "Bea" : "Al", i);
        out[w++] = (unsigned char)(len & 0xff);
        out[w++] = (unsigned char)(len >> 8);
        memcpy(out + w, name, (size_t)len);
        w += len;
    }
    return w;
}

static void moves_text(const Lane *L, char *moves) {
    replay_b32_encode(L->code, L->code_len, moves, CODE_CAP * 2);
}

// Everything one round does, on whichever thread calls it.
static int build_link(Lane *L, char *moves, char *link) {
    moves_text(L, moves);
    return replay_extras_link(moves, L->roster, L->roster_len, L->n_players, link, LINK_CAP);
}

static void reference(Lane *L, int k) {
    play_out(L, k);
    L->code_len = replay_encode_v6_from_game(&L->game, L->seed, FOOLISH_SEED_LEN, 1 << 30, L->code, CODE_CAP);
    CHECK(L->code_len > 0, "the reference game encodes");
    L->dec_len = replay_decode(L->code, L->code_len, L->dec, DEC_CAP);
    CHECK(L->dec_len > 0, "the reference code decodes");
    L->roster_len = make_roster(L->roster, k, L->n_players);
    L->link_len = build_link(L, L->moves, L->link);
    CHECK(L->link_len > (int)strlen(L->moves) + 1, "the reference link carries its roster");

    MsgEnvelope e;
    msg_envelope_init(&e);
    e.format = MSG_FORMAT_V6;
    e.phase = MSG_PHASE_FINISHED;
    e.game_id = 0x0123456789abcdefULL + (uint64_t)k;
    e.n_players = (uint8_t)L->n_players;
    memcpy(e.seed, L->seed, MSG_SEED_LEN);
    e.n_joins = L->n_players;
    for (int i = 0; i < L->n_players; i++) {
        e.joins[i].seat = (uint8_t)i;
        e.joins[i].name_len = 2;
        e.joins[i].name[0] = (char)('A' + k);
        e.joins[i].name[1] = (char)('0' + i);
    }
    static unsigned char body[N_THREADS][BODY_CAP];
    CHECK(msg_seal(&e, &L->game, MSG_NO_BASE, body[k], BODY_CAP, &g_scratch) == MSG_EOK, "the reference game seals");
    L->wire_len = msg_encode(&e, L->wire, WIRE_CAP);
    CHECK(L->wire_len > 0, "the reference envelope encodes");
    MsgEnvelope d;
    CHECK(msg_decode(L->wire, L->wire_len, &d) == MSG_EOK, "the reference envelope decodes");
    CHECK(msg_replay(&d, &L->replayed) == MSG_EOK, "the reference envelope replays");
}

static atomic_int g_go;

static void *codec_thread(void *arg) {
    const int k = (int)(intptr_t)arg;
    Lane *L = &g_lane[k];
    atomic_fetch_add(&g_go, 1);
    while (atomic_load(&g_go) < N_THREADS) { }

    for (int r = 0; r < ROUNDS; r++) {
        // table_replay_code: encode, then decode what was encoded.
        const int n = replay_encode_v6_from_game(&L->game, L->seed, FOOLISH_SEED_LEN, 1 << 30, L->r_code, CODE_CAP);
        if (n != L->code_len || memcmp(L->r_code, L->code, (size_t)n) != 0) L->bad_code++;
        const int dn = replay_decode(L->code, L->code_len, L->r_dec, DEC_CAP);
        if (dn != L->dec_len || memcmp(L->r_dec, L->dec, (size_t)dn) != 0) L->bad_dec++;
        // The share link and its roster.
        const int ln = build_link(L, L->moves, L->r_link);
        if (ln != L->link_len || strcmp(L->r_link, L->link) != 0) L->bad_link++;
        // The unfurl: msg_decode + msg_replay.
        MsgEnvelope d;
        if (msg_decode(L->wire, L->wire_len, &d) != MSG_EOK
            || msg_replay(&d, &L->r_replayed) != MSG_EOK
            || memcmp(&L->r_replayed, &L->replayed, sizeof(Game)) != 0) L->bad_msg++;
    }
    // The link alone, many more times: it is microseconds, and its scratch is
    // live for only part of each call.
    for (int r = 0; r < LINK_ROUNDS; r++) {
        const int ln = replay_extras_link(L->moves, L->roster, L->roster_len, L->n_players, L->r_link, LINK_CAP);
        if (ln != L->link_len || strcmp(L->r_link, L->link) != 0) L->bad_link++;
    }
    return 0;
}

// ---- part 2: the opening overrides ---------------------------------------------

static atomic_int g_phase;
static void meet(int target) {
    atomic_fetch_add(&g_phase, 1);
    while (atomic_load(&g_phase) < target) { }
}

static Game g_deal[N_THREADS];
static int g_pin_seat = -1;          // the seat thread 0 pins
static int g_fresh_derived = -1;     // the seat thread 1's fresh deal derives, alone
static int g_fresh_opened = -1;      // ...and the seat it opened on during the pin
static int g_derived_seen[N_THREADS], g_derived_want[N_THREADS];
static int g_bare_k = -1;            // a 2-seat deal with no trump in either hand
static int g_bare_rolled = -1;       // the seat it rolls, alone
static int g_bare_opened = -1;
static int g_refused[N_THREADS], g_reason[N_THREADS], g_reason_version[N_THREADS];       // ...and the seat it opened on while another thread forced one

static void deal_fresh(Game *g, int k, int n) {
    unsigned char s[FOOLISH_SEED_LEN];
    seed_of(s, 10 + k);
    game_set_deal_seed_bytes(s, FOOLISH_SEED_LEN);
    memset(g, 0, sizeof *g);
    g->num_players = (int8_t)n;
    for (int i = 0; i < n; i++) g->players[i].status = PLAYER_STATUS_READY;
    start_game(g);
}

static void *opening_thread(void *arg) {
    const int k = (int)(intptr_t)arg;
    // (a) thread 0 pins a seat; thread 1 deals while the pin is up.
    if (k == 0) game_open_at_seat(g_pin_seat);
    meet(2);
    if (k == 1) { deal_fresh(&g_deal[1], 1, 4); g_fresh_opened = g_deal[1].first_attacker; }
    meet(4);
    if (k == 0) game_open_at_seat(-1);
    // (b) each deals its own game, then both read their derived seat.
    deal_fresh(&g_deal[k], k == 0 ? 2 : 3, k == 0 ? 3 : 5);
    g_derived_want[k] = g_deal[k].first_attacker;   // no pin is up: the derived seat
    meet(6);
    g_derived_seen[k] = game_derived_opening();
    // (c) a deal with no trump to derive from rolls its opener, unless a replay
    // rebuild has forced one (replay_deal_start): thread 0 forces, thread 1
    // deals such a deal while the force is up.
    if (k == 0) game_force_first_attacker(1 - g_bare_rolled);
    meet(8);
    if (k == 1) { deal_fresh(&g_deal[1], g_bare_k, 2); g_bare_opened = g_deal[1].first_attacker; }
    meet(10);
    if (k == 0) game_force_first_attacker(-1);
    // (d) a refusal's reason: each thread decodes a code of an old version -
    // a one-byte code is an integer below 16, so its version symbol is the byte
    // itself, 1 on one thread and 2 on the other - and the two meet before
    // either reads its reason back.
    const unsigned char old[1] = { (unsigned char)(k + 1) };
    static unsigned char out[N_THREADS][64];
    g_refused[k] = replay_decode(old, 1, out[k], (int)sizeof out[k]);
    meet(12);
    g_reason[k] = replay_last_error_detail();
    g_reason_version[k] = replay_last_error()->version;
    return 0;
}

int main(void) {
    g_lane[0].n_players = 2;
    g_lane[1].n_players = 4;
    for (int k = 0; k < N_THREADS; k++) reference(&g_lane[k], k);
    CHECK(g_lane[0].code_len != g_lane[1].code_len || memcmp(g_lane[0].code, g_lane[1].code, (size_t)g_lane[0].code_len) != 0,
          "the two threads' games are different games");

    pthread_t th[N_THREADS];
    for (int k = 0; k < N_THREADS; k++)
        CHECK(pthread_create(&th[k], 0, codec_thread, (void *)(intptr_t)k) == 0, "a codec thread starts");
    for (int k = 0; k < N_THREADS; k++) pthread_join(th[k], 0);
    for (int k = 0; k < N_THREADS; k++) {
        char msg[128];
        const Lane *L = &g_lane[k];
        snprintf(msg, sizeof msg, "thread %d: every encode is its own game's code (%d of %d were not)", k, L->bad_code, ROUNDS);
        CHECK(L->bad_code == 0, msg);
        snprintf(msg, sizeof msg, "thread %d: every decode is its own code's stream (%d of %d were not)", k, L->bad_dec, ROUNDS);
        CHECK(L->bad_dec == 0, msg);
        snprintf(msg, sizeof msg, "thread %d: every share link is its own roster's (%d of %d were not)", k, L->bad_link, ROUNDS + LINK_ROUNDS);
        CHECK(L->bad_link == 0, msg);
        snprintf(msg, sizeof msg, "thread %d: every envelope replays to its own game (%d of %d did not)", k, L->bad_msg, ROUNDS);
        CHECK(L->bad_msg == 0, msg);
    }

    // Part 2. The seat thread 1's fresh deal derives on its own, and a pin that
    // is any other seat.
    deal_fresh(&g_deal[1], 1, 4);
    g_fresh_derived = g_deal[1].first_attacker;
    g_pin_seat = (g_fresh_derived + 1) % 4;
    for (int kk = 100; kk < 100000 && g_bare_k < 0; kk++) {
        deal_fresh(&g_deal[0], kk, 2);
        if (game_derived_opening() == -1) { g_bare_k = kk; g_bare_rolled = g_deal[0].first_attacker; }
    }
    CHECK(g_bare_k >= 0, "some 2-seat deal has no trump in either hand");
    deal_fresh(&g_deal[1], 1, 4);   // the main thread's last deal is the fresh one again
    for (int k = 0; k < N_THREADS; k++)
        CHECK(pthread_create(&th[k], 0, opening_thread, (void *)(intptr_t)k) == 0, "an opening thread starts");
    for (int k = 0; k < N_THREADS; k++) pthread_join(th[k], 0);
    {
        char msg[160];
        snprintf(msg, sizeof msg, "a deal opens on its own derived seat while another thread pins one (opened %d, derives %d, pinned %d)",
                 g_fresh_opened, g_fresh_derived, g_pin_seat);
        CHECK(g_fresh_opened == g_fresh_derived, msg);
        CHECK(g_derived_want[0] != g_derived_want[1], "the two deals derive different seats");
        for (int k = 0; k < N_THREADS; k++) {
            snprintf(msg, sizeof msg, "thread %d reads its own deal's derived seat (%d, its deal derived %d)",
                     k, g_derived_seen[k], g_derived_want[k]);
            CHECK(g_derived_seen[k] == g_derived_want[k], msg);
        }
    }
    {
        char msg[160];
        snprintf(msg, sizeof msg, "a trumpless deal rolls its own opener while another thread forces one (opened %d, rolls %d)",
                 g_bare_opened, g_bare_rolled);
        CHECK(g_bare_opened == g_bare_rolled, msg);
        for (int k = 0; k < N_THREADS; k++) {
            snprintf(msg, sizeof msg, "thread %d reads back its own refusal's reason (%d, %d for version %d)",
                     k, g_reason[k], g_reason_version[k], k + 1);
            CHECK(g_refused[k] == -REPLAY_EVERSION && g_reason[k] == k + 1 && g_reason_version[k] == k + 1, msg);
        }
    }
    CHECK(game_derived_opening() == g_fresh_derived, "the main thread still reads its own last deal's derived seat");

    if (g_fail) { fprintf(stderr, "replay_threads_test: %d FAILED\n", g_fail); return 1; }
    printf("replay_threads_test: ok (%d rounds x %d threads; codes %d and %d bytes)\n",
           ROUNDS, N_THREADS, g_lane[0].code_len, g_lane[1].code_len);
    return 0;
}
