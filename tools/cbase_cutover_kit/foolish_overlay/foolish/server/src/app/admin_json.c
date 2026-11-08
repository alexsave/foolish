// admin_json.c - the JSON of foolish's console pages (admin_json.h): its one JSON file, which the
// JSON gate allows (APP_JSON_FILES), on cbase's writer (util/json.h).
#include "admin_json.h"

#include "util/clock.h"
#include "util/err.h"
#include "util/json.h"

#include <stdlib.h>

struct fl_js_stream {
    cb_jw w;
};

static fl_js_stream *adm_open(cb_buf *out) {
    fl_js_stream *s = malloc(sizeof *s);
    if (!s) return NULL;
    cb_jw_init(&s->w, out);
    cb_jw_obj(&s->w);
    return s;
}

static int adm_close(fl_js_stream *s) {
    if (!s) return CB_E_NOMEM;
    cb_jw_end(&s->w);
    bool ok = cb_jw_ok(&s->w);
    free(s);
    return ok ? CB_OK : CB_E_NOMEM;
}

static void adm_kiso(cb_jw *w, const char *k, int64_t ms) {
    char t[40];
    if (ms <= 0 || cb_iso8601(ms, t, sizeof t) < 0) cb_jw_knull(w, k);
    else cb_jw_kstr(w, k, t);
}

static int adm_done(cb_jw *w) { return cb_jw_ok(w) ? CB_OK : CB_E_NOMEM; }

// One game's scalar columns and its seats, into an open object.
static void adm_game(cb_jw *w, const fl_js_admin_game *g) {
    cb_jw_kstr(w, "id", g->id);
    cb_jw_kint(w, "status", g->status);
    cb_jw_kstr(w, "status_label", g->status_label);
    cb_jw_kint(w, "version", g->version);
    cb_jw_kint(w, "round_epoch", g->round_epoch);
    cb_jw_kbool(w, "needs_bots", g->needs_bots);
    adm_kiso(w, "created_at", g->created_at);
    adm_kiso(w, "updated_at", g->updated_at);
    adm_kiso(w, "last_commit_at", g->last_commit_at);
    cb_jw_key(w, "seats");
    if (!g->seats_ok) {
        cb_jw_null(w);
        cb_jw_kint(w, "roster_error", g->roster_error);
        return;
    }
    cb_jw_arr(w);
    for (int i = 0; i < g->n_seats; i++) {
        cb_jw_obj(w);
        cb_jw_key(w, "name");
        cb_jw_strn(w, g->seats[i].name, g->seats[i].name_len);
        cb_jw_key(w, "id");
        cb_jw_strn(w, g->seats[i].id, g->seats[i].id_len);
        cb_jw_kbool(w, "bot", g->seats[i].bot);
        cb_jw_end(w);
    }
    cb_jw_end(w);
    cb_jw_key(w, "title");
    cb_jw_strn(w, g->title, g->title_len);
}

fl_js_stream *fl_js_admin_games_begin(cb_buf *out) {
    fl_js_stream *s = adm_open(out);
    if (!s) return NULL;
    cb_jw_key(&s->w, "games");
    cb_jw_arr(&s->w);
    return s;
}

void fl_js_admin_games_add(fl_js_stream *s, const fl_js_admin_game *g) {
    cb_jw_obj(&s->w);
    adm_game(&s->w, g);
    cb_jw_end(&s->w);
}

int fl_js_admin_games_end(fl_js_stream *s, const char *next) {
    if (s) {
        cb_jw_end(&s->w);
        cb_jw_kstr(&s->w, "next", next);
    }
    return adm_close(s);
}

int fl_js_admin_game_write(cb_buf *out, const fl_js_admin_game *g, const fl_js_admin_envelope *e) {
    cb_jw w;
    cb_jw_init(&w, out);
    cb_jw_obj(&w);
    adm_game(&w, g);
    cb_jw_kint(&w, "state_len", g->state_len);
    cb_jw_kint(&w, "roster_len", g->roster_len);
    cb_jw_kbool(&w, "has_seed", g->has_seed);
    if (e->view_ok) {
        if (e->envelope_hex) cb_jw_kstr(&w, "envelope_hex", e->envelope_hex);
        else cb_jw_knull(&w, "envelope_hex");
        cb_jw_kint(&w, "envelope_version", e->envelope_version);
    } else {
        cb_jw_knull(&w, "envelope_hex");
        cb_jw_kstr(&w, "envelope_error", e->envelope_error);
    }
    cb_jw_end(&w);
    return adm_done(&w);
}

fl_js_stream *fl_js_admin_bots_begin(cb_buf *out) {
    fl_js_stream *s = adm_open(out);
    if (!s) return NULL;
    cb_jw_key(&s->w, "bots");
    cb_jw_arr(&s->w);
    return s;
}

void fl_js_admin_bots_add(fl_js_stream *s, const fl_js_admin_bot *r) {
    cb_jw *w = &s->w;
    cb_jw_obj(w);
    cb_jw_kstr(w, "id", r->id);
    cb_jw_kstr(w, "nickname", r->nickname);
    cb_jw_kstr(w, "strategy_key", r->strategy_key);
    cb_jw_kint(w, "elo_rating", r->elo_rating);
    if (r->previous_elo < 0) cb_jw_knull(w, "previous_elo");
    else cb_jw_kint(w, "previous_elo", r->previous_elo);
    cb_jw_kint(w, "games_played", r->games_played);
    adm_kiso(w, "updated_at", r->updated_at);
    cb_jw_end(w);
}

int fl_js_admin_bots_end(fl_js_stream *s) {
    if (s) cb_jw_end(&s->w);
    return adm_close(s);
}

