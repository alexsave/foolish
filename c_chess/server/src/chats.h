/* chats.h - the server's chat (what a line may hold and the word filter are the kernel's, c/src/
 * chat.h): the last CW_CHAT_LOG lines of each room in memory, and who may say what.
 *
 * A ROOM is one the sockets already watch: a game's "g<id>" while the game is in memory (in play,
 * or finished in the last CC_FINISHED_KEEP_MS), an arena's "a<id>" while the arena is (upcoming,
 * running, or finished in the last hour), and "lobby". A room's lines leave memory with the room
 * (cc_chat_sweep, from the state's sweep). Nothing is journaled: chat is ephemeral.
 *
 * A LINE is said (cc_chat_post) by a signed-up player who is not a guest and not muted, at most one
 * a second (CC_CHAT_FLOOR_MS) and 30 a minute (c_base's CB_LIMIT_CHAT_USER), with no banned word;
 * three banned words within CC_CHAT_STRIKE_MS mute the player for CC_CHAT_TIMEOUT_MS. A mute is the
 * player's (cc_player.mute_until, journaled with the player), whether the filter or the console
 * (POST /admin/api/mute) set it, so a restart keeps it. The line goes to the room's ring and is
 * pushed to the room (CW_P_CHAT). */
#ifndef C_CHESS_CHATS_H
#define C_CHESS_CHATS_H

#include "cc_server.h"

#define CC_CHAT_FLOOR_MS 1000                 /* a player's lines at least this far apart */
#define CC_CHAT_STRIKES 3                      /* banned words ... */
#define CC_CHAT_STRIKE_MS (10 * 60 * 1000)     /* ... within this long ... */
#define CC_CHAT_TIMEOUT_MS (10 * 60 * 1000)    /* ... mute for this long */
#define CC_CHAT_MUTE_MAX_MIN (366 * 24 * 60)   /* the console mutes for at most a year */

/* ---- under cc_lock ---- */
/* Whether a room (NUL-terminated, at most CW_ROOM - 1 bytes) exists now. */
bool   cc_chat_room_ok(const char *room);
/* Says q's line as p: CW_E_NONE (the line is in the room and pushed), or a CW_E_CHAT_* code with
 * *wait_s for a mute or a rate and *why for a line the kernel refuses (chat.h CC_CHAT_*). now: mono
 * ms; wall: ms since the epoch (the line's time, and a mute's). */
int    cc_chat_post(cc_player *p, const cw_chat_req *q, int64_t now, int64_t wall, uint32_t *wait_s, int *why);
/* A room's log (cw_chat_log + its lines, oldest first) into out (at least CW_CHAT_LOG_MAX): its
 * length, 0 when there is no such room. A room nobody has talked in has an empty log. */
size_t cc_chat_log(const char *room, uint8_t *out);
/* Mutes p for minutes from wall (0: lifts it), journaled. */
void   cc_chat_mute(cc_player *p, uint32_t minutes, int64_t wall);
/* Drops the lines of rooms that have gone (the state's sweep). */
void   cc_chat_sweep(void);
void   cc_chat_free(void);
/* For /c/stats: lines said, lines refused, rooms holding lines, and the filter's time (ns, all lines). */
void   cc_chat_counts(uint64_t *lines, uint64_t *refused, uint64_t *rooms, uint64_t *filter_ns);

#endif
