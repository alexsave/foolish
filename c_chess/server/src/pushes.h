/* pushes.h - where c_chess's pushes go: a game's room (players and spectators SUB to "g<id>"), the
 * lobby's room ("lobby"), and a user's own sockets (no SUB: every socket of theirs hears it). */
#ifndef C_CHESS_PUSHES_H
#define C_CHESS_PUSHES_H

#include "cbase/app.h"
#include "ctx.h"

#include <stddef.h>
#include <stdint.h>

extern const cb_topic_family cc_fam_room;   /* s/{doc} */
extern const cb_topic_family cc_fam_user;   /* u/{user}/s */

void cc_pushes_init(cb_ctx *ctx);
void cc_push_room_game(uint32_t game, const uint8_t *frame, size_t n);
/* Any room by name (under 48 bytes): an arena's "a<id>". */
void cc_push_room(const char *room, const uint8_t *frame, size_t n);
void cc_push_user(const char *user_id, const uint8_t *frame, size_t n);
bool cc_push_lobby_heard(void);
void cc_push_lobby(const uint8_t *frame, size_t n);
/* A game's room name into out (at least 12 bytes): "g<id>". */
int  cc_room_of(uint32_t game, char *out);

#endif
