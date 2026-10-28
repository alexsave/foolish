// cup_regen.c - `build/cup_regen <game.sqlite> <botcup.bin> <dir> [match ...]`: a Bot Cup match's
// summary written again from the game server's own replays, not from what the watcher heard. For a
// summary the live story could not tell whole (before the stories grew without limit, a 551-move
// game was cut), or one whose games file is gone.
//
// The cup (cup.h's file) names each match's two bots by id and when the cup ran; the game server's
// SQLite file (opened read-only, so a running server is untouched) holds every finished game's v6
// replay with its players in seat order. The match's games are the replays of exactly its two bots
// from the cup's start to its end (or now), oldest first. Each is played back through the kernel
// (replay_steps_v6, as a spectator) and its events told by the same story the live watcher writes
// (cup.h be_cup_story_event, board.h be_board_event_kind), then the summary as cup.h writes it.
//
// Matches are 1..7 (7 the final); none named: every decided match. Writes <dir>/<match id>.games
// and <dir>/<match id>.summary.txt, replacing them.
#include "board.h"
#include "cup.h"

#include "client_table.h"
#include "replay_steps.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The file's own ids, so be_cup_load maps every row to itself.
static int file_ids(const char *path, char ids[][BE_BOT_ID + 1], int cap) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    uint32_t head[4];
    int n = -1;
    if (fread(head, sizeof head, 1, f) == 1 && (int)head[3] <= cap) {
        n = (int)head[3];
        for (int i = 0; i < n; i++) {
            if (fread(ids[i], BE_BOT_ID, 1, f) != 1) { n = -1; break; }
            ids[i][BE_BOT_ID] = 0;
        }
    }
    fclose(f);
    return n;
}

static bool uuid_pack(const char *s, unsigned char out[16]) {
    int k = 0;
    for (const char *p = s; *p && k < 32; p++) {
        if (*p == '-') continue;
        int v = *p >= '0' && *p <= '9' ? *p - '0' : *p >= 'a' && *p <= 'f' ? *p - 'a' + 10 : *p >= 'A' && *p <= 'F' ? *p - 'A' + 10 : -1;
        if (v < 0) return false;
        if (k % 2 == 0) out[k / 2] = (unsigned char)(v << 4);
        else out[k / 2] |= (unsigned char)v;
        k++;
    }
    return k == 32;
}

typedef struct {
    be_cup_story  story;
    const char   *names[2];
    bool          dealt;
    int           leader;
} regen;

static void sink(void *ctx, const EvwEvent *ev) {
    regen *r = ctx;
    const Game *g = ev->snap;
    if (!r->dealt) {
        if (ev->type == EVW_T_DEAL || !g || g->power_suit < 0) return;
        be_cup_story_deal(&r->story, r->names, g->power_suit, g->flipped.suit, g->flipped.value, r->leader, g->deck_count);
        r->dealt = true;
    }
    PushEvent pe;
    memset(&pe, 0, sizeof pe);
    pe.type = (int8_t)ev->type;
    pe.msg = (int8_t)ev->msg;
    int kind = be_board_event_kind(&pe);
    if (!kind || kind == BE_EV_DEALT) return;
    int8_t su[64], va[64];
    int n = ev->n_cards < 64 ? ev->n_cards : 64;
    for (int i = 0; i < n; i++) {
        su[i] = ev->cards[i].suit;
        va[i] = ev->cards[i].value;
    }
    be_cup_story_event(&r->story, r->names, g ? g->power_suit : -1, kind, ev->seat, su, va, n, g ? g->deck_count : -1);
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s <game.sqlite> <botcup.bin> <dir> [match 1..7 ...]\n", argv[0]);
        return 2;
    }
    static char ids[256][BE_BOT_ID + 1];
    int n_ids = file_ids(argv[2], ids, 256);
    const char *idp[256];
    for (int i = 0; i < n_ids; i++) idp[i] = ids[i];
    be_w_cup cup;
    if (n_ids < 0 || !be_cup_load(&cup, argv[2], idp, n_ids)) {
        fprintf(stderr, "cup_regen: %s is not a cup file this build reads\n", argv[2]);
        return 1;
    }
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(argv[1], &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        fprintf(stderr, "cup_regen: cannot open %s\n", argv[1]);
        return 1;
    }
    sqlite3_busy_timeout(db, 5000);
    // Each row's name: the bot's nickname.
    static char name_buf[256][64];
    const char *names[256];
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db, "SELECT nickname FROM bots WHERE id = ?", -1, &st, NULL);
    for (int i = 0; i < n_ids; i++) {
        unsigned char u[16];
        snprintf(name_buf[i], sizeof name_buf[i], "%s", ids[i]);
        if (st && uuid_pack(ids[i], u)) {
            sqlite3_reset(st);
            sqlite3_bind_blob(st, 1, u, 16, SQLITE_TRANSIENT);
            if (sqlite3_step(st) == SQLITE_ROW) snprintf(name_buf[i], sizeof name_buf[i], "%s", (const char *)sqlite3_column_text(st, 0));
        }
        names[i] = name_buf[i];
    }
    sqlite3_finalize(st);

    int want[BE_CUP_MATCHES], n_want = 0;
    for (int a = 4; a < argc; a++) {
        int m = atoi(argv[a]);
        if (m >= 1 && m <= BE_CUP_MATCHES) want[n_want++] = m - 1;
    }
    if (!n_want)
        for (int m = 0; m < BE_CUP_MATCHES; m++)
            if (cup.matches[m].winner != BE_TOUR_NONE) want[n_want++] = m;
    int64_t to = cup.finished_ms ? cup.finished_ms : INT64_MAX;
    int rc = 0;
    for (int w = 0; w < n_want; w++) {
        int m = want[w];
        const be_w_match *x = &cup.matches[m];
        if (x->a == BE_TOUR_NONE || x->b == BE_TOUR_NONE) continue;
        unsigned char ua[16], ub[16];
        if (!uuid_pack(ids[x->a], ua) || !uuid_pack(ids[x->b], ub)) continue;
        char id[64], games[1100], summary[1100];
        be_cup_match_id(&cup, m, id, sizeof id);
        snprintf(games, sizeof games, "%s/%s.games", argv[3], id);
        snprintf(summary, sizeof summary, "%s/%s.summary.txt", argv[3], id);
        remove(games);
        sqlite3_prepare_v2(db, "SELECT players, moves FROM replays WHERE created_at BETWEEN ? AND ? AND length(players) = 32 ORDER BY created_at",
                           -1, &st, NULL);
        sqlite3_bind_int64(st, 1, cup.started_ms);
        sqlite3_bind_int64(st, 2, to);
        int game_no = 0;
        while (sqlite3_step(st) == SQLITE_ROW) {
            const unsigned char *pl = sqlite3_column_blob(st, 0);
            bool ab = memcmp(pl, ua, 16) == 0 && memcmp(pl + 16, ub, 16) == 0;
            bool ba = memcmp(pl, ub, 16) == 0 && memcmp(pl + 16, ua, 16) == 0;
            if (!ab && !ba) continue;
            const unsigned char *code = sqlite3_column_blob(st, 1);
            int len = sqlite3_column_bytes(st, 1);
            static regen r;
            be_cup_story_reset(&r.story);
            r.dealt = false;
            r.names[0] = names[ab ? x->a : x->b];
            r.names[1] = names[ab ? x->b : x->a];
            ReplayHeader hdr;
            Card deck[64];
            int n_deck = 0, n_acts = 0;
            static ReplayAction acts[REPLAY_MAX_ACTIONS];
            r.leader = replay_deal_v6(code, len, &hdr, deck, 64, &n_deck, acts, REPLAY_MAX_ACTIONS, &n_acts) == REPLAY_EOK ? hdr.first_attacker : -1;
            int e = replay_steps_v6(code, len, VIEW_SPECTATOR, &hdr, sink, &r);
            if (e != REPLAY_EOK) {
                fprintf(stderr, "cup_regen: match %d: a replay would not play back (%d)\n", m + 1, e);
                rc = 1;
                continue;
            }
            game_no++;
            if (!be_cup_story_write(&r.story, r.names, hdr.fool, game_no, games)) {
                fprintf(stderr, "cup_regen: cannot write %s\n", games);
                rc = 1;
            }
        }
        sqlite3_finalize(st);
        if (game_no != x->games) fprintf(stderr, "cup_regen: match %d: %d replays found, the cup counts %d games\n", m + 1, game_no, x->games);
        // The standings as they stood when this match was decided: the later round-robin matches unplayed.
        be_w_cup then = cup;
        for (int k = m + 1; k < BE_CUP_ROBIN; k++) {
            be_w_match *y = &then.matches[k];
            y->winner = BE_TOUR_NONE;
            y->wins_a = y->wins_b = 0;
        }
        be_cup_standings(&then);
        if (x->winner != BE_TOUR_NONE && !be_cup_summary_write(&then, m, names, games, summary)) {
            fprintf(stderr, "cup_regen: cannot write %s\n", summary);
            rc = 1;
        }
        printf("match %d: %d games -> %s\n", m + 1, game_no, summary);
    }
    sqlite3_close(db);
    return rc;
}
