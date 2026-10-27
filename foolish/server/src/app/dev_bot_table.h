// dev_bot_table.h - POST /dev/bot_table: a table of server bots (format 2: and humans). DEV-ONLY.
//
// WHY IT EXISTS. birdseye (branch foolish-birdseye, never merged) runs a bot league: the 49
// server bots playing each other at human pace while their ratings move. The kernel cannot deal
// such a table through any client request: a table deals only inside table_add_bot and
// table_ready, both need the asker SEATED, and every seated player plays, so the asking human
// always holds a seat; a human leaving a lobby of ready bots deals nothing (table.c unseat).
// Changing that rule would be a product change (a human could start unwatched rated games), so
// the kernel stays as it is and this route drives the same kernel calls a lobby makes, in order.
//
// WHAT IT DOES, all through table_ops.h like any request: cb_table_create seats a house account
// (DEV_HOUSE_ID, never a real user), then ONE cb_table_run on that lobby: table_set_rules,
// table_add_bot for each bot named (the house is not ready, so nothing deals yet; at most seven,
// since the house holds a seat), table_leave of the house (the lobby is now only bots, every one
// READY), then the first bot seats an eighth if one was asked for (that add deals) or readies
// (that ready deals). The commit kicks the bot loop (bots.h) exactly as a deal from
// /v1/meta does; from there the game is the ordinary one: bot_drive at the kernel's own pace,
// table_rate through the commit, the replay job.
//
// DEV-ONLY, TWICE OVER. The route list is EMPTY unless CBASE_DEV=1 (cb_dev_bot_table_enable,
// called from serve_open, which runs before the routes are registered), so in production the
// path is not routed and answers what any unknown path answers. And a call must carry the admin
// console's token (admin.h cb_admin_authorized): 404 when none is configured, 401 otherwise.
//
// THE BYTES (no JSON):
//   request   u8 DEV_BOT_TABLE_FORMAT, u8 passing (1 classic, 0 podkidnoy), u8 n (2..MAX_PLAYERS),
//             n x { u8 len, bot id }
//   200       u8 gid_len, gid
//   refusals  ask.h's AskRefusal, as /v1/meta answers them (400 MALFORMED, 400 NO_BOT for an id
//             that names no bot, 400 REFUSED with the kernel's code, 500 INTERNAL)
//
// FORMAT 2: A HUMAN AT A TABLE OF BOTS. birdseye's tournament seats one human account (a person
// playing in the real web UI) against a bot. Each seat names its kind:
//   request   u8 DEV_BOT_TABLE_FORMAT_2, u8 passing, u8 n (2..MAX_PLAYERS),
//             n x { u8 kind (DEV_SEAT_BOT, DEV_SEAT_HUMAN), u8 len, id }
//   200       as format 1
//   refusals  as format 1, and 400 MALFORMED for a human id that names no account (there is no
//             AskRefusal for "no such account"; NO_BOT is a bot's, AUTH is the caller's own)
// A human seat is seated as ASK_JOIN seats its asker: table_join under the account's stored
// username, UNREADY. Seats are taken in the order given. With no human seat the table deals
// exactly as format 1 does. With one, NOTHING is readied by the route: every bot sits READY, the
// human UNREADY, the table stays in its lobby, and the kernel deals when the human presses Start
// (ASK_START -> table_ready over /v1/meta, whose commit kicks the bot loop as any deal does).
#ifndef CB_DEV_BOT_TABLE_H
#define CB_DEV_BOT_TABLE_H

#include "cbase/app.h"

#include <stdbool.h>

#define DEV_BOT_TABLE_FORMAT   1
#define DEV_BOT_TABLE_FORMAT_2 2
#define DEV_SEAT_BOT   0   // format 2: the id is a bot's (bots_list)
#define DEV_SEAT_HUMAN 1   // format 2: the id is an account's user id
#define DEV_HOUSE_ID   "00000000-0000-4000-8000-00000000b075"
#define DEV_HOUSE_NAME "house"

// Routed in foolish_app.c's list; its n is 0 until cb_dev_bot_table_enable.
extern cb_route_list cb_dev_bot_table_route_list;
// Called by serve_open: routes POST /dev/bot_table when `dev` (CBASE_DEV=1), nothing otherwise.
void cb_dev_bot_table_enable(bool dev);

#endif
