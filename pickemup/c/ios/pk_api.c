/* Pick 'Em Up - the bridge. See include/pk_api.h. */
#include "include/pk_api.h"
#include "pk_api_layout.h"
#include "../src/pk.h"
#include "../src/pk_msg.h"
#include "../src/pk_plan.h"
#include "../src/pk_say.h"
#include "../src/pk_view.h"
#include "../src/pk_beats.h"
#include "../i18n/keys.h"
#include <stddef.h>
#include <string.h>

/* The host's numbers against the kernel's, held together by the compiler. */
_Static_assert(PK_API_TEXT_MAX >= PK_MSG_MAX_TEXT, "the longest link fits the host buffer");
_Static_assert(PK_API_REC_BYTES == PK_REC_BYTES, "seat records");
_Static_assert(PK_API_SPECTATOR == PK_VIEW_SPECTATOR && PK_API_ALL == PK_VIEW_ALL, "viewers");
_Static_assert(PK_API_ME != PK_VIEW_SPECTATOR && PK_API_ME != PK_VIEW_ALL && PK_API_ME < 0, "PK_API_ME is its own viewer");

#ifndef SG_LAYOUT_HASH
#define SG_LAYOUT_HASH 0u            /* not stamped: matches no generated module */
#endif

#define MAX_ID 64

/* THE RESIDENT, and the scratch the calls fill. Nothing here is allocated. */
static struct {
    PkMsg    m;
    int      have;                   /* a message is resident                  */
    uint8_t  id[MAX_ID];
    int      id_n;
    uint8_t  nick[PK_NAME_MAX_BYTES];
    int      nick_n;
    /* this device's seat records, the host's bytes */
    uint8_t  rec[PK_REC_BYTES];
    int      rec_n;
    int      rec_dirty;
    /* THE SENDER FACT, about one message only */
    char     sent_text[PK_MSG_MAX_TEXT];
    int      sent_dm, sent_mine, sent_set;
    /* my seat in the resident message, and the witness */
    int      me, by;
    /* the roster's names, NUL-terminated, for the words */
    char     name[PK_MAX_SEATS][PK_NAME_MAX_BYTES + 1];
    const char *names[PK_MAX_SEATS];
    /* what a read hands back */
    PkApiTable  table;
    PkView      view;
    PkApiEvents events;
    PkSince     since;
    PkMsg       other, other2;       /* scratch for check, prefer, lobby plans */
    /* the motion: the current plan, a scratch for a lost race, the frame and
     * sample a read hands back, and the draft as the last build saw it */
    PkBeats     beats, beats2;
    uint32_t    beats_serial;
    PkBeatFrame frame;
    PkBeatSample sample;
    PkEvent     stage_prev[PK_API_EVENTS];
    int         stage_prev_n;
    uint8_t     stage_seed[32];
    uint16_t    stage_bubbles;
    char        text[PK_MSG_MAX_TEXT];
    int         beats_ok;            /* the newest build laid a plan out        */
    PkMsg       prior;               /* pk_api_adopt: the chain on screen       */
} S = { .me = -1 };

uint32_t pk_api_layout_hash(void) { return (uint32_t)SG_LAYOUT_HASH; }

/* ---- who I am -------------------------------------------------------------- */

void pk_api_me(const uint8_t *id, int n)
{
    if (!id || n < 0) n = 0;
    if (n > MAX_ID) n = MAX_ID;
    if (n) memcpy(S.id, id, (size_t)n);
    S.id_n = n;
}

void pk_api_nickname(const uint8_t *name, int n)
{
    if (!name || pk_name_verdict(name, n) != PK_NAME_OK) { S.nick_n = 0; return; }
    memcpy(S.nick, name, (size_t)n);
    S.nick_n = n;
}

int pk_api_name_verdict(const uint8_t *name, int n) { return pk_name_verdict(name, n); }

void pk_api_seats_load(const uint8_t *bytes, int n)
{
    if (!bytes || n < 0) n = 0;
    if (n > PK_REC_BYTES) n = PK_REC_BYTES;
    n -= n % PK_REC_LEN;
    if (n) memcpy(S.rec, bytes, (size_t)n);
    S.rec_n = n;
    S.rec_dirty = 0;
}

int pk_api_seats_dirty(void) { return S.rec_dirty; }

int pk_api_seats_save(uint8_t *out, int cap)
{
    if (!out || cap < S.rec_n) return -1;
    memcpy(out, S.rec, (size_t)S.rec_n);
    S.rec_dirty = 0;
    return S.rec_n;
}

/* ---- the resident ----------------------------------------------------------- */

static void my_tag(const PkMsg *m, uint8_t tag[PK_TAG_LEN])
{
    pk_tag(m->seed, S.id, S.id_n, tag);
}

static void names_of(void)
{
    for (int s = 0; s < PK_MAX_SEATS; s++) {
        int n = s < S.m.n_seats ? S.m.seat[s].name_len : 0;
        memcpy(S.name[s], S.m.seat[s].name, (size_t)n);
        S.name[s][n] = 0;
        S.names[s] = S.name[s];
    }
}

static void record(int seat)
{
    S.rec_n = pk_rec_put(S.rec, S.rec_n, &S.m, seat);
    S.rec_dirty = 1;
}

/* Which seat is mine in the resident message: the four witnesses (pk_msg.h),
 * and a seat found any way but the record is recorded. */
static void seat_me(int is_dm, int i_sent)
{
    uint8_t tag[PK_TAG_LEN];
    my_tag(&S.m, tag);
    int rec = pk_rec_find(S.rec, S.rec_n, &S.m);
    S.me = pk_msg_resolve(&S.m, rec, pk_msg_seat_of_tag(&S.m, tag), is_dm, i_sent,
                          S.nick, S.nick_n, &S.by);
    if (S.me >= 0 && S.by != PK_BY_RECORD) record(S.me);
    names_of();
}

/* The resident changed under a lobby action of mine: I am at `seat`. */
static void adopt_mine(int seat)
{
    S.have = 1;
    S.me = seat;
    S.by = PK_BY_RECORD;
    if (seat >= 0) record(seat);
    names_of();
}

int pk_api_new(const uint8_t seed[32], int dm)
{
    static PkMsg m;
    uint8_t tag[PK_TAG_LEN];
    pk_tag(seed, S.id, S.id_n, tag);
    int e = pk_msg_new(&m, seed, dm, tag, S.nick, S.nick_n);
    if (e) return e;
    S.m = m;
    S.sent_set = 0;
    adopt_mine(0);
    return PK_EOK;
}

int pk_api_check(const char *text)
{
    return pk_msg_text_decode(text, &S.other);
}

int pk_api_read(const char *text)
{
    int e = pk_msg_text_decode(text, &S.other);
    if (e) return e;
    S.m = S.other;
    S.have = 1;
    int mine = S.sent_set && text && !strcmp(text, S.sent_text);
    seat_me(mine ? S.sent_dm : 0, mine ? S.sent_mine : PK_SENT_UNKNOWN);
    return PK_EOK;
}

void pk_api_sender(const char *text, int is_dm, int i_sent)
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

int pk_api_text(char *out, int cap)
{
    if (!S.have) return PK_EGAME;
    if (S.m.phase != PK_PHASE_WAITING && S.m.game.b_open) {
        S.other = S.m;
        if (!pk_seal(&S.other.game)) return PK_EGAME;
        return pk_msg_text_encode(&S.other, out, cap);
    }
    return pk_msg_text_encode(&S.m, out, cap);
}

int pk_api_commit(void)
{
    if (!S.have || S.m.phase == PK_PHASE_WAITING || !S.m.game.b_open) return 0;
    return pk_seal(&S.m.game);
}

/* ---- the lobby -------------------------------------------------------------- */

int pk_api_join(void)
{
    if (!S.have) return PK_EGAME;
    uint8_t tag[PK_TAG_LEN];
    my_tag(&S.m, tag);
    S.other = S.m;
    int s = pk_msg_join(&S.other, tag, S.nick, S.nick_n);
    if (s < 0) return s;
    S.m = S.other;
    adopt_mine(s);
    return s;
}

int pk_api_leave(void)
{
    if (!S.have || S.me < 0) return PK_EREFUSED;
    S.other = S.m;
    int e = pk_msg_leave(&S.other, S.me);
    if (e) return e;
    /* THE RECORD STAYS: its tag now has no row, which is this device's word
     * that it left (PK_REC_GONE, D51), so a namesake who takes the freed name
     * is never my seat. A rejoin records the new row over it. */
    S.m = S.other;
    S.me = -1;
    S.by = PK_BY_NONE;
    names_of();
    return PK_EOK;
}

int pk_api_start(void)
{
    if (!S.have || S.me < 0) return PK_EREFUSED;
    S.other = S.m;
    int e = pk_msg_start(&S.other, S.me);
    if (e) return e;
    S.m = S.other;
    names_of();
    return PK_EOK;
}

int pk_api_join_start(void)
{
    if (!S.have) return PK_EGAME;
    uint8_t tag[PK_TAG_LEN];
    my_tag(&S.m, tag);
    S.other = S.m;
    int s = pk_msg_join_start(&S.other, tag, S.nick, S.nick_n);
    if (s < 0) return s;
    S.m = S.other;
    adopt_mine(s);
    return s;
}

/* ---- staging ----------------------------------------------------------------- */

static PkGame *live(void)
{
    if (!S.have || S.m.phase == PK_PHASE_WAITING || S.me < 0) return 0;
    return &S.m.game;
}

static int act(int kind, int a, int b)
{
    PkGame *g = live();
    if (!g || a < 0 || a > 255 || b < 0 || b > 255) return 0;
    PkAct x = { (uint8_t)kind, (uint8_t)a, (uint8_t)b, 0 };
    return pk_apply(g, S.me, x);
}

int pk_api_draw(void)              { return act(PK_A_DRAW, 0, 0); }
int pk_api_play(int pos, int suit) { return act(PK_A_PLAY, pos, suit); }
int pk_api_say_it(void)            { return act(PK_A_SAY_IT, 0, 0); }
int pk_api_catch(int seat)         { return act(PK_A_CALL_OUT, seat, 0); }
int pk_api_pass(void)              { return act(PK_A_PASS, 0, 0); }

/* Take-backs only ever touch MY draft. */
static PkGame *my_draft(void)
{
    PkGame *g = live();
    return g && g->b_open && g->b_sender == S.me ? g : 0;
}

int pk_api_undo(void)   { PkGame *g = my_draft(); return g ? pk_undo(g) : 0; }
int pk_api_unsay(void)  { PkGame *g = my_draft(); return g ? pk_unsay(g) : 0; }
int pk_api_uncall(void) { PkGame *g = my_draft(); return g ? pk_uncall(g) : 0; }
int pk_api_cancel(void) { PkGame *g = my_draft(); return g ? pk_to_floor(g) : 0; }

int pk_api_can_play(int pos)
{
    PkGame *g = live();
    return g && pos >= 0 && pos < PK_HAND_CAP ? pk_can_play(g, S.me, pos) : 0;
}

int pk_api_card_suit(int card)
{
    return card >= 0 && card < PK_DECK ? pk_suit((uint8_t)card) : -1;
}

int pk_api_card_rank(int card)
{
    return card >= 0 && card < PK_DECK ? pk_rank((uint8_t)card) : -1;
}

int pk_api_is_wild(int pos)
{
    PkGame *g = live();
    if (!g || pos < 0 || pos >= g->hand_n[S.me]) return 0;
    return pk_is_wild(g->hand[S.me][pos]) && g->hand_n[S.me] > 1;
}

/* ---- reading ------------------------------------------------------------------ */

const void *pk_api_table(void)
{
    PkApiTable *t = &S.table;
    memset(t, 0, sizeof *t);
    t->me = t->starter = t->sender = PK_SEAT_NONE;
    if (!S.have) return t;
    const PkMsg *m = &S.m;
    const int started = m->phase != PK_PHASE_WAITING;
    t->readable = 1;
    t->phase = (uint8_t)(!started ? PK_PHASE_WAITING : m->game.over ? PK_PHASE_FINISHED : PK_PHASE_LIVE);
    t->dm = m->dm;
    t->n_seats = m->n_seats;
    t->me = (uint8_t)(S.me >= 0 ? S.me : PK_SEAT_NONE);
    t->by = (uint8_t)S.by;
    if (!started) {
        PkLobby l;
        pk_msg_lobby(m, &l);
        t->offered = (uint8_t)pk_lobby_offered(&l, S.me);
        t->can_exit = (uint8_t)pk_lobby_can_exit(&l, S.me);
        t->can_join_start = (uint8_t)(S.me < 0 && pk_lobby_can_join_and_start(&l));
    }
    t->starter = m->starter;
    int snd = pk_msg_sender(m);
    t->sender = (uint8_t)(snd >= 0 ? snd : PK_SEAT_NONE);
    t->lobby_rev = m->lobby_rev;
    if (started) {
        const PkGame *g = &m->game;
        t->bubbles = g->bubbles;
        t->turns = g->turns;
        for (int i = g->hist_n - 1; i >= 0; i--)
            if (g->hist[i].kind == PK_A_BUBBLE && (g->hist[i].b & PK_BR_SEALED)) {
                t->tip_said = (g->hist[i].b & PK_BR_SAID) != 0;
                break;
            }
        t->draft = (uint8_t)(g->b_open && S.me >= 0 && g->b_sender == S.me);
        t->can_send = (uint8_t)(t->draft && pk_can_seal(g));
    }
    for (int s = 0; s < m->n_seats; s++) {
        t->seat[s].name_len = m->seat[s].name_len;
        memcpy(t->seat[s].name, m->seat[s].name, m->seat[s].name_len);
    }
    return t;
}

static int viewer_of(int viewer)
{
    if (viewer == PK_API_ME) return S.me >= 0 ? S.me : PK_VIEW_SPECTATOR;
    return viewer;
}

static int valid_viewer(int v)
{
    return v == PK_VIEW_SPECTATOR || v == PK_VIEW_ALL || (v >= 0 && v < S.m.n_seats);
}

const void *pk_api_view(int viewer)
{
    memset(&S.view, 0, sizeof S.view);
    int v = viewer_of(viewer);
    if (!S.have || S.m.phase == PK_PHASE_WAITING || !valid_viewer(v)) return &S.view;
    pk_view(&S.m.game, v, &S.view);
    return &S.view;
}

const void *pk_api_plan(int viewer, int from, int to)
{
    int v = viewer_of(viewer);
    if (!S.have || S.m.phase == PK_PHASE_WAITING || !valid_viewer(v)) return 0;
    int n = pk_plan(&S.m.game, v, from, to, S.events.ev, PK_API_EVENTS);
    if (n < 0) return 0;
    S.events.n = (uint16_t)n;
    return &S.events;
}

const void *pk_api_plan_draft(int viewer)
{
    int v = viewer_of(viewer);
    if (!S.have || S.m.phase == PK_PHASE_WAITING || !valid_viewer(v)) return 0;
    int n = pk_plan_draft(&S.m.game, v, S.events.ev, PK_API_EVENTS);
    if (n < 0) return 0;
    S.events.n = (uint16_t)n;
    return &S.events;
}

const void *pk_api_since(int from, int to)
{
    if (!S.have || S.m.phase == PK_PHASE_WAITING) return 0;
    return pk_since(&S.m.game, from, to, &S.since) ? &S.since : 0;
}

const void *pk_api_plan_lobby(const char *before)
{
    if (!S.have || pk_msg_text_decode(before, &S.other) != PK_EOK) return 0;
    if (!pk_msg_same_game(&S.other, &S.m)) return 0;
    int n = pk_msg_plan_lobby(&S.other, &S.m, S.events.ev, PK_API_EVENTS);
    if (n < 0) return 0;
    S.events.n = (uint16_t)n;
    return &S.events;
}

/* ---- the motion ------------------------------------------------------------------ */

static const PkGame *started(void)
{
    return S.have && S.m.phase != PK_PHASE_WAITING ? &S.m.game : 0;
}

void pk_api_beats_mark(void)
{
    const PkGame *g = started();
    S.stage_prev_n = 0;
    if (!g || S.me < 0 || !g->b_open || g->b_sender != S.me) return;
    int n = pk_plan_draft(g, S.me, S.stage_prev, PK_API_EVENTS);
    if (n < 0) return;
    S.stage_prev_n = n;
    memcpy(S.stage_seed, g->seed, 32);
    S.stage_bubbles = g->bubbles;
}

static const void *built(int n)
{
    pk_api_beats_mark();
    S.beats_ok = n >= 0;
    if (n < 0) { memset(&S.beats, 0, sizeof S.beats); return 0; }
    S.beats.serial = ++S.beats_serial;
    return &S.beats;
}

const void *pk_api_beats(int viewer, int from, int to, int mode)
{
    const PkGame *g = started();
    int v = viewer_of(viewer);
    if (!g || !valid_viewer(v) || v == PK_VIEW_ALL || from < -1 || to < from) return built(-1);
    if (mode != PK_BEATS_OPEN && mode != PK_BEATS_ARRIVAL) return built(-1);
    int n = pk_plan(g, v, from, to, S.events.ev, PK_API_EVENTS);
    if (n < 0) return built(-1);
    PkBeatFrame start;
    if (!pk_beats_pre(g, v, from, &start)) return built(-1);
    return built(pk_beats_build(S.events.ev, n, &start, v, g->n, mode, 0, 0, 0, &S.beats));
}

/* Put the host motion in beats2 in front of the plan in beats, which starts
 * once it is over. */
static const void *prepend_beats2(void)
{
    int k = S.beats2.n, n = S.beats.n;
    if (!S.beats_ok || k + n > PK_BEATS_MAX) return built(-1);
    pk_beats_delay(&S.beats, 0, S.beats2.total_ms);
    memmove(&S.beats.beat[k], &S.beats.beat[0], (size_t)n * sizeof(PkBeat));
    memcpy(&S.beats.beat[0], &S.beats2.beat[0], (size_t)k * sizeof(PkBeat));
    S.beats.n = (uint16_t)(k + n);
    if (S.beats2.total_ms > S.beats.total_ms) S.beats.total_ms = S.beats2.total_ms;
    S.beats.settle_ms = PK_T_COLLAPSE_WAIT + S.beats.total_ms + PK_T_COLLAPSE_REST;
    return &S.beats;
}

uint32_t pk_api_beats_serial(void) { return S.beats_serial; }

const void *pk_api_beats_stage(int flags)
{
    const PkGame *g = started();
    if (!g || S.me < 0 || !g->b_open || g->b_sender != S.me) return built(-1);
    int n = pk_plan_draft(g, S.me, S.events.ev, PK_API_EVENTS);
    if (n < 0) return built(-1);
    /* the previous tap's plan counts only for this very draft */
    int prev_n = S.stage_prev_n;
    if (memcmp(S.stage_seed, g->seed, 32) != 0 || S.stage_bubbles != g->bubbles) prev_n = 0;
    PkBeatFrame start;
    if (!pk_beats_pre(g, S.me, g->bubbles, &start)) return built(-1);
    if (!built(pk_beats_build(S.events.ev, n, &start, S.me, g->n, PK_BEATS_STAGE,
                              S.stage_prev, prev_n, flags & 0xFF, &S.beats))) return 0;
    if (!(flags & PK_BFL_PICKED)) return &S.beats;
    if (pk_beats_host(PK_HM_PICKER_PICK, (flags >> 8) & 3, 0, &S.beats.start, S.me, g->n, 0,
                      &S.beats2) < 0) return built(-1);
    return prepend_beats2();
}

const void *pk_api_beats_send(void)
{
    const PkGame *g = started();
    if (!g || S.me < 0 || g->bubbles < 1) return built(-1);
    int from = g->bubbles - 1;
    int n = pk_plan(g, S.me, from, g->bubbles, S.events.ev, PK_API_EVENTS);
    if (n < 0) return built(-1);
    PkBeatFrame start;
    if (!pk_beats_pre(g, S.me, from, &start)) return built(-1);
    return built(pk_beats_build(S.events.ev, n, &start, S.me, g->n, PK_BEATS_SEND, 0, 0, 0, &S.beats));
}

const void *pk_api_beats_host(int what, int a, int b)
{
    const PkGame *g = started();
    if (!g) return built(-1);
    PkView v;
    pk_view(g, S.me >= 0 ? S.me : PK_VIEW_SPECTATOR, &v);
    PkBeatFrame start;
    pk_beats_frame_of(&v, g->n, &start);
    return built(pk_beats_host(what, a, b, &start, S.me, g->n, 0, &S.beats));
}

const void *pk_api_beats_conflict(int card, int pos, int from, int to)
{
    if (!pk_api_beats(PK_API_ME, from, to, PK_BEATS_ARRIVAL)) return 0;
    /* the retraction first, from the board the winner starts from */
    if (pk_beats_host(PK_HM_RETRACT, card, pos, &S.beats.start, S.beats.viewer, S.beats.n_seats, 0,
                      &S.beats2) < 0) return built(-1);
    return prepend_beats2();
}

const void *pk_api_beats_frame(uint32_t now_ms)
{
    pk_beats_frame(&S.beats, now_ms, &S.frame);
    return &S.frame;
}

const void *pk_api_beat_sample(int i, int part, uint32_t now_ms)
{
    if (i < 0 || i >= S.beats.n) return 0;
    pk_beat_sample(&S.beats.beat[i], now_ms, part, &S.sample);
    return &S.sample;
}

/* ---- the words ------------------------------------------------------------------ */

int pk_api_string(int key, char *out, int cap)
{
    if (!out || cap < 1 || key < 0 || key >= PK_K_COUNT) return -1;
    const char *t = pk_text(key);
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

/* "2. Bo", or "2. Bo (You)" when the row is my seat's and it may say so. */
static int numbered_row(int number, int seat, int mine_marked, char *out, int cap)
{
    char num[8], who[PK_NAME_MAX_BYTES + 24];
    if (pk_itoa(number, num, sizeof num) < 0 || pk_say_seat(S.names, seat, who, sizeof who) < 0) return -1;
    const char *kv[] = { "n", num, "who", who, 0 };
    return pk_fill(out, cap, pk_text(mine_marked && seat == S.me ? PK_K_LOBBY_ROW_YOU : PK_K_LOBBY_ROW), kv);
}

typedef struct { uint8_t card[8]; int n; } Buried;

static void buried_one(const PkEvent *e, void *ctx)
{
    Buried *b = ctx;
    if (e->kind == PK_EV_BURY && b->n < 8) b->card[b->n++] = e->card;
}

int pk_api_buried(uint8_t out[8])
{
    if (!out || !S.have || S.m.phase == PK_PHASE_WAITING) return 0;
    const PkGame *g = &S.m.game;
    if (g->reshuffles) return 0;              /* the deck has been rebuilt since */
    Buried b = { { 0 }, 0 };
    if (pk_plan_each(g, PK_VIEW_ALL, -1, 0, buried_one, &b) < 0) return 0;
    int n = 0;
    for (int i = 0; i < b.n; i++)
        for (int d = 0; d < g->deck_n; d++)
            if (g->deck[d] == b.card[i]) { out[n++] = b.card[i]; break; }
    return n;
}

int pk_api_ranks(uint8_t out[8])
{
    if (!out || !S.have || S.m.phase == PK_PHASE_WAITING || !S.m.game.over) return 0;
    const PkGame *g = &S.m.game;
    int n = 0;
    if (g->winner < g->n) out[n++] = g->winner;
    /* an insertion sort of at most eight: fewest cards first, ties by seat */
    for (int s = 0; s < g->n; s++) {
        if (s == g->winner) continue;
        int k = n++;
        while (k > (g->winner < g->n) && g->hand_n[out[k - 1]] > g->hand_n[s]) { out[k] = out[k - 1]; k--; }
        out[k] = (uint8_t)s;
    }
    return n;
}

int pk_api_words(int what, int arg, char *out, int cap)
{
    if (!out || cap < 1 || what < 0 || what >= PK_API_W_COUNT) return -1;
    const int started = S.have && S.m.phase != PK_PHASE_WAITING;
    const PkGame *g = &S.m.game;
    const int me = S.me >= 0 ? S.me : PK_VIEW_SPECTATOR;
    switch (what) {
    case PK_API_W_RULES_TITLE: return pk_say_rules_title(out, cap);
    case PK_API_W_RULE:        return pk_say_rule(arg, out, cap);
    case PK_API_W_CARD:        return arg >= 0 && arg < PK_DECK ? pk_say_card((uint8_t)arg, out, cap) : -1;
    case PK_API_W_INVITE: case PK_API_W_JOINED: case PK_API_W_LEFT: {
        if (!S.have || arg < 0 || arg >= S.m.n_seats) return -1;
        int which = what == PK_API_W_INVITE ? PK_SAY_INVITE : what == PK_API_W_JOINED ? PK_SAY_JOINED : PK_SAY_LEFT;
        return pk_say_lobby_caption(which, S.names[arg], out, cap);
    }
    case PK_API_W_SEAT:
        if (!S.have || arg < 0 || arg >= S.m.n_seats) return -1;
        return pk_say_seat(S.names, arg, out, cap);
    case PK_API_W_SPOKEN_FAN:
        if (!S.have || arg < 0 || arg >= S.m.n_seats) return -1;
        return pk_say_spoken_fan(S.names, arg, out, cap);
    case PK_API_W_LOBBY_ROW: case PK_API_W_PUBLIC_ROW:
        if (!S.have || arg < 0 || arg >= S.m.n_seats) return -1;
        return numbered_row(arg + 1, arg, what == PK_API_W_LOBBY_ROW, out, cap);
    case PK_API_W_RANK_ROW: {
        uint8_t rank[PK_MAX_SEATS];
        int n = pk_api_ranks(rank);
        if (arg < 0 || arg >= n) return -1;
        return numbered_row(arg + 1, rank[arg], 1, out, cap);
    }
    case PK_API_W_LOBBY_DEALER: {
        if (!S.have || S.m.n_seats < 1) return -1;
        char who[PK_NAME_MAX_BYTES + 24];
        if (pk_say_seat(S.names, 0, who, sizeof who) < 0) return -1;
        const char *kv[] = { "who", who, 0 };
        return pk_fill(out, cap, pk_text(PK_K_LOBBY_DEALER), kv);
    }
    case PK_API_W_INDEX:
        /* the corner index a face prints; Skip, Reverse and a wild are glyphs */
        if (arg < 0 || arg >= PK_DECK) return -1;
        switch (pk_rank((uint8_t)arg)) {
        case PK_R_PLUS2:   return pk_api_string(PK_K_RANK_PLUS2, out, cap);
        case PK_R_WILD4:   return pk_api_string(PK_K_INDEX_PLUS4, out, cap);
        case PK_R_SKIP: case PK_R_REVERSE: case PK_R_WILD: return empty(out, cap);
        default:           return pk_itoa(pk_rank((uint8_t)arg), out, cap);
        }
    case PK_API_W_STRIP_DRAWS: {
        char num[8];
        if (arg < 1 || pk_itoa(arg, num, sizeof num) < 0) return -1;
        const char *kv[] = { "n", num, 0 };
        return pk_fill(out, cap, pk_text(PK_K_STRIP_DRAWS), kv);
    }
    case PK_API_W_ERROR:
        if (arg >= 0) return -1;
        /* a newer format is the one refusal with a remedy (4.7); every other
         * one is a link that was cut or changed on the way */
        return pk_api_string(arg == PK_EFORMAT ? PK_K_UNREADABLE_WHY : PK_K_DAMAGED, out, cap);
    }
    if (!started) return empty(out, cap);
    switch (what) {
    case PK_API_W_CAPTION:      return pk_say_caption(g, arg, S.names, out, cap);
    case PK_API_W_STAGED_CAPTION:
        /* the bubble pk_api_text would write: my draft sealed into a copy
         * (the resident keeps its draft), else the newest sealed bubble */
        if (g->b_open) {
            S.other = S.m;
            if (!pk_seal(&S.other.game)) return -1;
            return pk_say_caption(&S.other.game, S.other.game.bubbles, S.names, out, cap);
        }
        return pk_say_caption(g, g->bubbles, S.names, out, cap);
    case PK_API_W_HEADLINE:     return pk_say_headline(g, me, S.names, out, cap);
    case PK_API_W_SUBLINE:      return pk_say_subline(g, me, S.names, out, cap);
    case PK_API_W_DECK_LEFT:    return pk_say_deck_left(g, out, cap);
    case PK_API_W_DIR:          return pk_say_dir(g, out, cap);
    case PK_API_W_DECK_N:       return pk_say_deck_n(arg, out, cap);
    case PK_API_W_DIR_OF:
        if (arg != PK_DIR_CW && arg != PK_DIR_ACW) return -1;
        return pk_say_dir_of(g->n, arg, out, cap);
    case PK_API_W_SPOKEN_CARD:  return pk_say_spoken_card(g, me, arg, out, cap);
    case PK_API_W_SPOKEN_DECK:  return pk_say_spoken_deck(g, out, cap);
    case PK_API_W_SPOKEN_STACK: return pk_say_spoken_stack(g, out, cap);
    }
    return -1;
}

/* ---- two messages ---------------------------------------------------------------- */

int pk_api_prefer(const char *mine, const char *tapped)
{
    int a = pk_msg_text_decode(mine, &S.other) == PK_EOK;
    int b = pk_msg_text_decode(tapped, &S.other2) == PK_EOK;
    if (!a || !b) return a ? -1 : b ? 1 : 0;
    return pk_msg_prefer(&S.other, &S.other2);
}

int pk_api_same_game(const char *a, const char *b)
{
    return pk_msg_text_decode(a, &S.other) == PK_EOK && pk_msg_text_decode(b, &S.other2) == PK_EOK
        && pk_msg_same_game(&S.other, &S.other2);
}

int pk_api_common(const char *a, const char *b)
{
    if (pk_msg_text_decode(a, &S.other) != PK_EOK || pk_msg_text_decode(b, &S.other2) != PK_EOK) return -1;
    return pk_common_bubbles(&S.other, &S.other2);
}

/* ---- adopting, with its motion (I29) ------------------------------------------------ */

const void *pk_api_beats_now(void) { return S.beats_ok ? &S.beats : 0; }

int pk_api_adopt(const char *text, int arrival)
{
    /* THE CHAIN ON SCREEN, before the read replaces the one slot: whether it
     * was a live game, my open draft and the play staged in it, and the chain
     * as its link reads (a draft sealed into a copy, exactly what pk_api_text
     * writes; a draft that cannot be written yet is no chain to compare). */
    const int prior_live = S.have && S.m.phase != PK_PHASE_WAITING;
    const int prior_draft = prior_live && S.me >= 0 && S.m.game.b_open && S.m.game.b_sender == S.me;
    const int prior_bubbles = prior_live ? S.m.game.bubbles : 0;
    int staged_card = -1, staged_pos = -1;
    if (prior_draft) {
        int n = pk_plan_draft(&S.m.game, S.me, S.events.ev, PK_API_EVENTS);
        for (int i = n - 1; i >= 0; i--)
            if (S.events.ev[i].kind == PK_EV_PLAY && S.events.ev[i].seat == S.me) {
                staged_card = S.events.ev[i].card;
                staged_pos = S.events.ev[i].i;
                break;
            }
    }
    int prior_ok = S.have;
    if (prior_ok) {
        S.prior = S.m;
        if (prior_live && S.prior.game.b_open && !pk_seal(&S.prior.game)) prior_ok = 0;
        if (prior_ok && (pk_msg_text_encode(&S.prior, S.text, (int)sizeof S.text) <= 0
                         || pk_msg_text_decode(S.text, &S.prior) != PK_EOK)) prior_ok = 0;
    }

    int e = pk_api_read(text);
    if (e) return e;
    if (S.m.phase == PK_PHASE_WAITING) { built(-1); return PK_EOK; }
    const int to = S.m.game.bubbles;
    const int same = prior_ok && pk_msg_same_game(&S.prior, &S.m);
    if (same && prior_live) {
        if (staged_card >= 0) {
            /* my staged play is not in the adopted chain: a lost race (4.8) */
            int common = pk_common_bubbles(&S.prior, &S.m);
            if (common >= 0 && common <= to && common < prior_bubbles + 1) {
                pk_api_beats_conflict(staged_card, staged_pos, common, to);
                return PK_EOK;
            }
        }
        if (to > prior_bubbles) pk_api_beats(PK_API_ME, prior_bubbles, to, arrival ? PK_BEATS_ARRIVAL : PK_BEATS_OPEN);
        else built(-1);
        return PK_EOK;
    }
    /* cold, or the lobby this game was dealt from: the newest bubble, or the
     * deal when the start bubble is the newest (bubble 0, from -1) */
    pk_api_beats(PK_API_ME, to - 1, to, arrival && same ? PK_BEATS_ARRIVAL : PK_BEATS_OPEN);
    return PK_EOK;
}

/* ---- a tap on a seat's fan (I30) ---------------------------------------------------- */

int pk_api_tap_fan(int seat)
{
    PkGame *g = live();
    if (!g || seat < 0 || seat >= g->n) return PK_API_FAN_REFUSED;
    const int called = g->b_open && g->b_sender == S.me ? g->b_call : PK_SEAT_NONE;
    if (called == seat) return pk_uncall(g) ? PK_API_FAN_UNCALLED : PK_API_FAN_REFUSED;
    if (called == PK_SEAT_NONE) return act(PK_A_CALL_OUT, seat, 0) ? PK_API_FAN_CALLED : PK_API_FAN_REFUSED;
    /* moving the call: one call a bubble (3.6), so the old one comes off
     * first, on a copy, and a refused new one leaves the draft as it was */
    S.other = S.m;
    PkAct x = { PK_A_CALL_OUT, (uint8_t)seat, 0, 0 };
    if (!pk_uncall(&S.other.game) || !pk_apply(&S.other.game, S.me, x)) return PK_API_FAN_REFUSED;
    S.m = S.other;
    return PK_API_FAN_MOVED;
}
