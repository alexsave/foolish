// seeds_app.c - writes the seed inputs of foolish/server's fuzz targets into <dir>/<target>/
// (cbase's fuzz/seeds.c writes the platform's)
// (`make fuzz-seeds`, then `make fuzz-merge` minimises them with what the
// fuzzers found). Each seed is made by the code that makes the real thing:
// the kernel's own encoders and a real game played through the kernel, the
// DynamoDB items the store's own writers produce, the tests' recorded Lambda
// event and unfurl payloads, a real bcrypt hash and a real minted token. A
// seed that is bytes nobody sends teaches the fuzzer nothing a random byte
// would not.
//
// Seed files are named seed-<what>, so a merge keeps them recognisable.
#include "fz.h"

#include "auth/bcrypt.h"
#include "auth/jwt.h"
#include "aws_json.h"
#include "store/store_priv.h"
#include "net/ws.h"
#include "store/rows_foolish.h"
#include "b64/b64.h"

#include "mqtt/mqtt.h"
#include "hex/hex.h"
#include "util/str.h"

#include "fixture_cloudfront_event.h"
#include "fixture_unfurl.h"

#include "ask.h"
#include "awire.h"
#include "legal.h"
#include "netwire.h"
#include "table.h"

#include <errno.h>
#include <sys/stat.h>

static const char *g_root;
static int g_written;

static void put(const char *target, const char *name, const void *p, size_t n) {
    char dir[512], path[700];
    snprintf(dir, sizeof dir, "%s/%s", g_root, target);
    mkdir(dir, 0755);
    snprintf(path, sizeof path, "%s/seed-%s", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    if (n) fwrite(p, 1, n, f);
    fclose(f);
    g_written++;
}
static void puts_(const char *target, const char *name, const char *s) { put(target, name, s, strlen(s)); }
// A seed with one leading selector byte.
static void put1(const char *target, const char *name, uint8_t sel, const void *p, size_t n) {
    uint8_t *b = malloc(n + 1);
    b[0] = sel;
    memcpy(b + 1, p, n);
    put(target, name, b, n + 1);
    free(b);
}

#define U1 "a0000000-0000-4000-8000-000000000001"
#define U2 "a0000000-0000-4000-8000-000000000002"
#define B1 "b0000000-0000-4000-8000-000000000001"

// ---- a real game, played through the kernel ------------------------------------------------

static Game *g_game;
static TableSnaps *g_snaps;
static Table g_t;
static uint8_t g_env[8][1 << 16];
static int g_env_len[8], g_n_env;
static uint8_t g_cast[16][1 << 16];
static int g_cast_len[16], g_n_cast;
static uint8_t g_state[1 << 16], g_roster[ROSTER_BYTES];
static int g_state_len;
static uint8_t g_wire[32][64];
static int g_wire_len[32], g_n_wire;

static void grab(int viewer_count, uint32_t version) {
    for (int v = -1; v < viewer_count && g_n_env < 8; v++) {
        int n = table_envelope(&g_t, "ab12cd", 6, v, version, g_env[g_n_env], sizeof g_env[0]);
        if (n > 0) g_env_len[g_n_env++] = n;
        if (g_n_cast < 16) {
            int c = table_cast(&g_t, "ab12cd", 6, v, version, g_cast[g_n_cast], sizeof g_cast[0]);
            if (c > 0) g_cast_len[g_n_cast++] = c;
        }
    }
}

static void play_game(void) {
    g_game = calloc(1, sizeof *g_game);
    g_snaps = calloc(1, sizeof *g_snaps);
    uint8_t seed[32];
    for (int i = 0; i < 32; i++) seed[i] = (uint8_t)(i * 7 + 1);
    table_init(&g_t, g_game, g_snaps);
    if (table_create(&g_t, U1, 36, "ANN", 3) != TABLE_OK) { fprintf(stderr, "seeds: create failed\n"); exit(1); }
    grab(1, 1);
    if (table_join(&g_t, U2, 36, "BOB", 3) < 0) { fprintf(stderr, "seeds: join failed\n"); exit(1); }
    grab(2, 2);
    if (table_ready(&g_t, U1, 36, seed) < 0 || table_ready(&g_t, U2, 36, seed) < 0) {
        fprintf(stderr, "seeds: ready failed\n");
        exit(1);
    }
    grab(2, 3);
    // A few moves, each the first legal one of a seat that has one.
    static LegalMove moves[512];
    for (int step = 0; step < 12 && g_game->status == GAME_STATUS_PLAYING; step++) {
        for (int s = 0; s < g_game->num_players; s++) {
            int n = legal_moves_into(g_game, s, moves, 512), k = -1;
            for (int i = 0; i < n && k < 0; i++) if (moves[i].type != MOVE_WAIT) k = i;
            if (k < 0) continue;
            AwireAction a;
            memset(&a, 0, sizeof a);
            a.kind = moves[k].type;
            a.n = moves[k].n_cards;
            for (int i = 0; i < a.n; i++) { a.cards[i] = moves[k].cards[i]; a.attacks[i] = moves[k].attack_cards[i]; }
            uint8_t w[64];
            int wn = awire_encode(&a, w, sizeof w);
            if (wn <= 0) continue;
            if (g_n_wire < 32) { memcpy(g_wire[g_n_wire], w, (size_t)wn); g_wire_len[g_n_wire++] = wn; }
            (void)table_act(&g_t, s == 0 ? U1 : U2, 36, w, wn, -1, 0);
            if (step % 4 == 0) grab(2, (uint32_t)(4 + step));
            break;
        }
    }
    g_state_len = table_seal(&g_t, g_game, &g_t.r, g_state, sizeof g_state);
    if (g_state_len < 0) g_state_len = 0;
    (void)roster_encode(&g_t.r, g_roster, sizeof g_roster);
}

// ---- the targets -----------------------------------------------------------------------------

// cb_viewer_addr_parse: [cap][text] (core/fuzz/fz_viewer_addr.c); cb_viewer_key: [mode][cap][header NUL xff NUL peer]
// (core/fuzz/fz_viewer_key.c).
#define PUT1L(t, n, sel, lit) put1(t, n, sel, lit, sizeof(lit) - 1)

// Every object under an "Item" key, anywhere in a request body: the item
// layout exactly as the store writes it, and so as DynamoDB hands it back.
static int g_n_items;
static char *g_items[64];
static void harvest(cb_span v, int depth) {
    if (depth > 12 || v.n == 0) return;
    size_t pos = 0;
    cb_span k, x;
    if (v.p[0] == '{') {
        while (dw_object_next(v, &pos, &k, &x)) {
            if (k.n == 6 && memcmp(k.p, "\"Item\"", 6) == 0 && g_n_items < 64) g_items[g_n_items++] = fz_cstr((const uint8_t *)x.p, x.n);
            harvest(x, depth + 1);
        }
    } else if (v.p[0] == '[') {
        while (dw_array_next(v, &pos, &x)) harvest(x, depth + 1);
    }
}
// The DynamoDB store itself, answering 200 "{}" to everything: every request
// body it sends is harvested, so the items are exactly the ones its writes
// put (the row codecs in store/rows_foolish.c and rows_auth.c).
static int harvest_send(void *ud, const char *target, const uint8_t *body, size_t len, int *status, cb_buf *resp) {
    (void)ud; (void)target;
    harvest(cb_span_of((const char *)body, len), 0);
    *status = 200;
    cb_buf_puts(resp, "{}");
    return CB_OK;
}

static void dynamo_items(void) {
    cb_store *st = NULL;
    if (cb_store_open_dynamo_with("cbase", harvest_send, NULL, &st) != CB_OK) abort();
    cb_user_row u;
    memset(&u, 0, sizeof u);
    snprintf(u.id, sizeof u.id, U1);
    snprintf(u.email, sizeof u.email, "ann@x.io");
    snprintf(u.password_hash, sizeof u.password_hash, "$2b$04$abcdefghijklmnopqrstuuABCDEFGHIJKLMNOPQRSTUVWXYZ01234");
    snprintf(u.username, sizeof u.username, "ANN");
    u.created_at = 1000;
    (void)CB_STORE_CALL(st, user_create, &u);
    (void)CB_STORE_CALL(st, user_import, &u);
    cb_refresh_row rf;
    memset(&rf, 0, sizeof rf);
    memset(rf.token_hash, 'a', 64);
    snprintf(rf.user_id, sizeof rf.user_id, U1);
    snprintf(rf.session_id, sizeof rf.session_id, U2);
    rf.created_at = 5;
    rf.expires_at = 9000;
    (void)CB_STORE_CALL(st, refresh_put, &rf);
    cb_game_row g;
    memset(&g, 0, sizeof g);
    snprintf(g.id, sizeof g.id, "ab12cd");
    g.state = g_state; g.state_len = (size_t)g_state_len;
    g.roster = g_roster; g.roster_len = sizeof g_roster;
    static uint8_t logs[4] = { 1, 2, 3, 4 };
    g.list = logs; g.list_len = 4;
    g.has_seed = true; g.version = 3; g.needs_bots = true; g.created_at = 1; g.updated_at = 2;
    (void)CB_ROWS_CALL(st, game_create, &g, U1);
    cb_snapshot_row sn;
    memset(&sn, 0, sizeof sn);
    snprintf(sn.id, sizeof sn.id, U2);
    snprintf(sn.game_id, sizeof sn.game_id, "ab12cd");
    sn.n_players = 2;
    snprintf(sn.player_ids[0], sizeof sn.player_ids[0], U1);
    snprintf(sn.player_ids[1], sizeof sn.player_ids[1], U2);
    sn.moves = logs; sn.moves_len = 4;
    (void)CB_ROWS_CALL(st, snapshot_put, &sn);
    cb_chat_row c;
    memset(&c, 0, sizeof c);
    snprintf(c.id, sizeof c.id, U2);
    snprintf(c.game_id, sizeof c.game_id, "ab12cd");
    snprintf(c.user_id, sizeof c.user_id, U1);
    c.message = "hi \"there\"";
    (void)CB_ROWS_CALL(st, chat_put, &c);
    cb_rating_row rt;
    memset(&rt, 0, sizeof rt);
    snprintf(rt.user_id, sizeof rt.user_id, U1);
    snprintf(rt.username, sizeof rt.username, "ANN");
    rt.elo_rating = 1033; rt.previous_elo = 1020; rt.games_played = 7;
    (void)CB_ROWS_CALL(st, rating_import, &rt);
    cb_bot_row b;
    memset(&b, 0, sizeof b);
    snprintf(b.id, sizeof b.id, B1);
    snprintf(b.nickname, sizeof b.nickname, "%%Miami 1");
    snprintf(b.strategy_key, sizeof b.strategy_key, "random");
    b.elo_rating = 1010;
    (void)CB_ROWS_CALL(st, bots_replace, &b, 1);
    cb_store_close(st);
}

static void seeds_dynamo(void) {
    cb_buf b;
    cb_buf_init(&b);
    for (int i = 0; i < g_n_items; i++) {
        char name[32];
        cb_buf_reset(&b);
        cb_buf_putc(&b, 0);
        cb_buf_printf(&b, "{\"Item\":%s}", g_items[i]);
        snprintf(name, sizeof name, "item-%d", i);
        put("dynamo_rows", name, b.p, b.len);
        put("json", name, b.p, b.len);
        cb_buf_reset(&b);
        cb_buf_putc(&b, 0);
        cb_buf_printf(&b, "{\"Items\":[%s,%s],\"Count\":2,\"LastEvaluatedKey\":{\"pk\":{\"S\":\"x\"}},\"ConsumedCapacity\":{\"CapacityUnits\":1.5}}",
                      g_items[i], g_items[(i + 1) % g_n_items]);
        snprintf(name, sizeof name, "items-%d", i);
        put("dynamo_rows", name, b.p, b.len);
        cb_buf_reset(&b);
        cb_buf_putc(&b, 0);
        cb_buf_printf(&b, "{\"Responses\":{\"cbase\":[%s]},\"UnprocessedKeys\":{}}", g_items[i]);
        snprintf(name, sizeof name, "batch-%d", i);
        put("dynamo_rows", name, b.p, b.len);
    }
    put1("dynamo_rows", "cond", 0x80 | 0, "{\"__type\":\"com.amazonaws.dynamodb.v20120810#ConditionalCheckFailedException\",\"message\":\"The conditional request failed\",\"Item\":{\"pk\":{\"S\":\"GAME#ab12cd\"}}}", 132);
    puts_("dynamo_rows", "cancel",
          "\x80{\"__type\":\"com.amazonaws.dynamodb.v20120810#TransactionCanceledException\",\"message\":\"Transaction cancelled\","
          "\"CancellationReasons\":[{\"Code\":\"None\"},{\"Code\":\"ConditionalCheckFailed\",\"Item\":{\"pk\":{\"S\":\"x\"}}},{\"Code\":\"ThrottlingError\"}]}");
    puts_("dynamo_rows", "throttle", "\x80{\"__type\":\"com.amazonaws.dynamodb.v20120810#ProvisionedThroughputExceededException\",\"message\":\"slow down\"}");
    puts_("dynamo_rows", "describe",
          "\x01{\"Table\":{\"ProvisionedThroughput\":{\"ReadCapacityUnits\":25,\"WriteCapacityUnits\":25},"
          "\"GlobalSecondaryIndexes\":[{\"IndexName\":\"gsi1\",\"ProvisionedThroughput\":{\"ReadCapacityUnits\":5,\"WriteCapacityUnits\":5}}]}}");
    puts_("dynamo_rows", "execute", "\x01{\"Items\":[],\"NextToken\":\"abc\"}");
    // The one-pass decode's corners (core_json.c or aws_json.c "one pass over an item"): an
    // escaped key, a type twice, a second type, an escaped slash in a blob,
    // backslash runs before a quote, an object keyed "0" in ConsumedCapacity.
    puts_("dynamo_rows", "decode-escaped-key", "\x01{\"\\u0049tem\":{\"id\":{\"S\":\"a\\\\\\\"b\"},\"email\":{\"S\":\"e\"},\"password_hash\":{\"S\":\"h\"},\"created_at\":{\"N\":\"1\",\"N\":\"2\"}},\"ConsumedCapacity\":{\"0\":{\"CapacityUnits\":3}}}");
    puts_("dynamo_rows", "decode-types", "\x01{\"Item\":{\"id\":{\"X\":1,\"S\":\"g\"},\"status\":{\"N\":\"1\"},\"version\":{\"N\":\"1\"},\"state\":{\"B\":\"AQ\\/9\"},\"id\":{\"S\":\"h\"},\"version\":\"7\"}}");
    puts_("dynamo_rows", "decode-runs", "\x01{\"Items\":[{\"pk\":{\"S\":\"\\\\\\\\\"},\"sk\":{\"S\":\"\\\\\\\"\\\\\"}}],\"ConsumedCapacity\":[{\"CapacityUnits\":\"2\"}]}");
    cb_buf_free(&b);

    // The backend against a fake AWS: a program, then answers.
    // [split][program...][status pick][u16 len][body]...
    static const struct { uint8_t op; int item; } P[] = { { 0, 0 }, { 2, 3 }, { 3, -1 }, { 8, -1 }, { 11, -1 }, { 12, -1 }, { 13, -1 } };
    for (size_t i = 0; i < sizeof P / sizeof P[0]; i++) {
        cb_buf a;
        cb_buf_init(&a);
        cb_buf_putc(&a, 0);
        cb_buf_putc(&a, (char)P[i].op);
        a.p[0] = (uint8_t)((a.len) * 256 / 64);   // patched below
        size_t split = a.len;
        cb_buf body;
        cb_buf_init(&body);
        int it = P[i].item >= 0 && P[i].item < g_n_items ? P[i].item : (int)(i % (size_t)g_n_items);
        if (P[i].item >= 0) cb_buf_printf(&body, "{\"Item\":%s}", g_items[it]);
        else cb_buf_printf(&body, "{\"Items\":[%s],\"Count\":1}", g_items[it]);
        cb_buf_putc(&a, 0);
        uint16_t n = (uint16_t)body.len;
        cb_buf_putc(&a, (char)(n & 0xff));
        cb_buf_putc(&a, (char)(n >> 8));
        cb_buf_append(&a, body.p, body.len);
        // data[0] * (size - 1) / 256 + 1 == split
        size_t size = a.len;
        for (int d = 0; d < 256; d++)
            if (1 + (size_t)d * (size - 1) / 256 == split) { a.p[0] = (uint8_t)d; break; }
        char name[32];
        snprintf(name, sizeof name, "prog-%zu", i);
        put("dynamo_store_rows", name, a.p, a.len);
        cb_buf_free(&a);
        cb_buf_free(&body);
    }
}

static void seeds_table_wire(void) {
    for (int i = 0; i < g_n_wire && i < 6; i++) {
        uint8_t buf[128];
        memcpy(buf, "ab12cd", 6);
        memcpy(buf + 6, g_wire[i], (size_t)g_wire_len[i]);
        int n = table_request_encode(buf, sizeof buf, 6, g_wire_len[i], i & 1, 7);
        char name[32];
        snprintf(name, sizeof name, "request-%d", i);
        if (n > 0) put("table_wire", name, buf, (size_t)n);
        snprintf(name, sizeof name, "awire-%d", i);
        put("table_wire", name, g_wire[i], (size_t)g_wire_len[i]);
    }
    for (int k = ASK_JOIN; k <= ASK_BUMP; k++) {
        TableAsk a;
        memset(&a, 0, sizeof a);
        a.kind = (uint8_t)k;
        a.gid_len = 6; memcpy(a.gid, "ab12cd", 6);
        if (k == ASK_ADD_BOT || k == ASK_EXIT) { a.bot_len = 36; memcpy(a.bot, B1, 36); }
        if (k == ASK_EXIT) { a.player_len = 36; memcpy(a.player, U2, 36); }
        if (k == ASK_REARRANGE_HAND) { a.n_idx = 3; a.idx[0] = 2; a.idx[1] = 0; a.idx[2] = 1; }
        if (k == ASK_REARRANGE_PLAYERS) { a.n_order = 2; a.order[0].len = 36; memcpy(a.order[0].id, U2, 36); a.order[1].len = 36; memcpy(a.order[1].id, U1, 36); }
        if (k == ASK_UPDATE_NAME) { a.title_len = 9; memcpy(a.title, "Ann's \xe2\x9c\x93", 9); }
        if (k == ASK_SET_RULES) a.passing = 1;
        uint8_t out[ASK_BYTES_MAX];
        int n = ask_encode(&a, out, sizeof out);
        char name[32];
        snprintf(name, sizeof name, "ask-%d", k);
        if (n > 0) put("table_wire", name, out, (size_t)n);
    }
    uint8_t out[16];
    AskRefusal r = { ASK_WHY_REFUSED, -3, 7 };
    put("table_wire", "refusal", out, (size_t)ask_refusal_encode(&r, out, sizeof out));
    put("table_wire", "response", out, (size_t)table_action_response(TABLE_STATUS_REJECTED, 4, 9, out, sizeof out));
}

static void seeds_netwire(void) {
    static uint8_t out[1 << 17];
    NwRequest q;
    memset(&q, 0, sizeof q);
    q.kind = NW_SIGNIN; q.name_len = 3; memcpy(q.name, "ANN", 3); q.password_len = 8; memcpy(q.password, "password", 8);
    put1("netwire", "signin", 0, out, (size_t)nw_request_encode(&q, out, sizeof out));
    memset(&q, 0, sizeof q);
    q.kind = NW_CHAT_SEND; q.message_len = 5; memcpy(q.message, "hello", 5);
    put1("netwire", "chat", 0, out, (size_t)nw_request_encode(&q, out, sizeof out));
    static NwFrame f;
    memset(&f, 0, sizeof f);
    f.kind = NW_WS_VIEW; f.gid_len = 6; memcpy(f.gid, "ab12cd", 6); f.version = 3;
    f.blob.p = g_env[0]; f.blob.len = (uint32_t)g_env_len[0];
    put1("netwire", "ws-view", 1, out, (size_t)nw_frame_encode(&f, out, sizeof out));
    static NwSession s;
    memset(&s, 0, sizeof s);
    s.access_len = 5; memcpy(s.access, "a.b.c", 5); s.refresh_len = 4; memcpy(s.refresh, "rfsh", 4);
    s.expires_at_ms = 1790000000000ull; s.user_id_len = 36; memcpy(s.user_id, U1, 36); s.username_len = 3; memcpy(s.username, "ANN", 3);
    put1("netwire", "session", 2, out, (size_t)nw_session_encode(&s, out, sizeof out));
    NwBlob e = { g_env[1], (uint32_t)g_env_len[1] };
    put1("netwire", "view", 2, out, (size_t)nw_view_encode(5, e, out, sizeof out));
    NwBlob m = { g_wire[0], (uint32_t)g_wire_len[0] };
    put1("netwire", "replay", 2, out, (size_t)nw_replay_encode(m, e, out, sizeof out));
    // Lists.
    int at = nw_list_head(NW_GAMES, 2, out, sizeof out);
    at += nw_put_game(3, e, out + at, (int)sizeof out - at);
    NwBlob e2 = { g_env[2], (uint32_t)g_env_len[2] };
    at += nw_put_game(4, e2, out + at, (int)sizeof out - at);
    put1("netwire", "games", 3, out, (size_t)at);
    static NwChat c;
    memset(&c, 0, sizeof c);
    c.id_len = 36; memcpy(c.id, U2, 36); c.user_id_len = 36; memcpy(c.user_id, U1, 36); c.message_len = 2; memcpy(c.message, "hi", 2);
    at = nw_list_head(NW_CHATS, 1, out, sizeof out);
    at += nw_put_chat(&c, out + at, (int)sizeof out - at);
    put1("netwire", "chats", 3, out, (size_t)at);
    static NwHistoryRow h;
    memset(&h, 0, sizeof h);
    h.id_len = 36; memcpy(h.id, U2, 36); h.n_players = 2;
    h.seat = 1; h.rank = 2; h.fool = 0; h.duration_s = 61;
    h.n_names = 2; h.name_len[0] = 3; memcpy(h.name[0], "Ann", 3); h.name_len[1] = 3; memcpy(h.name[1], "Bob", 3);
    at = nw_history_head(1, 1033, 1, out, sizeof out);
    at += nw_put_history(&h, out + at, (int)sizeof out - at);
    put1("netwire", "history", 3, out, (size_t)at);
    static NwBoardRow br;
    memset(&br, 0, sizeof br);
    br.id_len = 36; memcpy(br.id, B1, 36); br.name_len = 3; memcpy(br.name, "BOT", 3); br.is_bot = 1; br.rating = 1200;
    at = nw_list_head(NW_BOARD, 1, out, sizeof out);
    at += nw_put_board(&br, out + at, (int)sizeof out - at);
    put1("netwire", "board", 3, out, (size_t)at);
}

static void seeds_client_table(void) {
    for (int i = 0; i < g_n_env; i++) {
        // [mode 0][identity len 0][envelope]
        uint8_t *b = malloc((size_t)g_env_len[i] + 3);
        b[0] = 0; b[1] = 0; b[2] = 0;
        memcpy(b + 3, g_env[i], (size_t)g_env_len[i]);
        char name[32];
        snprintf(name, sizeof name, "envelope-%d", i);
        put("client_table", name, b, (size_t)g_env_len[i] + 3);
        free(b);
    }
    for (int i = 0; i < g_n_cast; i++) {
        uint8_t *b = malloc((size_t)g_cast_len[i] + 3);
        b[0] = 1; b[1] = 0; b[2] = 0;
        memcpy(b + 3, g_cast[i], (size_t)g_cast_len[i]);
        char name[32];
        snprintf(name, sizeof name, "cast-%d", i);
        put("client_table", name, b, (size_t)g_cast_len[i] + 3);
        free(b);
    }
}

static void seeds_unfurl(void) {
    for (size_t i = 0; i < sizeof UNFURL_GOLDENS / sizeof UNFURL_GOLDENS[0]; i++) {
        char name[32];
        snprintf(name, sizeof name, "golden-%zu", i);
        puts_("unfurl", name, UNFURL_GOLDENS[i].payload);
    }
}

static void seeds_import(void) {
    char sh[2 * sizeof g_state + 1], rh[2 * ROSTER_BYTES + 1];
    hex_encode(g_state, (size_t)g_state_len, sh, sizeof sh);
    hex_encode(g_roster, sizeof g_roster, rh, sizeof rh);
    char hash[CB_BCRYPT_HASH_LEN + 1];
    cb_bcrypt_hash("password", 4, hash);
    cb_buf b;
    cb_buf_init(&b);
    cb_buf_printf(&b,
                  "id,email,encrypted_password,username,created_at,last_sign_in_at\n"
                  "\"" U1 "\",\"ann@x.io\",\"%s\",\"ANN\",\"2026-01-05 10:11:12.345678+00\",\"5000\"\n"
                  "\"" U2 "\",\"bob@x.io\",\"%s\",,\"2000\",\n", hash, hash);
    cb_buf_putc(&b, 0);
    cb_buf_puts(&b, "id,nickname,strategy_key,elo_rating,previous_elo,games_played,created_at,updated_at\n"
                    "\"" B1 "\",\"%Miami 1\",\"random\",\"1010\",\"1000\",\"4\",\"1\",\"2\"\n");
    cb_buf_putc(&b, 0);
    cb_buf_puts(&b, "user_id,username,elo_rating,previous_elo,games_played,updated_at\n\"" U1 "\",\"ANN\",\"1033\",\"1020\",\"7\",\"3000\"\n");
    cb_buf_putc(&b, 0);
    cb_buf_printf(&b,
                  "id,status,state,roster,needs_bots,game_seed,logs_packed,version,round_epoch,created_at,updated_at,last_commit_at\n"
                  "\"ab12cd\",\"playing\",\"\\x%s\",\"\\x%s\",\"false\",,,\"3\",\"0\",\"1000\",\"4000\",\"3900\"\n", sh, rh);
    cb_buf_putc(&b, 0);
    cb_buf_puts(&b, "game_id,player_id,joined_at\n\"ab12cd\",\"" U1 "\",\"1000\"\n\"ab12cd\",\"" U2 "\",\"1000\"\n");
    cb_buf_putc(&b, 0);
    cb_buf_puts(&b, "game_id,bot_id\n");
    cb_buf_putc(&b, 0);
    cb_buf_puts(&b, "id,game_id,player_ids,moves,extras,created_at\n\"" U2 "\",\"ab12cd\",\"" U1 ";" U2 "\",\"0102ff\",,\"4500\"\n");
    cb_buf_putc(&b, 0);
    cb_buf_puts(&b, "id,game_id,user_id,message,is_system,created_at\n\"" B1 "\",\"ab12cd\",\"" U1 "\",\"hi, \"\"you\"\"\n2nd line\",\"false\",\"4600\"\n");
    put("import", "export", b.p, b.len);
    cb_buf_free(&b);
}

// The netclient target's programs (fz_netclient.c): sign in, then ask for
// things and answer them with the server's real bodies, and feed the stream.
static void nc_raw(cb_buf *b, const uint8_t *p, size_t n) {   // body_of mode 0
    cb_buf_putc(b, 0);
    cb_buf_putc(b, (char)(n & 0xff));
    cb_buf_putc(b, (char)(n >> 8));
    cb_buf_append(b, p, n);
}
static void seeds_netclient(void) {
    static uint8_t out[1 << 16];
    cb_buf b;
    cb_buf_init(&b);
    cb_buf_putc(&b, 0);                                   // boards off
    // sign in: intent kind 1 -> NC_DO_SIGN_IN
    const uint8_t signin[] = { 0, 1, 0, 0, 0, 0 };
    cb_buf_append(&b, signin, sizeof signin);
    // the answer: step 1, status pick 0 (200), body mode 1 (a session), 8 token bytes, expiry
    const uint8_t sess[] = { 1, 0, 1, 8, 0, 'a', '.', 'b', '.', 'c', 'd', 'e', 'f', 0x10, 0x0e };
    cb_buf_append(&b, sess, sizeof sess);
    // the dashboard: intent kind 8 -> NC_DO_GAMES, answered with a real list
    const uint8_t games[] = { 0, 8, 0, 0, 0, 0 };
    cb_buf_append(&b, games, sizeof games);
    cb_buf_putc(&b, 1);
    cb_buf_putc(&b, 0);
    NwBlob e = { g_env[1], (uint32_t)g_env_len[1] };
    int at = nw_list_head(NW_GAMES, 1, out, sizeof out);
    at += nw_put_game(3, e, out + at, (int)sizeof out - at);
    nc_raw(&b, out, (size_t)at);
    // open the game: kind 9 -> NC_DO_OPEN_GAME, answered with a view (mode 2)
    const uint8_t open[] = { 0, 9, 0, 1, 0, 0 };
    cb_buf_append(&b, open, sizeof open);
    cb_buf_putc(&b, 1);
    cb_buf_putc(&b, 0);
    cb_buf_putc(&b, 0);
    int vn = nw_view_encode(4, e, out, sizeof out);
    cb_buf_putc(&b, (char)(vn & 0xff));
    cb_buf_putc(&b, (char)(vn >> 8));
    cb_buf_append(&b, out, (size_t)vn);
    // the stream: subscribe, open, a VIEW and a CAST frame
    const uint8_t sub[] = { 4, 2, 1, 1, 5, 1 };
    cb_buf_append(&b, sub, sizeof sub);
    static NwFrame f;
    memset(&f, 0, sizeof f);
    f.kind = NW_WS_VIEW; f.gid_len = 6; memcpy(f.gid, "ab12cd", 6); f.version = 5;
    f.blob.p = g_env[2]; f.blob.len = (uint32_t)g_env_len[2];
    int fn = nw_frame_encode(&f, out, sizeof out);
    cb_buf_putc(&b, 2); cb_buf_putc(&b, 0);
    nc_raw(&b, out, (size_t)fn);
    memset(&f, 0, sizeof f);
    f.kind = NW_WS_CAST; f.gid_len = 6; memcpy(f.gid, "ab12cd", 6); f.version = 6;
    f.blob.p = g_cast[3]; f.blob.len = (uint32_t)g_cast_len[3];
    fn = nw_frame_encode(&f, out, sizeof out);
    cb_buf_putc(&b, 2); cb_buf_putc(&b, 0);
    nc_raw(&b, out, (size_t)fn);
    const uint8_t drain[] = { 3, 8, 0x30, 0x75, 3 };
    cb_buf_append(&b, drain, sizeof drain);
    put("netclient", "session-games-stream", b.p, b.len);
    cb_buf_free(&b);
}

// The table target's programs (fz_table.c): a seed, the table's shape, then
// legal moves with a probe between them.
static void seeds_table(void) {
    static const uint8_t SHAPES[][2] = { { 1, 1 }, { 2, 0 }, { 1, 3 }, { 3, 3 }, { 0, 5 } };
    for (size_t k = 0; k < sizeof SHAPES / sizeof SHAPES[0]; k++) {
        uint8_t p[1024];
        size_t n = 0;
        for (int i = 0; i < 32; i++) p[n++] = (uint8_t)(i * 11 + k);
        p[n++] = SHAPES[k][0];       // humans - 1
        p[n++] = SHAPES[k][1];       // bots
        for (int b = 0; b < 5; b++) p[n++] = 0;   // every bot plays at random
        for (int i = 0; i < 150 && n + 8 < sizeof p; i++) {
            p[n++] = 0; p[n++] = (uint8_t)i; p[n++] = (uint8_t)(i * 7); p[n++] = 0;   // a legal move
            if (i % 5 == 2) { p[n++] = 3; p[n++] = (uint8_t)i; p[n++] = 0; p[n++] = 0; p[n++] = 0; p[n++] = (uint8_t)(i * 13); p[n++] = 1; }
            if (i % 9 == 4) { p[n++] = 7; }
        }
        char name[32];
        snprintf(name, sizeof name, "shape-%zu", k);
        put("table", name, p, n);
    }
}

// The sequence target's programs (fz_sequence.c): op = 12 * user + step.
#define OP(user, step) (uint8_t)(12 * (user) + (step))
static void seeds_sequence(void) {
    uint8_t p[1024];
    size_t n;
    // One player and a bot: create, add the bot, start, then moves and bot
    // drives, with reads and a stream subscription along the way.
    n = 0;
    p[n++] = 7; p[n++] = 0; p[n++] = 0; p[n++] = 0;          // seed
    p[n++] = OP(0, 0);                                        // create
    // conn0 CONNECT as user0: one packet, CONNECT, user 0, a good token, its own client id, keep-alive 60 s
    p[n++] = OP(0, 7); p[n++] = 0; p[n++] = 1; p[n++] = 0; p[n++] = 0; p[n++] = 1; p[n++] = 1; p[n++] = 0; p[n++] = 1;
    // conn0 SUBSCRIBE: user 0, one filter, its own seat's casts of game 0, packet id 1
    p[n++] = OP(0, 7); p[n++] = 0; p[n++] = 1; p[n++] = 1; p[n++] = 0; p[n++] = 0; p[n++] = 0; p[n++] = 1; p[n++] = 0; p[n++] = 1; p[n++] = 0;
    p[n++] = OP(0, 1); p[n++] = 2; p[n++] = 0; p[n++] = 0;  // ADD_BOT
    p[n++] = OP(0, 1); p[n++] = 1; p[n++] = 0; p[n++] = 0;  // START
    for (int i = 0; i < 40; i++) {
        p[n++] = OP(0, 3); p[n++] = 0; p[n++] = 0; p[n++] = (uint8_t)i; p[n++] = 0;   // a legal move
        p[n++] = OP(0, 8); p[n++] = 1; p[n++] = 0;                                  // the bots
        if (i % 6 == 3) { p[n++] = OP(0, 9); p[n++] = 0xff; p[n++] = 0x0f; p[n++] = OP(0, 8); p[n++] = 0; }   // time, then the heartbeat
        if (i % 11 == 7) { p[n++] = OP(0, 10); p[n++] = 4; p[n++] = 0; p[n++] = OP(0, 8); p[n++] = 0; p[n++] = OP(0, 8); p[n++] = 1; p[n++] = 0; }   // a planted marker, the scan, a drive
        if (i % 8 == 0) { p[n++] = OP(0, 5); p[n++] = 1; p[n++] = 0; }               // GET the game
    }
    put("sequence", "solo-vs-bot", p, n);
    // Two players: create, join, both start, play; the other one reads, a
    // spectator stream, an unauthorised probe, a CAS race.
    n = 0;
    p[n++] = 9; p[n++] = 0; p[n++] = 0; p[n++] = 0;
    p[n++] = OP(0, 0);
    p[n++] = OP(1, 1); p[n++] = 0; p[n++] = 0; p[n++] = 0;  // user1 JOIN
    p[n++] = OP(0, 1); p[n++] = 1; p[n++] = 0; p[n++] = 0;  // user0 START
    p[n++] = OP(1, 1); p[n++] = 1; p[n++] = 0; p[n++] = 0;  // user1 START
    // conn2: user2 CONNECTs and subscribes to the game's spectator casts (s/<game>)
    p[n++] = OP(2, 7); p[n++] = 2; p[n++] = 1; p[n++] = 0; p[n++] = 2; p[n++] = 1; p[n++] = 1; p[n++] = 0; p[n++] = 1;
    p[n++] = OP(2, 7); p[n++] = 2; p[n++] = 1; p[n++] = 1; p[n++] = 2; p[n++] = 0; p[n++] = 0; p[n++] = 3; p[n++] = 0; p[n++] = 1; p[n++] = 0;
    // conn1: user1 CONNECTs and subscribes to its views (u/<user1>/v)
    p[n++] = OP(1, 7); p[n++] = 1; p[n++] = 1; p[n++] = 0; p[n++] = 1; p[n++] = 1; p[n++] = 1; p[n++] = 0; p[n++] = 1;
    p[n++] = OP(1, 7); p[n++] = 1; p[n++] = 1; p[n++] = 1; p[n++] = 1; p[n++] = 0; p[n++] = 0; p[n++] = 0; p[n++] = 1; p[n++] = 0;
    // conn3, three packets in one message: user1's CONNECT, its seat's casts and chat, a PINGREQ
    p[n++] = OP(1, 7); p[n++] = 3; p[n++] = 0; p[n++] = 1;
    p[n++] = 0; p[n++] = 1; p[n++] = 1; p[n++] = 1; p[n++] = 7; p[n++] = 1;
    p[n++] = 1; p[n++] = 1; p[n++] = 1; p[n++] = 0; p[n++] = 1; p[n++] = 0; p[n++] = 0; p[n++] = 2; p[n++] = 0; p[n++] = 2; p[n++] = 0;
    p[n++] = 3; p[n++] = 1;
    p[n++] = OP(0, 10); p[n++] = 1;                         // a racing writer
    for (int i = 0; i < 40; i++) {
        p[n++] = OP(i & 1, 3); p[n++] = 0; p[n++] = 0; p[n++] = (uint8_t)(i * 3); p[n++] = 0;
        if (i % 10 == 0) { p[n++] = OP(1, 5); p[n++] = 0; }  // user1's dashboard
        if (i % 10 == 5) { p[n++] = OP(3, 6); p[n++] = (uint8_t)i; p[n++] = (uint8_t)(i % 5); p[n++] = 0; p[n++] = 4; p[n++] = 'x'; p[n++] = 'y'; p[n++] = 'z'; p[n++] = 'w'; }
    }
    p[n++] = OP(0, 1); p[n++] = 3; p[n++] = 0; p[n++] = 0;  // EXIT
    p[n++] = OP(1, 1); p[n++] = 3; p[n++] = 0; p[n++] = 1;  // EXIT
    put("sequence", "two-players", p, n);
}

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: seeds <corpus dir>\n"); return 2; }
    g_root = argv[1];
    if (mkdir(g_root, 0755) != 0 && errno != EEXIST) { perror(g_root); return 1; }
    play_game();
    dynamo_items();
    seeds_dynamo();
    seeds_table_wire();
    seeds_netwire();
    seeds_client_table();
    seeds_unfurl();
    seeds_import();
    seeds_netclient();
    seeds_table();
    seeds_sequence();
    printf("seeds: %d files (%d DynamoDB items, %d envelopes, %d casts, %d moves)\n", g_written, g_n_items, g_n_env,
           g_n_cast, g_n_wire);
    return 0;
}
