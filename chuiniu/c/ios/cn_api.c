/* Chui Niu - the bridge. See include/cn_api.h. */
#include "include/cn_api.h"
#include "cn_api_layout.h"
#include "../src/cn.h"
#include "../src/cn_msg.h"
#include "../src/cn_plan.h"
#include "../src/cn_say.h"
#include "../src/cn_view.h"
#include "../src/cn_beats.h"
#include "../src/cn_stage.h"
#include "../src/cn_scene.h"
#include "../i18n/keys.h"
#include <stddef.h>
#include <string.h>

/* The host's numbers against the kernel's, held together by the compiler. */
_Static_assert(CN_API_TEXT_MAX >= CN_MSG_MAX_TEXT, "the longest link fits the host buffer");
_Static_assert(CN_API_REC_BYTES == CN_REC_FILE_BYTES, "the seat records, stored");
_Static_assert(CN_API_SPECTATOR == CN_VIEW_SPECTATOR && CN_API_ALL == CN_VIEW_ALL, "viewers");
_Static_assert(CN_API_ME != CN_VIEW_SPECTATOR && CN_API_ME != CN_VIEW_ALL && CN_API_ME < 0, "CN_API_ME is its own viewer");
_Static_assert(CN_API_EVENTS >= CN_EVENTS_PER_MOVE + 1, "one move's plan fits");
_Static_assert(CN_API_W_RULE == 16 && CN_RULES_N == 6, "the rules the header promises");

#ifndef SG_LAYOUT_HASH
#define SG_LAYOUT_HASH 0u            /* not stamped: matches no generated module */
#endif

#define MAX_ID 64

/* THE RESIDENT, and the scratch the calls fill. Nothing here is allocated. */
static struct {
    CnMsg    m;
    int      have;                   /* a message is resident                  */
    uint8_t  id[MAX_ID];
    int      id_n;
    uint8_t  nick[CN_NAME_MAX_BYTES];
    int      nick_n;
    uint8_t  rec[CN_REC_BYTES];
    int      rec_n;
    int      rec_dirty;
    /* THE SENDER FACT, about one message only */
    char     sent_text[CN_MSG_MAX_TEXT];
    int      sent_dm, sent_mine, sent_set;
    /* my seat in the resident message, and the witness */
    int      me, by;
    /* my staged move: never part of the resident until commit */
    int      staged;
    CnMove   move;
    /* the roster's names, NUL-terminated, for the words */
    char     name[CN_MAX_SEATS][CN_NAME_MAX_BYTES + 1];
    const char *names[CN_MAX_SEATS];
    /* what a read hands back */
    CnApiTable  table;
    CnView      view;
    CnApiEvents events;
    CnMsg       other, other2, prior;
    CnGame      scratch;
    CnBeats     beats;
    CnBeatFrame frame;
    int         beats_ok;
    uint32_t    beats_serial;
} S = { .me = -1 };

uint32_t cn_api_layout_hash(void) { return (uint32_t)SG_LAYOUT_HASH; }

/* ---- who I am -------------------------------------------------------------- */

void cn_api_me(const uint8_t *id, int n)
{
    if (!id || n < 0) n = 0;
    if (n > MAX_ID) n = MAX_ID;
    if (n) memcpy(S.id, id, (size_t)n);
    S.id_n = n;
}

void cn_api_nickname(const uint8_t *name, int n)
{
    if (!name || msg_seat_name_verdict(name, n) != CN_NAME_OK) { S.nick_n = 0; return; }
    memcpy(S.nick, name, (size_t)n);
    S.nick_n = n;
}

int cn_api_name_verdict(const uint8_t *name, int n) { return msg_seat_name_verdict(name, n); }

void cn_api_seats_load(const uint8_t *bytes, int n)
{
    S.rec_n = cn_rec_load(S.rec, bytes, n);
    S.rec_dirty = 0;
}

int cn_api_seats_dirty(void) { return S.rec_dirty; }

int cn_api_seats_save(uint8_t *out, int cap)
{
    int n = cn_rec_save(S.rec, S.rec_n, out, cap);
    if (n >= 0) S.rec_dirty = 0;
    return n;
}

/* ---- the resident ----------------------------------------------------------- */

static int started(void) { return S.have && S.m.phase != CN_PHASE_WAITING; }

static void my_tag(const CnMsg *m, uint8_t tag[CN_TAG_LEN])
{
    cn_tag(m->seed, S.id, S.id_n, tag);
}

static void names_of(void)
{
    for (int s = 0; s < CN_MAX_SEATS; s++) {
        int n = s < S.m.n_seats ? S.m.seat[s].name_len : 0;
        memcpy(S.name[s], S.m.seat[s].name, (size_t)n);
        S.name[s][n] = 0;
        S.names[s] = S.name[s];
    }
}

static void record(int seat)
{
    S.rec_n = cn_rec_put(S.rec, S.rec_n, &S.m, seat);
    S.rec_dirty = 1;
}

/* Which seat is mine in the resident message: the four witnesses
 * (cn_msg.h), and a seat found any way but the record is recorded. */
static void seat_me(int is_dm, int i_sent)
{
    uint8_t tag[CN_TAG_LEN];
    my_tag(&S.m, tag);
    int rec = cn_rec_find(S.rec, S.rec_n, &S.m);
    S.me = cn_msg_resolve(&S.m, rec, cn_msg_seat_of_tag(&S.m, tag), is_dm, i_sent,
                          S.nick, S.nick_n, &S.by);
    if (S.me >= 0 && S.by != CN_BY_RECORD) record(S.me);
    names_of();
}

/* The resident changed under a lobby action of mine: I am at `seat`. */
static void adopt_mine(int seat)
{
    S.have = 1;
    S.staged = 0;
    S.me = seat;
    S.by = CN_BY_RECORD;
    if (seat >= 0) record(seat);
    names_of();
}

int cn_api_new(const uint8_t seed[32], int dm)
{
    uint8_t tag[CN_TAG_LEN];
    if (!seed) return CN_EGAME;
    cn_tag(seed, S.id, S.id_n, tag);
    int e = cn_msg_new(&S.other, seed, dm, tag, S.nick, S.nick_n);
    if (e) return e;
    S.m = S.other;
    S.sent_set = 0;
    S.beats_ok = 0;
    adopt_mine(0);
    return CN_EOK;
}

int cn_api_check(const char *text)
{
    return cn_msg_text_decode(text, &S.other);
}

int cn_api_read(const char *text)
{
    int e = cn_msg_text_decode(text, &S.other);
    if (e) return e;
    S.m = S.other;
    S.have = 1;
    S.staged = 0;
    int mine = S.sent_set && text && !strcmp(text, S.sent_text);
    seat_me(mine ? S.sent_dm : 0, mine ? S.sent_mine : CN_SENT_UNKNOWN);
    return CN_EOK;
}

void cn_api_sender(const char *text, int is_dm, int i_sent)
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

/* The resident with my staged move applied, in S.other. */
static int with_staged(void)
{
    S.other = S.m;
    return !S.staged || cn_apply(&S.other.game, S.me, S.move);
}

int cn_api_text(char *out, int cap)
{
    if (!S.have || !out) return CN_EGAME;
    if (!with_staged()) return CN_EGAME;
    return cn_msg_text_encode(&S.other, out, cap);
}

int cn_api_commit(void)
{
    if (!started() || !S.staged) return 0;
    if (!cn_apply(&S.m.game, S.me, S.move)) return 0;
    S.staged = 0;
    return 1;
}

/* ---- the lobby -------------------------------------------------------------- */

int cn_api_join(void)
{
    if (!S.have) return CN_EGAME;
    uint8_t tag[CN_TAG_LEN];
    my_tag(&S.m, tag);
    S.other = S.m;
    int s = cn_msg_join(&S.other, tag, S.nick, S.nick_n);
    if (s < 0) return s;
    S.m = S.other;
    adopt_mine(s);
    return s;
}

int cn_api_leave(void)
{
    if (!S.have || S.me < 0) return CN_EREFUSED;
    S.other = S.m;
    int e = cn_msg_leave(&S.other, S.me);
    if (e) return e;
    /* THE RECORD STAYS: its tag now has no row, which is this device's word
     * that it left (CN_REC_GONE), so a namesake who takes the freed name is
     * never my seat. A rejoin records the new row over it. */
    S.m = S.other;
    S.me = -1;
    S.by = CN_BY_NONE;
    S.staged = 0;
    names_of();
    return CN_EOK;
}

int cn_api_start(void)
{
    if (!S.have || S.me < 0) return CN_EREFUSED;
    S.other = S.m;
    int e = cn_msg_start(&S.other, S.me);
    if (e) return e;
    S.m = S.other;
    S.staged = 0;
    names_of();
    return CN_EOK;
}

int cn_api_join_start(void)
{
    if (!S.have) return CN_EGAME;
    uint8_t tag[CN_TAG_LEN];
    my_tag(&S.m, tag);
    S.other = S.m;
    int s = cn_msg_join_start(&S.other, tag, S.nick, S.nick_n);
    if (s < 0) return s;
    S.m = S.other;
    adopt_mine(s);
    return s;
}

/* ---- my move ------------------------------------------------------------------ */

static const CnGame *live(void)
{
    return started() && S.me >= 0 ? &S.m.game : 0;
}

static int stage(int q, int f)
{
    const CnGame *g = live();
    if (!g || q < 0 || q > 255 || f < 0 || f > 255) return 0;
    CnMove m = { (uint8_t)q, (uint8_t)f };
    if (!cn_is_legal(g, S.me, m)) return 0;
    S.move = m;
    S.staged = 1;
    return 1;
}

int cn_api_raise(int quantity, int face) { return quantity >= 1 && stage(quantity, face); }
int cn_api_call(void) { return stage(0, 0); }

int cn_api_cancel(void)
{
    if (!S.staged) return 0;
    S.staged = 0;
    return 1;
}

int cn_api_can_raise(int quantity, int face)
{
    const CnGame *g = live();
    if (!g || quantity < 1 || quantity > 255 || face < 0 || face > 255) return 0;
    CnMove m = { (uint8_t)quantity, (uint8_t)face };
    return cn_is_legal(g, S.me, m);
}

/* ---- reading it ------------------------------------------------------------------ */

const void *cn_api_table(void)
{
    CnApiTable *t = &S.table;
    memset(t, 0, sizeof *t);
    t->me = CN_SEAT_NONE;
    t->starter = CN_SEAT_NONE;
    t->sender = CN_SEAT_NONE;
    if (!S.have) return t;
    t->readable = 1;
    t->phase = S.m.phase;
    t->game_phase = started() ? S.m.game.phase : 0;
    t->dm = S.m.dm;
    t->n_seats = S.m.n_seats;
    t->me = S.me >= 0 ? (uint8_t)S.me : CN_SEAT_NONE;
    t->by = (uint8_t)S.by;
    t->offered = (uint8_t)cn_msg_offered(&S.m, S.me);
    CnLobby l;
    cn_msg_lobby(&S.m, &l);
    t->can_exit = (uint8_t)msg_lobby_roster_can_exit(&l, S.me);
    t->can_join_start = (uint8_t)(S.me < 0 && msg_lobby_roster_can_join_and_start(&l));
    t->starter = S.m.starter;
    int snd = cn_msg_sender(&S.m);
    t->sender = snd >= 0 ? (uint8_t)snd : CN_SEAT_NONE;
    if (S.staged) {
        t->staged = cn_is_call(S.move) ? CN_API_STAGED_CALL : CN_API_STAGED_BID;
        t->staged_q = S.move.q;
        t->staged_f = S.move.f;
    }
    t->lobby_rev = S.m.lobby_rev;
    t->moves = started() ? S.m.game.hist_n : 0;
    for (int s = 0; s < S.m.n_seats; s++) {
        t->seat[s].name_len = S.m.seat[s].name_len;
        memcpy(t->seat[s].name, S.m.seat[s].name, S.m.seat[s].name_len);
    }
    return t;
}

static int viewer_of(int viewer)
{
    if (viewer == CN_API_ME) return S.me >= 0 ? S.me : CN_VIEW_SPECTATOR;
    return viewer;
}

const void *cn_api_view(int viewer)
{
    if (!started()) return 0;
    cn_view(&S.m.game, viewer_of(viewer), &S.view);
    return &S.view;
}

const void *cn_api_plan(int from, int to)
{
    if (!started()) return 0;
    int n = cn_plan(&S.m.game, from, to, S.events.ev, CN_API_EVENTS);
    if (n < 0) return 0;
    S.events.n = (uint16_t)n;
    return &S.events;
}

/* THE STAGED PLAN STOPS AT THE CALL (K8): a staged call's reveal is not the
 * stager's to see before it is sent. */
static int staged_events(void)
{
    if (!live() || !S.staged) return 0;
    int n = cn_plan_move(&S.m.game, S.move, S.events.ev, CN_API_EVENTS);
    if (n < 0) return -1;
    for (int i = 0; i < n; i++)
        if (S.events.ev[i].kind == CN_EV_CALL) { n = i + 1; break; }
    return n;
}

const void *cn_api_plan_staged(void)
{
    int n = staged_events();
    if (n < 0) return 0;
    S.events.n = (uint16_t)n;
    return &S.events;
}

/* ---- the motion ---------------------------------------------------------------------- */

static const void *laid(int n)
{
    S.beats_ok = 0;
    if (n < 0) return 0;
    S.beats.serial = ++S.beats_serial;
    S.beats_ok = 1;
    return &S.beats;
}

const void *cn_api_beats(int from, int to)
{
    if (!started()) return laid(-1);
    int n = cn_plan(&S.m.game, from, to, S.events.ev, CN_API_EVENTS);
    if (n < 0 || !cn_replay(&S.scratch, &S.m.game, from < 0 ? 0 : from)) return laid(-1);
    CnBeatFrame start;
    cn_beats_start(&S.scratch, &start);
    return laid(cn_beats_build(S.events.ev, n, &start, &S.beats));
}

const void *cn_api_beats_staged(void)
{
    int n = staged_events();
    if (n <= 0) return laid(-1);
    CnBeatFrame start;
    cn_beats_start(&S.m.game, &start);
    return laid(cn_beats_build(S.events.ev, n, &start, &S.beats));
}

const void *cn_api_beats_now(void) { return S.beats_ok ? &S.beats : 0; }

const void *cn_api_beats_frame(uint32_t now_ms)
{
    if (!S.beats_ok) return 0;
    cn_beats_frame(&S.beats, now_ms, &S.frame);
    return &S.frame;
}

int cn_api_adopt(const char *text)
{
    const int had = S.have;
    S.prior = S.m;
    int e = cn_api_read(text);
    if (e) return e;
    S.beats_ok = 0;
    if (!started()) return CN_EOK;                          /* a lobby: no motion */
    const int tip = S.m.game.hist_n;
    const int warm = had && cn_msg_same_game(&S.prior, &S.m) && S.prior.phase != CN_PHASE_WAITING;
    if (warm) {
        int common = cn_common_moves(&S.prior, &S.m);
        if (common < tip) cn_api_beats(common, tip);
        return CN_EOK;                                      /* the same bubble: none */
    }
    cn_api_beats(tip ? tip - 1 : -1, tip);
    return CN_EOK;
}

/* ---- the words ------------------------------------------------------------------------ */

int cn_api_string(int key, char *out, int cap)
{
    if (key < 0 || key >= CN_K_COUNT || !out || cap < 1) return -1;
    const char *s = cn_text(key);
    int n = (int)strlen(s);
    if (n >= cap) return -1;
    memcpy(out, s, (size_t)n + 1);
    return n;
}

static int lobby_caption(int which, int seat, char *out, int cap)
{
    if (!S.have || seat < 0 || seat >= S.m.n_seats) return -1;
    return cn_say_lobby_caption(which, S.names[seat], out, cap);
}

static int staged_caption(char *out, int cap)
{
    if (!started()) {
        if (!S.have) return -1;
        if (S.m.left) { out[0] = 0; return 0; }        /* captioned by W_LEFT before the leave */
        int snd = cn_msg_sender(&S.m);
        return lobby_caption(S.m.n_seats == 1 ? CN_SAY_INVITE : CN_SAY_JOINED, snd, out, cap);
    }
    if (S.staged) {
        CnEvent ev[CN_EVENTS_PER_MOVE + 1];
        int n = cn_plan_move(&S.m.game, S.move, ev, (int)(sizeof ev / sizeof ev[0]));
        if (n < 0) return -1;
        return cn_say_caption_of(ev, n, S.names, out, cap);
    }
    return cn_say_caption(&S.m.game, S.m.game.hist_n, S.names, out, cap);
}

int cn_api_words(int what, int arg, char *out, int cap)
{
    if (!out || cap < 1) return -1;
    const CnGame *g = started() ? &S.m.game : 0;
    switch (what) {
    case CN_API_W_CAPTION:        return g ? cn_say_caption(g, arg, S.names, out, cap) : -1;
    case CN_API_W_STAGED_CAPTION: return staged_caption(out, cap);
    case CN_API_W_HEADLINE:
        if (!S.have) return -1;
        if (!g) return cn_api_string(cn_msg_offered(&S.m, S.me) == CN_LOBBY_FULL ? CN_K_LOBBY_FULL
                                                                                : CN_K_LOBBY_WAITING, out, cap);
        if (S.staged) return cn_say_staged(g, S.move, out, cap);
        return cn_say_headline(g, S.me, S.names, out, cap);
    case CN_API_W_SUBLINE:        return g ? cn_say_subline(g, S.names, out, cap) : -1;
    case CN_API_W_OUTCOME:        return g ? cn_say_outcome(g, S.names, out, cap) : -1;
    case CN_API_W_SEAT:           return S.have && arg >= 0 && arg < S.m.n_seats ? cn_say_seat(S.names, arg, out, cap) : -1;
    case CN_API_W_BID:            return cn_say_bid(arg / 8, arg % 8, 0, out, cap);
    case CN_API_W_DICE_N:         return cn_say_dice_n(arg, out, cap);
    case CN_API_W_TABLE:          return g ? cn_say_table(g, out, cap) : -1;
    case CN_API_W_REVEAL_COUNT:   return g ? cn_say_reveal_count(g, out, cap) : -1;
    case CN_API_W_LOBBY_ROW:
        return S.have && arg >= 0 && arg < S.m.n_seats ? cn_say_lobby_row(S.names, arg, arg == S.me, out, cap) : -1;
    case CN_API_W_INVITE:         return lobby_caption(CN_SAY_INVITE, arg, out, cap);
    case CN_API_W_JOINED:         return lobby_caption(CN_SAY_JOINED, arg, out, cap);
    case CN_API_W_LEFT:           return lobby_caption(CN_SAY_LEFT, arg, out, cap);
    case CN_API_W_ERROR:          return cn_say_error(arg, out, cap);
    case CN_API_W_RULES_TITLE:    return cn_say_rules_title(out, cap);
    case CN_API_W_RULE:           return cn_say_rule(arg, out, cap);
    default:                      return -1;
    }
}

int cn_api_caption_probe(int what, const char *who, int q, int f, char *out, int cap)
{
    if (!out || cap < 1 || !who) return -1;
    const char *names[CN_MAX_SEATS] = { who };
    CnEvent ev;
    memset(&ev, 0, sizeof ev);
    switch (what) {
    case CN_API_P_START:  ev.kind = CN_EV_ROUND; break;
    case CN_API_P_BID:    ev.kind = CN_EV_BID;  ev.q = (uint8_t)q; ev.f = (uint8_t)f; ev.move = 1; break;
    case CN_API_P_CALL:   ev.kind = CN_EV_CALL; ev.q = (uint8_t)q; ev.f = (uint8_t)f; ev.move = 1; break;
    case CN_API_P_INVITE: return cn_say_lobby_caption(CN_SAY_INVITE, who, out, cap);
    case CN_API_P_JOINED: return cn_say_lobby_caption(CN_SAY_JOINED, who, out, cap);
    case CN_API_P_LEFT:   return cn_say_lobby_caption(CN_SAY_LEFT, who, out, cap);
    case CN_API_P_PLATE_BID: return cn_say_bid(q, f, 0, out, cap);
    case CN_API_P_TALLY:  return cn_say_tally(q, out, cap);
    default:              return -1;
    }
    if (ev.kind != CN_EV_ROUND && (q < 1 || q > CN_MAX_DICE || f < 1 || f > CN_FACES)) return -1;
    return cn_say_caption_of(&ev, 1, names, out, cap);
}

int cn_api_caption_width(const char *line) { return cn_cap_width(line); }
int cn_api_caption_budget(void) { return CN_CAP_BUDGET; }
int cn_api_caption_unit(void) { return CN_CAP_UNIT; }

/* ---- two messages ------------------------------------------------------------------------ */

int cn_api_prefer(const char *mine, const char *tapped)
{
    int a = cn_msg_text_decode(mine, &S.other), b = cn_msg_text_decode(tapped, &S.other2);
    if (a || b) return a && b ? 0 : a ? 1 : -1;
    return cn_msg_prefer(&S.other, &S.other2);
}

int cn_api_same_game(const char *a, const char *b)
{
    if (cn_msg_text_decode(a, &S.other) || cn_msg_text_decode(b, &S.other2)) return 0;
    return cn_msg_same_game(&S.other, &S.other2);
}

int cn_api_common(const char *a, const char *b)
{
    if (cn_msg_text_decode(a, &S.other) || cn_msg_text_decode(b, &S.other2)) return -1;
    return cn_common_moves(&S.other, &S.other2);
}

/* ---- the stage ------------------------------------------------------------------------- */

/* ONE STAGE A PROCESS: the renderer is one (cn_scene.h), so its handle is too */
static CnStage STAGE;
static int stage_inited;

int cn_api_stage_init(const uint8_t *pack, size_t pack_len)
{
    int e = cn_stage_init(&STAGE, pack, pack_len);
    stage_inited = e == 0;
    return e;
}

void cn_api_stage_purge(void) { if (stage_inited) cn_stage_purge(&STAGE); }

int cn_api_stage_attach(void *arena, size_t bytes) { return stage_inited ? cn_stage_attach(&STAGE, arena, bytes) : CN_STAGE_E_ARENA; }

/* the current plan's beat of `kind`, or -1 */
static int beat_of(int kind)
{
    if (!S.beats_ok) return -1;
    for (int i = 0; i < S.beats.n; i++) if (S.beats.beat[i].kind == kind) return i;
    return -1;
}

/* ---- the throw: once a phone a round ------------------------------------------------
 *
 * A round's throw plays on this phone until it has been watched to its end,
 * and never again: the game's record keeps the newest round watched, so a new
 * launch of the extension, a bid arriving or the drawer changing size does
 * not throw it again. A throw cut off before its end was not watched. */
static int roll_pending(void)
{
    if (!started() || S.me < 0 || S.m.game.phase == CN_PH_OVER) return 0;
    return cn_rec_seen(S.rec, S.rec_n, &S.m) <= S.m.game.round;
}

int cn_api_roll_pending(void) { return roll_pending(); }

int cn_api_roll_seen(int round)
{
    if (!started() || S.me < 0 || round < 0 || round > S.m.game.round) return 0;
    if (!cn_rec_see(S.rec, S.rec_n, &S.m, round + 1)) return 0;
    S.rec_dirty = 1;
    return 1;
}

/* THE STAGE'S INPUT IS THE RESIDENT GAME'S (I22): the host names the screen,
 * the kernel fills the table. My dice are the view's (sorted, so the HUD's die
 * places line up with CnView.my_dice and shown), a reveal's are the newest
 * call's, and the throw's seed is the round's. A table throws while its round
 * is pending (cn_api_roll_pending): the kernel's call, never the host's. */
const void *cn_api_stage_begin(int kind, float w, float h, float scale)
{
    if (!stage_inited || !started() || S.me < 0) return 0;
    const CnGame *g = &S.m.game;
    CnView v;
    cn_view(g, S.me, &v);
    CnStageIn in;
    memset(&in, 0, sizeof in);
    in.kind = (uint8_t)kind; in.seats = g->n; in.me = (uint8_t)S.me;
    in.turn = g->turn != CN_SEAT_NONE ? g->turn : (uint8_t)((S.me + 1) % g->n);
    in.w = w; in.h = h; in.scale = scale;
    in.roll_at_ms = CN_STAGE_NO_ROLL;
    int round = g->round;
    if (kind == CN_STAGE_REVEAL) {
        if (!v.revealed) return 0;
        in.turn = (uint8_t)((S.me + 1) % g->n);
        for (int s = 0; s < g->n; s++) {
            in.dice[s] = v.shown_n[s];
            if (!v.shown_n[s]) in.out_mask |= (uint8_t)(1 << s);
            memcpy(&in.faces[s * CN_STAGE_DICE], &v.shown[s * CN_START_DICE], v.shown_n[s]);
        }
        in.known_mask = (uint8_t)((1 << g->n) - 1);
        round = g->phase == CN_PH_OVER ? g->round : g->round - 1;
    } else {
        if (kind == CN_STAGE_TABLE && g->phase == CN_PH_OVER) return 0;
        for (int s = 0; s < g->n; s++) {
            in.dice[s] = g->dice_n[s];
            if (!g->dice_n[s]) in.out_mask |= (uint8_t)(1 << s);
        }
        if (kind == CN_STAGE_TABLE) {
            memcpy(&in.faces[S.me * CN_STAGE_DICE], v.my_dice, v.my_n);
            in.known_mask = (uint8_t)(1 << S.me);
            if (roll_pending()) { int b = beat_of(CN_BK_SHAKE); in.roll_at_ms = b >= 0 ? S.beats.beat[b].start_ms : 0; }
        }
    }
    in.seed = cn_stage_round_seed(g->seed, round < 0 ? 0 : round);
    return cn_stage_begin(&STAGE, &in);
}

/* a reveal's cups lift with the current plan's LIFT beat; with none, they are up */
static float lift_at(uint32_t now_ms)
{
    int b = beat_of(CN_BK_LIFT);
    if (b < 0) return 1;
    CnBeatFrame f;
    cn_beats_frame(&S.beats, now_ms, &f);
    return f.prog[b];
}

float cn_api_stage_lift(uint32_t now_ms) { return lift_at(now_ms); }

int cn_api_stage_prepare_at(uint32_t now_ms, float peek, float lift)
{
    return stage_inited && cn_stage_prepare(&STAGE, now_ms, peek, lift);
}

int cn_api_stage_prepare(uint32_t now_ms, float peek) { return cn_api_stage_prepare_at(now_ms, peek, lift_at(now_ms)); }

_Static_assert(CN_API_STAGE_RGBA == CN_SCENE_OUT_RGBA && CN_API_STAGE_CA == CN_SCENE_OUT_PREMUL_BGRA, "the forms");
void cn_api_stage_output(int form) { cn_scene_output(form); }

void cn_api_stage_band(int pass, int band, int nbands) { if (stage_inited) cn_stage_band(&STAGE, pass, band, nbands); }

const uint8_t *cn_api_stage_pixels(void) { return stage_inited ? cn_stage_finish(&STAGE) : 0; }

const uint8_t *cn_api_stage_frame(uint32_t now_ms, float peek)
{
    if (!stage_inited) return 0;
    return cn_stage_frame(&STAGE, now_ms, peek, lift_at(now_ms), 0, 0);
}

const void *cn_api_stage_shot(void) { return stage_inited ? (const void *)cn_stage_shot(&STAGE) : 0; }

int cn_api_stage_name(int seat, const uint8_t *rgba, int w, int h, float w_pt, float h_pt)
{
    return stage_inited ? cn_stage_name(&STAGE, seat, rgba, w, h, w_pt, h_pt) : -1;
}

int cn_api_stage_done(uint32_t now_ms) { return stage_inited && cn_stage_done(&STAGE, now_ms); }

float cn_api_peek_ease(float t) { return cn_cam_peek_ease(t); }
