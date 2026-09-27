/* Pick 'Em Up - the lobby's deal and events. See pk_lobby.h. */
#include "pk_lobby.h"
#include "pk_internal.h"
#include <string.h>

static int deal(void *game, const MsgLobbyRoster *l, int starter, const uint8_t seed[32])
{
    return pk__new(game, seed, l->n_seats, starter, 0);
}

int pk_lobby_start(PkLobby *l, int seat, const uint8_t seed[32], PkGame *g)
{
    return msg_lobby_roster_start(l, seat, seed, g, deal);
}

int pk_plan_lobby(const PkLobby *before, const PkLobby *after, PkEvent *out, int cap)
{
    MsgLobbyRosterChange ch[MSG_LOBBY_ROSTER_MAX_CHANGES];
    int n = msg_lobby_roster_plan(before, after, ch);
    for (int i = 0; i < n && i < cap; i++) {
        memset(&out[i], 0, sizeof out[i]);
        out[i].kind = ch[i].kind == MSG_LOBBY_ROSTER_LEFT ? PK_EV_LOBBY_LEAVE : PK_EV_LOBBY_JOIN;
        out[i].seat = ch[i].seat;
        out[i].other = PK_SEAT_NONE;
        out[i].card = PK_CARD_NONE;
    }
    return n <= cap ? n : -1;
}
