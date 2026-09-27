/* Chui Niu - the lobby's rules. See cn_lobby.h. */
#include "cn_lobby.h"
#include <string.h>

int cn_lobby_cap(const CnLobby *l)
{
    return l->dm ? CN_LOBBY_DM_CAP : CN_LOBBY_GROUP_CAP;
}

void cn_lobby_new(CnLobby *l, int dm, uint16_t creator)
{
    memset(l, 0, sizeof *l);
    l->dm = (uint8_t)(dm != 0);
    l->n_seats = 1;
    l->who[0] = creator;
    l->newest = 0;
}

int cn_lobby_seat_of(const CnLobby *l, uint16_t who)
{
    for (int s = 0; s < l->n_seats; s++)
        if (l->who[s] == who) return s;
    return -1;
}

int cn_lobby_join(CnLobby *l, uint16_t who)
{
    if (l->started || l->n_seats >= cn_lobby_cap(l) || cn_lobby_seat_of(l, who) >= 0) return -1;
    int s = l->n_seats++;
    l->who[s] = who;
    l->newest = (uint8_t)s;
    l->rev++;
    return s;
}

int cn_lobby_can_exit(const CnLobby *l, int seat)
{
    return !l->started && seat >= 0 && seat < l->n_seats && l->n_seats >= 2;
}

int cn_lobby_leave(CnLobby *l, int seat)
{
    if (!cn_lobby_can_exit(l, seat)) return 0;
    for (int s = seat; s + 1 < l->n_seats; s++) l->who[s] = l->who[s + 1];
    l->n_seats--;
    l->who[l->n_seats] = 0;
    l->newest = CN_SEAT_NONE;
    l->rev++;
    return 1;
}

int cn_lobby_offered(const CnLobby *l, int seat)
{
    if (l->started) return 0;
    int cap = cn_lobby_cap(l);
    if (seat < 0 || seat >= l->n_seats) return l->n_seats < cap ? CN_LOBBY_JOIN : CN_LOBBY_FULL;
    int newest_is_me = l->newest == seat;
    /* foolish's M9 gate: the newest joiner stands aside while there is room,
     * so a racing Start cannot beat a joiner still arriving; a full table has
     * nobody still arriving, so it is exempt (or a DM strands). */
    if (l->n_seats >= 2 && (!newest_is_me || l->n_seats >= cap)) return CN_LOBBY_START;
    if (l->n_seats == 1 && !newest_is_me) return CN_LOBBY_INVITE;
    return CN_LOBBY_WAITING;
}

int cn_lobby_can_join_and_start(const CnLobby *l)
{
    return !l->started && l->n_seats + 1 == cn_lobby_cap(l);
}

int cn_lobby_start(CnLobby *l, int seat, const uint8_t seed[32], CnGame *g)
{
    if (cn_lobby_offered(l, seat) != CN_LOBBY_START) return 0;
    if (!cn_new(g, seed, l->n_seats)) return 0;
    l->started = 1;
    return 1;
}

static int ev(CnEvent *out, int cap, int n, int kind, int seat)
{
    if (n < cap) {
        memset(&out[n], 0, sizeof out[n]);
        out[n].kind = (uint8_t)kind;
        out[n].seat = (uint8_t)seat;
        out[n].other = CN_SEAT_NONE;
    }
    return n + 1;
}

int cn_plan_lobby(const CnLobby *before, const CnLobby *after, CnEvent *out, int cap)
{
    int n = 0;
    for (int s = 0; s < before->n_seats; s++)
        if (cn_lobby_seat_of(after, before->who[s]) < 0) n = ev(out, cap, n, CN_EV_LOBBY_LEAVE, s);
    for (int s = 0; s < after->n_seats; s++)
        if (cn_lobby_seat_of(before, after->who[s]) < 0) n = ev(out, cap, n, CN_EV_LOBBY_JOIN, s);
    return n <= cap ? n : -1;
}
