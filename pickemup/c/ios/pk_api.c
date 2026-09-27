/* Pick 'Em Up - the bridge. See include/pk_api.h. */
#include "include/pk_api.h"
#include "pk_api_layout.h"
#include "../src/pk.h"
#include "../src/pk_msg.h"
#include "../src/pk_plan.h"
#include "../src/pk_say.h"
#include "../src/pk_view.h"
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
    char        text[PK_MSG_MAX_TEXT];
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
    S.rec_n = pk_rec_forget(S.rec, S.rec_n, &S.m);
    S.rec_dirty = 1;
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
    }
    if (!started) return empty(out, cap);
    switch (what) {
    case PK_API_W_CAPTION:      return pk_say_caption(g, arg, S.names, out, cap);
    case PK_API_W_HEADLINE:     return pk_say_headline(g, me, S.names, out, cap);
    case PK_API_W_SUBLINE:      return pk_say_subline(g, me, S.names, out, cap);
    case PK_API_W_DECK_LEFT:    return pk_say_deck_left(g, out, cap);
    case PK_API_W_DIR:          return pk_say_dir(g, out, cap);
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
