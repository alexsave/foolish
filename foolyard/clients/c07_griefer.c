// Connects, watches, and never plays. Not a bug in anything - a question:
// foolish has no turn clock, so a seat that simply declines to act holds its
// table forever and the stall detector is what measures how long. Point one of
// these at a lineup to see exactly what one idle player costs everyone else.
#include "world.h"

static void settings(ClientState *cs) {
    cs->link.ordered = 1;
}

static void on_view(ClientCtx *ctx) { (void)ctx; }
static void on_wake(ClientCtx *ctx) { (void)ctx; }

const ClientImpl client_griefer = { "griefer", settings, on_view, on_wake };
