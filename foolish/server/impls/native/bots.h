// bots.h - how a bot seat gets to move in this server.
//
// One trampoline thread per game paces the bots (bot_thread), the kernel
// decides every actual move (bot_drive), and this server decides only how to
// wait between cycles and how a decision is seeded. No game rule lives here.
#ifndef FOOLISH_BOTS_H
#define FOOLISH_BOTS_H

#include <stdatomic.h>

#include "registry.h"

// Stage 4 (SERVER_SCALING.md "Stage 4 - spectators + octogen stress"):
// process-wide bot-decision counters, read by GET /stats and GET /metrics. A
// "decision" here is one BotDriveAction actually applied by bot_thread's
// bot_drive call (drv.actions[0..drv.n)) - the unit bot_stress.sh measures
// decisions/sec against, at 1 game and at N games. Stage 4 used this to
// quantify g_kernel_lock's ceiling on bot compute (see the "Locking" doc in
// registry.h); Stage 5 removed that lock and re-ran the same sweep to show the
// ceiling lifting (SERVER_SCALING.md "Stage 5"). `g_bot_decisions` counts every
// one of them, any strategy, any game; `g_octogen_decisions` narrows to actions
// applied by a seat whose strategy_key is octogen's STRAT_* brain id
// (g_octogen_strat, resolved once by bots_resolve_octogen_strat - read-only
// from every thread after that, so no lock is needed to read it from
// bot_thread). Plain atomics: relaxed is enough, same reasoning as g_seq -
// nothing else about these counters needs ordering with any other memory
// access.
extern atomic_ulong g_bot_decisions;
extern atomic_ulong g_octogen_decisions;

// Resolve "octogen"'s STRAT_* brain id ONCE, before any bot_thread can run.
// Called from main(); g_octogen_strat is read-only from every thread after
// that, so no lock is needed (same posture g_tls_ctx's doc takes).
void bots_resolve_octogen_strat(void);

// Install the per-decision seeding hook (bot_drive_pre_action_hook). Process-
// wide and installed once, before any thread starts and before the self-tests.
void bots_install_seed_hook(void);

// Spawn the game-loop for a freshly dealt game (idempotent). Caller MUST
// hold s->lock: bot_thread's very first action is to lock it too, so this
// just races the parent's own unlock (harmless - see bot_thread's doc).
void start_bot_loop(GameSlot *s);

// --self-test-bot-seeding: a bot cycle is a function of the game and its base,
// not of what this thread ran before. Every linked roster brain plays 3-seat games
// of itself; each cycle runs twice from the same board, once after a fixed RNG
// state and once after other RNG states and another game's cycle, and both runs
// must leave the same board and apply the same actions. The Monte-Carlo brains'
// rollout policies draw from the draw stream, which is what a missing seed shows.
// Returns a process exit code (0 = every brain passed).
int run_bot_seeding_self_test(void);

#endif
