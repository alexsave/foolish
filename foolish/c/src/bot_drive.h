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

// The values bot_pacing_ms prices a visible class at, adopted verbatim from the
// server as the one table (owner decision, July 2026): 3000ms is its tuned pace
// with a human watching (its own note records 4500ms as sluggish and 1500ms as
// too fast to follow), 300ms the bots-only pace nobody watches live.
#define BOT_PACE_MS_WITH_HUMANS 3000
#define BOT_PACE_MS_BOTS_ONLY    300

// ---------- the wait before a bot acts (a server's bot loop) ---------------
//
// HOW LONG A BOT MUST WAIT BEFORE IT ACTS ON THE BOARD IN FRONT OF IT, which is
// a question about the viewers and not about the bot's own last move.
//
// A bot answers a board its human viewers must first have SEEN: the last
// committed operation plays on their screens for its whole animation stream
// (anim_plan.h anim_stream_ms - a deal is a beat a card, a bout end a hold, a
// sweep and the refills), and only then does the reaction pace start. So with a
// human IN the bot may act at
//
//     settles_ms + BOT_PACE_MS_WITH_HUMANS
//
// where settles_ms is when the last shown operation finished playing, and in a
// bots-only game at shown_ms + BOT_PACE_MS_BOTS_ONLY, the pace from the last
// shown commit, with no animation term (nobody is watching live). The answer is
// that instant minus now_ms, never negative, and never past BOT_PACE_WAIT_MAX_MS.
//
// THIS REPLACES a fixed sleep after each bot cycle (bot_cycle_delay_ms, which
// the phone's local loop still uses: its board is on the same device, so its
// wait is what its own renderer needs). A fixed sleep counted from the bot's
// OWN commit ignored every other commit: the deal, whose seven beats ate the
// pace between the first two bot moves of a game, and a human's move, which a
// bot answered at t=0 - before the human's own screen had even landed it, so a
// throw-in made on the open bout that screen showed was refused (e2e
// web_bot_first_move_pace / web_throwin_vs_bot).
//
// The clock is the table's (view.h BoardClock, advanced by every commit and
// persisted in a v3 state blob); a clock of zero - a board never shown - asks
// for no wait. A v2 blob has no clock at all and reads as a zero one.
//
// THE CEILING is one ask's, not the longest wait: a host's lease must outlive
// any single wait (the Supabase bot lease is 25s, bot_actions.ts
// BOT_LEASE_TTL_MS), and a host asks again after every wait, so a longer one is
// waited out in ceiling-long slices. One exists in play: an eight-seat opening
// deal goes round the table a card at a time and plays about 20s
// (anim_plan.h ANIM_DEAL_CARD_MS), so its wait is two asks.
#define BOT_PACE_WAIT_MAX_MS 15000
// THE HORIZON is how far ahead of now a clock can be and still be one somebody
// is playing. Slicing alone would let a skewed or corrupt clock park a game ask
// after ask, so a clock past it is on another host's time (bot_clock_foreign)
// and asks for no wait at all. It sits well clear of the longest stream play
// makes (the eight-seat deal, with a move queued behind it and the pace; tests.c
// test_table_bot_wait holds the deal inside it).
#define BOT_CLOCK_HORIZON_MS 60000
int bot_wait_ms(const Game *g, uint32_t human_mask, int64_t shown_ms, int64_t settles_ms, int64_t now_ms);
// 1 when a board clock cannot be this host's: SHOWN further ahead of now than
// one wait (shown_ms is the committing host's own now, so a board shown in the
// future was shown on another clock), or SETTLING past the horizon. The one
// test both readers of a clock apply - bot_wait_ms, which then asks for no
// wait, and table.c next_clock, which then starts the next stream now instead
// of queueing it behind a stream nobody's screen is playing.
int bot_clock_foreign(int64_t shown_ms, int64_t settles_ms, int64_t now_ms);

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
// LCG and the draw LCG (under its search salt) are seeded from state_fnv before
// every choose, and the draw LCG again before every apply (table.c
// table_drive_seed, game.h GAME_SEED_SALT_*), so every stream is a pure function
// of the secret deal seed and the public board — reproducible to the server,
// unpredictable to everyone else, and blind to what the module ran before (the
// Monte-Carlo rollouts of robusta and firecracker draw from the draw stream, so
// leaving it unseeded made a decision depend on the module's history). A cycle
// drives several seats per call, so
// without this the seeding would happen once per CYCLE instead of once per
// DECISION, and bundling would silently change how bots play:
//
//   * seats acting after a stream-CONSUMING bot would draw from a shifted
//     stream (`random` and `handwritten_prod` call random_strategy_random; the
//     Monte-Carlo bots only read the state via random_strategy_rng_get);
//   * the two phases are SEPARATE because a strategy's search consumes the draw
//     stream (its rollouts refill scratch games), so a host that seeded the draw
//     LCG only before the choose would leave the real refill drawing from
//     whatever the search consumed.
//
// It is a hook rather than a bot_drive() argument because the derivation is
// host property: g_rng_base is a server-only secret the phone and the native
// arena do not have. They install nothing and keep their own seeding, which is
// why this must default to NULL. Same shape as engine_snap_hook (game.h).
extern void (*bot_drive_pre_action_hook)(const Game *g, int seat, int phase);

// The per-decision seeding every server host installs through that hook, so the
// policy has one definition: at CHOOSE the strategy stream and the draw stream
// (under its search salt), at APPLY the draw stream, each from game_state_seed of
// the board in front of the decision, the host's secret `base` and the game's
// PROGRESS. The Table (table.c), the native server (server/impls/native) and the
// wasm bridge's resident drive all call it.
//
// PROGRESS is the length of the game's session log at the decision: the records
// the board already holds (g->num_logs) plus `log_offset`, the records the row's
// log holds BELOW g->logs[0] for a host that did not load them (the Table loads
// them only for a brain that reads them). 0 when `g` holds the whole log.
//
// Why the board alone is not enough. Every other term of the seed is the public
// board, so two decisions on the SAME board draw the same numbers — and a table
// whose remaining players are all `random` bots can return to an exact earlier
// board, which then repeats its move, and the board, forever. Measured over 40
// seeds per seat count at 2 to 8 seats, one game each at 4, 5, 6 and 7 seats
// looped that way (period 12 to 15 cycles, from cycle 105 to 156), about 1 game
// in 40 at those seat counts; in production such a table holds `needs_bots`
// forever. The session log only grows, so folding its length in makes a repeated
// board draw a fresh number and the loop cannot close. It keeps the decision a
// pure function of the STORED ROW — state, roster and log — so every instance
// that loads that row still chooses the same move.
void bot_drive_seed_decision(const Game *g, uint32_t base, uint32_t log_offset, int phase);

// A host's secret base from its deal seed (FNV-1a over the bytes; the Table hashes
// the seed's hex text, the native server its 32 raw bytes). 0 for no seed.
uint32_t bot_drive_seed_base(const uint8_t *seed, int len);

// ---------- belief probe (test observability) ------------------------------
//
// What a bot SEARCH saw of the session log, recorded by the wasm bridge as the
// strategy was about to read it (c/wasm/wasm_bots_api.c). A host-side spy can
// only prove the log's bytes were handed over, never that they were spliced into
// the Game the strategy read - the gap the octogen-blind and cordite
// stale-belief regressions lived in. The records cross as this struct, read
// through the generated accessors (sdk/ts/gen/game_layout.<build>.ts), so no
// harness restates their layout.
#define BELIEF_PROBE_CAP 64

typedef struct {
    uint8_t  seat;
    uint16_t n_logs;   // log records spliced into the Game at the decision
    uint64_t cards;    // bit (suit*16 + value) per real card visible in that log
} BeliefProbe;

#endif
