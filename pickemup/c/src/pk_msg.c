/* Pick 'Em Up - the envelope. See pk_msg.h; everything that is not the game
 * is shared/c/msg_whole's. */
#include "pk_msg.h"
#include <string.h>

const MsgWholeSpec PK_MSG_SPEC = {
    .seat_salt = "pickemup.seat.1|", .game_salt = "pickemup.game.1|",
    .magic = PK_MSG_MAGIC, .format = PK_MSG_FORMAT,
    .flag_dm = PK_FLAG_DM, .flag_left = PK_FLAG_LEFT,
    .flags_known = PK_FLAGS_KNOWN, .flags_live = PK_FLAG_TIP_SAID,
    .counters = 2,                    /* bubbles, then turns */
    .group_cap = PK_LOBBY_GROUP_CAP,
    .max_bytes = PK_MSG_MAX_BYTES,
    .rec_len = PK_REC_LEN, .rec_bytes = PK_REC_BYTES,
};

void pk_tag(const uint8_t seed[32], const uint8_t *id, int id_len, uint8_t out[PK_TAG_LEN])
{
    msg_whole_tag(&PK_MSG_SPEC, seed, id, id_len, out);
}

void pk_game_id(const uint8_t seed[32], uint8_t out[8])
{
    msg_whole_game_id(&PK_MSG_SPEC, seed, out);
}

static int started(const PkMsg *m)
{
    return msg_whole_started(&m->w);
}

static int tip_said_of(const PkGame *g)
{
    for (int i = g->hist_n - 1; i >= 0; i--)
        if (g->hist[i].kind == PK_A_BUBBLE) return (g->hist[i].b & PK_BR_SAID) != 0;
    return 0;
}

/* ---- the lobby ------------------------------------------------------------------ */

int pk_msg_new(PkMsg *m, const uint8_t seed[32], int dm, const uint8_t tag[PK_TAG_LEN],
               const uint8_t *name, int name_len)
{
    MsgWhole w;
    int e = msg_whole_new(&w, seed, dm, tag, name, name_len);
    if (e) return e;
    memset(m, 0, sizeof *m);
    m->w = w;
    return PK_EOK;
}

void pk_msg_lobby(const PkMsg *m, PkLobby *l)
{
    msg_whole_lobby(&PK_MSG_SPEC, &m->w, l);
}

int pk_msg_offered(const PkMsg *m, int seat)
{
    return msg_whole_offered(&PK_MSG_SPEC, &m->w, seat);
}

int pk_msg_seat_of_tag(const PkMsg *m, const uint8_t tag[PK_TAG_LEN])
{
    return msg_whole_seat_of_tag(&m->w, tag);
}

int pk_msg_join(PkMsg *m, const uint8_t tag[PK_TAG_LEN], const uint8_t *name, int name_len)
{
    return msg_whole_join(&PK_MSG_SPEC, &m->w, tag, name, name_len);
}

int pk_msg_leave(PkMsg *m, int seat)
{
    return msg_whole_leave(&PK_MSG_SPEC, &m->w, seat);
}

int pk_msg_start(PkMsg *m, int seat)
{
    PkLobby l;
    pk_msg_lobby(m, &l);
    static PkGame g;
    if (!pk_lobby_start(&l, seat, m->w.seed, &g)) return PK_EREFUSED;
    m->game = g;
    msg_whole_mark_started(&m->w, seat);
    return PK_EOK;
}

int pk_msg_join_start(PkMsg *m, const uint8_t tag[PK_TAG_LEN], const uint8_t *name, int name_len)
{
    PkLobby l;
    pk_msg_lobby(m, &l);
    if (!msg_lobby_roster_can_join_and_start(&l)) return PK_EREFUSED;
    static PkMsg t;
    t = *m;
    int s = pk_msg_join(&t, tag, name, name_len);
    if (s < 0) return s;
    int e = pk_msg_start(&t, s);
    if (e) return e;
    *m = t;
    return s;
}

int pk_msg_plan_lobby(const PkMsg *before, const PkMsg *after, PkEvent *out, int cap)
{
    PkLobby lb, la;
    msg_whole_lobby_pair(&PK_MSG_SPEC, &before->w, &after->w, &lb, &la);
    return pk_plan_lobby(&lb, &la, out, cap);
}

/* ---- the bytes ------------------------------------------------------------------- */

/* The same message, by everything that is not derived (phase within a
 * started game, TIP_SAID): what the encoder is asked to write against what
 * it reads back. */
static int same_msg(const PkMsg *a, const PkMsg *b)
{
    return msg_whole_same(&a->w, &b->w) && (!started(a) || pk_hash(&a->game) == pk_hash(&b->game));
}

int pk_msg_encode(const PkMsg *m, uint8_t *out, int cap)
{
    static uint8_t buf[PK_MSG_MAX_BYTES];
    static PkMsg back;
    const int live = started(m);
    const PkGame *g = &m->game;
    int e = msg_whole_writable(&PK_MSG_SPEC, &m->w);
    if (e) return e;
    if (live) {
        if (g->n != m->w.n_seats || g->starter != m->w.starter) return PK_EROSTER;
        if (g->b_open) return PK_EGAME;
    }
    const uint16_t counters[2] = { live ? g->bubbles : 0, live ? g->turns : 0 };
    const int head = msg_whole_put_head(&PK_MSG_SPEC, &m->w,
                                        !live ? PK_PHASE_WAITING : g->over ? PK_PHASE_FINISHED : PK_PHASE_LIVE,
                                        live && tip_said_of(g) ? PK_FLAG_TIP_SAID : 0, counters, buf);
    int bn = 0;
    if (live) {
        bn = pk_code_encode(g, buf + head + PK_CHECK_LEN, PK_CODE_MAX);
        if (bn < 0) return PK_EGAME;
    }
    const int n = msg_whole_seal(buf, head, bn);

    /* WHAT WAS WRITTEN IS READ BACK: a payload this build would refuse, or one
     * that reads as a different game, is never handed to a host. */
    e = pk_msg_decode(buf, n, &back);
    if (e) return e;
    if (!same_msg(&back, m)) return PK_EGAME;
    if (n > cap) return PK_ECAP;
    memcpy(out, buf, (size_t)n);
    return n;
}

int pk_msg_decode(const uint8_t *in, int n, PkMsg *out)
{
    static PkMsg m;
    memset(&m, 0, sizeof m);
    int flags, at, bn;
    uint16_t c[2];
    int e = msg_whole_read(&PK_MSG_SPEC, in, n, &m.w, &flags, c, &at, &bn);
    if (e) return e;
    m.tip_said = (flags & PK_FLAG_TIP_SAID) != 0;
    if (started(&m)) {
        const int bubbles = c[0], turns = c[1];
        if (!pk_code_decode(&m.game, m.w.seed, m.w.n_seats, m.w.starter, bubbles, in + at, bn)) return PK_EGAME;
        /* the header says what the replay says, or nothing is believed */
        if (m.game.turns != turns || m.game.bubbles != bubbles
            || (m.game.over ? PK_PHASE_FINISHED : PK_PHASE_LIVE) != m.w.phase
            || tip_said_of(&m.game) != m.tip_said)
            return PK_EGAME;
    }
    *out = m;
    return PK_EOK;
}

int pk_msg_text_encode(const PkMsg *m, char *out, int cap)
{
    static uint8_t b[PK_MSG_MAX_BYTES];
    return msg_whole_text_put(b, pk_msg_encode(m, b, (int)sizeof b), out, cap);
}

int pk_msg_text_decode(const char *text, PkMsg *out)
{
    static char span[PK_MSG_MAX_TEXT];
    static uint8_t b[PK_MSG_MAX_BYTES];
    int n = msg_whole_text_get(text, span, (int)sizeof span, b, (int)sizeof b);
    return n < 0 ? n : pk_msg_decode(b, n, out);
}

/* ---- two messages ---------------------------------------------------------------- */

int pk_msg_same_game(const PkMsg *a, const PkMsg *b)
{
    return msg_whole_same_game(&a->w, &b->w);
}

int pk_msg_prefer(const PkMsg *mine, const PkMsg *tapped)
{
    if (!pk_msg_same_game(mine, tapped)) return 1;                                  /* 1 */
    int r;
    const int la = started(mine), lb = started(tapped);
    if ((r = msg_whole_more(la, lb))) return r;                                     /* 2 */
    if ((r = msg_whole_more(la ? mine->game.turns : 0, lb ? tapped->game.turns : 0))) return r;       /* 3 */
    if ((r = msg_whole_more(la ? mine->game.bubbles : 0, lb ? tapped->game.bubbles : 0))) return r;   /* 4 */
    if ((r = msg_whole_more(la && tip_said_of(&mine->game), lb && tip_said_of(&tapped->game)))) return r; /* 5 */
    if ((r = msg_whole_prefer_lobby(&mine->w, &tapped->w))) return r;               /* 6 */
    static uint8_t ba[PK_MSG_MAX_BYTES], bb[PK_MSG_MAX_BYTES];                      /* 7 */
    return msg_whole_prefer_bytes(ba, pk_msg_encode(mine, ba, (int)sizeof ba),
                                  bb, pk_msg_encode(tapped, bb, (int)sizeof bb));
}

/* Where bubble k (0-based) of `g` ends in hist: the next BUBBLE record, or
 * the end. */
static int bubble_end(const PkGame *g, int k)
{
    int seen = 0;
    for (int i = 0; i < g->hist_n; i++)
        if (g->hist[i].kind == PK_A_BUBBLE && seen++ == k + 1) return i;
    return g->hist_n;
}

int pk_common_bubbles(const PkMsg *a, const PkMsg *b)
{
    if (!pk_msg_same_game(a, b) || !started(a) || !started(b)) return 0;
    if (a->game.n != b->game.n || a->game.starter != b->game.starter) return 0;
    int most = a->game.bubbles < b->game.bubbles ? a->game.bubbles : b->game.bubbles;
    int k = 0;
    while (k < most) {
        int ea = bubble_end(&a->game, k), eb = bubble_end(&b->game, k);
        if (ea != eb || memcmp(a->game.hist, b->game.hist, sizeof(PkAct) * (size_t)ea)) break;
        k++;
    }
    return k;
}

/* ---- the seats --------------------------------------------------------------------- */

int pk_msg_sender(const PkMsg *m)
{
    if (!started(m)) return msg_whole_lobby_sender(&m->w);
    const PkGame *g = &m->game;
    for (int i = g->hist_n - 1; i >= 0; i--)
        if (g->hist[i].kind == PK_A_BUBBLE && (g->hist[i].b & PK_BR_SEALED)) return g->hist[i].a;
    return m->w.starter < m->w.n_seats ? m->w.starter : -1;
}

int pk_msg_resolve(const PkMsg *m, int record, int tag_seat, int is_dm, int i_sent,
                   const uint8_t *name, int name_len, int *by)
{
    return msg_whole_resolve(&m->w, pk_msg_sender(m), record, tag_seat, is_dm, i_sent, name, name_len, by);
}

int pk_rec_find(const uint8_t *recs, int n, const PkMsg *m)
{
    return msg_whole_rec_find(&PK_MSG_SPEC, recs, n, &m->w);
}

int pk_rec_forget(uint8_t *recs, int n, const PkMsg *m)
{
    return msg_whole_rec_forget(&PK_MSG_SPEC, recs, n, &m->w);
}

int pk_rec_put(uint8_t *recs, int n, const PkMsg *m, int seat)
{
    return msg_whole_rec_put(&PK_MSG_SPEC, recs, n, &m->w, seat);
}
