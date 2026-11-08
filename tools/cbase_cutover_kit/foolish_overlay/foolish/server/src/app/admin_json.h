// admin_json.h - foolish's one JSON file's shapes (admin_json.c): the console pages of its games
// and bots (admin_panels.c), which cbase's console draws from the panels the app declares
// (cbase/app.h cb_admin_panel).
#ifndef FL_ADMIN_JSON_H
#define FL_ADMIN_JSON_H

#include "util/str.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// A document being written: begun, added to, ended (which frees it), as cbase's core_json streams.
typedef struct fl_js_stream fl_js_stream;

// One game as the console shows it (admin_panels.c fills it: it
// reads its row and decodes its seats with the kernel's roster_decode):
// its scalar columns and its seats (seats_ok false: "seats": null and
// "roster_error"), and for the one-game call its byte sizes.
typedef struct {
    const char *name, *id;
    size_t      name_len, id_len;
    bool        bot;
} fl_js_admin_seat;
typedef struct {
    const char             *id;
    int                     status;
    const char             *status_label;
    int64_t                 version, round_epoch;
    bool                    needs_bots;
    int64_t                 created_at, updated_at, last_commit_at;
    bool                    seats_ok;
    int                     roster_error;
    const fl_js_admin_seat *seats;
    int                     n_seats;
    const char             *title;
    size_t                  title_len;
    int64_t                 state_len, roster_len;   // the game call's
    bool                    has_seed;
} fl_js_admin_game;
// GET games: {"games":[game..],"next"}.
fl_js_stream *fl_js_admin_games_begin(cb_buf *out);
void          fl_js_admin_games_add(fl_js_stream *s, const fl_js_admin_game *g);
int           fl_js_admin_games_end(fl_js_stream *s, const char *next);
// GET game: the game, its byte sizes, and the spectator envelope as hex
// (view_ok false: envelope_hex null and envelope_error; envelope_hex NULL
// with view_ok: null).
typedef struct {
    bool        view_ok;
    const char *envelope_hex;
    int64_t     envelope_version;
    const char *envelope_error;
} fl_js_admin_envelope;
int fl_js_admin_game_write(cb_buf *out, const fl_js_admin_game *g, const fl_js_admin_envelope *e);

// GET bots: {"bots":[{id,nickname,strategy_key,elo_rating,previous_elo (null
// when < 0),games_played,updated_at}..]}.
fl_js_stream *fl_js_admin_bots_begin(cb_buf *out);
typedef struct {
    const char *id, *nickname, *strategy_key;
    int64_t     elo_rating, previous_elo, games_played, updated_at;   // previous_elo < 0: null
} fl_js_admin_bot;
void          fl_js_admin_bots_add(fl_js_stream *s, const fl_js_admin_bot *b);
int           fl_js_admin_bots_end(fl_js_stream *s);

#endif
