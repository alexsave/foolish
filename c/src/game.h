// Game state, players, logs. Mirrors the TS structures in types.ts and the
// behaviors in common_utils.ts / actions/*.ts. We don't model the production
// fields (animations, ELO, message broadcasting) — only what the bots need.
#ifndef CNITRO_GAME_H
#define CNITRO_GAME_H

#include "card.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define MAX_PLAYERS    8          // 2..8 players; deck size boundary is
                                  // configurable (see card.h).
#define MAX_HAND_SIZE  64         // generous; pickup can stack many cards.
#ifndef MAX_BATTLES
#define MAX_BATTLES    32         // build parameter; WASM uses 64 (a defender
                                  // holding 33+ cards can legally face 33+
                                  // simultaneous attacks).
#endif
#define MAX_DECK       64         // 36 for 2p, with slack.
#ifndef MAX_LOGS
#define MAX_LOGS       1024       // build parameter, sized on p99 — NOT on max.
                                  // Game length is unbounded (pickups can cycle,
                                  // even at 3p), so a max is only ever whatever
                                  // the sample happened to reach: the same config
                                  // gave max 641 over 400 games/pc and 853 over
                                  // 1500. p99 is stable; a max is not.
                                  //
                                  // MEASURED under the SHIPPED caps
                                  // (MAX_LOG_PAIRS=64 etc.), robusta, 400 full
                                  // games per player count, uncapped:
                                  //
                                  //   np   p50   p90   p99  p99.9
                                  //   2    116   144   169   181
                                  //   4    177   237   285   342
                                  //   6    368   459   531   550
                                  //   7    378   491   576   623   <- worst
                                  //   8    375   479   567   600
                                  //
                                  // 6+ players deal the 52-card deck, which is
                                  // why they run long — 7p is the worst case, not
                                  // 8p. The old 512 sat BELOW p99 for 6-8p and so
                                  // overflowed 2-5% of those games. 1024 is 1.8x
                                  // the worst p99 and 1.6x its p99.9.
                                  //
                                  // NO cap is safe, because the tail is unbounded
                                  // — this only makes overflow rare, it does not
                                  // retire it. Overflow is still ungraceful:
                                  // log_alloc DROPS records, so belief bots
                                  // silently forget discards AND
                                  // replay_encode_v6_from_game refuses the
                                  // truncated stream, leaving the game
                                  // unshareable and, over iMessage, UNSENDABLE.
                                  // The real fix is for the encoder not to need
                                  // the whole log (docs/IMESSAGE_BODY_CODEC.md
                                  // §4); raising this buys time, not correctness.
                                  //
                                  // MEASURE before changing it: the caps are not
                                  // independent. MAX_LOG_PAIRS truncates the CARDS
                                  // in a record, and belief bots read discard logs
                                  // back — so the native default (16) makes bots
                                  // play differently, and longer, than production
                                  // (64). A measurement at the wrong caps is not
                                  // about this game.
                                  //
                                  // Costs +66KB per FULL-SIZE Game (68,680 ->
                                  // 136,264) plus the 64 KiB g_io growth, which
                                  // the solve_ws overlay absorbs for free.
                                  // It does NOT touch the Monte-Carlo hot loop:
                                  // sampled-world slots are sized by WORLD_LOG_CAP
                                  // (cordite_sim.h WORLD_SLOT_BYTES), so a rollout
                                  // clone is 6,376 B at ANY MAX_LOGS, and beliefs
                                  // are built from the session log once and never
                                  // carried down the recursion. Nothing holds an
                                  // array of full Games.
                                  //
                                  // The session-log importers (bots.wasm, TS
                                  // MAX_KERNEL_LOGS) mirror this — move both.
                                  // rules.wasm overrides to 128 and is unaffected.
#endif

#define LOG_GAME_START      0
#define LOG_ATTACK          1
#define LOG_COVER           2
#define LOG_PASS            3
#define LOG_PICKUP          4
#define LOG_GOOD            5
#define LOG_DISCARD         6
#define LOG_DEFENDER_CHANGE 7
#define LOG_PLAYER_OUT      8
#define LOG_DRAW            9

#define PLAYER_STATUS_IDLE  0
#define PLAYER_STATUS_READY 1
#define PLAYER_STATUS_IN    2
#define PLAYER_STATUS_OUT   3

#define GAME_STATUS_WAITING   0
#define GAME_STATUS_PLAYING   1
#define GAME_STATUS_GAME_OVER 2

typedef struct {
    Card attack;
    Card defense;     // CARD_NONE when uncovered
} Battle;

// Each pair has a primary card and an optional target card. For COVER, target
// is the attack card the cover defends; otherwise unused.
//
// Capacity is a build parameter: the native arena keeps the compact 16 (its
// Game struct is memcpy-cloned in Monte-Carlo hot loops), while the WASM
// production build uses 64 (-DMAX_LOG_PAIRS=64) because the TS engine logs
// every card of a big pickup/discard and the replay codec needs them all.
#ifndef MAX_LOG_PAIRS
#define MAX_LOG_PAIRS 16
#endif
typedef struct {
    Card  primary;
    Card  target;
} LogPair;

typedef struct {
    int8_t  log_type;
    int8_t  player_idx;     // -1 = system event
    int8_t  defender_index; // -1 if not a defender_change
    int8_t  num_pairs;
    LogPair pairs[MAX_LOG_PAIRS];
} GameLog;

typedef struct {
    int8_t  status;            // PLAYER_STATUS_*
    int8_t  hand_count;
    bool    awaiting_attack;
    int8_t  strategy_key;      // application-defined; STRATEGY_KEY_HUMAN = a human seat
    Card    hand[MAX_HAND_SIZE];
    char    name[24];
    char    player_id[24];
} Player;

typedef struct {
    int8_t  status;
    int8_t  num_players;
    int8_t  power_suit;
    int8_t  first_attacker;
    int8_t  defender;
    int8_t  num_battles;
    int16_t deck_count;
    int16_t discard_pile_length;
    bool    has_flipped;
    // Seed-dealt game: the deck was ChaCha-shuffled once at the deal and every
    // draw pops the top, so the whole game is reproducible from the seed. Set at
    // the deal, carried in the durable state blob (state format v2), and read by
    // every mid-game refill. Legacy (LCG) games leave it false and draw at
    // random exactly as before. Not part of the ephemeral IO marshal.
    bool    deterministic_deck;
    Card    flipped;
    Card    deck[MAX_DECK];
    Battle  table_battles[MAX_BATTLES];
    Player  players[MAX_PLAYERS];

    // Elimination order: indices into players[]; length = num_eliminated.
    int8_t  elimination_order[MAX_PLAYERS];
    int8_t  num_eliminated;

    // good_players: bitmask of player indices that have said good.
    uint32_t good_players_mask;
    bool     has_good_timestamp;

    // Log storage control for SHORT-log instances (sampled-world slots whose
    // logs[] array is allocated smaller than MAX_LOGS — see WORLD_LOG_CAP in
    // cordite_sim.h). 0 (the default everywhere else) = full MAX_LOGS
    // capacity, byte-identical behavior to before these fields existed.
    // When log_cap > 0, log_alloc keeps ONLY LOG_DISCARD entries (the one
    // log type any rollout policy reads back) up to log_cap, and log_virt
    // counts every append — kept or filtered — so the MAX_LOGS capacity
    // cliff lands on exactly the same append as a full-size instance.
    int16_t  log_cap;
    int16_t  log_virt;

    // Logs (append-only).
    int      num_logs;
    GameLog  logs[MAX_LOGS];
} Game;

// ---------- RNG ---------------------------------------------------------

// Mirrors `seededRandom` in common_utils.ts and `setRandomSeed` in
// random_strategy.ts: two independent LCGs with the same recurrence.
void   game_set_seed(uint32_t s);
double game_random(void);            // 0..1
uint32_t game_random_u32(void);

// The wide deal seed's length: two 128-bit ChaCha lanes. A SHORT seed does not
// half-seed the deal — it silently leaves wide mode off and degrades the game
// to the legacy 32-bit LCG, which is catastrophic for any host that must
// reproduce a deal from it (iMessage deals the same game on both devices; the
// v6 replay encoder re-derives a finished game's true hands from it). So the
// length is a named constant every producer and consumer rejects against,
// rather than a 32 repeated at each call site.
#define FOOLISH_SEED_LEN 32

// Wide, reproducible, full-universe deal seed (see deal_rng.h). Supplying
// FOOLISH_SEED_LEN bytes (two 128-bit lanes) switches the DEAL's random card
// picks from the 32-bit LCG to an unbiased ChaCha stream — lifting reachable
// deals from 2^32 to the whole 52!/36! space and making the deal reproducible
// from the seed. len must be >= FOOLISH_SEED_LEN; fewer bytes is ignored (wide
// mode stays off). Any later game_set_seed() call turns wide mode back off, so
// the legacy 32-bit path and its pinned test streams are byte-for-byte
// unchanged when no wide seed is set.
void   game_set_deal_seed_bytes(const uint8_t *seed, int len);
int    game_deal_seed_active(void);  // 1 if a wide deal seed is in effect

// Save/restore the DEAL RNG — the wide flag plus the ChaCha stream position —
// the same idea as game_rng_get/set one level down. A scratch re-deal
// (replay_encode_v6_from_game re-derives a finished game's deal from its seed)
// calls start_game, which consumes the stream and sets wide mode; wide mode is
// read by draw_index for EVERY game in this thread, so a re-deal that did not
// put it back could change how the NEXT game in a warm isolate draws. Opaque
// blob, sized by a static assert in game.c.
#define GAME_DEAL_RNG_STATE_MAX 144
void   game_deal_rng_get(unsigned char out[GAME_DEAL_RNG_STATE_MAX]);
void   game_deal_rng_set(const unsigned char in[GAME_DEAL_RNG_STATE_MAX]);

// A seed-dealt game records deterministic_deck=true in its state (see the Game
// field). Mid-game kernel calls need no seed: deserializing the durable blob
// restores that flag, and draws pop the pre-shuffled deck. game_set_seed()
// clears the wide flag; the persisted per-game flag is untouched by it.

// Save/restore the game LCG state. Lets a strategy run internal
// simulations (which consume game_random via draws and rollout policies)
// without perturbing the outer game's random stream, and gives all
// simulations of competing moves identical RNG streams (common random
// numbers). Purely additive — no behavior change for existing callers.
uint32_t game_rng_get(void);
void     game_rng_set(uint32_t s);

void     random_strategy_set_seed(uint32_t s);
double   random_strategy_random(void);
uint32_t random_strategy_rng_get(void);

// ---------- Engine observation hooks ------------------------------------
//
// Optional callback fired at exactly the points where the production TS
// server captured an intermediate game-state snapshot for an animation
// event (see actions/*.ts). NULL (the default) costs nothing; the WASM
// bridge installs one to reconstruct the TS AnimationEvent stream from C
// transitions. `aux` is the acting/affected player index, or the battle
// index for ENGINE_HOOK_COVER.

#define ENGINE_HOOK_ATTACK           1
#define ENGINE_HOOK_OUT              2
#define ENGINE_HOOK_COVER            3
#define ENGINE_HOOK_DISCARD          4
#define ENGINE_HOOK_DRAW             5
#define ENGINE_HOOK_DEFENDER_MOVE    6
#define ENGINE_HOOK_PASS             7
#define ENGINE_HOOK_PICKUP           8
#define ENGINE_HOOK_MAGIC_TRANSITION 9
#define ENGINE_HOOK_TRASH            10
#define ENGINE_HOOK_START_MAGIC      11
#define ENGINE_HOOK_DEAL             12
#define ENGINE_HOOK_FLIPPED          13
#define ENGINE_HOOK_START_DEFENDER   14

extern void (*engine_snap_hook)(const Game *g, int tag, int aux);

// ---------- Rejection reasons --------------------------------------------
//
// Why the last handle_* / validation returned false. The TS bridge maps
// these to the exact production error messages; native callers may ignore
// them. Reset to ENGINE_REJECT_NONE at the top of every handler.

#define ENGINE_REJECT_NONE                0
#define ENGINE_REJECT_NOT_PLAYING         1
#define ENGINE_REJECT_EMPTY               2
#define ENGINE_REJECT_IS_DEFENDER         3
#define ENGINE_REJECT_NOT_DEFENDER        4
#define ENGINE_REJECT_NOT_IN_HAND         5
#define ENGINE_REJECT_DUPLICATES          6
#define ENGINE_REJECT_NOT_SAME_VALUE      7
#define ENGINE_REJECT_NOT_FIRST_ATTACKER  8
#define ENGINE_REJECT_VALUE_NOT_ON_TABLE  9
#define ENGINE_REJECT_DEFENDER_CAPACITY   10
#define ENGINE_REJECT_NO_UNCOVERED        11
#define ENGINE_REJECT_ATTACK_NOT_ON_TABLE 12
#define ENGINE_REJECT_CANNOT_COVER        13
#define ENGINE_REJECT_NO_TABLE_CARDS      14
#define ENGINE_REJECT_COVER_PRESENT       15
#define ENGINE_REJECT_PASS_VALUES         16
#define ENGINE_REJECT_PASS_CAPACITY       17
#define ENGINE_REJECT_NOT_IN_STATUS       18
#define ENGINE_REJECT_ALREADY_GOOD        19
#define ENGINE_REJECT_FIRST_MUST_ATTACK   20
#define ENGINE_REJECT_PASS_OVERFLOW       21

extern int engine_last_reject;

// ---------- Helpers -----------------------------------------------------

bool can_cover(Card attack, Card defense, int power_suit);
int  get_next_player_index(const Game *g, int current);
int  game_done(const Game *g);   // returns loser index, or -1

// A seat whose strategy_key is this is a HUMAN: the auto-driver (bot_drive) must
// not act for it. strategy_key is otherwise a bot's roster index (>= 0), so this
// sentinel never collides. It is application-defined data the host writes and is
// NOT serialized (identity stays with the caller, see the Player comment) — but
// it is the one value the kernel reserves, so a host that marks its human seats
// can ask the kernel for the drive mask (game_human_mask) instead of tracking an
// is_ai array of its own.
#define STRATEGY_KEY_HUMAN (-1)

// The seats the auto-driver must NOT act for: those a host marked human. A host
// passes this to bot_drive instead of hand-rolling the mask from its own roster.
// bot_drive still takes an EXPLICIT mask, so replay/spectate can drive every
// seat by passing 0 regardless of who is marked human.
uint32_t game_human_mask(const Game *g);

// Seat `n` players and DEAL, in one kernel call — the whole "go from a lobby to a
// dealt board" the hosts used to hand-roll. Sets the seat count; if
// `strategy_keys` is non-NULL, writes each seat's kind (STRATEGY_KEY_HUMAN, or a
// bot roster index); then deals via start_game, which assigns each seated
// player's status. Pass NULL to keep the strategy_key the seats already hold (a
// host that wired kinds incrementally as players joined). The host owns identity
// (names/player_id/tokens), set on the Player array before or after — the deal
// never touches it. A bad seat count (n < 2 or > MAX_PLAYERS) is a no-op.
void game_seat_and_deal(Game *g, const int8_t *strategy_keys, int n);
// Records the end of a game on its OWN status: once game_done fires, the kernel
// (not each host) flips g->status to GAME_OVER, so g->status is the single
// lifecycle truth every view carries and no server recomputes game_done to keep
// a parallel status of its own. Idempotent; a no-op mid-game or in a lobby. The
// apply paths (awire_apply, bot_drive) call it after a move settles.
void game_settle_status(Game *g);
void start_game(Game *g);

// start_game with the deck supplied instead of shuffled: `deck` is the
// PRE-deal pile in pop order (deal_initial takes CARDS_PER_PLAYER per seat,
// player-major, then the flip). Everything after the deck exists — the deal,
// the flip, first_attacker/defender, and every snapshot hook — is the SAME
// code start_game runs, so a game rebuilt this way is a real played game and
// not a projection of one.
//
// This is how a v6 replay comes back to life (replay_steps.c): its decode
// yields the exact hands and the exact draw order, which is exactly a deck.
// The caller owns feasibility — a deck that cannot produce the recorded game
// simply produces a different one; replay_steps_v6 is the checked entry.
void start_game_with_deck(Game *g, const Card *deck, int n_deck);

// Pin the opening seat for the ONE deal that cannot derive it: when no player
// holds a trump, the engine rolls for the first attacker, and a replay has no
// way to reproduce that roll. A replay passes the seat its code recorded; -1
// (the default) restores the roll. Consulted only on that branch — see
// determine_lowest_power_index. Set it before start_game*, clear it after.
void game_force_first_attacker(int seat);

// THE FOOL'S PENALTY. A rematch played by the SAME people in the SAME cycle
// does not open on the lowest trump: it opens on the seat to the RIGHT of the
// previous game's fool, so the fool is the first player attacked. (Right, not
// left: attacks travel to the attacker's left - see the rulebook's Attacking
// section - so the seat to the fool's right is the one whose attack lands on
// the fool.) This pins that seat for the next deal; -1 (the default) restores
// the ordinary lowest-trump derivation.
//
// UNCONDITIONAL, unlike game_force_first_attacker: it overrides a seat the
// deal could perfectly well derive. Set it before start_game*, clear it after.
// Who is entitled to it is NOT decided here - msg_wire.c owns that rule
// (msg_rematch_opening) and the answer travels on the wire, so every device
// deals the same board.
void game_open_at_seat(int seat);

// What the last deal DERIVED for its opening seat, before any override: the
// lowest-trump seat, or -1 when no player was dealt a trump. A v8 replay code
// records this so a rebuilt deal can still be checked against it even when the
// override decided the actual opener.
int game_derived_opening(void);

// The lobby a finished game resets to on "continue"/rematch — the one
// definition of that transition (docs/C_CORE_CONSOLIDATION.md F6). Three hosts
// hand-zeroed this list independently: the server (handleContinue), the web
// client (clientReconcile.resetToLobby, which had to "match byte-for-byte or
// the UI snaps"), and iOS was specced to port it a third time.
//
// `bot_mask` bit s = seat s is a bot, which resets to READY; humans reset to
// IDLE. It is a PARAMETER because it cannot be a guess: seat identity
// (is_ai/strategy_key/player_id/name) is deliberately not in the state blob —
// it lives with the caller — so the kernel is told, not left to infer.
//
// Logs are left alone: this is a board reset, not a new game. start_game
// clears them.
void game_reset_to_lobby(Game *g, unsigned int bot_mask);

// In-place game clone (used by collect's `before` snapshot).
void game_clone(Game *dst, const Game *src);

// ---------- Action handlers (return false on validation failure) --------

bool handle_attack(Game *g, int player_idx, const Card *cards, int n_cards);
bool handle_cover(Game *g, int player_idx,
                  const Card *cover_cards, const Card *attack_cards, int n);
bool handle_pass(Game *g, int player_idx, const Card *cards, int n_cards);
bool handle_pickup(Game *g, int player_idx);
bool handle_good(Game *g, int player_idx);

// ---------- Loop helpers ------------------------------------------------

bool should_bot_act(const Game *g, int bot_idx);

// Public entries for the two round-lifecycle phases the TS server also
// exposed standalone (executeRoundTransition / refillPlayerHandsWithEvents).
void engine_run_round_transition(Game *g);
void engine_run_refill(Game *g);

#endif
