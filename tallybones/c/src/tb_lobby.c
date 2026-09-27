/* Tallybones - the lobby's rules, Pick 'Em Up's copied (T2). See tb_lobby.h. */
#include "tb_lobby.h"
#include "tb_internal.h"
#include <string.h>

int tb_lobby_cap(const TbLobby *l)
{
    return l->dm ? TB_LOBBY_DM_CAP : TB_LOBBY_GROUP_CAP;
}

void tb_lobby_new(TbLobby *l, int dm, uint16_t creator)
{
    memset(l, 0, sizeof *l);
    l->dm = (uint8_t)(dm != 0);
    l->n_seats = 1;
    l->who[0] = creator;
    l->newest = 0;
}

int tb_lobby_seat_of(const TbLobby *l, uint16_t who)
{
    for (int s = 0; s < l->n_seats; s++)
        if (l->who[s] == who) return s;
    return -1;
}

int tb_lobby_join(TbLobby *l, uint16_t who)
{
    if (l->started || l->n_seats >= tb_lobby_cap(l) || tb_lobby_seat_of(l, who) >= 0) return -1;
    int s = l->n_seats++;
    l->who[s] = who;
    l->newest = (uint8_t)s;
    l->rev++;
    return s;
}

int tb_lobby_can_exit(const TbLobby *l, int seat)
{
    return !l->started && seat >= 0 && seat < l->n_seats && l->n_seats >= 2;
}

int tb_lobby_leave(TbLobby *l, int seat)
{
    if (!tb_lobby_can_exit(l, seat)) return 0;
    for (int s = seat; s + 1 < l->n_seats; s++) l->who[s] = l->who[s + 1];
    l->n_seats--;
    l->who[l->n_seats] = 0;
    l->newest = TB_SEAT_NONE;      /* the leaver sent it, and has no seat now */
    l->rev++;
    return 1;
}

int tb_lobby_offered(const TbLobby *l, int seat)
{
    if (l->started) return 0;
    int cap = tb_lobby_cap(l);
    if (seat < 0 || seat >= l->n_seats) return l->n_seats < cap ? TB_LOBBY_JOIN : TB_LOBBY_FULL;
    int newest_is_me = l->newest == seat;
    /* foolish's M9 gate: the newest sender stands aside while there is room,
     * so a racing Start cannot beat the joiner who is still arriving; a full
     * table has nobody still arriving, so the exemption (or a DM strands). */
    if (l->n_seats >= 2 && (!newest_is_me || l->n_seats >= cap)) return TB_LOBBY_START;
    if (l->n_seats == 1 && !newest_is_me) return TB_LOBBY_INVITE;
    return TB_LOBBY_WAITING;
}

int tb_lobby_can_join_and_start(const TbLobby *l)
{
    return !l->started && l->n_seats + 1 == tb_lobby_cap(l);
}

int tb_lobby_start(TbLobby *l, int seat, const uint8_t seed[32], TbGame *g)
{
    if (tb_lobby_offered(l, seat) != TB_LOBBY_START) return 0;
    if (!tb_new(g, seed, l->n_seats, seat)) return 0;
    l->started = 1;
    return 1;
}

static int ev(TbEvent *out, int cap, int n, int kind, int seat)
{
    if (n < cap) {
        memset(&out[n], 0, sizeof out[n]);
        out[n].kind = (uint8_t)kind;
        out[n].seat = (uint8_t)seat;
        out[n].other = TB_SEAT_NONE;
    }
    return n + 1;
}

int tb_plan_lobby(const TbLobby *before, const TbLobby *after, TbEvent *out, int cap)
{
    int n = 0;
    for (int s = 0; s < before->n_seats; s++)
        if (tb_lobby_seat_of(after, before->who[s]) < 0) n = ev(out, cap, n, TB_EV_LOBBY_LEAVE, s);
    for (int s = 0; s < after->n_seats; s++)
        if (tb_lobby_seat_of(before, after->who[s]) < 0) n = ev(out, cap, n, TB_EV_LOBBY_JOIN, s);
    return n <= cap ? n : -1;
}
