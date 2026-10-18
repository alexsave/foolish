/* Chui Niu - the envelope. See cn_msg.h; everything that is not the game is
 * shared/c/msg_whole's. */
#include "cn_msg.h"
#include <string.h>

const MsgWholeSpec CN_MSG_SPEC = {
    .seat_salt = "chuiniu.seat.1|", .game_salt = "chuiniu.game.1|",
    .magic = CN_MSG_MAGIC, .format = CN_MSG_FORMAT,
    .flag_dm = CN_FLAG_DM, .flag_left = CN_FLAG_LEFT,
    .flags_known = CN_FLAGS_KNOWN, .flags_live = 0,
    .counters = 1,                    /* moves */
    .group_cap = CN_LOBBY_GROUP_CAP,
    .max_bytes = CN_MSG_MAX_BYTES,
    .rec_len = CN_REC_LEN, .rec_bytes = CN_REC_BYTES,
};

void cn_tag(const uint8_t seed[32], const uint8_t *id, int id_len, uint8_t out[CN_TAG_LEN])
{
    msg_whole_tag(&CN_MSG_SPEC, seed, id, id_len, out);
}

void cn_game_id(const uint8_t seed[32], uint8_t out[8])
{
    msg_whole_game_id(&CN_MSG_SPEC, seed, out);
}

static int started(const CnMsg *m)
{
    return msg_whole_started(&m->w);
}

/* ---- the lobby ------------------------------------------------------------------ */

int cn_msg_new(CnMsg *m, const uint8_t seed[32], int dm, const uint8_t tag[CN_TAG_LEN],
               const uint8_t *name, int name_len)
{
    MsgWhole w;
    int e = msg_whole_new(&w, seed, dm, tag, name, name_len);
    if (e) return e;
    memset(m, 0, sizeof *m);
    m->w = w;
    return CN_EOK;
}

void cn_msg_lobby(const CnMsg *m, CnLobby *l)
{
    msg_whole_lobby(&CN_MSG_SPEC, &m->w, l);
}

int cn_msg_offered(const CnMsg *m, int seat)
{
    return msg_whole_offered(&CN_MSG_SPEC, &m->w, seat);
}

int cn_msg_seat_of_tag(const CnMsg *m, const uint8_t tag[CN_TAG_LEN])
{
    return msg_whole_seat_of_tag(&m->w, tag);
}

int cn_msg_join(CnMsg *m, const uint8_t tag[CN_TAG_LEN], const uint8_t *name, int name_len)
{
    return msg_whole_join(&CN_MSG_SPEC, &m->w, tag, name, name_len);
}

int cn_msg_leave(CnMsg *m, int seat)
{
    return msg_whole_leave(&CN_MSG_SPEC, &m->w, seat);
}

int cn_msg_start(CnMsg *m, int seat)
{
    CnLobby l;
    cn_msg_lobby(m, &l);
    static CnGame g;
    if (!cn_lobby_start(&l, seat, m->w.seed, &g)) return CN_EREFUSED;
    m->game = g;
    msg_whole_mark_started(&m->w, seat);
    return CN_EOK;
}

int cn_msg_join_start(CnMsg *m, const uint8_t tag[CN_TAG_LEN], const uint8_t *name, int name_len)
{
    CnLobby l;
    cn_msg_lobby(m, &l);
    if (!msg_lobby_roster_can_join_and_start(&l)) return CN_EREFUSED;
    static CnMsg t;
    t = *m;
    int s = cn_msg_join(&t, tag, name, name_len);
    if (s < 0) return s;
    int e = cn_msg_start(&t, s);
    if (e) return e;
    *m = t;
    return s;
}

int cn_msg_plan_lobby(const CnMsg *before, const CnMsg *after, CnEvent *out, int cap)
{
    CnLobby lb, la;
    msg_whole_lobby_pair(&CN_MSG_SPEC, &before->w, &after->w, &lb, &la);
    return cn_plan_lobby(&lb, &la, out, cap);
}

/* ---- the bytes ------------------------------------------------------------------- */

/* The same message, by everything that is not derived (the phase within a
 * started game): what the encoder is asked to write against what it reads
 * back. */
static int same_msg(const CnMsg *a, const CnMsg *b)
{
    return msg_whole_same(&a->w, &b->w) && (!started(a) || cn_hash(&a->game) == cn_hash(&b->game));
}

int cn_msg_encode(const CnMsg *m, uint8_t *out, int cap)
{
    static uint8_t buf[CN_MSG_MAX_BYTES];
    static CnMsg back;
    const int live = started(m);
    const CnGame *g = &m->game;
    int e = msg_whole_writable(&CN_MSG_SPEC, &m->w);
    if (e) return e;
    if (live && g->n != m->w.n_seats) return CN_EROSTER;
    const uint16_t moves = live ? g->hist_n : 0;
    const int head = msg_whole_put_head(&CN_MSG_SPEC, &m->w,
                                        !live ? CN_PHASE_WAITING : g->phase == CN_PH_OVER ? CN_PHASE_FINISHED : CN_PHASE_LIVE,
                                        0, &moves, buf);
    int bn = 0;
    if (live) {
        bn = cn_code_encode(g, buf + head + CN_CHECK_LEN, CN_CODE_MAX);
        if (bn < 0) return CN_EGAME;
    }
    const int n = msg_whole_seal(buf, head, bn);

    /* WHAT WAS WRITTEN IS READ BACK: a payload this build would refuse, or one
     * that reads as a different game, is never handed to a host. */
    e = cn_msg_decode(buf, n, &back);
    if (e) return e;
    if (!same_msg(&back, m)) return CN_EGAME;
    if (n > cap) return CN_ECAP;
    memcpy(out, buf, (size_t)n);
    return n;
}

int cn_msg_decode(const uint8_t *in, int n, CnMsg *out)
{
    static CnMsg m;
    memset(&m, 0, sizeof m);
    int flags, at, bn;
    uint16_t moves;
    int e = msg_whole_read(&CN_MSG_SPEC, in, n, &m.w, &flags, &moves, &at, &bn);
    if (e) return e;
    if (started(&m)) {
        if (!cn_code_decode(&m.game, m.w.seed, m.w.n_seats, moves, in + at, bn)) return CN_EGAME;
        /* the header says what the replay says, or nothing is believed */
        if (m.game.hist_n != moves || (m.game.phase == CN_PH_OVER ? CN_PHASE_FINISHED : CN_PHASE_LIVE) != m.w.phase)
            return CN_EGAME;
    }
    *out = m;
    return CN_EOK;
}

int cn_msg_text_encode(const CnMsg *m, char *out, int cap)
{
    static uint8_t b[CN_MSG_MAX_BYTES];
    return msg_whole_text_put(b, cn_msg_encode(m, b, (int)sizeof b), out, cap);
}

int cn_msg_text_decode(const char *text, CnMsg *out)
{
    static char span[CN_MSG_MAX_TEXT];
    static uint8_t b[CN_MSG_MAX_BYTES];
    int n = msg_whole_text_get(text, span, (int)sizeof span, b, (int)sizeof b);
    return n < 0 ? n : cn_msg_decode(b, n, out);
}

/* ---- two messages ---------------------------------------------------------------- */

int cn_msg_same_game(const CnMsg *a, const CnMsg *b)
{
    return msg_whole_same_game(&a->w, &b->w);
}

int cn_msg_prefer(const CnMsg *mine, const CnMsg *tapped)
{
    if (!cn_msg_same_game(mine, tapped)) return 1;                                  /* 1 */
    int r;
    const int la = started(mine), lb = started(tapped);
    if ((r = msg_whole_more(la, lb))) return r;                                     /* 2 */
    if ((r = msg_whole_more(la ? mine->game.hist_n : 0, lb ? tapped->game.hist_n : 0))) return r;     /* 3 */
    if ((r = msg_whole_prefer_lobby(&mine->w, &tapped->w))) return r;               /* 4 */
    static uint8_t ba[CN_MSG_MAX_BYTES], bb[CN_MSG_MAX_BYTES];                      /* 5 */
    return msg_whole_prefer_bytes(ba, cn_msg_encode(mine, ba, (int)sizeof ba),
                                  bb, cn_msg_encode(tapped, bb, (int)sizeof bb));
}

int cn_common_moves(const CnMsg *a, const CnMsg *b)
{
    if (!cn_msg_same_game(a, b) || !started(a) || !started(b) || a->game.n != b->game.n) return 0;
    int most = a->game.hist_n < b->game.hist_n ? a->game.hist_n : b->game.hist_n;
    int k = 0;
    while (k < most && a->game.hist[k].q == b->game.hist[k].q && a->game.hist[k].f == b->game.hist[k].f) k++;
    return k;
}

/* ---- the seats --------------------------------------------------------------------- */

int cn_msg_sender(const CnMsg *m)
{
    if (!started(m)) return msg_whole_lobby_sender(&m->w);
    const CnGame *g = &m->game;
    if (g->hist_n > 0 && g->last_seat < m->w.n_seats) return g->last_seat;
    return m->w.starter < m->w.n_seats ? m->w.starter : -1;
}

int cn_msg_resolve(const CnMsg *m, int record, int tag_seat, int is_dm, int i_sent,
                   const uint8_t *name, int name_len, int *by)
{
    return msg_whole_resolve(&m->w, cn_msg_sender(m), record, tag_seat, is_dm, i_sent, name, name_len, by);
}

int cn_rec_find(const uint8_t *recs, int n, const CnMsg *m)
{
    return msg_whole_rec_find(&CN_MSG_SPEC, recs, n, &m->w);
}

int cn_rec_forget(uint8_t *recs, int n, const CnMsg *m)
{
    return msg_whole_rec_forget(&CN_MSG_SPEC, recs, n, &m->w);
}

/* A game already recorded keeps its seen byte: msg_whole_rec_put keeps a
 * record's own bytes. */
int cn_rec_put(uint8_t *recs, int n, const CnMsg *m, int seat)
{
    return msg_whole_rec_put(&CN_MSG_SPEC, recs, n, &m->w, seat);
}

int cn_rec_seen(const uint8_t *recs, int n, const CnMsg *m)
{
    int i = msg_whole_rec_at(&CN_MSG_SPEC, recs, n, &m->w);
    return i < 0 ? 0 : recs[i + CN_REC_SEEN];
}

int cn_rec_see(uint8_t *recs, int n, const CnMsg *m, int seen)
{
    int i = msg_whole_rec_at(&CN_MSG_SPEC, recs, n, &m->w);
    if (i < 0 || seen < 1 || seen > 255 || recs[i + CN_REC_SEEN] >= seen) return 0;
    recs[i + CN_REC_SEEN] = (uint8_t)seen;
    return 1;
}

int cn_rec_load(uint8_t *recs, const uint8_t *bytes, int n)
{
    if (!bytes || n < 0) return 0;
    if (n >= CN_REC_MAGIC_LEN && !memcmp(bytes, CN_REC_MAGIC, CN_REC_MAGIC_LEN)) {
        n = msg_whole_rec_n(&CN_MSG_SPEC, n - CN_REC_MAGIC_LEN);
        if (n) memcpy(recs, bytes + CN_REC_MAGIC_LEN, (size_t)n);
        return n;
    }
    /* THE FIRST FORM: id and tag, no seen byte; every round reads unseen */
    int w = 0;
    for (int i = 0; i + CN_REC_LEN_V1 <= n && w < CN_REC_BYTES; i += CN_REC_LEN_V1) {
        memcpy(recs + w, bytes + i, CN_REC_LEN_V1);
        recs[w + CN_REC_SEEN] = 0;
        w += CN_REC_LEN;
    }
    return w;
}

int cn_rec_save(const uint8_t *recs, int n, uint8_t *out, int cap)
{
    n = msg_whole_rec_n(&CN_MSG_SPEC, n);
    if (!out || cap < CN_REC_MAGIC_LEN + n) return -1;
    memcpy(out, CN_REC_MAGIC, CN_REC_MAGIC_LEN);
    if (n) memcpy(out + CN_REC_MAGIC_LEN, recs, (size_t)n);
    return CN_REC_MAGIC_LEN + n;
}
