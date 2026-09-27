/* Pick 'Em Up - the lobby's rules. See pk_lobby.h. */
#include "pk_lobby.h"
#include "pk_internal.h"
#include <string.h>

int pk_lobby_cap(const PkLobby *l)
{
    return l->dm ? PK_LOBBY_DM_CAP : PK_LOBBY_GROUP_CAP;
}

void pk_lobby_new(PkLobby *l, int dm, uint16_t creator)
{
    memset(l, 0, sizeof *l);
    l->dm = (uint8_t)(dm != 0);
    l->n_seats = 1;
    l->who[0] = creator;
    l->newest = 0;
}

int pk_lobby_seat_of(const PkLobby *l, uint16_t who)
{
    for (int s = 0; s < l->n_seats; s++)
        if (l->who[s] == who) return s;
    return -1;
}

int pk_lobby_join(PkLobby *l, uint16_t who)
{
    if (l->started || l->n_seats >= pk_lobby_cap(l) || pk_lobby_seat_of(l, who) >= 0) return -1;
    int s = l->n_seats++;
    l->who[s] = who;
    l->newest = (uint8_t)s;
    l->rev++;
    return s;
}

int pk_lobby_can_exit(const PkLobby *l, int seat)
{
    return !l->started && seat >= 0 && seat < l->n_seats && l->n_seats >= 2;
}

int pk_lobby_leave(PkLobby *l, int seat)
{
    if (!pk_lobby_can_exit(l, seat)) return 0;
    for (int s = seat; s + 1 < l->n_seats; s++) l->who[s] = l->who[s + 1];
    l->n_seats--;
    l->who[l->n_seats] = 0;
    l->newest = PK_SEAT_NONE;      /* the leaver sent it, and has no seat now */
    l->rev++;
    return 1;
}

int pk_lobby_offered(const PkLobby *l, int seat)
{
    if (l->started) return 0;
    int cap = pk_lobby_cap(l);
    if (seat < 0 || seat >= l->n_seats) return l->n_seats < cap ? PK_LOBBY_JOIN : PK_LOBBY_FULL;
    int newest_is_me = l->newest == seat;
    /* foolish's M9 gate: the newest sender stands aside while there is room,
     * so a racing Start cannot beat the joiner who is still arriving; a full
     * table has nobody still arriving, so the exemption (or a DM strands). */
    if (l->n_seats >= 2 && (!newest_is_me || l->n_seats >= cap)) return PK_LOBBY_START;
    if (l->n_seats == 1 && !newest_is_me) return PK_LOBBY_INVITE;
    return PK_LOBBY_WAITING;
}

int pk_lobby_can_join_and_start(const PkLobby *l)
{
    return !l->started && l->n_seats + 1 == pk_lobby_cap(l);
}

int pk_lobby_start(PkLobby *l, int seat, const uint8_t seed[32], PkGame *g)
{
    if (pk_lobby_offered(l, seat) != PK_LOBBY_START) return 0;
    if (!pk__new(g, seed, l->n_seats, seat, 0)) return 0;
    l->started = 1;
    return 1;
}

static int ev(PkEvent *out, int cap, int n, int kind, int seat)
{
    if (n < cap) {
        memset(&out[n], 0, sizeof out[n]);
        out[n].kind = (uint8_t)kind;
        out[n].seat = (uint8_t)seat;
        out[n].other = PK_SEAT_NONE;
        out[n].card = PK_CARD_NONE;
    }
    return n + 1;
}

int pk_plan_lobby(const PkLobby *before, const PkLobby *after, PkEvent *out, int cap)
{
    int n = 0;
    for (int s = 0; s < before->n_seats; s++)
        if (pk_lobby_seat_of(after, before->who[s]) < 0) n = ev(out, cap, n, PK_EV_LOBBY_LEAVE, s);
    for (int s = 0; s < after->n_seats; s++)
        if (pk_lobby_seat_of(before, after->who[s]) < 0) n = ev(out, cap, n, PK_EV_LOBBY_JOIN, s);
    return n <= cap ? n : -1;
}
