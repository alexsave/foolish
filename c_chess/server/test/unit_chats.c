// unit_chats.c - chat through the state and the journal without a server: which rooms exist (a
// game in memory, an arena in memory, the lobby), a room keeps its last 64 lines, the refusals (a
// guest, a line the kernel refuses, a room that is not), the floor of one line a second and the
// burst of 30 a minute, three banned words in ten minutes mute for ten, the console's mute, a mute
// kept through a restart from the journal, and a game's lines leaving memory with the game.
#include "cc_server.h"
#include "arenas.h"
#include "chats.h"
#include "journal.h"
#include "config.h"
#include "store/store.h"
#include "util/err.h"
#include "../../c/test/t.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static cb_ctx g_ctx;
static cb_config g_cfg;
static char g_dir[256];

static void open_state(void) {
    char err[256];
    CHECK(cc_state_init(&g_ctx) == 0, "state");
    CHECK(cc_journal_recover(&g_ctx, err, sizeof err) == CB_OK, "%s", err);
    CHECK(cc_journal_start(&g_ctx) == CB_OK, "journal");
    cc_arena_schedule(false);
}

static void close_state(void) {
    cc_journal_sync();
    cc_state_free();
    cc_journal_stop();
}

#define NP 6
static const char *const NAMES[NP] = { "ann", "ben", "cat", "dan", "Guest-zz", "eve" };
static cc_player *P[NP];
static char IDS[NP][CB_UUID_LEN + 1];

static void players(void) {
    for (int i = 0; i < NP; i++) {
        snprintf(IDS[i], sizeof IDS[i], "0%d000000-0000-4000-8000-00000000000%d", i, i);
        P[i] = cc_player_of(IDS[i], NAMES[i]);
    }
}

static void reload_players(void) {
    for (int i = 0; i < NP; i++) P[i] = cc_player_get(IDS[i]);
}

static int64_t T, W;   // the test's mono and wall clocks, moved by hand
static uint32_t g_game, g_arena;
static char g_room[CW_ROOM], a_room[CW_ROOM];

static cw_chat_req req(const char *room, const char *text) {
    cw_chat_req q;
    memset(&q, 0, sizeof q);
    memcpy(q.room, room, strlen(room));
    size_t n = strlen(text);
    q.len = (uint8_t)(n > 255 ? 255 : n);
    memcpy(q.text, text, n > CW_CHAT_MAX ? CW_CHAT_MAX : n);
    return q;
}

static uint32_t g_wait;
static int g_why;
static int say(int who, const char *room, const char *text) {
    cw_chat_req q = req(room, text);
    return cc_chat_post(P[who], &q, T, W, &g_wait, &g_why);
}

static uint8_t LOG[CW_CHAT_LOG_MAX];
static cw_chat_log log_of(const char *room, size_t *len) {
    cw_chat_log h;
    memset(&h, 0, sizeof h);
    *len = cc_chat_log(room, LOG);
    if (*len >= sizeof h) memcpy(&h, LOG, sizeof h);
    return h;
}
static cw_chat_line line_at(int i) {
    cw_chat_line l;
    memcpy(&l, LOG + sizeof(cw_chat_log) + (size_t)i * sizeof l, sizeof l);
    return l;
}

static void rooms(void) {
    pthread_mutex_lock(&cc_lock);
    T = cc_mono();
    W = 1800000000000;
    players();
    cw_seek tc = { 180, 2, 1 };
    cc_live *l = cc_live_start(P[0], P[1], &tc, T);
    g_game = l->id;
    snprintf(g_room, sizeof g_room, "g%u", g_game);
    cw_arena_req aq;
    memset(&aq, 0, sizeof aq);
    aq.base_s = 60;
    aq.start_in_s = 60;
    aq.duration_s = 600;
    memcpy(aq.name, "Chat Arena", 10);
    cc_arena *a = NULL;
    CHECK(cc_arena_create(P[0], &aq, W, &a) == CW_E_NONE && a, "arena");
    g_arena = a ? a->id : 0;
    snprintf(a_room, sizeof a_room, "a%u", g_arena);

    TEST("the lobby, a game in memory and an arena in memory are rooms");
    CHECK(cc_chat_room_ok("lobby"), "lobby");
    CHECK(cc_chat_room_ok(g_room), "%s", g_room);
    CHECK(cc_chat_room_ok(a_room), "%s", a_room);
    TEST("anything else is not");
    char other[CW_ROOM];
    snprintf(other, sizeof other, "g%u", g_game + 1);
    CHECK(!cc_chat_room_ok(other), "a game that is not: %s", other);
    snprintf(other, sizeof other, "a%u", g_arena + 1);
    CHECK(!cc_chat_room_ok(other), "an arena that is not: %s", other);
    snprintf(other, sizeof other, "g0%u", g_game);
    CHECK(!cc_chat_room_ok(other), "a leading zero: %s", other);
    CHECK(!cc_chat_room_ok("g0") && !cc_chat_room_ok("g") && !cc_chat_room_ok("") && !cc_chat_room_ok("lobbyx") && !cc_chat_room_ok("Lobby"), "names");
    CHECK(!cc_chat_room_ok("g4294967297") && !cc_chat_room_ok("x1") && !cc_chat_room_ok("g1a"), "more names");
    size_t len;
    log_of(other, &len);
    CHECK(len == 0, "the log of no room: %zu", len);
    cw_chat_log h = log_of(a_room, &len);
    CHECK(len == sizeof h && h.n == 0, "an arena nobody has talked in: %zu bytes, %u lines", len, h.n);
    pthread_mutex_unlock(&cc_lock);
}

static void saying(void) {
    pthread_mutex_lock(&cc_lock);
    TEST("a line is said: in the room's log with its sayer, time and room");
    CHECK(say(0, g_room, "gl hf") == CW_E_NONE, "said");
    size_t len;
    cw_chat_log h = log_of(g_room, &len);
    CHECK(h.n == 1 && len == sizeof h + sizeof(cw_chat_line), "%u lines, %zu bytes", h.n, len);
    cw_chat_line l = line_at(0);
    CHECK(l.v == CW_V && l.kind == CW_P_CHAT && l.len == 5 && memcmp(l.text, "gl hf", 5) == 0 && l.text[5] == 0, "the line");
    CHECK(strcmp(l.name, "ann") == 0 && strcmp(l.room, g_room) == 0 && l.at == W, "who, where, when: %s %s %lld", l.name, l.room, (long long)l.at);
    T += 1000; W += 1000;
    CHECK(say(1, g_room, "you too") == CW_E_NONE, "the other player");
    CHECK(say(2, g_room, "watching") == CW_E_NONE, "a spectator: one chat for players and spectators");
    h = log_of(g_room, &len);
    CHECK(h.n == 3 && strcmp(line_at(2).name, "cat") == 0, "three lines, oldest first");

    TEST("refused: a guest, a line the kernel refuses, a room that is not");
    CHECK(say(4, g_room, "hello") == CW_E_CHAT_GUEST, "a guest");
    char long_line[CW_CHAT_MAX + 2];
    memset(long_line, 'x', sizeof long_line - 1);
    long_line[CW_CHAT_MAX + 1] = 0;
    cw_chat_req q = req(g_room, long_line);
    q.len = CW_CHAT_MAX + 1;
    CHECK(cc_chat_post(P[3], &q, T, W, &g_wait, &g_why) == CW_E_CHAT_TEXT && g_why == CC_CHAT_LONG, "141 bytes: why %d", g_why);
    CHECK(say(3, g_room, "a\tb") == CW_E_CHAT_TEXT && g_why == CC_CHAT_CONTROL, "a tab: why %d", g_why);
    CHECK(say(3, g_room, "") == CW_E_CHAT_TEXT && g_why == CC_CHAT_EMPTY, "nothing");
    CHECK(say(3, "g999999", "hi") == CW_E_CHAT_ROOM, "no such game");
    q = req("lobby", "hi");
    memset(q.room, 'x', CW_ROOM);
    CHECK(cc_chat_post(P[3], &q, T, W, &g_wait, &g_why) == CW_E_CHAT_ROOM, "a room with no end");
    h = log_of(g_room, &len);
    CHECK(h.n == 3, "nothing refused was kept: %u", h.n);

    TEST("one line a second");
    T += 1000; W += 1000;
    CHECK(say(0, "lobby", "now") == CW_E_NONE, "the lobby");
    T += 500;
    CHECK(say(0, "lobby", "again") == CW_E_CHAT_FAST && g_wait == 1, "half a second later: wait %u", g_wait);
    T += 500;
    CHECK(say(0, "lobby", "again") == CW_E_NONE, "a second later");
    pthread_mutex_unlock(&cc_lock);
}

static void ring(void) {
    pthread_mutex_lock(&cc_lock);
    TEST("a room keeps its last 64 lines: the 65th drops the first");
    for (int i = 0; i < 65; i++) {
        T += 2000; W += 2000;   // under the burst's rate too
        char t[16];
        snprintf(t, sizeof t, "line %d", i);
        CHECK(say(3, a_room, t) == CW_E_NONE, "line %d", i);
    }
    size_t len;
    cw_chat_log h = log_of(a_room, &len);
    CHECK(h.n == CW_CHAT_LOG && len == CW_CHAT_LOG_MAX, "%u lines, %zu bytes", h.n, len);
    cw_chat_line first = line_at(0), last = line_at(CW_CHAT_LOG - 1);
    CHECK(strcmp(first.text, "line 1") == 0, "the oldest kept: %s", first.text);
    CHECK(strcmp(last.text, "line 64") == 0, "the newest: %s", last.text);
    T += 2000; W += 2000;
    CHECK(say(3, a_room, "line 65") == CW_E_NONE, "one more");
    h = log_of(a_room, &len);
    CHECK(h.n == CW_CHAT_LOG && strcmp(line_at(0).text, "line 2") == 0 && strcmp(line_at(63).text, "line 65") == 0, "%s .. %s",
          line_at(0).text, line_at(63).text);
    pthread_mutex_unlock(&cc_lock);
}

static void burst(void) {
    pthread_mutex_lock(&cc_lock);
    TEST("30 lines a minute (c_base's CB_LIMIT_CHAT_USER), the floor kept");
    int said = 0, fast = 0;
    for (int i = 0; i < 31; i++) {
        T += 1000;   // a second apart on the mono clock, the same minute on the wall's
        int rc = say(2, "lobby", "spam");
        said += rc == CW_E_NONE;
        if (rc == CW_E_CHAT_FAST) fast++;
    }
    CHECK(said == 30 && fast == 1 && g_wait >= 1, "said %d, refused %d, wait %u", said, fast, g_wait);
    pthread_mutex_unlock(&cc_lock);
}

static void words(void) {
    pthread_mutex_lock(&cc_lock);
    TEST("a banned word is refused and not kept");
    T += 1000; W += 1000;
    size_t len;
    cw_chat_log before = log_of(g_room, &len);
    CHECK(say(1, g_room, "you fucking idiot") == CW_E_CHAT_WORDS, "the first");
    CHECK(log_of(g_room, &len).n == before.n, "kept");
    TEST("three in ten minutes mute for ten");
    T += 60000; W += 60000;
    CHECK(say(1, g_room, "sh1t") == CW_E_CHAT_WORDS, "the second");
    T += 8 * 60000; W += 8 * 60000;
    CHECK(say(1, g_room, "b1tch") == CW_E_CHAT_MUTED && g_wait == 600, "the third: wait %u", g_wait);
    CHECK(P[1]->mute_until == W + CC_CHAT_TIMEOUT_MS, "mute_until");
    T += 1000; W += 1000;
    CHECK(say(1, g_room, "sorry") == CW_E_CHAT_MUTED && g_wait == 599, "muted: a clean line too, wait %u", g_wait);
    T += 599000; W += 599000;
    CHECK(say(1, g_room, "sorry") == CW_E_NONE, "ten minutes later");
    TEST("three spread over more than ten minutes do not mute");
    T += 1000; W += 1000;
    CHECK(say(3, "lobby", "fuck") == CW_E_CHAT_WORDS, "one");
    T += 5 * 60000; W += 5 * 60000;
    CHECK(say(3, "lobby", "fuck") == CW_E_CHAT_WORDS, "two");
    T += 5 * 60000 + 1; W += 5 * 60000 + 1;
    CHECK(say(3, "lobby", "fuck") == CW_E_CHAT_WORDS && P[3]->mute_until == 0, "three, the first past ten minutes");
    TEST("two banned words a few seconds after boot do not mute: an empty slot is no strike");
    int64_t t = T;
    T = 5000;   // the monotonic clock of a server just started
    CHECK(say(5, "lobby", "fuck") == CW_E_CHAT_WORDS, "one");
    T = 6000;
    CHECK(say(5, "lobby", "fuck") == CW_E_CHAT_WORDS && P[5]->mute_until == 0, "two");
    T = t;
    pthread_mutex_unlock(&cc_lock);
}

static void console(void) {
    pthread_mutex_lock(&cc_lock);
    TEST("the console's mute, and lifting it");
    T += 1000; W += 1000;
    cc_chat_mute(P[0], 5, W);
    W += 500;
    CHECK(say(0, "lobby", "hello") == CW_E_CHAT_MUTED && g_wait == 300, "muted 5 minutes, half a second ago: wait %u", g_wait);
    cc_chat_mute(P[0], 0, W);
    CHECK(say(0, "lobby", "hello") == CW_E_NONE, "lifted");
    cc_chat_mute(P[0], 60, W);   // kept through the restart
    pthread_mutex_unlock(&cc_lock);
}

static void restart(void) {
    TEST("a mute is kept through a restart from the journal");
    int64_t ann_until = P[0]->mute_until;
    close_state();
    open_state();
    pthread_mutex_lock(&cc_lock);
    reload_players();
    CHECK(P[0] && P[0]->mute_until == ann_until && ann_until == W + 3600000, "ann's mute %lld, was %lld",
          P[0] ? (long long)P[0]->mute_until : 0, (long long)ann_until);
    T += 1000; W += 1000;
    CHECK(P[0] && say(0, "lobby", "hello") == CW_E_CHAT_MUTED && g_wait == 3599, "still muted: wait %u", g_wait);
    CHECK(P[2] && P[2]->mute_until == 0 && say(2, "lobby", "hello") == CW_E_NONE, "nobody else is");
    TEST("lines are not journaled");
    size_t len;
    cw_chat_log h = log_of("lobby", &len);
    CHECK(h.n == 1, "the lobby after the restart: %u", h.n);
    pthread_mutex_unlock(&cc_lock);
}

static void sweep(void) {
    pthread_mutex_lock(&cc_lock);
    TEST("a game's lines leave memory with the game");
    cc_live *l = cc_live_get(g_game);
    CHECK(l != NULL, "the game came back");
    if (!l) { pthread_mutex_unlock(&cc_lock); return; }
    T = cc_mono();
    W += 1000;
    CHECK(say(3, g_room, "the game's room again") == CW_E_NONE, "said");   // dan: cat spoke at the old clock
    CHECK(cc_game_resign(&l->g, CH_WHITE, T) == CC_OK, "resign");
    cc_live_changed(l, T);
    T += 1000; W += 1000;
    CHECK(say(1, g_room, "gg") == CW_E_NONE, "after the end, while the game is kept");
    uint64_t lines, refused, rooms_before, rooms_after, fns;
    cc_chat_counts(&lines, &refused, &rooms_before, &fns);
    cc_state_sweep(T + 1000);
    size_t len;
    CHECK(log_of(g_room, &len).n == 2 && len > 0, "kept a minute");
    cc_state_sweep(T + CC_FINISHED_KEEP_MS + 1000);
    log_of(g_room, &len);
    CHECK(len == 0 && !cc_chat_room_ok(g_room), "gone with the game");
    cc_chat_counts(&lines, &refused, &rooms_after, &fns);
    CHECK(rooms_after == rooms_before - 1, "rooms %llu, were %llu", (unsigned long long)rooms_after, (unsigned long long)rooms_before);
    T += 2000; W += 2000;
    CHECK(say(1, g_room, "late") == CW_E_CHAT_ROOM, "no room to say it in");
    pthread_mutex_unlock(&cc_lock);
}

int main(void) {
    ch_init();
    snprintf(g_dir, sizeof g_dir, "/tmp/unit_chats_%d", (int)getpid());
    char cmd[300], db[300];
    snprintf(cmd, sizeof cmd, "rm -rf '%s' && mkdir -p '%s'", g_dir, g_dir);
    if (system(cmd) != 0) return 1;
    snprintf(db, sizeof db, "%s/c.sqlite", g_dir);
    snprintf(g_cfg.db_path, sizeof g_cfg.db_path, "%s", db);
    g_ctx.cfg = &g_cfg;
    if (cb_store_open_sqlite(db, &g_ctx.store) != CB_OK) { fprintf(stderr, "no store\n"); return 1; }
    open_state();
    rooms();
    saying();
    ring();
    burst();
    words();
    console();
    restart();
    sweep();
    close_state();
    cb_store_close(g_ctx.store);
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", g_dir);
    if (system(cmd) != 0) return 1;
    return report("unit_chats");
}
