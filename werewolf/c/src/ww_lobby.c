#include "ww_lobby.h"

int ww_lobby_capacity(int chat_is_dm) {
    // The wire's maximum in a group, two in a 1:1. Not "whatever the creator
    // set": there is no player-count picker, because a count chosen at creation
    // is a count the creator can tune, and a lobby that can be tuned before it
    // commits is a lobby that can be re-created until it suits.
    return chat_is_dm ? 2 : WW_MAX_PLAYERS;
}

int ww_lobby_impossible(int capacity) {
    return capacity < WW_MIN_PLAYERS;
}

int ww_lobby_can_start(int joined, int capacity) {
    if (ww_lobby_impossible(capacity)) return 0;
    return joined >= WW_MIN_PLAYERS && joined <= capacity;
}

int ww_lobby_needs(int joined) {
    return joined >= WW_MIN_PLAYERS ? 0 : WW_MIN_PLAYERS - joined;
}

int ww_lobby_can_exit(int my_seat, int joined) {
    return my_seat >= 0 && joined >= 2;
}

int ww_lobby_offered(int my_seat, int joined, int capacity, int i_sent_the_newest) {
    if (my_seat < 0) return joined < capacity ? WW_LOBBY_JOIN : WW_LOBBY_FULL;
    // Seated, in a chat that can never hold a table. Said once, plainly, rather
    // than counting toward a Start that cannot arrive.
    if (ww_lobby_impossible(capacity)) return WW_LOBBY_TOO_FEW;
    if (ww_lobby_can_start(joined, capacity)) {
        // THE NEWEST SENDER STANDS ASIDE WHILE THERE IS ROOM. Whoever just posted
        // is the one person the thread does not need to hear from again, and a
        // lobby that let them deal would let the fastest tapper decide the table
        // size the moment it became legal. A FULL table has nobody left to stand
        // aside for, so the exemption ends exactly there.
        if (i_sent_the_newest && joined < capacity) return WW_LOBBY_WAITING;
        return WW_LOBBY_START;
    }
    // In, and short. The invite is offered only to somebody who did NOT put the
    // newest bubble there - otherwise the screen asks for a second copy of what is
    // already in the thread.
    return i_sent_the_newest ? WW_LOBBY_WAITING : WW_LOBBY_INVITE;
}

int ww_lobby_free_seat(const uint8_t *claimed_seats, int n_claimed) {
    for (int seat = 0; seat < WW_MAX_PLAYERS; seat++) {
        int taken = 0;
        for (int i = 0; i < n_claimed; i++) if (claimed_seats[i] == (uint8_t)seat) taken = 1;
        if (!taken) return seat;
    }
    return WW_NO_SEAT;
}

int ww_lobby_start(WwGame *g, const uint8_t seed[32], int joined) {
    // THE range check is ww_deal's, and there is deliberately only one of it: a
    // second copy here was written first and then deleted, because a mutation of it
    // could not be made to fail - ww_deal refuses 4 and 11 on its own, so the copy
    // was a line no test could ever reach. What this function is FOR is being the
    // one named path from a lobby to roles, so that no caller reaches ww_deal by
    // habit and no caller has to remember what a lobby owes it.
    return ww_deal(g, seed, joined);
}
