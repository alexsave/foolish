#ifndef FOOLYARD_WORLD_H
#define FOOLYARD_WORLD_H

#include "types.h"
#include "constants.h"
#include "rng.h"
#include "sch.h"
#include "net.h"
#include "client.h"
#include "server.h"
#include "invariant.h"

typedef struct Knobs {
    u32 base_latency_us;
    u32 jitter_us;
    u32 loss_pct;
    u32 dup_pct;

    u32 srv_service_us;    // time the per-game lock is held for one request

    // On: bots also wait the kernel's own pacing, the way the live server
    // slows them so a human can follow. Off: a seat waits only its own
    // think_us, which is what makes a bot-vs-bot speed matchup legible.
    u8  bot_kernel_pacing;

    u64 stall_us;          // quiet time that counts as a stall
    u64 sweep_period_us;
    u64 lobby_delay_us;    // game over -> reset -> re-deal
    u64 stop_after;        // end the run once this many games have finished; 0 = never

    u8  deep;              // clone-and-compare every rejected move
    u8  csv;               // machine-readable per-seat lines for the sweep driver
    u8  verbose;
} Knobs;

typedef struct World {
    SCH sch;
    u64 rng;

    Knobs    knobs;
    Net      net;
    Findings findings;

    GameSlot *games;
    u32       n_games;

    ClientState *clients;
    u32          n_clients;

    Game *scratch;         // decode target for client views; the Game struct is
                           // far too big to give every client one of its own

    u64 moves_sent, moves_applied, moves_rejected, bot_actions, deals, finished;
    u64 stale_accepts;              // applied against a version the mover had not seen
    u64 fools[MAX_PLAYERS];         // who lost, by seat: the speed matchup's scoreboard
    u64 seat_moves[MAX_PLAYERS];
} World;

#endif
