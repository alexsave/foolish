// The bot drive cycle: pick eligible bot seats, choose, apply, stop.
//
// docs/C_CORE_CONSOLIDATION.md F2/F3. This was written twice — the server's
// leased loop (bot_actions.ts) and the phone's seat walk (LocalGame.swift over
// fio_bot_step_json) — and the two had drifted into playing different games:
//
//   * FAIRNESS. The server picks among simultaneously-eligible bots with a
//     shuffle; fio_bot_step_json walked seats and took the first eligible one.
//     In bot-heavy games that hands low seats a systematic tempo advantage the
//     site does not have.
//   * BUNDLING. The server coalesces zero-event passive actions (silent
//     "good"s) into one cycle; the phone paid a full UX delay for each, so a
//     round with five passives felt padded.
//
// Both are decisions about what happens next in a game of Durak, so they are
// kernel property, not host property. What stays with the host is everything
// about waiting, persisting and drawing: the server keeps its lease, CAS
// commit, broadcast and CPU budget; iOS keeps timers, thermal policy and
// rendering (§5).

#ifndef CNITRO_BOT_DRIVE_H
#define CNITRO_BOT_DRIVE_H

#include <stdint.h>

#include "game.h"
#include "legal.h"

// ---------- pacing (F3) ----------------------------------------------------
//
// What a move is worth pausing for. The kernel classifies; the host sleeps.
#define BOT_PACE_NONE             0  // nothing to watch
#define BOT_PACE_BUNDLED_PASSIVE  1  // a silent action folded into this cycle
#define BOT_PACE_MOVE             2  // a visible move — cards changed hands
#define BOT_PACE_ROUND_TRANSITION 3  // the bout resolved (discard/pickup)

// Milliseconds a host should wait after an action of this class.
// `humans_present` = at least one human is still in the game and watching.
//
// ONE table, so a change to the game's feel lands on every surface at once.
// The values are the server's, which the phone now adopts (owner decision,
// July 2026): the phone had been running 600-1200ms while claiming in a
// comment to "mirror the server", which was never true.
int bot_pacing_ms(int pacing_class, int humans_present);

// ---------- the drive cycle (F2) -------------------------------------------

// Why the drive stopped.
#define BOT_STOP_NO_ELIGIBLE 0  // no bot seat can act (a human's move is owed)
#define BOT_STOP_ENDED       1  // the game is over
#define BOT_STOP_EVENTS      2  // an event-bearing action landed — go render it
#define BOT_STOP_MAX         3  // max_actions reached; call again to continue

// Bundled passives cannot outnumber the seats, so one cycle can never apply
// more than MAX_PLAYERS silent actions plus the one visible action that ends it.
#define BOT_DRIVE_MAX_ACTIONS (MAX_PLAYERS + 1)

typedef struct {
    int8_t    seat;
    uint8_t   pacing_class;   // BOT_PACE_*
    LegalMove move;           // what was applied
} BotDriveAction;

typedef struct {
    BotDriveAction actions[BOT_DRIVE_MAX_ACTIONS];
    int n;        // actions applied, 0..BOT_DRIVE_MAX_ACTIONS
    int stop;     // BOT_STOP_*
    int ended;    // game_done() after the drive: loser seat, or -1
} BotDriveOut;

// Drive bot seats until a stop condition, applying 0..n actions to `g`.
//
// `human_mask` is a bitmask of seats the kernel must NOT drive. It does NOT
// mean "seats to wait for": a human being eligible is not a stop condition
// (owner decision, July 2026 — the site's rule is canonical). During a bout
// the defender and every attacker that has not said good are eligible AT ONCE
// (should_bot_act), so yielding to any eligible human would stop bots from
// ever throwing in while a human deliberates — a large online gameplay change,
// and one that can stall a bout on an idle player.
//
// Selection among simultaneously-eligible bots is a shuffle seeded from the
// game's PUBLIC state, so it is fair, identical on every host, and reproducible
// in replays and tests. It deliberately consumes no RNG: the deal/refill stream
// must not shift (deterministic_deck games are reproducible from their seed).
//
// Returns the number of actions applied, or -1 on a bad argument.
int bot_drive(Game *g, uint32_t human_mask, int max_actions, BotDriveOut *out);

// The bot seats that could act right now (bitmask), ignoring human_mask seats.
// Hosts use this to decide I/O the kernel cannot do — the server hydrates the
// belief log only when a bot that reads it is about to choose.
uint32_t bot_drive_eligible_mask(const Game *g, uint32_t human_mask);

#endif
