/* Tallybones - the bridge. See include/tb_api.h. */
#include "include/tb_api.h"
#include "tb_api_layout.h"
#include "../src/tb.h"
#include "../src/tb_code.h"
#include "../src/tb_msg.h"
#include "../src/tb_plan.h"
#include "../src/tb_say.h"
#include "../src/tb_view.h"
#include "../src/tb_beats.h"
#include "../i18n/keys.h"
#include <stddef.h>
#include <string.h>

/* The host's numbers against the kernel's, held together by the compiler. */
_Static_assert(TB_API_TEXT_MAX >= TB_MSG_MAX_TEXT, "the longest link fits the host buffer");
_Static_assert(TB_API_REC_BYTES == TB_REC_BYTES, "the seat records");
_Static_assert(TB_ROW_N == 16 && TB_ROW_TOTAL == TB_ROW_N - 1 && TB_ROW_THREE_ALIKE == TB_UPPER_CATS + 2
               && TB_ROW_ANY - TB_ROW_THREE_ALIKE == TB_C_ANY - TB_C_THREE_ALIKE, "the scorecard's rows");

#ifndef SG_LAYOUT_HASH
#define SG_LAYOUT_HASH 0u            /* not stamped: matches no generated module */
#endif

#define MAX_ID 64

/* THE RESIDENT, MY STAGED MOVE, and the scratch the calls fill. Nothing here
 * is allocated. The resident game is never a draft: `draft` is the resident
 * with the staged move applied (tb_draft), and nothing in it was derived. */
static struct {
    TbMsg    m;
    int      have;
    int      staged;
    TbMove   move;
    TbGame   draft;
    uint8_t  id[MAX_ID];
    int      id_n;
    uint8_t  nick[TB_NAME_MAX_BYTES];
    int      nick_n;
    uint8_t  rec[TB_REC_BYTES];
    int      rec_n;
    int      rec_dirty;
    char     sent_text[TB_MSG_MAX_TEXT];
    int      sent_dm, sent_mine, sent_set;
    int      me, by;
    char     name[TB_MAX_SEATS][TB_NAME_MAX_BYTES + 1];
    const char *names[TB_MAX_SEATS];
    TbApiTable  table;
    TbView      view;
    TbApiEvents events;
    TbMsg       other, other2, prior;
    TbBeats     beats;
    uint32_t    beats_serial;
    int         beats_ok;
    TbBeatFrame frame;
    TbBeatSample sample;
    char        text[TB_MSG_MAX_TEXT];
} S = { .me = -1 };

uint32_t tb_api_layout_hash(void) { return (uint32_t)SG_LAYOUT_HASH; }

/* ---- who I am -------------------------------------------------------------- */

void tb_api_me(const uint8_t *id, int n)
{
    if (!id || n < 0) n = 0;
    if (n > MAX_ID) n = MAX_ID;
    if (n) memcpy(S.id, id, (size_t)n);
    S.id_n = n;
}

void tb_api_nickname(const uint8_t *name, int n)
{
    if (!name || tb_name_verdict(name, n) != TB_NAME_OK) { S.nick_n = 0; return; }
    memcpy(S.nick, name, (size_t)n);
    S.nick_n = n;
}

int tb_api_name_verdict(const uint8_t *name, int n) { return tb_name_verdict(name, n); }

void tb_api_seats_load(const uint8_t *bytes, int n)
{
    if (!bytes || n < 0) n = 0;
    if (n > TB_REC_BYTES) n = TB_REC_BYTES;
    n -= n % TB_REC_LEN;
    if (n) memcpy(S.rec, bytes, (size_t)n);
    S.rec_n = n;
    S.rec_dirty = 0;
}

int tb_api_seats_dirty(void) { return S.rec_dirty; }

int tb_api_seats_save(uint8_t *out, int cap)
{
    if (!out || cap < S.rec_n) return -1;
    memcpy(out, S.rec, (size_t)S.rec_n);
    S.rec_dirty = 0;
    return S.rec_n;
}

/* ---- the resident ----------------------------------------------------------- */

static void names_of(void)
{
    for (int s = 0; s < TB_MAX_SEATS; s++) {
        int n = s < S.m.n_seats ? S.m.seat[s].name_len : 0;
        memcpy(S.name[s], S.m.seat[s].name, (size_t)n);
        S.name[s][n] = 0;
        S.names[s] = S.name[s];
    }
}

static void record(int seat)
{
    S.rec_n = tb_rec_put(S.rec, S.rec_n, &S.m, seat);
    S.rec_dirty = 1;
}

static void seat_me(int is_dm, int i_sent)
{
    uint8_t tag[TB_TAG_LEN];
    tb_tag(S.m.seed, S.id, S.id_n, tag);
    int rec = tb_rec_find(S.rec, S.rec_n, &S.m);
    S.me = tb_msg_resolve(&S.m, rec, tb_msg_seat_of_tag(&S.m, tag), is_dm, i_sent, S.nick, S.nick_n, &S.by);
    if (S.me >= 0 && S.by != TB_BY_RECORD) record(S.me);
    names_of();
}

static void adopt_mine(int seat)
{
    S.have = 1;
    S.staged = 0;
    S.me = seat;
    S.by = TB_BY_RECORD;
    if (seat >= 0) record(seat);
    names_of();
}

static int started(void)
{
    return S.have && S.m.phase != TB_PHASE_WAITING;
}

/* The game as a host draws it: the resident, my staged move applied. */
static const TbGame *cur(void)
{
    return S.staged ? &S.draft : &S.m.game;
}

/* The message pk_api_text writes: the resident, the staged move as its
 * newest bubble. */
static const TbMsg *outgoing(void)
{
    if (!S.staged) return &S.m;
    S.other = S.m;
    S.other.game = S.draft;
    return &S.other;
}

int tb_api_new(const uint8_t seed[32], int dm)
{
    static TbMsg m;
    uint8_t tag[TB_TAG_LEN];
    tb_tag(seed, S.id, S.id_n, tag);
    int e = tb_msg_new(&m, seed, dm, tag, S.nick, S.nick_n);
    if (e) return e;
    S.m = m;
    S.sent_set = 0;
    adopt_mine(0);
    return TB_EOK;
}

int tb_api_check(const char *text)
{
    return tb_msg_text_peek(text, &S.other);
}

/* Is `text` my own staged, unsent bubble? Read WITHOUT deriving, and
 * compared move by move with the resident and the staged move. */
static int is_my_staged(const char *text)
{
    if (!S.staged || tb_msg_text_peek(text, &S.other2) != TB_EOK) return 0;
    const TbGame *g = &S.other2.game;
    if (!tb_msg_same_game(&S.other2, &S.m) || g->hist_n != S.m.game.hist_n + 1) return 0;
    return !memcmp(g->hist, S.m.game.hist, sizeof(TbMove) * S.m.game.hist_n)
        && !memcmp(&g->hist[g->hist_n - 1], &S.move, sizeof S.move);
}

int tb_api_read(const char *text)
{
    if (is_my_staged(text)) return TB_ESTAGED;
    int e = tb_msg_text_decode(text, &S.other);
    if (e) return e;
    S.m = S.other;
    S.have = 1;
    S.staged = 0;
    int mine = S.sent_set && text && !strcmp(text, S.sent_text);
    seat_me(mine ? S.sent_dm : 0, mine ? S.sent_mine : TB_SENT_UNKNOWN);
    return TB_EOK;
}

void tb_api_sender(const char *text, int is_dm, int i_sent)
{
    S.sent_set = 0;
    if (!text || (i_sent != 0 && i_sent != 1)) return;
    size_t n = strlen(text);
    if (n >= sizeof S.sent_text) return;
    memcpy(S.sent_text, text, n + 1);
    S.sent_dm = is_dm != 0;
    S.sent_mine = i_sent;
    S.sent_set = 1;
}

int tb_api_text(char *out, int cap)
{
    if (!S.have) return TB_EGAME;
    return tb_msg_text_encode(outgoing(), out, cap);
}

/* ---- the lobby -------------------------------------------------------------- */

int tb_api_stage_join(void)
{
    if (!S.have) return TB_EGAME;
    uint8_t tag[TB_TAG_LEN];
    tb_tag(S.m.seed, S.id, S.id_n, tag);
    S.other = S.m;
    int s = tb_msg_join(&S.other, tag, S.nick, S.nick_n);
    if (s < 0) return s;
    S.m = S.other;
    adopt_mine(s);
    return s;
}

static int stage(TbMove m)
{
    static TbGame d;
    if (!started() || S.me < 0 || !tb_draft(&d, &S.m.game, m)) return 0;
    S.draft = d;
    S.move = m;
    S.staged = 1;
    return 1;
}

static TbMove my(int kind, int arg)
{
    TbMove m = { (uint8_t)kind, (uint8_t)(S.me >= 0 ? S.me : TB_SEAT_NONE), (uint8_t)arg, 0 };
    return m;
}

int tb_api_stage_leave(void)
{
    if (started()) return stage(my(TB_M_LEAVE, 0));
    if (!S.have || S.me < 0) return TB_EREFUSED;
    S.other = S.m;
    int e = tb_msg_leave(&S.other, S.me);
    if (e) return e;
    /* THE RECORD STAYS: its tag now has no row, which is this device's word
     * that it left (TB_REC_GONE) */
    S.m = S.other;
    S.me = -1;
    S.by = TB_BY_NONE;
    names_of();
    return TB_EOK;
}

int tb_api_stage_start(void)
{
    if (!S.have || S.me < 0) return TB_EREFUSED;
    S.other = S.m;
    int e = tb_msg_start(&S.other, S.me);
    if (e) return e;
    S.m = S.other;
    S.staged = 0;
    names_of();
    return TB_EOK;
}

int tb_api_stage_join_start(void)
{
    if (!S.have) return TB_EGAME;
    uint8_t tag[TB_TAG_LEN];
    tb_tag(S.m.seed, S.id, S.id_n, tag);
    S.other = S.m;
    int s = tb_msg_join_start(&S.other, tag, S.nick, S.nick_n);
    if (s < 0) return s;
    S.m = S.other;
    adopt_mine(s);
    return s;
}

/* ---- staging ----------------------------------------------------------------- */

int tb_api_stage_keep(int mask)
{
    return mask >= 0 && mask < TB_ALL_KEPT ? stage(my(TB_M_KEEP, mask)) : 0;
}

int tb_api_stage_score(int cat)
{
    return cat >= 0 && cat < TB_CATS ? stage(my(TB_M_SCORE, cat)) : 0;
}

int tb_api_cancel(void)
{
    if (!S.staged) return 0;
    S.staged = 0;
    return 1;
}

int tb_api_can_keep(int mask)
{
    return started() && S.me >= 0 && mask >= 0 && mask < TB_ALL_KEPT && tb_is_legal(&S.m.game, my(TB_M_KEEP, mask));
}

int tb_api_can_score(int cat)
{
    return started() && S.me >= 0 && cat >= 0 && cat < TB_CATS && tb_is_legal(&S.m.game, my(TB_M_SCORE, cat));
}

int tb_api_score_if(int cat)
{
    const TbGame *g = cur();
    if (!started() || cat < 0 || cat >= TB_CATS || g->over || g->turn >= g->n) return -1;
    if (g->filled[g->turn] >> cat & 1) return -1;
    return tb_score_of(g->dice, cat);          /* -1 while a die is unknown */
}

/* ---- the send echo ---------------------------------------------------------------- */

static const void *built(int n);
const void *tb_api_beats(int from, int to, int mode);

int tb_api_mark_sent(void)
{
    if (!S.staged || !started()) return 0;
    int n = tb_msg_text_encode(outgoing(), S.text, (int)sizeof S.text);
    /* THE RESIDENT REPLAY of the bubble just sent: its roll is derived here,
     * for the first time, from the history through it (T11) */
    if (n <= 0 || tb_msg_text_decode(S.text, &S.other2) != TB_EOK) return 0;
    S.m = S.other2;
    S.staged = 0;
    names_of();
    tb_api_beats(S.m.game.hist_n - 1, S.m.game.hist_n, TB_BEATS_SEND);
    return 1;
}

/* ---- reading ------------------------------------------------------------------ */

const void *tb_api_table(void)
{
    TbApiTable *t = &S.table;
    memset(t, 0, sizeof *t);
    t->me = t->starter = t->sender = TB_SEAT_NONE;
    if (!S.have) return t;
    const TbMsg *m = &S.m;
    const int live = started();
    t->readable = 1;
    t->phase = (uint8_t)(!live ? TB_PHASE_WAITING : m->game.over ? TB_PHASE_FINISHED : TB_PHASE_LIVE);
    t->dm = m->dm;
    t->n_seats = m->n_seats;
    t->me = (uint8_t)(S.me >= 0 ? S.me : TB_SEAT_NONE);
    t->by = (uint8_t)S.by;
    if (!live) {
        TbLobby l;
        tb_msg_lobby(m, &l);
        t->offered = (uint8_t)tb_lobby_offered(&l, S.me);
        t->can_exit = (uint8_t)tb_lobby_can_exit(&l, S.me);
        t->can_join_start = (uint8_t)(S.me < 0 && tb_lobby_can_join_and_start(&l));
    }
    t->starter = m->starter;
    int snd = tb_msg_sender(m);
    t->sender = (uint8_t)(snd >= 0 ? snd : TB_SEAT_NONE);
    t->lobby_rev = m->lobby_rev;
    if (live) {
        t->bubbles = m->game.hist_n;
        t->turns = m->game.turns;
        t->my_turn = (uint8_t)(S.me >= 0 && !m->game.over && m->game.turn == S.me);
        t->staged = (uint8_t)S.staged;
        t->staged_kind = S.staged ? S.move.kind : 0;
        t->staged_arg = S.staged ? S.move.arg : 0;
    }
    for (int s = 0; s < m->n_seats; s++) {
        t->seat[s].name_len = m->seat[s].name_len;
        memcpy(t->seat[s].name, m->seat[s].name, m->seat[s].name_len);
    }
    return t;
}

const void *tb_api_view(void)
{
    memset(&S.view, 0, sizeof S.view);
    if (!started()) return &S.view;
    tb_view(cur(), &S.view);
    return &S.view;
}

const void *tb_api_plan(int from, int to)
{
    if (!started()) return 0;
    int n = tb_plan(&S.m.game, from, to, S.events.ev, TB_API_EVENTS);
    if (n < 0) return 0;
    S.events.n = (uint16_t)n;
    return &S.events;
}

const void *tb_api_plan_draft(void)
{
    if (!started()) return 0;
    int n = S.staged ? tb_plan_move(&S.m.game, S.move, S.events.ev, TB_API_EVENTS) : 0;
    if (n < 0) return 0;
    S.events.n = (uint16_t)n;
    return &S.events;
}

const void *tb_api_plan_lobby(const char *before)
{
    if (!S.have || tb_msg_text_peek(before, &S.other) != TB_EOK) return 0;
    if (!tb_msg_same_game(&S.other, &S.m)) return 0;
    int n = tb_msg_plan_lobby(&S.other, &S.m, S.events.ev, TB_API_EVENTS);
    if (n < 0) return 0;
    S.events.n = (uint16_t)n;
    return &S.events;
}

int tb_api_ranks(uint8_t out[8])
{
    if (!out || !started()) return 0;
    const TbGame *g = &S.m.game;
    int n = 0;
    for (int s = 0; s < g->n; s++) {
        /* an insertion sort of at most eight: in before out, then the total */
        int k = n++;
        while (k > 0) {
            int p = out[k - 1];
            int before = tb_is_in(g, p) != tb_is_in(g, s) ? tb_is_in(g, p) : tb_total(g, p) >= tb_total(g, s);
            if (before) break;
            out[k] = out[k - 1];
            k--;
        }
        out[k] = (uint8_t)s;
    }
    return n;
}

/* ---- the words ------------------------------------------------------------------ */

int tb_api_string(int key, char *out, int cap)
{
    if (!out || cap < 1 || key < 0 || key >= TB_K_COUNT) return -1;
    const char *t = tb_text(key);
    int n = (int)strlen(t);
    if (n + 1 > cap) return -1;
    memcpy(out, t, (size_t)n + 1);
    return n;
}

static int empty(char *out, int cap)
{
    if (!out || cap < 1) return -1;
    out[0] = 0;
    return 0;
}

static int numbered_row(int number, int seat, int mine_marked, int total, char *out, int cap)
{
    char num[8], who[TB_NAME_MAX_BYTES + 24], tot[8];
    if (tb_itoa(number, num, sizeof num) < 0 || tb_say_seat(S.names, seat, who, sizeof who) < 0) return -1;
    tb_itoa(total, tot, sizeof tot);
    const char *kv[] = { "n", num, "who", who, "total", tot, 0 };
    int key = total >= 0 ? TB_K_RANK_ROW : mine_marked && seat == S.me ? TB_K_LOBBY_ROW_YOU : TB_K_LOBBY_ROW;
    return tb_fill(out, cap, tb_text(key), kv);
}

int tb_api_words(int what, int arg, char *out, int cap)
{
    if (!out || cap < 1 || what < 0 || what >= TB_API_W_COUNT) return -1;
    const int live = started();
    const TbGame *g = &S.m.game;
    const int me = S.me >= 0 ? S.me : -1;
    switch (what) {
    case TB_API_W_RULES_TITLE: return tb_say_rules_title(out, cap);
    case TB_API_W_RULE:        return tb_say_rule(arg, out, cap);
    case TB_API_W_CAT:         return tb_say_cat(arg, out, cap);
    case TB_API_W_ROW_LABEL:   return tb_say_row_label(arg, out, cap);
    case TB_API_W_INVITE: case TB_API_W_JOINED: case TB_API_W_LEFT: {
        if (!S.have || arg < 0 || arg >= S.m.n_seats) return -1;
        int which = what == TB_API_W_INVITE ? TB_SAY_INVITE : what == TB_API_W_JOINED ? TB_SAY_JOINED : TB_SAY_LEFT;
        char who[TB_NAME_MAX_BYTES + 24];
        if (tb_say_seat(S.names, arg, who, sizeof who) < 0) return -1;
        return tb_say_lobby_caption(which, who, out, cap);
    }
    case TB_API_W_SEAT:
        if (!S.have || arg < 0 || arg >= S.m.n_seats) return -1;
        return tb_say_seat(S.names, arg, out, cap);
    case TB_API_W_LOBBY_ROW: case TB_API_W_PUBLIC_ROW:
        if (!S.have || arg < 0 || arg >= S.m.n_seats) return -1;
        return numbered_row(arg + 1, arg, what == TB_API_W_LOBBY_ROW, -1, out, cap);
    case TB_API_W_RANK_ROW: {
        uint8_t rank[TB_MAX_SEATS];
        int n = tb_api_ranks(rank);
        if (arg < 0 || arg >= n) return -1;
        return numbered_row(arg + 1, rank[arg], 0, tb_total(g, rank[arg]), out, cap);
    }
    case TB_API_W_ERROR:
        if (arg >= 0) return -1;
        if (arg == TB_ESTAGED) return tb_api_string(TB_K_STAGED_OWN, out, cap);
        if (arg == TB_EFORMAT) return tb_fill(out, cap, tb_text(TB_K_UNREADABLE_WHY), 0);
        return tb_api_string(TB_K_DAMAGED, out, cap);
    }
    if (!live) return empty(out, cap);
    switch (what) {
    case TB_API_W_CAPTION:  return tb_say_caption(g, arg, S.names, out, cap);
    case TB_API_W_SUMMARY:  return tb_say_summary(g, arg, S.names, out, cap);
    case TB_API_W_STAGED_CAPTION: case TB_API_W_STAGED_SUMMARY: {
        const int full = what == TB_API_W_STAGED_SUMMARY;
        if (S.staged) return tb_say_move(&S.m.game, S.move, S.names, full, out, cap);
        return full ? tb_say_summary(g, g->hist_n, S.names, out, cap) : tb_say_caption(g, g->hist_n, S.names, out, cap);
    }
    case TB_API_W_HEADLINE:    return tb_say_headline(cur(), me, S.names, out, cap);
    case TB_API_W_SUBLINE:     return tb_say_subline(cur(), me, S.names, out, cap);
    case TB_API_W_SPOKEN_DIE:  return tb_say_spoken_die(cur(), arg, out, cap);
    }
    return -1;
}

/* ---- the motion ------------------------------------------------------------------ */

static const void *built(int n)
{
    S.beats_ok = n >= 0;
    if (n < 0) { memset(&S.beats, 0, sizeof S.beats); return 0; }
    S.beats.serial = ++S.beats_serial;
    return &S.beats;
}

const void *tb_api_beats(int from, int to, int mode)
{
    if (!started() || from < -1 || to < from || to > S.m.game.hist_n) return built(-1);
    if (mode != TB_BEATS_OPEN && mode != TB_BEATS_ARRIVAL && mode != TB_BEATS_SEND) return built(-1);
    const TbGame *g = &S.m.game;
    int n = tb_plan(g, from, to, S.events.ev, TB_API_EVENTS);
    if (n < 0) return built(-1);
    TbBeatFrame start;
    if (!tb_beats_pre(g, from, &start)) return built(-1);
    return built(tb_beats_build(S.events.ev, n, &start, g->n, mode, &S.beats));
}

const void *tb_api_beats_now(void) { return S.beats_ok ? &S.beats : 0; }

uint32_t tb_api_beats_serial(void) { return S.beats_serial; }

const void *tb_api_beats_frame(uint32_t now_ms)
{
    tb_beats_frame(&S.beats, now_ms, &S.frame);
    return &S.frame;
}

const void *tb_api_beat_sample(int i, int part, uint32_t now_ms)
{
    if (i < 0 || i >= S.beats.n) return 0;
    tb_beat_sample(&S.beats.beat[i], now_ms, part, &S.sample);
    return &S.sample;
}

int tb_api_adopt(const char *text, int arrival)
{
    const int prior_live = started();
    const int prior_ok = S.have;
    if (prior_ok) S.prior = S.m;
    int e = tb_api_read(text);
    if (e) return e;
    if (!started()) { built(-1); return TB_EOK; }
    const int to = S.m.game.hist_n;
    const int mode = arrival ? TB_BEATS_ARRIVAL : TB_BEATS_OPEN;
    if (prior_ok && prior_live && tb_msg_same_game(&S.prior, &S.m)) {
        /* further along the chain on screen, or a chain that won a race:
         * everything past what the two share */
        int common = tb_common_bubbles(&S.prior, &S.m);
        if (common < to) tb_api_beats(common, to, mode);
        else built(-1);
        return TB_EOK;
    }
    /* cold, or the lobby this game was started from: the newest bubble */
    tb_api_beats(to - 1, to, prior_ok && tb_msg_same_game(&S.prior, &S.m) ? mode : TB_BEATS_OPEN);
    return TB_EOK;
}

/* ---- two messages ---------------------------------------------------------------- */

int tb_api_prefer(const char *mine, const char *tapped)
{
    int a = tb_msg_text_peek(mine, &S.other) == TB_EOK;
    int b = tb_msg_text_peek(tapped, &S.other2) == TB_EOK;
    if (!a || !b) return a ? -1 : b ? 1 : 0;
    return tb_msg_prefer(&S.other, &S.other2);
}

int tb_api_same_game(const char *a, const char *b)
{
    return tb_msg_text_peek(a, &S.other) == TB_EOK && tb_msg_text_peek(b, &S.other2) == TB_EOK
        && tb_msg_same_game(&S.other, &S.other2);
}

int tb_api_common(const char *a, const char *b)
{
    if (tb_msg_text_peek(a, &S.other) != TB_EOK || tb_msg_text_peek(b, &S.other2) != TB_EOK) return -1;
    return tb_common_bubbles(&S.other, &S.other2);
}
