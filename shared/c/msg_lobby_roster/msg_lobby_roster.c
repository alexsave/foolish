/* The lobby's rules. See msg_lobby_roster.h. */
#include "msg_lobby_roster.h"
#include <string.h>

int msg_lobby_roster_cap(const MsgLobbyRoster *l)
{
    return l->dm ? MSG_LOBBY_ROSTER_DM_CAP : l->group_cap;
}

void msg_lobby_roster_new(MsgLobbyRoster *l, int dm, int group_cap, uint16_t creator)
{
    memset(l, 0, sizeof *l);
    l->dm = (uint8_t)(dm != 0);
    l->group_cap = (uint8_t)group_cap;
    l->n_seats = 1;
    l->who[0] = creator;
    l->newest = 0;
}

int msg_lobby_roster_seat_of(const MsgLobbyRoster *l, uint16_t who)
{
    for (int s = 0; s < l->n_seats; s++)
        if (l->who[s] == who) return s;
    return -1;
}

int msg_lobby_roster_join(MsgLobbyRoster *l, uint16_t who)
{
    if (l->started || l->n_seats >= msg_lobby_roster_cap(l) || msg_lobby_roster_seat_of(l, who) >= 0)
        return -1;
    int s = l->n_seats++;
    l->who[s] = who;
    l->newest = (uint8_t)s;
    l->rev++;
    return s;
}

int msg_lobby_roster_can_exit(const MsgLobbyRoster *l, int seat)
{
    return !l->started && seat >= 0 && seat < l->n_seats && l->n_seats >= 2;
}

int msg_lobby_roster_leave(MsgLobbyRoster *l, int seat)
{
    if (!msg_lobby_roster_can_exit(l, seat)) return 0;
    for (int s = seat; s + 1 < l->n_seats; s++) l->who[s] = l->who[s + 1];
    l->n_seats--;
    l->who[l->n_seats] = 0;
    l->newest = MSG_LOBBY_ROSTER_NO_SEAT;   /* the leaver sent it, and has no seat now */
    l->rev++;
    return 1;
}

int msg_lobby_roster_offered(const MsgLobbyRoster *l, int seat)
{
    if (l->started) return 0;
    int cap = msg_lobby_roster_cap(l);
    if (seat < 0 || seat >= l->n_seats) return l->n_seats < cap ? MSG_LOBBY_ROSTER_JOIN : MSG_LOBBY_ROSTER_FULL;
    int newest_is_me = l->newest == seat;
    /* The newest sender stands aside while there is room, so a racing Start
     * cannot beat the joiner who is still arriving; a full table has nobody
     * still arriving, so it is exempt (or a two-person chat strands). */
    if (l->n_seats >= 2 && (!newest_is_me || l->n_seats >= cap)) return MSG_LOBBY_ROSTER_START;
    if (l->n_seats == 1 && !newest_is_me) return MSG_LOBBY_ROSTER_INVITE;
    return MSG_LOBBY_ROSTER_WAITING;
}

int msg_lobby_roster_can_join_and_start(const MsgLobbyRoster *l)
{
    return !l->started && l->n_seats + 1 == msg_lobby_roster_cap(l);
}

int msg_lobby_roster_start(MsgLobbyRoster *l, int seat, const uint8_t seed[32], void *game,
                           MsgLobbyRosterStartFn start)
{
    if (msg_lobby_roster_offered(l, seat) != MSG_LOBBY_ROSTER_START) return 0;
    if (!start(game, l, seat, seed)) return 0;
    l->started = 1;
    return 1;
}

int msg_lobby_roster_plan(const MsgLobbyRoster *before, const MsgLobbyRoster *after,
                          MsgLobbyRosterChange out[MSG_LOBBY_ROSTER_MAX_CHANGES])
{
    int n = 0;
    for (int s = 0; s < before->n_seats; s++)
        if (msg_lobby_roster_seat_of(after, before->who[s]) < 0)
            out[n++] = (MsgLobbyRosterChange){ MSG_LOBBY_ROSTER_LEFT, (uint8_t)s };
    for (int s = 0; s < after->n_seats; s++)
        if (msg_lobby_roster_seat_of(before, after->who[s]) < 0)
            out[n++] = (MsgLobbyRosterChange){ MSG_LOBBY_ROSTER_JOINED, (uint8_t)s };
    return n;
}
