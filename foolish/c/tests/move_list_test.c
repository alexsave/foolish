// move_list_test.c - the session's move list (src/move_list.h) and the replay
// it cuts (table.h table_replay_code_list / table_replay_extras_list).
//
// 1. The codec: one exact spelling for a hand-built session, every refusal of
//    the reader, the broken list, the reset at a GAME_START, negative and large
//    time steps.
// 2. THE PROOF: tables played the way the server plays them (a lobby of a human
//    and bots dealt by table_ready, one commit per human move or bot cycle,
//    TableCommit.logs appended to the session log as the server stores it -
//    cleared when the operation dealt or the board is a lobby - and to the move
//    list), with rematches, bundled bot cycles, podkidnoy, 2 to 8 seats and
//    commit clocks that repeat, jump and step back, and imposed openers. At every game's end and at
//    about one commit in twelve mid-game, the code and the extras cut from the
//    move list must be the bytes cut from the session log, or the same refusal.
//
// Usage: move_list_test [games] [seed0]   (defaults 120, 1)
//   ML_VERBOSE=1   one line per mismatch
#include "../src/game.h"
#include "../src/legal.h"
#include "../src/awire.h"
#include "../src/strategy.h"
#include "../src/bot_drive.h"
#include "../src/replay.h"
#include "../src/replay_extras.h"
#include "../src/table.h"
#include "../src/move_list.h"
#include "../../../shared/c/le_bytes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RS(s) (s), (int)strlen(s)

static int g_fail = 0, g_checks = 0;
static void check(int ok, const char *what) {
    g_checks++;
    if (!ok) { g_fail++; fprintf(stderr, "FAIL: %s\n", what); }
}

// ---- 1. the codec ---------------------------------------------------------------

// One session-log record in the stored layout: u48 ms, type, seat, defender, n, pairs.
static int rec(uint8_t *out, int at, int64_t ms, int type, int seat, int n, const uint8_t *pairs) {
    le_put_u48(out + at, (uint64_t)ms);
    out[at + 6] = (uint8_t)type;
    out[at + 7] = (uint8_t)seat;
    out[at + 8] = 0xFF;
    out[at + 9] = (uint8_t)n;
    if (n) memcpy(out + at + 10, pairs, (size_t)(2 * n));
    return at + 10 + 2 * n;
}

static int parse_all(const uint8_t *list, int len) {
    MoveListTail t;
    return move_list_scan(list, len, &t);
}

static void test_codec(void) {
    static uint8_t log[4096], list[4096], bad[4096];
    const uint8_t a1[2] = { 7, 0xFF }, cov[2] = { 20, 7 }, a4[8] = { 1, 0xFF, 14, 0xFF, 27, 0xFF, 40, 0xFF };
    const uint8_t p2[4] = { 2, 0xFF, 15, 0xFF }, pick[4] = { 1, 0xFF, 14, 0xFF }, hid[2] = { 0xFE, 0xFF };
    const int64_t t0 = 1700000000000LL;
    int at = 0;
    at = rec(log, at, t0, LOG_GAME_START, -1, 0, 0);
    at = rec(log, at, t0, LOG_DRAW, 0, 1, hid);
    at = rec(log, at, t0 + 1000, LOG_ATTACK, 0, 1, a1);
    at = rec(log, at, t0 + 1000, LOG_COVER, 1, 1, cov);          // the same commit: no TICK
    at = rec(log, at, t0 + 1000, LOG_GOOD, 0, 0, 0);
    at = rec(log, at, t0 + 1000, LOG_DISCARD, -1, 2, cov);        // after a GOOD: ROUND_END
    at = rec(log, at, t0 + 1000, LOG_DRAW, 0, 1, hid);            // after no GOOD: nothing
    at = rec(log, at, t0 + 1007, LOG_ATTACK, 1, 4, a4);           // 7 ms: a one-byte TICK; 4 cards: the count byte
    at = rec(log, at, t0 + 1007 - 300, LOG_PASS, 0, 2, p2);       // the clock stepped back
    at = rec(log, at, t0 + 1007 - 300, LOG_PICKUP, 1, 2, pick);
    at = rec(log, at, t0 + 1007 - 300, LOG_GOOD, 2, 0, 0);
    at = rec(log, at, t0 + 1007 - 300, LOG_DEFENDER_CHANGE, 1, 0, 0);   // after a GOOD: CUT
    at = rec(log, at, t0 + 1007 - 300, LOG_GOOD, 1, 0, 0);        // pending at the end
    const int len = table_move_list_append(list, 0, sizeof(list), log, at);
    // The expected bytes, spelled out by hand from move_list.h.
    uint8_t exp[64];
    int e = 0;
    exp[e++] = MOVE_LIST_FORMAT;
    {   // TICK of zigzag(t0)
        uint64_t z = (uint64_t)t0 << 1;
        exp[e++] = (uint8_t)(0xC0 | 16 | (z & 15));
        for (z >>= 4; z; z >>= 7) exp[e++] = (uint8_t)((z & 0x7f) | (z > 0x7f ? 0x80 : 0));
    }
    exp[e++] = 0x00;                            // START
    exp[e++] = 0xC0 | 16 | ((2000 & 15));       // TICK +1000 ms: z 2000, low nibble 0
    exp[e++] = 2000 >> 4;                       // 125
    exp[e++] = 1 << 5 | 0 << 3 | 0; exp[e++] = 7;               // ATTACK seat 0, one card
    exp[e++] = 2 << 5 | 1; exp[e++] = 20; exp[e++] = 7;         // COVER seat 1
    exp[e++] = 5 << 5 | 0;                                      // GOOD seat 0
    exp[e++] = 0x01;                                            // ROUND_END
    exp[e++] = 0xC0 | 14;                                       // TICK +7: z 14
    exp[e++] = 1 << 5 | 3 << 3 | 1; exp[e++] = 4;               // ATTACK seat 1, n byte
    exp[e++] = 1; exp[e++] = 14; exp[e++] = 27; exp[e++] = 40;
    exp[e++] = 0xC0 | 16 | (599 & 15); exp[e++] = 599 >> 4;     // TICK -300: z 599
    exp[e++] = 3 << 5 | 1 << 3 | 0; exp[e++] = 2; exp[e++] = 15; // PASS seat 0, two cards
    exp[e++] = 4 << 5 | 1;                                      // PICKUP seat 1
    exp[e++] = 5 << 5 | 2;                                      // GOOD seat 2
    exp[e++] = 0x02;                                            // CUT
    exp[e++] = 5 << 5 | 1;                                      // GOOD seat 1
    check(len == e && memcmp(list, exp, (size_t)e) == 0, "codec: a session's list is exactly its spelling");
    if (len != e || memcmp(list, exp, (size_t)e) != 0) {
        fprintf(stderr, "  got %d:", len);
        for (int i = 0; i < len; i++) fprintf(stderr, " %02x", list[i]);
        fprintf(stderr, "\n  want %d:", e);
        for (int i = 0; i < e; i++) fprintf(stderr, " %02x", exp[i]);
        fprintf(stderr, "\n");
    }

    // The reader gives back every item, every time and the tail.
    MoveListCursor c;
    MoveListItem it;
    int kinds[32], n = 0, r;
    int64_t times[32];
    move_list_open(&c, list, len);
    while ((r = move_list_next(&c, &it)) > 0 && n < 32) { times[n] = it.ms; kinds[n++] = it.kind; }
    const int wk[] = { MOVE_LIST_START, LOG_ATTACK, LOG_COVER, LOG_GOOD, MOVE_LIST_ROUND_END, LOG_ATTACK, LOG_PASS,
                       LOG_PICKUP, LOG_GOOD, MOVE_LIST_CUT, LOG_GOOD };
    int same = r == 0 && n == 11;
    for (int i = 0; same && i < n; i++) same = kinds[i] == wk[i];
    check(same, "codec: the reader gives back the items in order");
    check(n == 11 && times[0] == t0 && times[1] == t0 + 1000 && times[2] == t0 + 1000 && times[5] == t0 + 1007
          && times[6] == t0 + 707, "codec: and the exact millisecond of every item, equal ones equal");
    MoveListTail tl;
    check(move_list_scan(list, len, &tl) == 0 && tl.ms == t0 + 707 && tl.last_good && !tl.broken,
          "codec: the tail is the last time and a pending GOOD");

    // Appending commit by commit is appending the whole log at once.
    int l2 = 0, q = 0;
    while (q < at) {
        const int k = 10 + 2 * log[q + 9];
        l2 = table_move_list_append(list + 2048, l2, 2048, log + q, k);
        q += k;
    }
    check(l2 == len && memcmp(list + 2048, list, (size_t)len) == 0, "codec: record by record is the same list");

    // A GAME_START starts the list again.
    uint8_t gs[10];
    rec(gs, 0, 5, LOG_GAME_START, -1, 0, 0);
    memcpy(bad, list, (size_t)len);
    const int l3 = table_move_list_append(bad, len, sizeof(bad), gs, 10);
    check(l3 == 3 && bad[0] == MOVE_LIST_FORMAT && bad[1] == (0xC0 | 10) && bad[2] == 0x00,
          "codec: a GAME_START starts a new list");

    // Every refusal of the reader.
    check(parse_all(list, len) == 0 && parse_all(list, 0) == 0, "codec: the list and the empty list read");
    struct { const char *what; uint8_t b[8]; int n; } refused[] = {
        { "an unknown format byte", { 2, 0x00 }, 2 },
        { "a broken list with bytes after it", { MOVE_LIST_BROKEN, 0x00 }, 2 },
        { "a TICK of zero", { 1, 0xC0, 0x00 }, 3 },
        { "a TICK whose LEB128 ends in a zero byte", { 1, 0xD2, 0x81, 0x00, 0x20, 0x07 }, 6 },
        { "a TICK before a GOOD", { 1, 0xC2, 0xA0 }, 3 },
        { "a TICK at the end", { 1, 0xC2 }, 2 },
        { "two TICKs", { 1, 0xC2, 0xC2, 0x00 }, 4 },
        { "a ROUND_END after no GOOD", { 1, 0x20, 0x07, 0x01 }, 4 },
        { "a CUT first", { 1, 0x02 }, 2 },
        { "a START after an item", { 1, 0x20, 0x07, 0x00 }, 4 },
        { "an unknown mark", { 1, 0x03 }, 2 },
        { "kind 7", { 1, 0xE0 }, 2 },
        { "a card past 51", { 1, 0x20, 52 }, 3 },
        { "a count byte under 4", { 1, 0x38, 3, 1, 2, 3 }, 6 },
        { "a COVER with count bits", { 1, 0x48, 1, 2 }, 4 },
        { "a GOOD with count bits", { 1, 0xA8 }, 2 },
        { "a cut-off attack", { 1, 0x28, 1 }, 3 },
        { "a time below zero", { 1, 0xC1, 0x20, 0x07 }, 4 },
    };
    for (int i = 0; i < (int)(sizeof(refused) / sizeof(refused[0])); i++) {
        char what[96];
        snprintf(what, sizeof(what), "codec: the reader refuses %s", refused[i].what);
        check(parse_all(refused[i].b, refused[i].n) == MOVE_LIST_E_PARSE, what);
    }
    const uint8_t ok_time[] = { 1, 0xC2, 0x20, 0x07, 0xC1, 0x28, 1, 2 };   // +1 then -1: back to 0
    check(parse_all(ok_time, sizeof(ok_time)) == 0, "codec: a step back to zero reads");
    const uint8_t ok_leb[] = { 1, 0xD2, 0x01, 0x20, 0x07 };   // the same +9 ms spelled minimally
    check(parse_all(ok_leb, sizeof(ok_leb)) == 0, "codec: a minimal LEB128 TICK reads");

    // The broken list: a hidden card where an atom needs one, a seat past 7, a full buffer.
    uint8_t one[20];
    int b1 = rec(one, 0, 9, LOG_ATTACK, 0, 1, hid);
    check(table_move_list_append(bad, 0, sizeof(bad), one, b1) == 1 && bad[0] == MOVE_LIST_BROKEN,
          "codec: an attack with a hidden card breaks the list");
    b1 = rec(one, 0, 9, LOG_GOOD, 9, 0, 0);
    check(table_move_list_append(bad, 0, sizeof(bad), one, b1) == 1 && bad[0] == MOVE_LIST_BROKEN,
          "codec: a seat past 7 breaks the list");
    check(table_move_list_append(bad, 0, 12, log, at) == 1 && bad[0] == MOVE_LIST_BROKEN,
          "codec: a list past its room is broken");
    b1 = rec(one, 0, 9, LOG_ATTACK, 0, 1, a1);
    check(table_move_list_append(bad, 1, sizeof(bad), one, b1) == 1 && bad[0] == MOVE_LIST_BROKEN,
          "codec: a broken list stays broken");
    check(table_move_list_append(bad, 1, sizeof(bad), gs, 10) == 3 && bad[0] == MOVE_LIST_FORMAT,
          "codec: until the next GAME_START");
    check(table_move_list_append(bad, 0, 0, one, b1) == TABLE_E_CAP, "codec: no room at all is refused");
    check(table_move_list_append(bad, 0, sizeof(bad), log, at - 1) == TABLE_E_WIRE, "codec: records cut short are refused");
}

// ---- 2. the proof -----------------------------------------------------------------

static Game g_game;
static TableSnaps g_snaps;
static Table g_t;
static uint8_t g_arena[1 << 20];
static uint8_t g_state[8192], g_roster[ROSTER_BYTES];
static int g_state_len;
static uint8_t g_log[1 << 18], g_list[MOVE_LIST_MAX];
static int g_log_len, g_list_len;
static uint8_t g_code_a[1 << 16], g_code_b[1 << 16], g_ex_a[1 << 16], g_ex_b[1 << 16], g_scratch[1 << 21];
static BotDriveOut g_drv;
static LegalMoves g_moves;
static uint64_t g_rng;
static uint32_t g_version;
static int64_t g_now;
static int g_verbose;

static uint32_t rnd(void) {
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17;
    return (uint32_t)(g_rng >> 11);
}

// The commit clock: mostly a bot's or a person's pace, sometimes the same
// millisecond, sometimes a long pause, now and then a step back.
static void tick(void) {
    const uint32_t r = rnd() % 100;
    if (r < 12) return;
    if (r < 20) g_now += 1 + rnd() % 40;
    else if (r < 85) g_now += 250 + rnd() % 3000;
    else if (r < 96) g_now += 5000 + rnd() % 120000;
    else if (r < 98) g_now += 3600000LL * (1 + rnd() % 30);
    else g_now -= 1 + rnd() % 900;
}

static int commit(TableCommit *c) {
    const int n = table_commit_products(&g_t, RS("g"), ++g_version, g_now, c, g_arena, sizeof(g_arena));
    if (n < 0) return n;
    memcpy(g_state, g_arena + c->state.off, (size_t)c->state.len);
    g_state_len = c->state.len;
    memcpy(g_roster, g_arena + c->roster.off, ROSTER_BYTES);
    // As the server stores the session (table_ops.c section_row): a commit that
    // dealt, or leaves a lobby, starts it again.
    if (c->logs_reset || c->status == GAME_STATUS_WAITING) { g_log_len = 0; g_list_len = 0; }
    if (g_log_len + c->logs.len > (int)sizeof(g_log)) return -1;
    memcpy(g_log + g_log_len, g_arena + c->logs.off, (size_t)c->logs.len);
    g_log_len += c->logs.len;
    g_list_len = table_move_list_append(g_list, g_list_len, sizeof(g_list), g_arena + c->logs.off, c->logs.len);
    return g_list_len < 0 ? g_list_len : n;
}

static int reload(void) {
    if (table_load(&g_t, g_state, g_state_len, g_roster, ROSTER_BYTES) != TABLE_OK) return -1;
    table_set_deal_seed(&g_t, "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff", 64);
    if (table_bots_need_logs(&g_t)) table_set_session_log(&g_t, g_log, g_log_len);
    return 0;
}

static int human_move(void) {
    if (!should_bot_act(&g_game, 0)) return 0;
    calculate_legal_moves(&g_game, 0, &g_moves);
    if (g_moves.n == 0) return 0;
    const int pick = rnd() % 3 == 0 ? (int)(rnd() % (uint32_t)g_moves.n)
                                    : handwritten_strategy_choose(&g_game, 0, &g_moves, 0);
    const LegalMove *m = &g_moves.moves[pick];
    AwireAction a;
    memset(&a, 0, sizeof(a));
    a.kind = m->type; a.n = m->n_cards;
    for (int i = 0; i < m->n_cards; i++) { a.cards[i] = m->cards[i]; a.attacks[i] = m->attack_cards[i]; }
    uint8_t buf[256];
    const int wl = awire_encode(&a, buf, sizeof(buf));
    return table_act(&g_t, RS("h"), buf, wl, -1, 0) == TABLE_APPLIED;
}

typedef struct {
    int compared, mid, code_same, code_refused_same, extras_same, longer, mismatch;
    long list_bytes, log_bytes, moves, games_by_np[MAX_PLAYERS + 1];
    long list_by_np[MAX_PLAYERS + 1], log_by_np[MAX_PLAYERS + 1], moves_by_np[MAX_PLAYERS + 1];
    int max_list, forced, podkidnoy, pending_good, bundled, gated, log_short;
} Stats;
static Stats S;

static int count_moves(void) {
    MoveListCursor c;
    MoveListItem it;
    int n = 0;
    if (move_list_open(&c, g_list, g_list_len) != 0) return 0;
    while (move_list_next(&c, &it) > 0)
        if (it.kind == LOG_ATTACK || it.kind == LOG_COVER || it.kind == LOG_PASS || it.kind == LOG_PICKUP) n++;
    return n;
}

// A build at the compact arena caps (MAX_LOG_PAIRS 16: the native tests and the
// coverage build, not the server) logs a pickup or discard of more cards SHORT,
// and the session log's own gate then refuses the game: a fact about the log at
// these caps, which the list (no pickup cards, no discard) does not share. Only
// such a log may tell the two apart, and never at the server's caps.
static int log_cut_short(void) {
    if (MAX_LOG_PAIRS >= REPLAY_MAX_PAIRS) return 0;
    for (int at = 0; at + 10 <= g_log_len; at += 10 + 2 * g_log[at + 9])
        if (g_log[at + 9] >= MAX_LOG_PAIRS) return 1;
    return 0;
}

// The code and the extras both ways, on the row as committed.
static void compare(const uint8_t *seed, int game, int end) {
    if (reload() < 0) { S.mismatch++; return; }
    const int lb = table_replay_code_list(&g_t, seed, FOOLISH_SEED_LEN, g_list, g_list_len, g_code_b, sizeof(g_code_b),
                                          g_scratch, sizeof(g_scratch));
    const int eb = table_replay_extras_list(&g_t, g_list, g_list_len, g_ex_b, sizeof(g_ex_b));
    if (reload() < 0) { S.mismatch++; return; }
    const int la = table_replay_code(&g_t, seed, FOOLISH_SEED_LEN, g_log, g_log_len, g_code_a, sizeof(g_code_a),
                                     g_scratch, sizeof(g_scratch));
    const int ea = table_replay_extras(&g_t, g_log, g_log_len, g_ex_a, sizeof(g_ex_a));
    S.compared++;
    if (!end) S.mid++;
    int ok = 1;
    if (la > 0 && lb == la && memcmp(g_code_a, g_code_b, (size_t)la) == 0) {
        S.code_same++;
        ReplayHeader hdr;
        memset(&hdr, 0, sizeof hdr);
        if (replay_decode_atoms_v6(g_code_a, la, &hdr, NULL, NULL) == REPLAY_EOK) {
            if (end && hdr.forced_opening) S.forced++;
            if (end && !hdr.pass_allowed) S.podkidnoy++;
        }
        MoveListCursor mc;
        MoveListItem mi;
        int last = -1;
        if (move_list_open(&mc, g_list, g_list_len) == 0) while (move_list_next(&mc, &mi) > 0) last = mi.kind;
        if (last == LOG_GOOD) S.pending_good++;
    }
    else if (la <= 0 && lb == la) S.code_refused_same++;
    else if (la == -REPLAY_ETOOLONG && lb > 0) S.longer++;   // the log's MAX_LOGS ceiling, which the list has not
    else if (la == TABLE_E_REPLAY_VERIFY && lb > 0 && log_cut_short()) S.log_short++;
    else ok = 0;
    if (ea == eb && (ea <= 0 || memcmp(g_ex_a, g_ex_b, (size_t)ea) == 0)) S.extras_same++;
    else ok = 0;
    if (!ok) {
        S.mismatch++;
        if (g_verbose || S.mismatch <= 5)
            fprintf(stderr, "  game %d (%d seats, %s): code log %d list %d, extras log %d list %d\n", game,
                    g_game.num_players, end ? "end" : "mid", la, lb, ea, eb);
    }
    // THE GATE: a PASS handed to another seat encodes the same (the encoder
    // reads only the defender's cards) and must not decode back to the list.
    if (end && lb > 0 && S.gated < 50) {
        MoveListCursor mc;
        MoveListItem mi;
        int head = -1;
        if (move_list_open(&mc, g_list, g_list_len) == 0)
            while (head < 0 && move_list_next(&mc, &mi) > 0)
                if (mi.kind == LOG_PASS && mi.n <= 3) head = mc.at - mi.n - 1;
        if (head > 0) {
            static uint8_t forged[MOVE_LIST_MAX];
            memcpy(forged, g_list, (size_t)g_list_len);
            const int seat = forged[head] & 7;
            forged[head] = (uint8_t)((forged[head] & ~7) | (seat + 1) % g_game.num_players);
            const int gate = table_replay_code_list(&g_t, seed, FOOLISH_SEED_LEN, forged, g_list_len, g_code_b,
                                                    sizeof(g_code_b), g_scratch, sizeof(g_scratch));
            if (gate == TABLE_E_REPLAY_VERIFY) S.gated++;
            else { S.mismatch++; fprintf(stderr, "  game %d: a PASS by the wrong seat came back %d\n", game, gate); }
        }
    }
    if (end) {
        const int np = g_game.num_players, mv = count_moves();
        S.games_by_np[np]++;
        S.list_by_np[np] += g_list_len; S.log_by_np[np] += g_log_len; S.moves_by_np[np] += mv;
        S.list_bytes += g_list_len; S.log_bytes += g_log_len; S.moves += mv;
        if (g_list_len > S.max_list) S.max_list = g_list_len;
    }
}

static const char *const BRAINS[] = { "random", "handwritten", "simple_heuristic" };

static void seed_of(uint8_t *seed, int game, int deal) {
    uint64_t x = 0x9E3779B97F4A7C15ULL * (uint64_t)(game * 7 + deal + 1);
    for (int i = 0; i < FOOLISH_SEED_LEN; i++) { x ^= x >> 29; x *= 0xBF58476D1CE4E5B9ULL; seed[i] = (uint8_t)(x >> 56); }
}

static void play_table(int game) {
    TableCommit c;
    const int np = 2 + game % 7;
    const int deals = 1 + game % 3;
    uint8_t seed[FOOLISH_SEED_LEN];
    g_rng = 0x2545F4914F6CDD1DULL ^ ((uint64_t)game * 0x9E3779B97F4A7C15ULL);
    g_now = 1700000000000LL + (int64_t)game * 86400000LL;
    g_version = 0;
    g_log_len = g_list_len = 0;
    seed_of(seed, game, 0);
    table_init(&g_t, &g_game, &g_snaps);
    table_create(&g_t, RS("h"), RS("Human"));
    if ((game / 7) % 2) table_set_rules(&g_t, RS("h"), 0);
    for (int b = 1; b < np; b++) {
        char id[8], name[16];
        snprintf(id, sizeof(id), "b%d", b);
        snprintf(name, sizeof(name), "Bot %d", b);
        const char *brain = BRAINS[(game + b) % 3];
        table_add_bot(&g_t, RS("h"), RS(id), RS(name), brain, (int)strlen(brain), seed);
    }
    if (commit(&c) < 0) { S.mismatch++; return; }
    for (int d = 0; d < deals; d++) {
        seed_of(seed, game, d);
        if (reload() < 0) { S.mismatch++; return; }
        if (d > 0) {
            if (table_continue(&g_t, RS("h")) != TABLE_OK) break;
            tick();
            if (commit(&c) < 0) { S.mismatch++; return; }
            if (reload() < 0) { S.mismatch++; return; }
        }
        game_set_seed(1);
        // An imposed opener (the fool's penalty, which only the iMessage chain
        // deals today) on one deal in four: the list records it as its first
        // ATTACK's seat and the encoder tells it from the deal's.
        if (rnd() % 4 == 0) game_open_at_seat((int)(rnd() % (uint32_t)np));
        const int ready = table_ready(&g_t, RS("h"), seed);
        game_open_at_seat(-1);
        if (ready != TABLE_OK || !g_t.dealt_now) { S.mismatch++; return; }
        tick();
        if (commit(&c) < 0) { S.mismatch++; return; }
        for (int step = 0; step < 8000; step++) {
            if (reload() < 0 || g_game.status != GAME_STATUS_PLAYING) break;
            const int bots = bot_drive_eligible_mask(&g_game, game_human_mask(&g_game)) != 0;
            int acted = 0;
            if (!bots || (should_bot_act(&g_game, 0) && rnd() % 2)) acted = human_move();
            if (!acted && bots) {
                const int na = table_bot_drive(&g_t, 0, 0, rnd() % 4 == 0 ? 0 : 1, &g_drv);
                acted = na > 0;
                if (na > 1) S.bundled++;
            }
            if (!acted) break;
            tick();
            if (commit(&c) < 0) { S.mismatch++; return; }
            if (rnd() % 12 == 0 && c.status == GAME_STATUS_PLAYING) compare(seed, game, 0);
        }
        if (reload() < 0 || g_game.status != GAME_STATUS_GAME_OVER) { S.mismatch++; return; }
        compare(seed, game, 1);
    }
}

int main(int argc, char **argv) {
    const int games = argc > 1 ? atoi(argv[1]) : 120;
    const int seed0 = argc > 2 ? atoi(argv[2]) : 1;
    g_verbose = getenv("ML_VERBOSE") != 0;
    test_codec();
    memset(&S, 0, sizeof S);
    for (int g = seed0; g < seed0 + games; g++) play_table(g);
    printf("move list: %d cuts compared (%d mid-game, %d at a game's end) over %d tables\n", S.compared, S.mid,
           S.compared - S.mid, games);
    printf("  code identical %d, the same refusal %d, the list has a code past the log's MAX_LOGS %d; extras identical %d; "
           "mismatches %d\n", S.code_same, S.code_refused_same, S.longer, S.extras_same, S.mismatch);
    for (int np = 2; np <= MAX_PLAYERS; np++) {
        if (!S.games_by_np[np]) continue;
        printf("  %d seats: %ld games, list %ld B, log %ld B per game, %.2f list B per move\n", np, S.games_by_np[np],
               S.list_by_np[np] / S.games_by_np[np], S.log_by_np[np] / S.games_by_np[np],
               S.moves_by_np[np] ? (double)S.list_by_np[np] / (double)S.moves_by_np[np] : 0.0);
    }
    printf("  all: list %.2f B per move (log %.2f), longest list %d B\n",
           S.moves ? (double)S.list_bytes / (double)S.moves : 0.0, S.moves ? (double)S.log_bytes / (double)S.moves : 0.0,
           S.max_list);
    if (S.log_short) printf("  (at these caps, MAX_LOG_PAIRS %d: %d cuts the session log's gate refused, its pickup logged short)\n",
                            MAX_LOG_PAIRS, S.log_short);
    printf("  exercised: %d forced openers (the fool's penalty), %d podkidnoy games, %d cuts ending on a pending GOOD, "
           "%d bundled bot cycles\n", S.forced, S.podkidnoy, S.pending_good, S.bundled);
    check(S.mismatch == 0, "proof: every cut from the move list is the session log's, code and extras");
    check(games < 60 || S.gated > 0, "proof: a list whose PASS names another seat fails the round-trip gate");
    check(games < 60 || (S.forced > 0 && S.podkidnoy > 0 && S.pending_good > 0 && S.bundled > 0),
          "proof: forced openers, podkidnoy, pending GOODs and bundled cycles were all compared");
    check(S.compared - S.mid >= games && S.code_same >= games / 2, "proof: whole games were compared (else this proves nothing)");
    printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
