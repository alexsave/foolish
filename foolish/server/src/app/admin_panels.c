// admin_panels.c - foolish's panels in the platform's console (cbase/app.h
// admin_calls; admin/admin.h has the gate and the platform's own calls):
//   GET  games    ?prefix=&cursor=&limit=  browse "games", then game_get each
//   GET  game     ?id=  the row, its seats (kernel roster_decode) and the
//                 spectator envelope (cb_table_view) as hex
//   GET  bots     bots_list
// and the overview's counter line, the games needing bots. The answers'
// shapes are core_json.c's.
#include "admin_panels.h"

#include "functions_http.h"
#include "table_ops.h"

#include "admin/admin.h"
#include "core_json.h"
#include "store/rows_foolish.h"
#include "util/str.h"

#include "roster.h"

#include "../../../../shared/c/hex/hex.h"

#include <stdlib.h>
#include <string.h>

// ---- games ----------------------------------------------------------------------------

// One game as the console shows it: the row, and its seats read with the
// kernel's own decoder (no byte layout here). r and seats are the storage
// the answer points into.
typedef struct {
    Roster           r;
    cb_js_admin_seat seats[MAX_PLAYERS];
} game_store;

static cb_js_admin_game game_of(const cb_game_row *g, game_store *st) {
    cb_js_admin_game a = {
        .id = g->id, .status = g->status, .status_label = cb_table_status_label(g->status), .version = g->version,
        .round_epoch = g->round_epoch, .needs_bots = g->needs_bots, .created_at = g->created_at,
        .updated_at = g->updated_at, .last_commit_at = g->last_commit_at, .state_len = (int64_t)g->state_len,
        .roster_len = (int64_t)g->roster_len, .logs_len = (int64_t)g->logs_len, .has_seed = g->has_seed,
    };
    int rc = g->roster ? roster_decode(&st->r, g->roster, (int)g->roster_len) : ROSTER_E_LENGTH;
    a.seats_ok = rc == ROSTER_OK;
    a.roster_error = rc;
    if (a.seats_ok) {
        const Roster *r = &st->r;
        for (int i = 0; i < r->n && i < MAX_PLAYERS; i++)
            st->seats[i] = (cb_js_admin_seat){ r->seats[i].name, r->seats[i].id, r->seats[i].name_len, r->seats[i].id_len,
                                               r->seats[i].brain_len > 0 };
        a.seats = st->seats;
        a.n_seats = r->n < MAX_PLAYERS ? r->n : MAX_PLAYERS;
        a.title = r->title;
        a.title_len = r->title_len;
    }
    return a;
}

#define GAMES_PAGE_MAX 100

typedef struct {
    char ids[GAMES_PAGE_MAX][CB_GAME_ID_MAX + 1];
    int  n, cap;
} id_sink;

static const char *col(int n, const char *const *names, const char *const *values, const char *want) {
    for (int i = 0; i < n; i++)
        if (names[i] && strcmp(names[i], want) == 0) return values[i];
    return NULL;
}

// A browse row of the "games" collection names a game: SQLite's games.id, or
// DynamoDB's pk "GAME#<id>" on the sk "ROW" item (MEMBER and BOT items under
// the same pk are skipped).
static bool take_game_id(int n, const char *const *names, const char *const *values, void *ud) {
    id_sink *s = ud;
    const char *id = col(n, names, values, "id");
    if (!id) {
        const char *pk = col(n, names, values, "pk"), *sk = col(n, names, values, "sk");
        if (pk && cb_starts_with(pk, "GAME#") && (!sk || strcmp(sk, "ROW") == 0)) id = pk + 5;
    }
    if (id && s->n < s->cap && strlen(id) <= CB_GAME_ID_MAX) cb_strlcpy(s->ids[s->n++], id, sizeof s->ids[0]);
    return s->n < s->cap;
}

// GET /admin/api/games?cursor=&limit=&prefix=
static void api_games(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    if (!cb_admin_need_store(ctx, res)) return;
    char prefix[64], cursor[512], next[512] = "";
    cb_admin_q_str(req, "prefix", prefix, sizeof prefix);
    cb_admin_q_str(req, "cursor", cursor, sizeof cursor);
    id_sink *s = calloc(1, sizeof *s);
    game_store *gs = calloc(1, sizeof *gs);
    if (!s || !gs) { free(s); free(gs); cb_router_error(res, 500, "out of memory"); return; }
    s->cap = cb_admin_q_int(req, "limit", 25, 1, GAMES_PAGE_MAX);
    int rc = CB_STORE_CALL(ctx->store, admin_browse, CB_ADMIN_GAMES, prefix, cursor[0] ? cursor : NULL, s->cap, take_game_id, s,
                           next, sizeof next);
    if (rc < 0) { free(s); free(gs); cb_admin_store_error(ctx, res, "games", rc); return; }
    cb_buf b;
    cb_buf_init(&b);
    cb_js_stream *st = cb_js_admin_games_begin(&b);
    if (!st) { cb_buf_free(&b); free(s); free(gs); cb_admin_no_memory(res); return; }
    for (int i = 0; i < s->n; i++) {
        cb_game_row g;
        memset(&g, 0, sizeof g);
        if (CB_ROWS_CALL(ctx->store, game_get, s->ids[i], &g) != CB_OK) continue;   // deleted since the browse
        cb_js_admin_game a = game_of(&g, gs);
        cb_js_admin_games_add(st, &a);
        cb_game_row_free(&g);
    }
    cb_admin_send_json(res, 200, &b, cb_js_admin_games_end(st, next));
    cb_buf_free(&b);
    free(s);
    free(gs);
}

// GET /admin/api/game?id=: the row, its byte sizes, and the spectator envelope
// (app/table_ops.h cb_table_view, the same bytes spectator_views answers) as hex.
static void api_game(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    if (!cb_admin_need_store(ctx, res)) return;
    char id[CB_GAME_ID_MAX + 1];
    cb_admin_q_str(req, "id", id, sizeof id);
    if (!id[0]) { cb_router_error(res, 400, "game: id is required"); return; }
    cb_game_row g;
    memset(&g, 0, sizeof g);
    int rc = CB_ROWS_CALL(ctx->store, game_get, id, &g);
    if (rc < 0) { cb_admin_store_error(ctx, res, "game", rc); return; }
    cb_view_row v;
    memset(&v, 0, sizeof v);
    int vrc = cb_table_view(ctx, id, NULL, &v);
    cb_js_admin_envelope e = { vrc == CB_OK, NULL, 0, NULL };
    char *hex = NULL;
    if (vrc == CB_OK) {
        hex = malloc(v.view_len * 2 + 1);
        if (hex && hex_encode(v.view, v.view_len, hex, v.view_len * 2 + 1) >= 0) e.envelope_hex = hex;
        e.envelope_version = v.version;
        cb_view_row_free(&v);
    } else {
        e.envelope_error = cb_strerror(vrc);
    }
    game_store *gs = calloc(1, sizeof *gs);
    if (!gs) { free(hex); cb_game_row_free(&g); cb_admin_no_memory(res); return; }
    cb_js_admin_game a = game_of(&g, gs);
    cb_buf b;
    cb_buf_init(&b);
    cb_admin_send_json(res, 200, &b, cb_js_admin_game_write(&b, &a, &e));
    cb_buf_free(&b);
    free(hex);
    free(gs);
    cb_game_row_free(&g);
}

// ---- bots -----------------------------------------------------------------------------

static bool put_bot(const cb_bot_row *r, void *ud) {
    cb_js_admin_bot b = { r->id, r->nickname, r->strategy_key, r->elo_rating, r->previous_elo, r->games_played, r->updated_at };
    cb_js_admin_bots_add(ud, &b);
    return true;
}

static void api_bots(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    if (!cb_admin_need_store(ctx, res)) return;
    cb_buf b;
    cb_buf_init(&b);
    cb_js_stream *s = cb_js_admin_bots_begin(&b);
    if (!s) { cb_buf_free(&b); cb_admin_no_memory(res); return; }
    int rc = CB_ROWS_CALL(ctx->store, bots_list, put_bot, s);
    int enc = cb_js_admin_bots_end(s);
    if (rc < 0) cb_admin_store_error(ctx, res, "bots", rc);
    else cb_admin_send_json(res, 200, &b, enc);
    cb_buf_free(&b);
}


// ---- the overview's counter ---------------------------------------------------------------

static bool count_name(const char *name, void *ud) { (*(int *)ud)++; return true; }

// The heartbeat's own criteria (bots.h cb_bots_scan): untouched for 10 s,
// active within the hour.
int cb_admin_games_needing_bots(cb_ctx *ctx, int64_t now) {
    int due = 0;
    int rc = CB_ROWS_CALL(ctx->store, games_needing_bots, now - 10000, now - 3600000, 1000, count_name, &due);
    return rc < 0 ? -1 : due;
}

const cb_admin_call cb_foolish_admin_calls[3] = {
    { "GET", "games", api_games },
    { "GET", "game", api_game },
    { "GET", "bots", api_bots },
};
