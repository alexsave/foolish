/* Tallybones - the lobby's new game and events. See tb_lobby.h. */
#include "tb_lobby.h"
#include <string.h>

static int begin(void *game, const MsgLobbyRoster *l, int starter, const uint8_t seed[32])
{
    return tb_new(game, seed, l->n_seats, starter);
}

int tb_lobby_start(TbLobby *l, int seat, const uint8_t seed[32], TbGame *g)
{
    return msg_lobby_roster_start(l, seat, seed, g, begin);
}

int tb_plan_lobby(const TbLobby *before, const TbLobby *after, TbEvent *out, int cap)
{
    MsgLobbyRosterChange ch[MSG_LOBBY_ROSTER_MAX_CHANGES];
    int n = msg_lobby_roster_plan(before, after, ch);
    for (int i = 0; i < n && i < cap; i++) {
        memset(&out[i], 0, sizeof out[i]);
        out[i].kind = ch[i].kind == MSG_LOBBY_ROSTER_LEFT ? TB_EV_LOBBY_LEAVE : TB_EV_LOBBY_JOIN;
        out[i].seat = ch[i].seat;
        out[i].other = TB_SEAT_NONE;
    }
    return n <= cap ? n : -1;
}
