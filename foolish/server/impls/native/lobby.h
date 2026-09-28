// lobby.h - everything that happens to a game before (and between) deals.
//
// Creating a table, joining it, seating a bot, saying you are ready (which is
// also what deals, once the kernel says the lobby may), and resetting to a
// lobby for a rematch. Every one of those is a kernel call wrapped in this
// server's identity: game_lobby_seat, game_lobby_ready, game_lobby_can_deal,
// game_seat_and_deal and game_reset_to_lobby decide, and this file only
// supplies the ids and names the kernel deliberately does not carry.
#ifndef FOOLISH_LOBBY_H
#define FOOLISH_LOBBY_H

#include "httpd.h"

// POST /create (bearer) - a fresh table with the caller in seat 0. Answers a
// CTL_GAME frame (ctl_wire.h).
void h_create(Req *r, Conn *conn);

// POST /meta (bearer) - one lobby verb (CTL_META_JOIN / START / ADD_BOT /
// CONTINUE) against an existing game. Answers a CTL_LOBBY frame.
void h_meta(Req *r, Conn *conn);

#endif
