// league.c - league.h.
#include "league.h"

#include "be_metrics.h"
#include "board.h"
#include "client.h"
#include "lib_client.h"
#include "util/clock.h"
#include "util/err.h"
#include "util/log.h"
#include "world.h"

#include "ask.h"
#include "client_table.h"
#include "netwire.h"
#include "../../../shared/c/rng.h"
#include "../../../shared/c/stream_frame.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define LINGER_MS        8000     // a finished table stays on the grid this long before it closes
#define STALL_MS         180000   // a table that has not changed in this long is given up
#define FIRST_VIEW_MS    15000    // a table whose first envelope never came is given up
#define CREATE_GAP_MS    1500     // between two new tables, so they do not all deal at once
#define BOARD_EVERY_MS   60000    // the leaderboard read again
#define RATING_RETRY_MS  1000
#define RATING_TRIES     20
#define RECONNECT_MS     2000

enum { L_FREE, L_WATCH, L_PLAYING, L_RATING, L_DONE };
enum { R_VIEW = 1 };

typedef struct {
    int          state;
    char         gid[NW_GID_MAX + 1];
    int          gid_len;
    int          n;
    bool         podkidnoy;
    int          bots[MAX_PLAYERS];   // league bot indices, in the order asked (the seat order)
    ClientTable *ct;
    ClientSlot  *slot;
    ClientDelta *delta;
    bool         have, refetching;
    int          rating_tries;
    int64_t      created_ms, dealt_ms, last_change_ms, over_ms, retry_ms;
    uint32_t     moves;
} lg_table;

static be_league_opts g_o;
static int            g_port;
static pthread_t      g_tid;
static atomic_bool    g_stop;
static bool           g_started;
static be_client      g_w;              // the watcher
static int64_t        g_reopen_ms;
static lg_table      *g_tables;
static int            g_n_tables;
static uint64_t       g_rng;
static atomic_uint    g_seated;

// The standings and the history, under one lock (the routes read them from the request pool).
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static be_w_bot       g_bots[BE_LEAGUE_BOTS];
static int            g_n_bots;
static be_w_result   *g_results;
static uint32_t       g_n_results, g_cap_results;
static FILE          *g_file;

static void nap_ms(int ms) {
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static uint32_t rnd(uint32_t n) { return n ? rng_u32(&g_rng) % n : 0; }

static void publish(be_world *w, uint8_t kind, const void *rec, size_t n) {
    if (!w->publish) return;
    uint8_t frame[2 + sizeof(be_w_result) + sizeof(be_w_bot)];
    if (n > sizeof frame - 2) return;
    frame[0] = BE_SF_V;
    frame[1] = kind;
    memcpy(frame + 2, rec, n);
    w->publish(frame, 2 + n);
}

// ---- the sizes ---------------------------------------------------------------------------------

// Per mille of tables at each size, 2 to 8: two-player games most, every size real volume.
static const int SIZE_WEIGHT[9] = { 0, 0, 340, 150, 140, 110, 100, 80, 80 };

int be_league_pick_size(int free_bots, uint32_t r) {
    int max = free_bots < MAX_PLAYERS ? free_bots : MAX_PLAYERS;
    if (max < 2) return 0;
    int total = 0;
    for (int n = 2; n <= max; n++) total += SIZE_WEIGHT[n];
    uint32_t at = (uint32_t)((uint64_t)r * (uint64_t)total / 1000u);
    for (int n = 2; n <= max; n++) {
        if (at < (uint32_t)SIZE_WEIGHT[n]) return n;
        at -= (uint32_t)SIZE_WEIGHT[n];
    }
    return max;
}

// ---- the history file --------------------------------------------------------------------------
//
//   "BELG", u32 version, u32 sizeof(be_w_result), u32 n bots, n x BE_BOT_ID bytes (the ids, NUL
//   padded, in the order a result's bot[] indexes), then the results back to back.

#define FILE_MAGIC   0x474C4542u   // "BELG"
#define FILE_VERSION 1u

static bool results_push(const be_w_result *r) {
    if (g_n_results == g_cap_results) {
        uint32_t cap = g_cap_results ? g_cap_results * 2 : 1024;
        be_w_result *nr = realloc(g_results, (size_t)cap * sizeof *nr);
        if (!nr) return false;
        g_results = nr;
        g_cap_results = cap;
    }
    g_results[g_n_results++] = *r;
    return true;
}

// Counts a result into its bots' rows (league games, fools, firsts, the last change).
static void count_result(const be_w_result *r) {
    for (int s = 0; s < r->n; s++) {
        if (r->bot[s] >= g_n_bots) continue;
        be_w_bot *b = &g_bots[r->bot[s]];
        if (b->league_games == 0) b->start_rating = r->before[s];
        b->league_games++;
        if (r->fool == s) b->fools++;
        if (r->first == s) b->firsts++;
        b->last_delta = r->delta[s];
        b->last_ms = r->at_ms;
    }
}

static void file_head(FILE *f) {
    uint32_t head[4] = { FILE_MAGIC, FILE_VERSION, (uint32_t)sizeof(be_w_result), (uint32_t)g_n_bots };
    fwrite(head, sizeof head, 1, f);
    for (int i = 0; i < g_n_bots; i++) {
        char id[BE_BOT_ID];
        memset(id, 0, sizeof id);
        memcpy(id, g_bots[i].id, g_bots[i].id_len);
        fwrite(id, sizeof id, 1, f);
    }
}

// Reads the file (results remapped to today's bot order by id; a result naming a bot the server no
// longer has is dropped), then rewrites it in today's order and keeps it open for appends.
static void file_open(void) {
    if (!g_o.file[0]) return;
    FILE *f = fopen(g_o.file, "rb");
    if (f) {
        uint32_t head[4];
        if (fread(head, sizeof head, 1, f) == 1 && head[0] == FILE_MAGIC && head[1] == FILE_VERSION &&
            head[2] == sizeof(be_w_result) && head[3] <= 4096) {
            int map[4096];
            bool ok = true;
            for (uint32_t i = 0; i < head[3]; i++) {
                char id[BE_BOT_ID + 1];
                if (fread(id, BE_BOT_ID, 1, f) != 1) { ok = false; break; }
                id[BE_BOT_ID] = 0;
                map[i] = be_bots_by_id(id, (int)strnlen(id, BE_BOT_ID));
            }
            be_w_result r;
            while (ok && fread(&r, sizeof r, 1, f) == 1) {
                bool keep = r.n >= 2 && r.n <= BE_SEATS;
                for (int s = 0; keep && s < r.n; s++) {
                    if (r.bot[s] >= head[3] || map[r.bot[s]] < 0) keep = false;
                    else r.bot[s] = (uint8_t)map[r.bot[s]];
                }
                if (keep) {
                    r.seq = g_n_results + 1;
                    if (!results_push(&r)) break;
                    count_result(&r);
                }
            }
        } else CB_WARN("birdseye", "league: %s is not a history this build reads: starting a new one", g_o.file);
        fclose(f);
    }
    char tmp[600];
    snprintf(tmp, sizeof tmp, "%s.new", g_o.file);
    FILE *w = fopen(tmp, "wb");
    if (!w) {
        CB_WARN("birdseye", "league: cannot write %s (%s): the history is not kept", tmp, strerror(errno));
        return;
    }
    file_head(w);
    if (g_n_results) fwrite(g_results, sizeof *g_results, g_n_results, w);
    if (fflush(w) != 0 || rename(tmp, g_o.file) != 0) {
        CB_WARN("birdseye", "league: cannot replace %s: the history is not kept", g_o.file);
        fclose(w);
        return;
    }
    g_file = w;
    CB_INFO("birdseye", "league: %u rated games in the history (%s)", g_n_results, g_o.file);
}

static void file_append(const be_w_result *r) {
    if (!g_file) return;
    if (fwrite(r, sizeof *r, 1, g_file) != 1 || fflush(g_file) != 0) {
        CB_WARN("birdseye", "league: the history stopped being written (%s)", strerror(errno));
        fclose(g_file);
        g_file = NULL;
    }
}

// ---- the standings -----------------------------------------------------------------------------

static void bots_from_roster(void) {
    g_n_bots = 0;
    for (int i = 0; i < be_bots_n() && g_n_bots < BE_LEAGUE_BOTS; i++) {
        const be_bot *r = be_bots_at(i);
        be_w_bot *b = &g_bots[g_n_bots++];
        memset(b, 0, sizeof *b);
        size_t n = strlen(r->id), nn = strlen(r->name), kn = strlen(r->key);
        b->id_len = (uint8_t)(n < BE_BOT_ID ? n : BE_BOT_ID);
        memcpy(b->id, r->id, b->id_len);
        b->name_len = (uint8_t)(nn < BE_NAME ? nn : BE_NAME);
        memcpy(b->name, r->name, b->name_len);
        b->key_len = (uint8_t)(kn < BE_BOT_KEY ? kn : BE_BOT_KEY);
        memcpy(b->key, r->key, b->key_len);
    }
}

// GET /v1/leaderboard: each bot's rating and games as the server keeps them. A bot whose numbers
// moved is published. false when the board could not be read.
static bool read_board(bool publish_changes) {
    tc_response r;
    if (tc_http(g_port, "GET", "/v1/leaderboard", NULL, NULL, 0, &r) != 0) return false;
    int n = r.status == 200 ? nw_list_open(r.body.p, (int)r.body.len, NW_BOARD) : -1;
    for (int i = 0; i < n; i++) {
        NwBoardRow row;
        if (nw_get_board(r.body.p, (int)r.body.len, i, &row) != NW_OK) break;
        if (!row.is_bot) continue;
        int b = be_bots_by_id(row.id, row.id_len);
        if (b < 0 || b >= g_n_bots) continue;
        pthread_mutex_lock(&g_mu);
        be_w_bot *w = &g_bots[b];
        bool moved = w->rating != (int32_t)row.rating || w->games != row.games;
        w->rating = (int32_t)row.rating;
        w->games = row.games;
        if (w->league_games == 0) w->start_rating = w->rating;
        be_w_bot copy = *w;
        pthread_mutex_unlock(&g_mu);
        if (moved && publish_changes) publish(&be_world_g, BE_F_BOT, &copy, sizeof copy);
    }
    tc_response_free(&r);
    return n >= 0;
}

// ---- a table's life ----------------------------------------------------------------------------

static lg_table *table_at(const char *gid, int gid_len) {
    for (int i = 0; i < g_n_tables; i++) {
        lg_table *t = &g_tables[i];
        if (t->state != L_FREE && t->gid_len == gid_len && memcmp(t->gid, gid, (size_t)gid_len) == 0) return t;
    }
    return NULL;
}

static void event_at(lg_table *t, int kind, int seat, const Card *cards, int n_cards, uint32_t version) {
    be_w_event e;
    memset(&e, 0, sizeof e);
    e.gid_len = (uint8_t)t->gid_len;
    memcpy(e.gid, t->gid, (size_t)t->gid_len);
    e.kind = (uint8_t)kind;
    e.seat = (int8_t)seat;
    if (n_cards > BE_EVENT_CARDS) n_cards = BE_EVENT_CARDS;
    e.n_cards = (uint8_t)n_cards;
    for (int i = 0; i < n_cards; i++) e.cards[i] = (be_w_card){ cards[i].suit, cards[i].value };
    e.version = version;
    e.at_ms = cb_now_ms();
    be_world_lock(&be_world_g);
    be_world_event(&be_world_g, be_world_find(&be_world_g, t->gid, t->gid_len), &e);
    be_world_unlock(&be_world_g);
}

static void report(lg_table *t) {
    if (!t->have) return;
    be_world_lock(&be_world_g);
    be_table *bt = be_world_find(&be_world_g, t->gid, t->gid_len);
    if (bt) be_board_fill(&be_world_g, bt, &t->ct->view, t->n, t->moves);
    be_world_unlock(&be_world_g);
}

static void set_seated(lg_table *t, bool seated) {
    pthread_mutex_lock(&g_mu);
    be_w_bot copies[MAX_PLAYERS];
    for (int i = 0; i < t->n; i++) {
        uint8_t *n = &g_bots[t->bots[i]].seated;
        if (seated) (*n)++;
        else if (*n) (*n)--;
        copies[i] = g_bots[t->bots[i]];
    }
    pthread_mutex_unlock(&g_mu);
    for (int i = 0; i < t->n; i++) publish(&be_world_g, BE_F_BOT, &copies[i], sizeof copies[i]);
    if (seated) atomic_fetch_add(&g_seated, (unsigned)t->n);
    else atomic_fetch_sub(&g_seated, (unsigned)t->n);
}

static void table_close(lg_table *t) {
    if (g_w.ws >= 0) be_client_sub(&g_w, t->gid, t->gid_len, true);
    be_world_lock(&be_world_g);
    be_table *bt = be_world_find(&be_world_g, t->gid, t->gid_len);
    if (bt) {
        be_world_set_phase(&be_world_g, bt, BE_PHASE_GONE);
        be_world_changed(&be_world_g, bt);
    }
    be_world_unlock(&be_world_g);
    set_seated(t, false);
    t->state = L_FREE;
    t->have = false;
}

static void fetch_view(lg_table *t) {
    char path[96];
    snprintf(path, sizeof path, "/v1/games/%s", t->gid);
    if (be_client_req(&g_w, R_VIEW, SF_METH_GET, path, NULL, 0, 0, (uint32_t)(t - g_tables))) t->refetching = true;
}

static void adopt(lg_table *t, const uint8_t *env, int n, int64_t now_ms) {
    client_init(t->ct, t->slot);
    client_delta_attach(t->ct, t->delta);
    if (client_adopt_envelope(t->ct, env, n) < 0) {
        CB_WARN("birdseye", "league: an envelope of %d bytes at %s was refused (%d)", n, t->gid, t->ct->detail);
        return;
    }
    t->have = true;
    t->last_change_ms = now_ms;
    report(t);
}

// The game's rating stamp, as its envelope carries it, becomes a result.
static void rated(lg_table *t, int64_t now_ms) {
    const TableView *v = &t->ct->view;
    be_w_result r;
    memset(&r, 0, sizeof r);
    r.gid_len = (uint8_t)t->gid_len;
    memcpy(r.gid, t->gid, (size_t)t->gid_len);
    r.n = (uint8_t)v->num_players;
    r.podkidnoy = (uint8_t)!v->passing;
    r.fool = v->fool;
    r.first = v->num_eliminated > 0 ? v->elimination[0] : -1;
    for (int s = 0; s < v->num_players && s < BE_SEATS; s++) {
        int b = be_bots_by_id(v->seats[s].id, v->seats[s].id_len);
        r.bot[s] = (uint8_t)(b >= 0 ? b : 255);
        r.before[s] = v->rating_before[s];
        r.after[s] = v->rating_after[s];
        r.delta[s] = v->rating_delta[s];
    }
    r.dur_s = t->dealt_ms ? (uint32_t)((now_ms - t->dealt_ms) / 1000) : 0;
    r.at_ms = cb_now_ms();
    be_w_bot copies[MAX_PLAYERS];
    pthread_mutex_lock(&g_mu);
    r.seq = g_n_results + 1;
    results_push(&r);
    count_result(&r);
    int nc = 0;
    for (int s = 0; s < r.n; s++) {
        if (r.bot[s] >= g_n_bots) continue;
        be_w_bot *b = &g_bots[r.bot[s]];
        b->rating = r.after[s];
        b->games++;
        copies[nc++] = *b;
    }
    pthread_mutex_unlock(&g_mu);
    file_append(&r);
    publish(&be_world_g, BE_F_RESULT, &r, sizeof r);
    for (int i = 0; i < nc; i++) publish(&be_world_g, BE_F_BOT, &copies[i], sizeof copies[i]);
}

static void apply_cast(lg_table *t, const uint8_t *cast, int n, uint32_t version, int64_t now_ms) {
    if (!t->have) return;
    uint8_t ident[4096];
    int il = client_identity(t->ct, ident, sizeof ident);
    if (il < 0) il = 0;
    uint8_t board[CLIENT_BOARD_KEEP_MAX];
    int bl = client_board_keep(t->ct, board, sizeof board);
    if (bl < 0) bl = 0;
    int64_t t0 = cb_mono_us();
    if (client_cast_open(t->ct, cast, n, ident, il, board, bl) < 0) {
        if (version > t->ct->view.version && !t->refetching) {
            cb_metrics_add(be_mx, BE_M_RESYNCS, 1);
            fetch_view(t);
        }
        return;
    }
    int rc;
    while ((rc = client_push_next(t->ct)) == 1) {
        const PushEvent *e = &t->ct->event;
        int kind = be_board_event_kind(e);
        if (kind == BE_EV_DEALT) {
            if (!t->dealt_ms) {
                t->dealt_ms = now_ms;
                event_at(t, kind, -1, NULL, 0, t->ct->version);
            }
        } else if (kind) {
            if (kind == BE_EV_ATTACK || kind == BE_EV_COVER || kind == BE_EV_PASS || kind == BE_EV_PICKUP) {
                t->moves++;
                cb_metrics_rate(be_mx, BE_M_MOVES, cb_now_ms(), 1);
            }
            event_at(t, kind, e->seat, e->cards, e->n_cards, t->ct->version);
        }
    }
    rc = client_push_final(t->ct);
    cb_metrics_observe(be_mx, BE_M_CAST_DECODE, (uint64_t)(cb_mono_us() - t0));
    if (rc < 0) {
        if (!t->refetching) fetch_view(t);
        return;
    }
    t->last_change_ms = now_ms;
    report(t);
}

// After every change: the table's state from its board.
static void advance(lg_table *t, int64_t now_ms) {
    if (!t->have) return;
    const TableView *v = &t->ct->view;
    if ((t->state == L_WATCH || t->state == L_PLAYING) && v->status == GAME_STATUS_PLAYING) {
        t->state = L_PLAYING;
        // The server deals in the request that makes the table, before the watcher hears the
        // room: the first board it reads is already dealt, and that is the deal it shows.
        if (!t->dealt_ms) {
            t->dealt_ms = now_ms;
            event_at(t, BE_EV_DEALT, -1, NULL, 0, v->version);
        }
    }
    if ((t->state == L_WATCH || t->state == L_PLAYING) && v->status == GAME_STATUS_GAME_OVER) {
        t->state = L_RATING;
        t->over_ms = now_ms;
        t->rating_tries = 0;
        cb_metrics_rate(be_mx, BE_M_GAMES, cb_now_ms(), 1);
        if (t->dealt_ms) cb_metrics_observe(be_mx, BE_M_GAME_S, (uint64_t)((now_ms - t->dealt_ms) / 1000));
        event_at(t, BE_EV_OVER, v->fool, NULL, 0, v->version);
    }
    if (t->state == L_RATING) {
        if (v->rating_known && v->rated) {
            rated(t, now_ms);
            t->state = L_DONE;
            t->over_ms = now_ms;
        } else if (now_ms >= t->retry_ms && t->rating_tries < RATING_TRIES && !t->refetching) {
            // A board from a cast says nothing of the rating: the envelope does (read again
            // while it says unrated: the rating is the commit's, so it is there or never).
            t->rating_tries++;
            t->retry_ms = now_ms + RATING_RETRY_MS;
            fetch_view(t);
        }
    }
}

// Picks the bots and asks the game server for the table. false when nothing could be started.
static bool table_new(int64_t now_ms) {
    lg_table *t = NULL;
    for (int i = 0; i < g_n_tables && !t; i++)
        if (g_tables[i].state == L_FREE) t = &g_tables[i];
    if (!t) return false;
    // The free bots, fewest league games first (random among equals), so everyone gets volume.
    int free_idx[BE_LEAGUE_BOTS], nf = 0;
    uint32_t key[BE_LEAGUE_BOTS];
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < g_n_bots; i++)
        if (g_bots[i].seated < g_o.tables_per_bot) {
            // Fewest league games first, then the fewest tables now, then at random.
            key[nf] = (g_bots[i].league_games << 16) | ((uint32_t)g_bots[i].seated << 12) | rnd(4096);
            free_idx[nf++] = i;
        }
    pthread_mutex_unlock(&g_mu);
    int n = be_league_pick_size(nf, rnd(1000));
    if (n == 0) return false;
    for (int i = 0; i < n; i++) {   // a selection of the n smallest keys
        int best = i;
        for (int j = i + 1; j < nf; j++)
            if (key[j] < key[best]) best = j;
        uint32_t k = key[i]; key[i] = key[best]; key[best] = k;
        int f = free_idx[i]; free_idx[i] = free_idx[best]; free_idx[best] = f;
    }
    // The seat order is a shuffle of the chosen, so the first seat is not always the newest bot.
    for (int i = n - 1; i > 0; i--) {
        int j = (int)rnd((uint32_t)i + 1);
        int f = free_idx[i]; free_idx[i] = free_idx[j]; free_idx[j] = f;
    }
    bool podkidnoy = (int)rnd(100) < g_o.podkidnoy;
    uint8_t body[3 + MAX_PLAYERS * (1 + BE_BOT_ID)];
    size_t at = 0;
    body[at++] = 1;   // dev_bot_table.h DEV_BOT_TABLE_FORMAT
    body[at++] = podkidnoy ? 0 : 1;
    body[at++] = (uint8_t)n;
    for (int i = 0; i < n; i++) {
        const be_w_bot *b = &g_bots[free_idx[i]];
        body[at++] = b->id_len;
        memcpy(body + at, b->id, b->id_len);
        at += b->id_len;
    }
    char headers[256];
    snprintf(headers, sizeof headers, "Authorization: Bearer %s\r\nContent-Type: application/octet-stream\r\n", g_o.admin_token);
    int64_t t0 = cb_mono_us();
    tc_response r;
    if (tc_http(g_port, "POST", "/dev/bot_table", headers, body, at, &r) != 0) return false;
    cb_metrics_observe(be_mx, BE_M_REQ_CREATE, (uint64_t)(cb_mono_us() - t0));
    bool ok = r.status == 200 && r.body.len >= 2 && r.body.p[0] == r.body.len - 1 && r.body.p[0] <= NW_GID_MAX;
    if (!ok) {
        AskRefusal why = { 0, 0, 0 };
        bool told = r.status != 200 && ask_refusal_decode(r.body.p, (int)r.body.len, &why) == ASK_OK;
        CB_WARN("birdseye", "league: POST /dev/bot_table (%d seats) answered %d%s, why %d code %d detail %d", n, r.status,
                r.status == 404 ? " (the game server is not in dev mode, or not this branch's)" : "", told ? why.why : -1, why.code,
                why.detail);
        tc_response_free(&r);
        return false;
    }
    // A fresh row; the client table's storage is the slot's for good.
    ClientTable *ct = t->ct;
    ClientSlot *slot = t->slot;
    ClientDelta *delta = t->delta;
    memset(t, 0, sizeof *t);
    t->ct = ct;
    t->slot = slot;
    t->delta = delta;
    t->gid_len = r.body.p[0];
    memcpy(t->gid, r.body.p + 1, (size_t)t->gid_len);
    t->gid[t->gid_len] = 0;
    tc_response_free(&r);
    t->n = n;
    t->podkidnoy = podkidnoy;
    for (int i = 0; i < n; i++) t->bots[i] = free_idx[i];
    t->state = L_WATCH;
    t->created_ms = t->last_change_ms = now_ms;
    set_seated(t, true);
    be_world_lock(&be_world_g);
    be_table *bt = be_world_open(&be_world_g, t->gid, t->gid_len);
    if (bt) {
        bt->w.n_seats = (uint8_t)n;
        bt->w.podkidnoy = (uint8_t)podkidnoy;
        be_world_changed(&be_world_g, bt);
    }
    be_world_unlock(&be_world_g);
    event_at(t, BE_EV_NEW, -1, NULL, 0, 0);
    be_client_sub(&g_w, t->gid, t->gid_len, false);
    fetch_view(t);
    return true;
}

static void table_tick(lg_table *t, int64_t now_ms) {
    switch (t->state) {
    case L_WATCH:
        if (!t->have && now_ms - t->created_ms > FIRST_VIEW_MS) {
            CB_WARN("birdseye", "league: no view of %s came: giving it up", t->gid);
            table_close(t);
        }
        break;
    case L_PLAYING:
        if (now_ms - t->last_change_ms > STALL_MS) {
            cb_metrics_add(be_mx, BE_M_STALLS, 1);
            event_at(t, BE_EV_STALL, -1, NULL, 0, t->have ? t->ct->view.version : 0);
            CB_WARN("birdseye", "league: %s stalled: giving it up", t->gid);
            table_close(t);
        }
        break;
    case L_RATING:
        advance(t, now_ms);
        if (t->state == L_RATING && t->rating_tries >= RATING_TRIES && now_ms >= t->retry_ms) {
            CB_WARN("birdseye", "league: %s ended unrated: no result recorded", t->gid);
            t->state = L_DONE;
            t->over_ms = now_ms;
        }
        break;
    case L_DONE:
        if (now_ms - t->over_ms >= LINGER_MS) {
            event_at(t, BE_EV_GONE, -1, NULL, 0, t->have ? t->ct->view.version : 0);
            table_close(t);
        }
        break;
    default: break;
    }
}

// ---- the watcher's socket ----------------------------------------------------------------------

static void on_message(void *ud, const uint8_t *msg, size_t n) {
    int64_t now_ms = cb_mono_ms(), wall = cb_now_ms();
    cb_metrics_rate(be_mx, BE_M_PUSHES, wall, 1);
    cb_metrics_rate(be_mx, BE_M_PUSH_BYTES, wall, n);
    sf_frame f;
    if (sf_parse(msg, n, &f) == 0) {
        if (f.kind != SF_REPLY) return;   // READY, PONG
        be_pending q;
        if (!be_client_take(&g_w, f.id, &q) || q.kind != R_VIEW || q.intent >= (uint32_t)g_n_tables) return;
        cb_metrics_observe(be_mx, BE_M_REQ_READ, (uint64_t)(cb_mono_us() - q.t0_us));
        lg_table *t = &g_tables[q.intent];
        if (t->state == L_FREE) return;
        t->refetching = false;
        uint32_t version;
        NwBlob env;
        if (f.status == 200 && nw_view_decode(f.body, (int)f.body_len, &version, &env) == NW_OK && env.len) {
            adopt(t, env.p, (int)env.len, now_ms);
            advance(t, now_ms);
        }
        return;
    }
    if (!sf_is_push(msg, n)) return;
    NwFrame nf;
    if (nw_frame_decode(msg, (int)n, &nf) != NW_OK) return;
    lg_table *t = table_at(nf.gid, nf.gid_len);
    if (!t) return;
    if (nf.kind == NW_WS_SPECT) apply_cast(t, nf.blob.p, (int)nf.blob.len, nf.version, now_ms);
    else if (nf.kind == NW_WS_VIEW && nf.blob.len) adopt(t, nf.blob.p, (int)nf.blob.len, now_ms);
    else return;
    advance(t, now_ms);
}

static bool watcher_up(int64_t now_ms) {
    if (g_w.ws >= 0) return true;
    if (now_ms < g_reopen_ms) return false;
    g_reopen_ms = now_ms + RECONNECT_MS;
    if (!be_client_sign_in(&g_w, g_port) || !be_client_open(&g_w, g_port)) return false;
    // Every live table heard again, and read whole: a cast missed while the socket was down.
    for (int i = 0; i < g_n_tables; i++)
        if (g_tables[i].state != L_FREE) {
            be_client_sub(&g_w, g_tables[i].gid, g_tables[i].gid_len, false);
            g_tables[i].refetching = false;
            fetch_view(&g_tables[i]);
        }
    return g_w.ws >= 0;
}

static void *league_main(void *arg) {
    (void)arg;
    int64_t next_create = 0, next_board = cb_mono_ms() + BOARD_EVERY_MS;
    while (!atomic_load(&g_stop)) {
        int64_t now_ms = cb_mono_ms();
        if (!watcher_up(now_ms)) {
            nap_ms(200);
            continue;
        }
        be_client_ping(&g_w, now_ms);
        int live = 0;
        for (int i = 0; i < g_n_tables; i++) {
            table_tick(&g_tables[i], now_ms);
            if (g_tables[i].state != L_FREE) live++;
        }
        if (live < g_o.tables && now_ms >= next_create) {
            next_create = now_ms + (table_new(now_ms) ? CREATE_GAP_MS : 5000);
        }
        if (now_ms >= next_board) {
            read_board(true);
            next_board = now_ms + BOARD_EVERY_MS;
        }
        struct pollfd pfd = { g_w.ws, POLLIN, 0 };
        if (poll(&pfd, 1, 100) > 0 && (pfd.revents & (POLLIN | POLLHUP | POLLERR))) be_client_read(&g_w, on_message, NULL);
    }
    be_client_close(&g_w);
    return NULL;
}

// ---- start, stop, the routes -------------------------------------------------------------------

int be_league_start(const be_league_opts *o, char *err, size_t cap) {
    g_o = *o;
    const char *colon = strrchr(o->server, ':');
    g_port = colon ? atoi(colon + 1) : 0;
    if (g_port <= 0 || g_port > 65535) {
        snprintf(err, cap, "birdseye: BE_SERVER '%s' names no port", o->server);
        return CB_E_INVALID;
    }
    if (!o->admin_token[0]) {
        snprintf(err, cap, "birdseye: the league needs BE_ADMIN_TOKEN (the game server's CBASE_ADMIN_TOKEN)");
        return CB_E_INVALID;
    }
    if (g_o.tables_per_bot < 1) g_o.tables_per_bot = 1;
    g_rng = (o->seed + 7) * 0x9E3779B97F4A7C15ull;
    g_n_tables = o->tables;
    g_tables = calloc((size_t)g_n_tables, sizeof *g_tables);
    if (!g_tables) {
        snprintf(err, cap, "birdseye: no memory for the league");
        return CB_E_NOMEM;
    }
    for (int i = 0; i < g_n_tables; i++) {
        g_tables[i].ct = calloc(1, sizeof(ClientTable));
        g_tables[i].slot = calloc(1, sizeof(ClientSlot));
        g_tables[i].delta = calloc(1, sizeof(ClientDelta));
        if (!g_tables[i].ct || !g_tables[i].slot || !g_tables[i].delta) {
            snprintf(err, cap, "birdseye: no memory for the league's tables");
            return CB_E_NOMEM;
        }
    }
    char name[32];
    snprintf(name, sizeof name, "LEAGUE%llX", (unsigned long long)(o->seed & 0xFFFF));
    char password[48];
    snprintf(password, sizeof password, "birdseye-league-%llu", (unsigned long long)o->seed);
    if (!be_client_init(&g_w, name, password)) {
        snprintf(err, cap, "birdseye: no memory for the league's watcher");
        return CB_E_NOMEM;
    }
    // The bots and their standing first: the history file is read against them.
    int nb = -1;
    for (int tries = 0; tries < 50 && nb < 0; tries++) {
        nb = be_bots_fetch(g_port, NULL);
        if (nb < 0) nap_ms(200);
    }
    if (nb < 2) {
        snprintf(err, cap, "birdseye: the game server at %s lists %d bots: the league needs two", o->server, nb);
        return CB_E_STATE;
    }
    bots_from_roster();
    read_board(false);
    file_open();
    CB_INFO("birdseye", "league: %d bots, %d tables at once, up to %d a bot, podkidnoy %d%%, against %s", g_n_bots, g_n_tables, g_o.tables_per_bot, g_o.podkidnoy, o->server);
    atomic_store(&g_stop, false);
    if (pthread_create(&g_tid, NULL, league_main, NULL) != 0) {
        snprintf(err, cap, "birdseye: cannot start the league");
        return CB_E_IO;
    }
    g_started = true;
    return CB_OK;
}

void be_league_stop(void) {
    if (!g_started) return;
    atomic_store(&g_stop, true);
    pthread_join(g_tid, NULL);
    g_started = false;
    for (int i = 0; i < g_n_tables; i++) {
        free(g_tables[i].ct);
        free(g_tables[i].slot);
        free(g_tables[i].delta);
    }
    free(g_tables);
    g_tables = NULL;
    g_n_tables = 0;
    be_client_free(&g_w);
    if (g_file) fclose(g_file);
    g_file = NULL;
    pthread_mutex_lock(&g_mu);
    free(g_results);
    g_results = NULL;
    g_n_results = g_cap_results = 0;
    pthread_mutex_unlock(&g_mu);
}

void be_league_gauges(void) {
    cb_metrics_set(be_mx, BE_M_BOTS_SEATED, atomic_load(&g_seated));
    cb_metrics_set(be_mx, BE_M_PLAYERS, g_started ? 1 : 0);
}

size_t be_league_snapshot(void **out, uint8_t mode) {
    pthread_mutex_lock(&g_mu);
    be_w_league h;
    memset(&h, 0, sizeof h);
    h.hash = be_wire_hash();
    h.mode = mode;
    h.n_bots = (uint32_t)g_n_bots;
    h.bots_at = (uint32_t)sizeof h;
    h.bot_size = (uint32_t)sizeof(be_w_bot);
    h.n_results = g_n_results;
    h.results_at = h.bots_at + h.n_bots * h.bot_size;
    h.result_size = (uint32_t)sizeof(be_w_result);
    size_t total = (size_t)h.results_at + (size_t)h.n_results * h.result_size;
    uint8_t *buf = malloc(total);
    if (buf) {
        memcpy(buf, &h, sizeof h);
        if (g_n_bots) memcpy(buf + h.bots_at, g_bots, (size_t)g_n_bots * sizeof(be_w_bot));
        if (g_n_results) memcpy(buf + h.results_at, g_results, (size_t)g_n_results * sizeof(be_w_result));
    }
    pthread_mutex_unlock(&g_mu);
    *out = buf;
    return buf ? total : 0;
}
