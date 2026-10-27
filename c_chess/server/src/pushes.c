/* pushes.c - see pushes.h. */
#include "pushes.h"

#include "realtime/realtime.h"

#include <stdio.h>

const cb_topic_family cc_fam_room = { "room", "s/{doc}" };
const cb_topic_family cc_fam_user = { "user", "u/{user}/s" };

static cb_ctx *CTX;

void cc_pushes_init(cb_ctx *ctx) { CTX = ctx; }

int cc_room_of(uint32_t game, char *out) { return snprintf(out, 12, "g%u", game); }

void cc_push_room_game(uint32_t game, const uint8_t *frame, size_t n) {
    if (!CTX || !CTX->rt) return;
    char room[12];
    cc_room_of(game, room);
    cb_rt_to to = { &cc_fam_room, NULL, room, NULL };
    cb_rt_publish(CTX->rt, &to, 1, frame, n);
}

void cc_push_room(const char *room, const uint8_t *frame, size_t n) {
    if (!CTX || !CTX->rt) return;
    cb_rt_to to = { &cc_fam_room, NULL, room, NULL };
    cb_rt_publish(CTX->rt, &to, 1, frame, n);
}

void cc_push_user(const char *user_id, const uint8_t *frame, size_t n) {
    if (!CTX || !CTX->rt) return;
    cb_rt_to to = { &cc_fam_user, user_id, NULL, NULL };
    cb_rt_publish(CTX->rt, &to, 1, frame, n);
}

bool cc_push_lobby_heard(void) { return CTX && CTX->rt && cb_rt_room_heard(CTX->rt, "lobby"); }

void cc_push_lobby(const uint8_t *frame, size_t n) {
    if (!CTX || !CTX->rt) return;
    cb_rt_to to = { &cc_fam_room, NULL, "lobby", NULL };
    cb_rt_publish(CTX->rt, &to, 1, frame, n);
}
