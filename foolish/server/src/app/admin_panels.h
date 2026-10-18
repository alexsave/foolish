// admin_panels.h - foolish's panels in the platform's console (admin_panels.c).
#ifndef CB_ADMIN_PANELS_H
#define CB_ADMIN_PANELS_H

#include "cbase/app.h"

#include <stdint.h>

extern const cb_admin_call cb_foolish_admin_calls[3];
// The overview's counter line: the games the heartbeat would drive now.
int cb_admin_games_needing_bots(cb_ctx *ctx, int64_t now);

#endif
