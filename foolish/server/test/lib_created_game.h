// lib_created_game.h - the id of the game a create answered with.
//
// THE ONE WAY a test learns the id of a game it just created: from the
// create's own answer. That answer is the creator's envelope (view.h), and its
// roster trailer (roster.h) names the game, the way netclient.c
// envelope_names reads it.
//
// Never from the dashboard's "newest game first": every store orders a user's
// games by updated_at, a test's fake clock gives every game the same one, and a
// tie can name an older game (a finished one, where the joins and starts that
// follow do nothing).
#ifndef CB_LIB_CREATED_GAME_H
#define CB_LIB_CREATED_GAME_H

#include "store/game_id.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The game id the `len`-byte create answer at body names, into gid. false,
// and gid empty, for an answer that is not an envelope naming a game id.
bool tc_created_game(const uint8_t *body, size_t len, char gid[CB_GAME_ID_MAX + 1]);

#endif
