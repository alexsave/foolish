/* Tallybones - the envelope. See tb_msg.h; everything that is not the game
 * is shared/c/msg_whole's. */
#include "tb_msg.h"
#include <string.h>

const MsgWholeSpec TB_MSG_SPEC = {
    .seat_salt = "tallybones.seat.1|", .game_salt = "tallybones.game.1|",
    .magic = TB_MSG_MAGIC, .format = TB_MSG_FORMAT,
    .flag_dm = TB_FLAG_DM, .flag_left = TB_FLAG_LEFT,
    .flags_known = TB_FLAGS_KNOWN, .flags_live = 0,
    .counters = 2,                    /* bubbles, then turns */
    .group_cap = TB_LOBBY_GROUP_CAP,
    .max_bytes = TB_MSG_MAX_BYTES,
    .rec_len = TB_REC_LEN, .rec_bytes = TB_REC_BYTES,
};

void tb_tag(const uint8_t seed[32], const uint8_t *id, int id_len, uint8_t out[TB_TAG_LEN])
{
    msg_whole_tag(&TB_MSG_SPEC, seed, id, id_len, out);
}

void tb_game_id(const uint8_t seed[32], uint8_t out[8])
{
    msg_whole_game_id(&TB_MSG_SPEC, seed, out);
}

static int started(const TbMsg *m)
{
    return msg_whole_started(&m->w);
}

/* The moves a game says: its resident history, then its pending move. */
static int bubbles_of(const TbGame *g)
{
    return g->hist_n + (g->draft ? 1 : 0);
}

static TbMove move_at(const TbGame *g, int i)
{
    return i < g->hist_n ? g->hist[i] : g->pending;
}

static int same_moves(const TbGame *a, const TbGame *b)
{
    if (bubbles_of(a) != bubbles_of(b)) return 0;
    for (int i = 0; i < bubbles_of(a); i++) {
        TbMove x = move_at(a, i), y = move_at(b, i);
        if (memcmp(&x, &y, sizeof x)) return 0;
    }
    return 1;
}

/* ---- the lobby ------------------------------------------------------------------ */

int tb_msg_new(TbMsg *m, const uint8_t seed[32], int dm, const uint8_t tag[TB_TAG_LEN],
               const uint8_t *name, int name_len)
{
    MsgWhole w;
    int e = msg_whole_new(&w, seed, dm, tag, name, name_len);
    if (e) return e;
    memset(m, 0, sizeof *m);
    m->w = w;
    return TB_EOK;
}

void tb_msg_lobby(const TbMsg *m, TbLobby *l)
{
    msg_whole_lobby(&TB_MSG_SPEC, &m->w, l);
}

int tb_msg_offered(const TbMsg *m, int seat)
{
    return msg_whole_offered(&TB_MSG_SPEC, &m->w, seat);
}

int tb_msg_seat_of_tag(const TbMsg *m, const uint8_t tag[TB_TAG_LEN])
{
    return msg_whole_seat_of_tag(&m->w, tag);
}

int tb_msg_join(TbMsg *m, const uint8_t tag[TB_TAG_LEN], const uint8_t *name, int name_len)
{
    return msg_whole_join(&TB_MSG_SPEC, &m->w, tag, name, name_len);
}

int tb_msg_leave(TbMsg *m, int seat)
{
    return msg_whole_leave(&TB_MSG_SPEC, &m->w, seat);
}

int tb_msg_start(TbMsg *m, int seat)
{
    TbLobby l;
    tb_msg_lobby(m, &l);
    static TbGame g;
    if (!tb_lobby_start(&l, seat, m->w.seed, &g)) return TB_EREFUSED;
    m->game = g;
    msg_whole_mark_started(&m->w, seat);
    return TB_EOK;
}

int tb_msg_join_start(TbMsg *m, const uint8_t tag[TB_TAG_LEN], const uint8_t *name, int name_len)
{
    TbLobby l;
    tb_msg_lobby(m, &l);
    if (!msg_lobby_roster_can_join_and_start(&l)) return TB_EREFUSED;
    static TbMsg t;
    t = *m;
    int s = tb_msg_join(&t, tag, name, name_len);
    if (s < 0) return s;
    int e = tb_msg_start(&t, s);
    if (e) return e;
    *m = t;
    return s;
}

int tb_msg_plan_lobby(const TbMsg *before, const TbMsg *after, TbEvent *out, int cap)
{
    TbLobby lb, la;
    msg_whole_lobby_pair(&TB_MSG_SPEC, &before->w, &after->w, &lb, &la);
    return tb_plan_lobby(&lb, &la, out, cap);
}

/* ---- the bytes ------------------------------------------------------------------- */

/* The same message, by everything that is not derived (the phase within a
 * started game, the dice): what the encoder is asked to write against what
 * it reads back without deriving. */
static int same_msg(const TbMsg *a, const TbMsg *b)
{
    return msg_whole_same(&a->w, &b->w) && (!started(a) || same_moves(&a->game, &b->game));
}

static int decode(const uint8_t *in, int n, TbMsg *out, int derive);

int tb_msg_encode(const TbMsg *m, uint8_t *out, int cap)
{
    static uint8_t buf[TB_MSG_MAX_BYTES];
    static TbMsg back;
    const int live = started(m);
    const TbGame *g = &m->game;
    int e = msg_whole_writable(&TB_MSG_SPEC, &m->w);
    if (e) return e;
    if (live && (g->n != m->w.n_seats || g->starter != m->w.starter)) return TB_EROSTER;
    const uint16_t counters[2] = { (uint16_t)(live ? bubbles_of(g) : 0), live ? g->turns : 0 };
    const int head = msg_whole_put_head(&TB_MSG_SPEC, &m->w,
                                        !live ? TB_PHASE_WAITING : g->over ? TB_PHASE_FINISHED : TB_PHASE_LIVE,
                                        0, counters, buf);
    int bn = 0;
    if (live) {
        bn = tb_code_encode(g, buf + head + TB_CHECK_LEN, TB_CODE_MAX);
        if (bn < 0) return TB_EGAME;
    }
    const int n = msg_whole_seal(buf, head, bn);

    /* WHAT WAS WRITTEN IS READ BACK: a payload this build would refuse, or one
     * that reads as a different game, is never handed to a host. */
    e = decode(buf, n, &back, 0);
    if (e) return e;
    if (!same_msg(&back, m)) return TB_EGAME;
    if (n > cap) return TB_ECAP;
    memcpy(out, buf, (size_t)n);
    return n;
}

static int decode(const uint8_t *in, int n, TbMsg *out, int derive)
{
    static TbMsg m;
    memset(&m, 0, sizeof m);
    int flags, at, bn;
    uint16_t c[2];
    int e = msg_whole_read(&TB_MSG_SPEC, in, n, &m.w, &flags, c, &at, &bn);
    if (e) return e;
    if (started(&m)) {
        const int bubbles = c[0], turns = c[1];
        if (!tb_code_decode(&m.game, m.w.seed, m.w.n_seats, m.w.starter, bubbles, in + at, bn, derive))
            return TB_EGAME;
        /* the header says what the replay says, or nothing is believed */
        if (m.game.turns != turns || m.game.hist_n != bubbles
            || (m.game.over ? TB_PHASE_FINISHED : TB_PHASE_LIVE) != m.w.phase)
            return TB_EGAME;
    }
    *out = m;
    return TB_EOK;
}

int tb_msg_decode(const uint8_t *in, int n, TbMsg *out)
{
    return decode(in, n, out, 1);
}

int tb_msg_text_encode(const TbMsg *m, char *out, int cap)
{
    static uint8_t b[TB_MSG_MAX_BYTES];
    return msg_whole_text_put(b, tb_msg_encode(m, b, (int)sizeof b), out, cap);
}

static int text_decode(const char *text, TbMsg *out, int derive)
{
    static char span[TB_MSG_MAX_TEXT];
    static uint8_t b[TB_MSG_MAX_BYTES];
    int n = msg_whole_text_get(text, span, (int)sizeof span, b, (int)sizeof b);
    return n < 0 ? n : decode(b, n, out, derive);
}

int tb_msg_text_decode(const char *text, TbMsg *out)
{
    return text_decode(text, out, 1);
}

int tb_msg_text_peek(const char *text, TbMsg *out)
{
    return text_decode(text, out, 0);
}

/* ---- two messages ---------------------------------------------------------------- */

int tb_msg_same_game(const TbMsg *a, const TbMsg *b)
{
    return msg_whole_same_game(&a->w, &b->w);
}

int tb_msg_prefer(const TbMsg *mine, const TbMsg *tapped)
{
    if (!tb_msg_same_game(mine, tapped)) return 1;                                  /* 1 */
    int r;
    const int la = started(mine), lb = started(tapped);
    if ((r = msg_whole_more(la, lb))) return r;                                     /* 2 */
    if ((r = msg_whole_more(la ? mine->game.turns : 0, lb ? tapped->game.turns : 0))) return r;       /* 3 */
    if ((r = msg_whole_more(la ? bubbles_of(&mine->game) : 0, lb ? bubbles_of(&tapped->game) : 0))) return r; /* 4 */
    if ((r = msg_whole_prefer_lobby(&mine->w, &tapped->w))) return r;               /* 5 */
    static uint8_t ba[TB_MSG_MAX_BYTES], bb[TB_MSG_MAX_BYTES];                      /* 6 */
    return msg_whole_prefer_bytes(ba, tb_msg_encode(mine, ba, (int)sizeof ba),
                                  bb, tb_msg_encode(tapped, bb, (int)sizeof bb));
}

int tb_common_bubbles(const TbMsg *a, const TbMsg *b)
{
    if (!tb_msg_same_game(a, b) || !started(a) || !started(b)) return 0;
    if (a->game.n != b->game.n || a->game.starter != b->game.starter) return 0;
    int na = bubbles_of(&a->game), nb = bubbles_of(&b->game), most = na < nb ? na : nb, k = 0;
    while (k < most) {
        TbMove x = move_at(&a->game, k), y = move_at(&b->game, k);
        if (memcmp(&x, &y, sizeof x)) break;
        k++;
    }
    return k;
}

/* ---- the seats --------------------------------------------------------------------- */

int tb_msg_sender(const TbMsg *m)
{
    if (!started(m)) return msg_whole_lobby_sender(&m->w);
    const TbGame *g = &m->game;
    if (bubbles_of(g) > 0) return move_at(g, bubbles_of(g) - 1).seat;
    return m->w.starter < m->w.n_seats ? m->w.starter : -1;
}

int tb_msg_resolve(const TbMsg *m, int record, int tag_seat, int is_dm, int i_sent,
                   const uint8_t *name, int name_len, int *by)
{
    return msg_whole_resolve(&m->w, tb_msg_sender(m), record, tag_seat, is_dm, i_sent, name, name_len, by);
}

int tb_rec_find(const uint8_t *recs, int n, const TbMsg *m)
{
    return msg_whole_rec_find(&TB_MSG_SPEC, recs, n, &m->w);
}

int tb_rec_forget(uint8_t *recs, int n, const TbMsg *m)
{
    return msg_whole_rec_forget(&TB_MSG_SPEC, recs, n, &m->w);
}

int tb_rec_put(uint8_t *recs, int n, const TbMsg *m, int seat)
{
    return msg_whole_rec_put(&TB_MSG_SPEC, recs, n, &m->w, seat);
}
