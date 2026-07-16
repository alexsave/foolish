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

// A move a seat already decided on, to be reused IF it is still legal.
//
// Only for the server's CAS-retry path. executeWithGameLock re-runs the whole
// operation on a version conflict, and a bot that re-chooses from scratch each
// attempt re-runs its search — for cordite's Monte-Carlo that is seconds of
// CPU, and a few attempts blow the edge's ~2s budget and get the isolate killed
// while holding the lease. So the host hands back what the failed attempt
// chose. The kernel still decides whether it may be played: a legal,
// slightly-stale choice beats a CPU kill, an illegal one is simply re-chosen.
typedef struct {
    int8_t    seat;
    LegalMove move;
} BotDrivePref;

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
// `pref`/`n_pref` (may be NULL/0) offer per-seat moves to reuse when still
// legal — see BotDrivePref. They change only whether a seat SEARCHES, never
// which seats are eligible, the order they are picked in, or what is legal.
//
// Returns the number of actions applied, or -1 on a bad argument.
int bot_drive(Game *g, uint32_t human_mask, int max_actions,
              const BotDrivePref *pref, int n_pref, BotDriveOut *out);

// The wait for one completed drive cycle (max visible pacing class, priced +
// human-reduced) in one call, so no host re-reduces `drv`'s actions itself. The
// loop and the sleep stay host-side; this is only the "how long". `drv` is the
// output of the bot_drive that just ran; `human_mask` its seats.
int bot_cycle_delay_ms(const Game *g, uint32_t human_mask, const BotDriveOut *drv);

// The bot seats that could act right now (bitmask), ignoring human_mask seats.
// Hosts use this to decide I/O the kernel cannot do — the server hydrates the
// belief log only when a bot that reads it is about to choose.
uint32_t bot_drive_eligible_mask(const Game *g, uint32_t human_mask);

// The two points a host may re-seed at, in the order they happen.
#define BOT_DRIVE_PHASE_CHOOSE 0  // about to run the strategy
#define BOT_DRIVE_PHASE_APPLY  1  // about to apply the chosen move

// Called at each phase of each seat's action, if installed. NULL by default.
//
// For hosts that re-seed their RNG per decision. The server does: the strategy
// LCG is seeded from state_fnv before every choose and the mid-game draw LCG
// before every apply (wasm_api.c), so both streams are a pure function of the
// secret deal seed and the public board — reproducible to the server, and
// unpredictable to everyone else. A cycle drives several seats per call, so
// without this the seeding would happen once per CYCLE instead of once per
// DECISION, and bundling would silently change how bots play:
//
//   * seats acting after a stream-CONSUMING bot would draw from a shifted
//     stream (`random` and `handwritten_prod` call random_strategy_random; the
//     Monte-Carlo bots only read the state via random_strategy_rng_get);
//   * the two phases are SEPARATE because a strategy's search consumes the draw
//     stream (its rollouts refill scratch games), so a host that re-seeds the
//     draw LCG before the choose would both feed the search a value the
//     single-move path never gave it and leave the real refill drawing from
//     whatever the search consumed. Both were caught by
//     e2e/bot_drive_parity.test.ts as a changed bot move.
//
// It is a hook rather than a bot_drive() argument because the derivation is
// host property: g_rng_base is a server-only secret the phone and the native
// arena do not have. They install nothing and keep their own seeding, which is
// why this must default to NULL. Same shape as engine_snap_hook (game.h).
extern void (*bot_drive_pre_action_hook)(const Game *g, int seat, int phase);

#endif
