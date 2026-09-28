/* Chui Niu - the lobby's first roll and events. See cn_lobby.h. */
#include "cn_lobby.h"
#include <string.h>

static int roll(void *game, const MsgLobbyRoster *l, int starter, const uint8_t seed[32])
{
    (void)starter;                      /* round 0 does not depend on who started it */
    return cn_new(game, seed, l->n_seats);
}

int cn_lobby_start(CnLobby *l, int seat, const uint8_t seed[32], CnGame *g)
{
    return msg_lobby_roster_start(l, seat, seed, g, roll);
}

int cn_plan_lobby(const CnLobby *before, const CnLobby *after, CnEvent *out, int cap)
{
    MsgLobbyRosterChange ch[MSG_LOBBY_ROSTER_MAX_CHANGES];
    int n = msg_lobby_roster_plan(before, after, ch);
    for (int i = 0; i < n && i < cap; i++) {
        memset(&out[i], 0, sizeof out[i]);
        out[i].kind = ch[i].kind == MSG_LOBBY_ROSTER_LEFT ? CN_EV_LOBBY_LEAVE : CN_EV_LOBBY_JOIN;
        out[i].seat = ch[i].seat;
        out[i].other = CN_SEAT_NONE;
    }
    return n <= cap ? n : -1;
}
