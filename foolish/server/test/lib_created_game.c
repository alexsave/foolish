// lib_created_game.c - see lib_created_game.h.
#include "lib_created_game.h"

#include "roster.h"
#include "view.h"

#include <string.h>

bool tc_created_game(const uint8_t *body, size_t len, char gid[CB_GAME_ID_MAX + 1]) {
    gid[0] = 0;
    EnvHeader h;
    if (!body || len > INT32_MAX || !env_header_read(body, (int)len, &h) || h.trailer_at >= (int)len) return false;
    Roster r;
    char id[ROSTER_GAME_ID_MAX + 1];
    int n = 0, status = 0, passing = 0, used = 0;
    uint32_t ai = 0;
    if (roster_trailer_read(&r, id, &n, &status, &ai, &passing, body + h.trailer_at, (int)len - h.trailer_at, &used) !=
            ROSTER_OK ||
        n <= 0 || n > CB_GAME_ID_MAX)
        return false;
    memcpy(gid, id, (size_t)n);
    gid[n] = 0;
    return true;
}
