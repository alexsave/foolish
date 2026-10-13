// WebAssembly bridge for the cnitro rules kernel (game.c + legal.c).
//
// The TS side (sdk/ts/wasm/engine.ts) marshals the Game
// through ONE compact byte layout in a shared IO buffer - deliberately
// independent of the C struct's padding, so TS never touches struct offsets.
// Every state field crosses as explicit little-endian bytes.
//
// Snapshots: game.c fires engine_snap_hook at exactly the points where the
// production TS handlers captured an intermediate game state for an
// animation event; we copy the whole Game into a slot per hook. The TS
// bridge reads them back to synthesize the identical AnimationEvent stream.
//
// Freestanding: no libc. memcpy/memset are provided here (clang lowers
// struct copies to them on wasm32).

#include <string.h>   /* memcpy/memset: ../../shared/c/wasm/libc.c, linked into every module */
#include "game.h"
#include "wire.h"
#include "legal.h"
#include "replay.h"
#include "msg_wire.h"
#include "wasm_overlay.h"
#include "rules_overlay.h"
#include "view.h"
#include "awire.h"
#include "evwire.h"
#include "anim_plan.h"


// ---------- shared buffers ----------------------------------------------

// Sized by the widest export, which is the LOG export worst case:
// 2 + MAX_LOGS x (4 + MAX_LOG_PAIRS x 2) - written unchecked by
// wasm_export_logs, so IO_CAP must clear it at the build's MAX_LOGS:
// 67,586B at bots' 512/64 (the 72KB default), 16,898B at rules' 128/64
// (why the rules build overrides to 24KB - see the Makefile L1 notes).
// Everything else is far smaller (state export <1.1KB, env strings, the
// chosen move), and the legal-move menu is not copied into it at all: a host
// reads the resident LegalMoves where it lies (wasm_legal_moves_ptr), so the
// menu no longer sizes IO_CAP.
#ifndef WASM_IO_CAP
#define WASM_IO_CAP (72 * 1024)
#endif
#define IO_CAP WASM_IO_CAP

// export_logs writes the whole log stream into g_io with NO bounds check (see
// there), so IO_CAP being large enough is not an optimization - it is the only
// thing standing between a long game and a heap smash. That was a COMMENT until
// a MAX_LOGS bump compiled cleanly straight past it; it is a build error now.
//
//   2 (u16 count) + MAX_LOGS x (4 header bytes + MAX_LOG_PAIRS x 2 card bytes)
#define LOG_EXPORT_WORST (2u + (unsigned)MAX_LOGS * (4u + (unsigned)MAX_LOG_PAIRS * 2u))
_Static_assert(LOG_EXPORT_WORST <= IO_CAP,
               "IO_CAP cannot hold the log export at this MAX_LOGS/MAX_LOG_PAIRS "
               "- raise WASM_IO_CAP or lower MAX_LOGS (wasm_api.c export_logs "
               "writes unchecked)");
// Snapshot slots for ONE action window of the wasm_* action exports (the ring
// resets per action, begin_action). NOT MAX_SNAPS (game.h), which is sized for
// a deal: no host deals through this ring and reads it back. The server deals
// through the Table (table.c) and a replay through replay_steps.c, and both of
// those stores are MAX_SNAPS. What does reach the ring is an action, or a bot
// cycle (a silent bundle ahead of one visible action): a round transition at
// worst, MAGIC + TRASH + <=num_players+1 per-seat refill draws, measured 12
// over 63K games (tests/l1_measure.c). 16 is 1.33x that.
//
// Kept small ON PURPOSE: 48 more slots here are 37 KiB of bss in bots.wasm and
// web.wasm, which is a page of linear memory in each (e2e/mem/wasm_memory.test.ts
// pins both). wasm_start_game and msg_decode's re-deal still fire the deal's
// hooks into it; past 16 they drop - cleanly, the ring never overruns - and
// nothing reads a deal back from here.
#ifndef SNAP_RING_CAP
#define SNAP_RING_CAP 16
#endif
#define MAX_IN_CARDS 128

#ifdef CD_RULES_OVERLAY
// R1 (docs/RULES_GUARDS_WASM_MEMORY_PLAN.md): the rules.wasm arena that hosts
// BOTH buffer families (see rules_overlay.h). Sized by the larger (action)
// family; the replay family aliases it from offset 0. rules.wasm-only - the
// bots build uses solve_ws (CD_WASM_OVERLAY) and native uses plain statics.
_Alignas(16) static unsigned char g_rules_arena[RULES_ARENA_SIZE];
unsigned char *const rules_overlay = g_rules_arena;
_Static_assert(RULES_OVL_ACTION_END <= RULES_ARENA_SIZE, "action family overflows rules arena");
_Static_assert(RULES_OVL_REPLAY_END <= RULES_ARENA_SIZE, "replay family overflows rules arena");
#endif

#ifdef CD_WASM_OVERLAY
// M9 (docs/BOTS_WASM_MEMORY_PLAN.md) aliased g_io into solve_ws - a THIRD
// non-concurrent tenant, disjoint from the replay scratch above - to save 72 KiB.
// That holds only while g_io FITS the slot. It no longer does: g_io is now sized
// to accept an UNTRIMMED session log on import (~3072 records, 400 KiB) so the
// kernel can filter dead goods itself instead of making TS pre-filter to fit the
// buffer. 400 KiB does not fit a 272 KiB arena, so on this build g_io is its own
// static and the M9 saving is spent - deliberately, to keep a rules concern out
// of TypeScript. The condition is compiled, not assumed: shrink IO_CAP back
// under the slot and the overlay silently returns.
#if IO_CAP <= (CD_OVL_GIO_END - CD_OVL_GIO_OFF)
#define g_io ((unsigned char *)(cd_overlay + CD_OVL_GIO_OFF))
#else
static unsigned char g_io[IO_CAP];
#endif
#elif defined(CD_RULES_OVERLAY)

// R1: g_io is the ACTION family's I/O slot. Disjoint from g_moves/g_snaps
// (the action call reads a move / writes an export into g_io while g_moves and
// g_snaps hold the menu / snapshots - all three live at once, at distinct
// offsets), and aliased over the replay family (dead during any replay call).
_Static_assert(IO_CAP <= RULES_OVL_ACTION_END - RULES_OVL_IO_OFF, "g_io overflows its overlay slot");
#define g_io ((unsigned char *)(rules_overlay + RULES_OVL_IO_OFF))
#else
static unsigned char g_io[IO_CAP];
#endif
static Game g_game;

// Snapshots never carry logs (animation game_states are log-stripped
// downstream), so each slot stores only the Game prefix up to num_logs -
// ~2.5 KB instead of the full log-laden struct.
#define GAME_PREFIX_SIZE (__builtin_offsetof(Game, num_logs))
typedef struct { _Alignas(8) unsigned char bytes[GAME_PREFIX_SIZE]; } SnapSlot;
#ifdef CD_RULES_OVERLAY
// R1: g_snaps is the ACTION family's snapshot slot. 16-aligned arena +
// 8-aligned offset satisfies SnapSlot's _Alignas(8).
_Static_assert(_Alignof(SnapSlot) <= 16, "SnapSlot alignment exceeds the arena's 16");
_Static_assert(RULES_OVL_SNAPS_OFF % _Alignof(SnapSlot) == 0, "g_snaps offset misaligned");
_Static_assert(sizeof(SnapSlot) * SNAP_RING_CAP <= RULES_OVL_IO_OFF - RULES_OVL_SNAPS_OFF, "g_snaps overflows its overlay slot");
#define g_snaps ((SnapSlot *)(rules_overlay + RULES_OVL_SNAPS_OFF))
#else
static SnapSlot g_snaps[SNAP_RING_CAP];
#endif
static int g_snap_tags[SNAP_RING_CAP];
static int g_snap_aux[SNAP_RING_CAP];
static int g_n_snaps;
#ifdef CD_RULES_OVERLAY
// R1: g_moves is the ACTION family's menu slot (offset 0).
_Static_assert(_Alignof(LegalMoves) <= 16, "LegalMoves alignment exceeds the arena's 16");
_Static_assert(sizeof(LegalMoves) <= RULES_OVL_SNAPS_OFF - RULES_OVL_MOVES_OFF, "g_moves overflows its overlay slot");
#define g_moves (*(LegalMoves *)(rules_overlay + RULES_OVL_MOVES_OFF))
#else
static LegalMoves g_moves;
#endif
// TS writes 1-byte wire cards into the raw buffers; decode_in_cards
// converts (and clamps) them to the in-memory Card.
static unsigned char g_in_raw_a[MAX_IN_CARDS];
static unsigned char g_in_raw_b[MAX_IN_CARDS];
static Card g_in_a[MAX_IN_CARDS];   // action cards (attack/pass/cover covers)
static Card g_in_b[MAX_IN_CARDS];   // cover: the attack cards being covered

static void decode_in_cards(const unsigned char *raw, Card *out, int n) {
    if (n > MAX_IN_CARDS) n = MAX_IN_CARDS;
    for (int i = 0; i < n; i++) out[i] = card_from_wire_state(raw[i]);
}

unsigned char *wasm_io_ptr(void) { return g_io; }
int wasm_io_cap(void) { return IO_CAP; }

// For sibling bridge units (wasm_bots_api.c) that operate on the same
// working game and scratch move list (LegalMoves is ~330KB at the wasm
// build's MAX_LEGAL_MOVES=4096 with 1-byte cards - still not worth a
// second copy).
Game *wasm_game_ptr_internal(void) { return &g_game; }
LegalMoves *wasm_moves_ptr_internal(void) { return &g_moves; }

// Card input buffers: TS writes 1-byte wire cards.
unsigned char *wasm_cards_a_ptr(void) { return g_in_raw_a; }
unsigned char *wasm_cards_b_ptr(void) { return g_in_raw_b; }

// ---------- snapshot hook -------------------------------------------------

static void snap_cb(const Game *g, int tag, int aux) {
    if (g_n_snaps >= SNAP_RING_CAP) return;
    memcpy(g_snaps[g_n_snaps].bytes, g, GAME_PREFIX_SIZE);
    g_snap_tags[g_n_snaps] = tag;
    g_snap_aux[g_n_snaps] = aux;
    g_n_snaps++;
}

// Production configuration: install the snapshot hook. (The deck-size rule
// is hardcoded in card.h - one rule for every deployment.)
void wasm_init(void) {
    engine_snap_hook = snap_cb;
}

void wasm_set_seed(unsigned int s) { game_set_seed(s); }

// Wide, reproducible deal seed: the caller writes 32 bytes (two 128-bit lanes)
// to the front of the io buffer, then calls this before wasm_start_game(). The
// deal then reaches the whole 52!/36! space and replays exactly from the same
// bytes. A following wasm_set_seed() reverts to the legacy 32-bit LCG (so the
// per-move reseed the engine already does keeps mid-game behavior unchanged).
void wasm_set_deal_seed_bytes(void) { game_set_deal_seed_bytes(g_io, 32); }

int wasm_reject_reason(void) { return engine_last_reject; }

// ---------- state (de)serialization ---------------------------------------
//
// Layout (little-endian, byte-packed):
//   u8  status            u8  num_players     i8 power_suit
//   i8  first_attacker    i8  defender
//   u16 discard_pile_length
//   u8  flags             u8 flipped wire-card
//       (flags: bit 0 has_flipped, bits 1-7 Game.rules - view.h STATE_FLAGS_AT)
//   u32 good_players_mask u8 has_good_timestamp
//   u16 deck_count,   deck_count x u8 wire-card
//   u8  num_battles,  num_battles x (u8 attack, u8 defense; 0xFF = uncovered)
//   num_players x (u8 status, u8 awaiting, u8 hand_count, hand x u8 wire-card)
//   u8  num_eliminated, num_eliminated x i8
//
// The implementation is single-sourced in src/view.c (state_put/state_get),
// shared with the guards bridge and with the per-viewer MASKED serialization
// (deck + other hands as WIRE_CARD_HIDDEN) the packed wire pipeline uses.

static int put_state(const Game *g, unsigned char *p) {
    return state_put(g, VIEW_UNMASKED, p);
}

// TS -> C: parse the IO buffer into the working game - if the kernel accepts
// it. Returns GAME_VALID (0), or a negative GAME_INVALID_* reason (game.h
// game_validate) with the working game left exactly as it was: a host never
// gets to install a state the kernel could not have produced.
//
// `len` is how many bytes the host wrote at wasm_io_ptr(), not the IO buffer's
// size. The kernel measures the payload against it before reading a field, so
// a truncated or over-long state is refused (GAME_INVALID_COUNT) instead of
// read past its end. A host that forgets the argument gets len 0, which every
// state fails - loudly, which is the point.
//
// `masked` is the host saying what it is handing over. 0: a whole game, every
// card real (the server's own Game). 1: one seat's knowledge, with placeholder
// cards standing in for the deck and the hands it cannot see (the replay
// oracle's deliberation state) - their identities are then not judged, and
// everything face-up still is. A host that omits the argument gets 0.
//
// The ephemeral IO format
// carries no deterministic_deck flag (only the durable blob does), so reset it:
// a fresh deal has start_game set it, and a legacy game draws at random. This
// also stops a reused engine instance inheriting a prior game's flag. The
// caller re-asserts the flag right after (wasm_set_deterministic_deck) for a
// seed-dealt game - otherwise the bot path (which imports rather than
// deserializes) would draw at random mid-game and diverge from the deal seed.
int wasm_import_state(int len, int masked) {
    const int r = state_import(&g_game, g_io, len, masked ? 1 : 0);
    if (r != GAME_VALID) return r;
    g_game.deterministic_deck = false;
    return GAME_VALID;
}

// Re-assert the deterministic-deck flag after wasm_import_state. Seed-dealt
// games carry it in the durable blob (wasm_state_deserialize restores it), but
// the transient import path drops it - so the bot loop, which marshals a JS
// Game instead of loading the blob, must set it back or every mid-game refill
// pops a RANDOM card and the game stops being reproducible from its deal seed.
void wasm_set_deterministic_deck(int on) { g_game.deterministic_deck = on != 0; }

// SECRET base folded into every mid-game RNG seed below. Set from the 32-byte
// deal seed (games.game_seed) - which is SERVER-ONLY: it never appears on
// PublicGame, in the state blob, or in any per-viewer view, so a player can't
// see or reconstruct it. This is the whole point of the field: the mid-game bot
// RNG must NOT be derivable from anything on the public board, or a source-code
// holder could recompute octogen's world-sampling seed and predict its every
// move. The visible-state bytes below add decorrelation only; the base is what
// makes the seed unpredictable. 0 only under the test seed-source or a game with
// no deal seed (legacy) - live games always set it (see wasm_set_rng_base).
static uint32_t g_rng_base = 0u;
void wasm_set_rng_base(uint32_t base) { g_rng_base = base; }
// For sibling bridge units (wasm_table_api.c): the base the table seeds its
// mid-game draws from, so a table action draws exactly what wasm_apply_action
// after wasm_seed_rng_deterministic does.
uint32_t wasm_rng_base_internal(void) { return g_rng_base; }

// Per-decision RNG seed for the Monte-Carlo bots' world sampling (salt
// 0x9E3779B9) and the mid-game deal RNG (salt 0). It folds the SECRET
// g_rng_base = rngBaseFromSeed(game_seed) together with the salt and the PUBLIC
// board state, then avalanches.
//
// Two properties have to hold at once:
//   * Reproducible - every hashed term must be recoverable from a shared replay,
//     so a recorded game replays bit-exactly. That rules OUT num_logs (records
//     the codec may drop) and the ORDERED hands / face-down deck (hidden from
//     the bot, and a player can even permute their own hand via rearrange).
//   * Varying per decision - g_rng_base alone is constant for the whole game, so
//     seeding from it only would hand every decision the same RNG. So we mix in
//     the PUBLIC, replay-recoverable state that moves every turn: the cards on
//     the table, each seat's hand COUNT (not its cards), deck/discard sizes and
//     the defender. All face-up, all a pure function of the move log.
// Unpredictability still rests entirely on the secret seed - the public terms
// are known to everyone, but without g_rng_base they can't yield the stream.
static uint32_t state_fnv(uint32_t salt) {
    return game_state_seed(&g_game, g_rng_base, salt);
}

// Seed the mid-game LCG (game_random) deterministically from the CURRENT game
// state instead of Math.random. Called once per move-application in place of
// the old per-move crypto/Math.random reseed: the state is itself a pure
// function of the deal seed, so mixing it here keeps every game_random draw
// (legacy random deals, bot tie-breaks) reproducible - the whole game replays
// from the deal seed alone. Seed-dealt games pop the pre-shuffled deck and
// never consume this, but it costs nothing and covers the legacy path too.
void wasm_seed_rng_deterministic(void) { game_rng_set(state_fnv(GAME_SEED_SALT_DRAW)); }

// Seed the STRATEGY LCG (random_strategy_random, consumed by the Monte-Carlo
// bots' rollout opponent models) deterministically from state, replacing the
// per-decision Math.random the bot bridge used to draw. This is what made even
// octogen non-reproducible: its rollouts sample opponent replies off this
// stream, so a fresh Math.random each decision meant a different choice from
// identical state. A distinct salt keeps it decorrelated from the draw stream.
void wasm_set_strategy_seed_deterministic(void) {
    random_strategy_set_seed(state_fnv(GAME_SEED_SALT_STRATEGY));
}

// Debug/analysis hook: the strategy seed that would be chosen for the CURRENT
// marshaled state. Lets a harness confirm the seed varies per decision (public
// board changes) yet reproduces across a replay. Behavior-neutral.
uint32_t wasm_strategy_seed_probe(void) { return state_fnv(GAME_SEED_SALT_STRATEGY); }

// C -> TS: serialize the working game into the IO buffer; returns length.
int wasm_export_state(void) { return put_state(&g_game, g_io); }

// ---------- durable state codec (versioned) -------------------------------
//
// The format, the layout and the reasoning are view.h's (state_blob_put /
// state_blob_load): the table layer writes the same column through the same
// pair, so these entries are the bridge and nothing more.

// Serialize the working game into g_io as a versioned durable blob; returns
// the byte length (>=2), or STATE_BLOB_E_RULES for a game whose rules the
// format written cannot carry (view.h).
int wasm_state_serialize(void) {
    return state_blob_put(&g_game, 0, g_io);
}

// Load a durable blob (already written into g_io) back into the working game.
// Returns 1 on success, 0 if the leading version byte is one this kernel does
// not understand (caller must treat as unreadable, never as an empty game), or
// a negative GAME_INVALID_* reason if the state inside is one the kernel
// refuses (game.h game_validate) - the working game is then left as it was.
int wasm_state_deserialize(int len) {
    return state_blob_load(&g_game, g_io, len, 0);
}

// The version this kernel writes - lets the TS bridge assert the embed it
// loaded matches the format it expects without hardcoding the number twice.
int wasm_state_format_version(void) { return STATE_BLOB_FORMAT; }

// The Game layout this module was compiled against: tools/structgen's hash of
// tools/structgen/specs/game_layout.args under THIS build's flags, which
// c/Makefile computes and passes in (see "Layout hash" there). The TS hosts
// compare it with LAYOUT_HASH in sdk/ts/gen/game_layout.<build>.ts once per
// instance and refuse the module on a mismatch. Defined once, here, for every
// module that links this file - oracle-mt.wasm included (wasm_oracle_mt.c would
// be a duplicate symbol) - and a build that forgets the flag does not compile.
#ifndef SG_LAYOUT_HASH
#error "SG_LAYOUT_HASH is not set: build the wasm modules through c/Makefile"
#endif
uint32_t wasm_layout_hash(void) { return SG_LAYOUT_HASH; }

// ---------- logs -----------------------------------------------------------
// u16 num_logs, then per log: i8 type, i8 player_idx, i8 defender_index,
// u8 num_pairs, num_pairs x (u8 primary, u8 target) - wire cards, target
// 0xFF when the pair has none, 0xFE for the hidden card

// Pre-action flip state, captured by begin_action for the DRAW-privacy rule.
static Card g_pre_flip;
static int g_pre_has_flip;

// `start` is the log index this action's records begin at. It is 0 for every
// path that marshals fresh (wasm_import_state zeroes the log), and non-zero
// only for the bot drive, whose belief bots need the SESSION log resident
// while they choose - see wasm_export_logs_masked_from.
static int export_logs(int mask_draws, int start) {
    // The DRAW-privacy rule (the TS appendLogs convention, now kernel-side) is
    // view.c log_record_put's: drawn-card identities are hidden EXCEPT the
    // flipped trump, whose draw is public. "The flip was drawn during this
    // action" is the pre-action has_flipped (captured by begin_action) going false.
    if (start < 0 || start > g_game.num_logs) start = 0;
    const int n = g_game.num_logs - start;
    unsigned char *q = g_io;
    *q++ = (unsigned char)(n & 0xff);
    *q++ = (unsigned char)((n >> 8) & 0xff);
    for (int i = start; i < g_game.num_logs; i++)
        q += log_record_put(&g_game.logs[i], mask_draws, g_pre_has_flip, g_pre_flip,
                            g_game.has_flipped, q);
    return (int)(q - g_io);
}

int wasm_export_logs(void) { return export_logs(0, 0); }

// The durable/session variant: what leaves the kernel for storage and (via
// the packed session log) other players' belief imports - draw identities
// masked per the rule above. See docs/PACKED_WIRE_CUTOVER.md.
int wasm_export_logs_masked(void) { return export_logs(1, 0); }

// The same, from a log offset - the bot drive's export (F2). A cycle whose
// bots read the session log has that whole log resident BENEATH the records
// the cycle just wrote (wasm_import_logs loads it into the game's log store,
// which is where the belief bots read it from), so exporting from zero would
// hand the commit the entire session again, to be appended a second time.
// `start` is where the drive began - see wasm_bot_drive_log_start.
int wasm_export_logs_masked_from(int start) { return export_logs(1, start); }

// ---------- snapshots -------------------------------------------------------

int wasm_snap_count(void) { return g_n_snaps; }
int wasm_snap_tag(int i) { return g_snap_tags[i]; }
int wasm_snap_aux(int i) { return g_snap_aux[i]; }
int wasm_export_snapshot(int i) {
    // put_state only reads prefix fields, which is exactly what a slot holds.
    return put_state((const Game *)(const void *)g_snaps[i].bytes, g_io);
}

// ---------- actions ---------------------------------------------------------
// Cards are read from the input buffers (byte pairs). Each action clears the
// snapshot buffer first; on success the caller reads state/logs/snapshots.

static void begin_action(void) {
    g_n_snaps = 0;
    g_pre_flip = g_game.flipped;
    g_pre_has_flip = g_game.has_flipped ? 1 : 0;
}

// The same reset, for the bot bridge (wasm_bot_drive). bot_drive applies a
// whole CYCLE through the kernel's handlers directly rather than through
// wasm_apply_action, so it opens the action ONCE and lets the snapshots, the
// logs and the pre-action flip span the bundle. That accumulation is the
// point: the cycle's products are what the server commits and broadcasts, so
// they must be read as one action's worth (docs/C_CORE_CONSOLIDATION.md F2).
void wasm_begin_action_internal(void) { begin_action(); }

int wasm_start_game(void) {
    begin_action();
    start_game(&g_game);
    return 1;
}

int wasm_attack(int player_idx, int n_cards) {
    begin_action();
    decode_in_cards(g_in_raw_a, g_in_a, n_cards);
    return handle_attack(&g_game, player_idx, g_in_a, n_cards) ? 1 : 0;
}

int wasm_cover(int player_idx, int n) {
    begin_action();
    decode_in_cards(g_in_raw_a, g_in_a, n);
    decode_in_cards(g_in_raw_b, g_in_b, n);
    return handle_cover(&g_game, player_idx, g_in_a, g_in_b, n) ? 1 : 0;
}

int wasm_pass(int player_idx, int n_cards) {
    begin_action();
    decode_in_cards(g_in_raw_a, g_in_a, n_cards);
    return handle_pass(&g_game, player_idx, g_in_a, n_cards) ? 1 : 0;
}

int wasm_pickup(int player_idx) {
    begin_action();
    return handle_pickup(&g_game, player_idx) ? 1 : 0;
}

int wasm_good(int player_idx) {
    begin_action();
    return handle_good(&g_game, player_idx) ? 1 : 0;
}

int wasm_transition(void) {
    begin_action();
    engine_run_round_transition(&g_game);
    return 1;
}

int wasm_refill(void) {
    begin_action();
    engine_run_refill(&g_game);
    return 1;
}

// Seat KINDS, from g_io: one byte per seat, STRATEGY_KEY_HUMAN (-1, as 0xff)
// for a human or a STRAT_* brain id for a bot. Lives here rather than in the
// bots bridge because game_human_mask below reads exactly this, and the rules
// module answers that question too.
//
// espresso's opponent modelling also branches on a specific brain (whether an
// opponent is `random`), which is why the exact id and not just the sign is
// carried on the paths that run a strategy.
void wasm_import_strategy_keys(void) {
    const unsigned char *q = g_io;
    for (int i = 0; i < g_game.num_players; i++)
        g_game.players[i].strategy_key = (int8_t)q[i];
}

// The seats a host must NOT drive (game.c game_human_mask), so no host rebuilds
// an is_ai mask of its own before every call that wants one.
//
// Only meaningful once the seat kinds have been said. They do NOT ride in the
// state wire: state_put/state_get are shared with the DURABLE blob
// (wasm_state_serialize is [version][flag][state_put(VIEW_UNMASKED)]), so a
// byte added for this would silently re-interpret every persisted games.state
// row while the version byte still read 2. The kinds arrive through
// wasm_import_strategy_keys instead, which marshalGame now always calls - see
// the note there. A blob-loaded game has no kinds at all (identity stays with
// the caller, game.h), so its host still owns the question.
int wasm_human_mask(void) {
    return (int)game_human_mask(&g_game);
}

// The rematch reset: a finished game back to its lobby, ready to deal again.
// `bot_mask` is the seats that come back READY (the bots); everyone else lands
// IDLE and has to ready up.
//
// game_reset_to_lobby has existed in game.c since the lifecycle consolidation
// and had no caller outside a unit test, while BOTH hosts kept their own copy:
// the edge's handleContinue and the browser's optimistic resetToLobby, which
// had to "match byte-for-byte on the public fields or the user sees a snap".
// They already disagreed - handleContinue left good_players/good_timestamp set
// and leaned on the next deal to clear them, so between the reset and the deal
// the lobby showed stale good state. The kernel settles that the client's way,
// and adopting it here is that fix.
//
// No begin_action(): a reset emits no logs and no animation snapshots. It is
// the board being put away, not a move.
int wasm_reset_to_lobby(int bot_mask) {
    game_reset_to_lobby(&g_game, (unsigned int)bot_mask);
    return 1;
}

// ---------- packed wire pipeline (docs/PACKED_WIRE_CUTOVER.md) --------------
//
// One call per client move: the action-wire bytes the browser validated with
// guards.wasm are applied verbatim. Wire is read from input buffer A (it is
// at most 2 + 2*AWIRE_MAX_CARDS = 58 bytes, well under MAX_IN_CARDS).
// Returns 1 applied, 0 rejected (wasm_reject_reason), -1 malformed wire.
int wasm_apply_action(int player_idx, int wire_len) {
    AwireAction a;
    if (wire_len < 0 || wire_len > MAX_IN_CARDS) return -1;
    if (!awire_decode(g_in_raw_a, wire_len, &a)) return -1;
    begin_action();
    switch (a.kind) {
        case AWIRE_ATTACK: return handle_attack(&g_game, player_idx, a.cards, a.n) ? 1 : 0;
        case AWIRE_COVER:  return handle_cover(&g_game, player_idx, a.cards, a.attacks, a.n) ? 1 : 0;
        case AWIRE_PASS:   return handle_pass(&g_game, player_idx, a.cards, a.n) ? 1 : 0;
        case AWIRE_PICKUP: return handle_pickup(&g_game, player_idx) ? 1 : 0;
        case AWIRE_GOOD:   return handle_good(&g_game, player_idx) ? 1 : 0;
        default:           return -1; // unreachable: awire_decode bounds kind
    }
}

// The TS check_win_sync, kernel-side: if the game is done, set GAME_OVER and
// park every seat (bots READY, humans IDLE - the ai seat bitmask is the one
// fact the kernel doesn't model). Returns the fool's seat, or -1 if the game
// is not over (state untouched).
int wasm_finalize_win(unsigned int ai_mask) {
    const int fool = game_done(&g_game);
    if (fool < 0) return -1;
    g_game.status = GAME_STATUS_GAME_OVER;
    for (int i = 0; i < g_game.num_players; i++) {
        g_game.players[i].status = (ai_mask >> i) & 1u
            ? PLAYER_STATUS_READY : PLAYER_STATUS_IDLE;
    }
    return fool;
}

// Per-viewer masked view blob: [VIEW_FORMAT_VERSION | viewer | masked
// put_state]. viewer < 0 = spectator. This is what get_game returns and what
// the client imports - other hands and the deck never leave as real bytes.
int wasm_view_serialize(int viewer) {
    const int v = viewer < 0 ? VIEW_SPECTATOR : viewer;
    g_io[0] = (unsigned char)VIEW_FORMAT_VERSION;
    g_io[1] = viewer < 0 ? 0xFFu : (unsigned char)viewer;
    return 2 + state_put(&g_game, v, g_io + 2);
}

// Per-viewer packed animation sequence (evwire.h) for the resident game's
// last action: hook snapshots + fresh logs + the current (post-action,
// post-finalize) state as the trailer. Call once per recipient BEFORE any
// other kernel call disturbs the snapshots. Returns length, or -1 on
// overflow/corrupt input.
//
// `log_start` is where the action's fresh logs begin; everything below it is
// resident session log the emitter must not read as new (see
// wasm_export_logs_masked_from). Zero for every path that marshals fresh.
int wasm_events_serialize_from(int viewer, int actor, int append_final_transition,
                               int log_start) {
    EvSnap refs[SNAP_RING_CAP];
    for (int i = 0; i < g_n_snaps; i++) {
        // put_state/state_put only read prefix fields, which is exactly what
        // a snapshot slot holds.
        refs[i].g = (const Game *)(const void *)g_snaps[i].bytes;
        refs[i].tag = g_snap_tags[i];
        refs[i].aux = g_snap_aux[i];
    }
    if (log_start < 0 || log_start > g_game.num_logs) log_start = 0;
    return evwire_serialize(refs, g_n_snaps, g_game.logs + log_start,
                            g_game.num_logs - log_start,
                            &g_game, viewer < 0 ? VIEW_SPECTATOR : viewer,
                            actor, append_final_transition, g_io, IO_CAP);
}

int wasm_events_serialize(int viewer, int actor, int append_final_transition) {
    return wasm_events_serialize_from(viewer, actor, append_final_transition, 0);
}

// Reorder a seat's own hand to the given index order - the rearrange-hand
// meta action, validated in the kernel. Indices are single bytes in input
// buffer A. The permutation check is load-bearing (see actions/rearrange.ts
// history): n must equal the hand count, every index in range, and each used
// EXACTLY once - otherwise a hostile payload mints duplicate cards. Returns
// 1 applied, 0 invalid (state untouched).
int wasm_rearrange_hand(int seat, int n) {
    return game_rearrange_hand(&g_game, seat, g_in_raw_a, n);
}

// ---------- queries ----------------------------------------------------------

int wasm_game_done(void) { return game_done(&g_game); }
int wasm_should_act(int idx) { return should_bot_act(&g_game, idx) ? 1 : 0; }
int wasm_next_player(int cur) { return get_next_player_index(&g_game, cur); }
int wasm_can_cover(int as, int av, int ds, int dv, int power_suit) {
    Card a = { (int8_t)as, (int8_t)av }, d = { (int8_t)ds, (int8_t)dv };
    return can_cover(a, d, power_suit) ? 1 : 0;
}

// ---------- replay codec ----------------------------------------------------
// One call per direction, through a dedicated buffer (the shared g_io is
// sized for single-action exports; a whole decoded game stream is bigger).
// Formats are documented in replay.h. Both entries return bytes written or
// a negative REPLAY_E* code; wasm_replay_error_detail carries the message
// parameter (unsupported version, menu size).
//
// Capacity is a build parameter (default keeps the historical 2MB). The
// wasm builds pass 32KB: decode's worst CONFORMING stream is ~50KB only for
// a hypothetical every-log-has-52-pairs monster; the measured worst over
// 28K engine games (tests/l1_measure.c) is 3,117B in and a 200B blob out,
// so 32KB is >10x the observed ceiling. Oversized inputs - hostile integers
// or absurd streams - fail with a clean REPLAY_ECAP instead of unbounded
// growth (the TS reference grew an unbounded array there), and encode/
// decode stay self-consistent: any stream encode accepts fits decode's
// output bound by construction (they share this buffer).

#ifndef WASM_REPLAY_IO_CAP
#define WASM_REPLAY_IO_CAP (2 * 1024 * 1024)
#endif
#define REPLAY_IO_CAP WASM_REPLAY_IO_CAP
#ifdef CD_WASM_OVERLAY
// M8: g_replay_io aliases into solve_ws (see wasm_overlay.h). The TS bridge
// writes the input at this pointer, calls the codec, then reads the output -
// all within one synchronous function, so no choose call interleaves. cd_overlay
// is a fixed static address, so a cached wasm_replay_io_ptr() stays valid.
_Static_assert(REPLAY_IO_CAP <= CD_OVL_END - CD_OVL_IO_OFF, "g_replay_io overflows its overlay slot");
#define g_replay_io ((unsigned char *)(cd_overlay + CD_OVL_IO_OFF))
#elif defined(CD_RULES_OVERLAY)
// R1: g_replay_io is the REPLAY family's blob slot, aliased over the action
// family. rules_overlay is a fixed static address, so a cached
// wasm_replay_io_ptr() stays valid across calls.
_Static_assert(REPLAY_IO_CAP <= RULES_OVL_REPLAY_END - RULES_OVL_REPLAY_IO_OFF, "g_replay_io overflows its overlay slot");
#define g_replay_io ((unsigned char *)(rules_overlay + RULES_OVL_REPLAY_IO_OFF))
#else
static unsigned char g_replay_io[REPLAY_IO_CAP];
#endif

unsigned char *wasm_replay_io_ptr(void) { return g_replay_io; }
int wasm_replay_io_cap(void) { return REPLAY_IO_CAP; }

// In-place: input is fully consumed before any output byte is written.
// Defense in depth: the TS bridge already checks lengths against
// wasm_replay_io_cap, but a hostile/stale caller must get a clean error,
// never reads past the replay buffer.
int wasm_replay_decode(int in_len) {
    if (in_len < 0 || in_len > REPLAY_IO_CAP) return -REPLAY_ECAP;
    return replay_decode(g_replay_io, in_len, g_replay_io, REPLAY_IO_CAP);
}
// The marshalled encoder (replay.h): an action stream plus the real hidden
// cards. Same in-place contract.
int wasm_replay_encode_v6(int in_len) {
    if (in_len < 0 || in_len > REPLAY_IO_CAP) return -REPLAY_ECAP;
    return replay_encode_v6(g_replay_io, in_len, g_replay_io, REPLAY_IO_CAP);
}

int wasm_replay_error_detail(void) { return replay_last_error_detail(); }
// The same refusal as a struct (replay.h ReplayError), so a host words the
// message from named fields instead of unpacking log_type << 16 | menu.
const void *wasm_replay_error_ptr(void) { return replay_last_error(); }

// ---------- FMSG: the iMessage envelope (src/msg_wire.h) -----------------
//
// The envelope crosses in g_replay_io, NOT g_io, and that is load-bearing. In
// rules.wasm (-DCD_RULES_OVERLAY) the replay scratch family aliases OVER the
// action family, legal because "replay encode/decode vs action/menu are
// top-level exports that never nest" (see the Makefile). An FMSG call IS a
// replay call - its body is a v6 code and decoding it runs the bignum scratch -
// so an envelope parked in g_io would be clobbered mid-decode, and MsgEnvelope
// BORROWS the bytes it decoded from. Keeping it in the replay family's own
// buffer keeps that invariant true.
//
// The web READS an envelope and never writes one: sealing, rebasing and Rule P
// are the phone's, through ios_api_msg.c, and are tested natively
// (tests/msg_wire_test.c). So decode is the one FMSG entry a shipped module
// exports; wasm_msg_rematch below is the test build's.
//
// A decode replays into the resident g_game.

// THE HEADER, as a struct (msg_wire.h MsgHeader), not a byte string. A bridge
// reads and writes it through the readers and writers tools/structgen generates
// from that declaration, so the offsets exist once, in C, and a host cannot
// mis-parse them. It used to be a hand-packed blob with a fixed join slot,
// written here and re-read field by field in sdk/ts/wasm/bots.ts.
//
// It lives OUTSIDE g_replay_io deliberately: a decode leaves the envelope's own
// bytes in that buffer, which the next call is free to overwrite.
static MsgHeader g_msg_header;

// The address of that header. The whole ABI: the caller reads it after a decode.
void *wasm_msg_header_ptr(void) { return &g_msg_header; }

// in:  g_replay_io[0 .. in_len) = the envelope bytes
// out: the header at wasm_msg_header_ptr (MsgHeader)
// Replays the chain into g_game on the way. Returns MSG_EOK, or -MSG_E* negated
// into the same space the replay errors use (see the TS bridge).
int wasm_msg_decode(int in_len) {
    if (in_len < 0 || in_len > REPLAY_IO_CAP) return MSG_ECAP;
    MsgEnvelope e;
    const int rc = msg_decode(g_replay_io, in_len, &e);
    if (rc != MSG_EOK) return rc;

    // Digest the envelope BEFORE the blob overwrites it - Rule P needs it, and
    // these bytes are about to stop existing.
    uint8_t digest[SHA256_DIGEST_LEN];
    msg_digest(g_replay_io, in_len, digest);

    const int rrc = msg_replay(&e, &g_game);   // e borrows g_replay_io; still intact
    if (rrc != MSG_EOK) return rrc;

    // Safe now: replay is done with the borrowed body.
    // The header, at wasm_msg_header_ptr. `actions` is dropped on the way: it
    // borrows g_replay_io, which the next call is free to overwrite.
    g_msg_header.e = e;
    g_msg_header.e.actions = 0;
    g_msg_header.e.actions_len = 0;
    memcpy(g_msg_header.digest, digest, SHA256_DIGEST_LEN);
    return MSG_EOK;
}

// THE REMATCH LOBBY (msg_wire.h msg_rematch_lobby) for the FINISHED envelope in
// g_replay_io[0, in_len): the same game dealt again, written back into
// g_replay_io. Returns its length or a negative MSG_E*. Adopts nothing - the
// resident game is untouched; decode the answer to put it on screen. The same
// C the phone's fio_msg_rematch calls, exported from the TEST build only
// (Makefile WASM_BOTS_UNSHIPPED_API) so e2e/msg_rematch.test.ts can hold the
// wasm kernel to the bytes the native one is pinned to.
// `tapped_at_ms` crosses as a double: a JS number holds every millisecond
// timestamp exactly, and wasm32 has no 64-bit integer a JS caller can pass
// without BigInt.
int wasm_msg_rematch(int in_len, double tapped_at_ms, int creator_seat) {
    // The scratch Game the lobby is replayed and dealt in (a Game is far too
    // big for a wasm stack). Not held across a return.
    static Game g_msg_scratch;
    if (in_len < 0 || in_len > REPLAY_IO_CAP) return MSG_ECAP;
    // The answer is written where the question was, so the question is copied
    // out first: the lobby is built while the finished chain is still read.
    static unsigned char finished[REPLAY_IO_CAP];
    memcpy(finished, g_replay_io, (size_t)in_len);
    if (!(tapped_at_ms >= 0.0) || tapped_at_ms > 9007199254740991.0) return MSG_ESHORT;
    return msg_rematch_lobby(finished, in_len, (uint64_t)tapped_at_ms, creator_seat,
                             g_replay_io, REPLAY_IO_CAP, &g_msg_scratch);
}

// ---------- animation core (anim_plan.h) --------------------------------------
//
// The platform-independent animation policy, exported so the web pure modules
// (src/state/*) delegate here instead of re-deriving it - the same "one kernel
// behind every host" argument FMSG makes above. Cards cross as 1-byte wire cards
// (wire.h: 0..51 real, 0xFE hidden, 0xFF none). These are pure functions over
// g_io - no resident game, no session log - so they run in any module that
// exports them (the web loads bots.wasm, which is where they ship).

// The version gate (anim_should_drop_stale). Pure scalars; the
// two has_* flags model TS null (a replay sequence has no version -> never gated).
int wasm_anim_should_drop_stale(int has_last, int last, int has_incoming, int incoming) {
    return anim_should_drop_stale(has_last, last, has_incoming, incoming);
}

// anim_stale_optimistic_on_table, asked as a value
// (anim_plan.h AnimStaleAsk): the host writes the three card lists through the
// generated writer and reads the release indices through the generated reader
// (sdk/ts/gen/view_layout.bots.ts), so the layout of neither is written down
// anywhere but the header. Returns the release count, or a negative ANIM_E*.
static AnimStaleAsk     g_anim_stale_ask;
static AnimStaleRelease g_anim_stale_release;
AnimStaleAsk *wasm_anim_stale_ask_ptr(void)         { return &g_anim_stale_ask; }
AnimStaleRelease *wasm_anim_stale_release_ptr(void) { return &g_anim_stale_release; }
int wasm_anim_stale_optimistic(void) {
    return anim_stale_optimistic_ask(&g_anim_stale_ask, &g_anim_stale_release);
}

// THE REVERSAL'S ORDER (anim_plan.h anim_reversal_order), for a host that
// already holds its verdicts - which every SERVER-transport host does, since
// anim_conflict_verdict is the only entry that asks the AnimServerHope.
//
// This is the entry that REPLACES a web rule rather than being reconciled with
// one. AnimationContext had four branches deciding where a doomed card's return
// flight went relative to the arriving stream's own events - before a magic
// transition, before the first attack "for parallel visual effect", before a
// pickup, or first - and the kernel's rule is simply that the board reverses
// what it must before it plays anything else, last group first.
//
// Asked as a value (anim_plan.h AnimReversalAsk) and answered in the
// AnimConflictPlan itself, read where it lies with its counts: the generated
// writer and reader are the only things that know either layout.
// Returns the step count, or a negative ANIM_E*.
static AnimReversalAsk  g_anim_reversal_ask;
static AnimConflictPlan g_anim_reversal_plan;
AnimReversalAsk *wasm_anim_reversal_ask_ptr(void)   { return &g_anim_reversal_ask; }
AnimConflictPlan *wasm_anim_reversal_plan_ptr(void) { return &g_anim_reversal_plan; }
int wasm_anim_reversal_order(void) {
    return anim_reversal_order_ask(&g_anim_reversal_ask, &g_anim_reversal_plan);
}

// THE HAND'S ORDER (anim_plan.h anim_hand_laid_out_masked): the array a fan is
// actually given, backs and all.
//
// THREE IMPLEMENTATIONS OF THIS SHIPPED ON THE WEB and one in C, which is the
// hand-order divergence the iMessage work found: a replay reconciled its
// face-down slots by COUNT and the live hand reconciled by KEY, so the same
// rearrangement could come out two different ways depending on which screen was
// drawing it. One door, so they cannot.
//
// Asked as a value, in CARDS (anim_plan.h AnimHandAsk): the hand, the viewer's
// order and the deferred set go in through the generated writer - the set as
// the one 64-bit field it is, where this entry used to take it as two 32-bit
// arguments - and the laid-out hand comes back through the generated reader.
// The byte alphabet the rule speaks (a dense id, or ANIM_TABLE_UNKNOWN for a
// back) is translated in anim_hand_laid_out_ask, so no host carries it.
// Returns the count laid out, or a negative ANIM_E*.
static AnimHandAsk  g_anim_hand_ask;
static AnimHandLaid g_anim_hand_laid;
AnimHandAsk *wasm_anim_hand_ask_ptr(void)   { return &g_anim_hand_ask; }
AnimHandLaid *wasm_anim_hand_laid_ptr(void) { return &g_anim_hand_laid; }
int wasm_anim_hand_laid_out(void) {
    return anim_hand_laid_out_ask(&g_anim_hand_ask, &g_anim_hand_laid);
}

// THE TRANSPORT (anim_plan.h), said once by the host. Every wasm host is the
// server shape - a browser, an edge function, a test harness driving either -
// but the kernel will not assume that, so bots.ts states it at module init.
// The FMSG suites drive CHAIN behaviour from this same module, so it is a
// setter and not a compile-time constant.
int wasm_anim_set_transport(int transport) { return anim_set_transport(transport); }

// ---------- the plan and the frame ---------------------------------------------
//
// THE ENTRIES A HOST WITH A FRAME LOOP NEEDS, and the reason they cross
// here and not earlier: the web's plan bridge was DELETED in 0cda5e85 for being
// a bridge with no caller, so the builders wait for the consumer that asks them
// (docs/C_GAME_SHAPE_MIGRATION.md Phase 9). iOS reaches the same C natively
// through fio_anim_plan / fio_anim_beats. The beats have no wasm entry: the web
// plays a plan's steps, and the beat rules are pinned on anim_build_beats itself
// (c/tests).
//
// THE STRUCTS ARE READ WHERE THEY LIE, like wasm_legal_moves_ptr: a plan is
// 15,856 bytes and a host that copied it through g_io once per frame would
// spend more time marshalling than animating. The caller reads them through the
// generated accessors (sdk/ts/gen/anim.bots.ts), which is also why AnimFrame is
// a structgen root: no TypeScript may know a byte offset.
static AnimPlan  g_anim_plan;
static AnimFrame g_anim_frame;

void *wasm_anim_plan_ptr(void)  { return &g_anim_plan; }
void *wasm_anim_frame_ptr(void) { return &g_anim_frame; }

// THE PLAN is asked with the boards a client holds, so its door is the client
// slot's (wasm_table_api.c wasm_client_plan); the answer is left in g_anim_plan.

// WHERE THE PLAN STANDS AT now_ms (anim_plan.h anim_plan_at), measured from the
// sequence's start. The clock is an argument: the kernel imports nothing, so
// the host owns the origin and re-asks every frame. The answer lands in
// g_anim_frame. Returns ANIM_EOK or a negative ANIM_E*.
int wasm_anim_plan_at(int now_ms) {
    return anim_plan_at(&g_anim_plan, now_ms, &g_anim_frame);
}

// ---------- the role beat (anim_plan.h "the role beat") ------------------------
//
// WHICH MARKS CHANGE AT WHICH POINT OF A SEQUENCE. The three rules are already
// there and iOS has reached them natively since round 28 (c/ios/ios_api_anim.c
// fio_roles_*); the web had no way to ask at all, so every mark it drew came off
// whatever board the last step committed - the defect docs/ANIM_TIMING_AUDIT.md
// section 12 names. Nothing about the rules changes here: this is the bridge the
// audit's own summary says the web "only has to start asking".
//
// `shown` is what the badges are WEARING - the host's frozen ledger, not the
// resident game - and it crosses as three scalars for that reason: a caller that
// could pass a board would pass the wrong one.
//
// The answer, for all three entries: the address of the AnimRoles to show next
// (read through its generated reader) when the marks change, 0 when nothing
// does, or a negative ANIM_E*. A seat is -1 for none, in and out.
static AnimRoles g_anim_roles;
static int wasm_roles_answer(int changed) {
    return changed == 1 ? (int)(uintptr_t)&g_anim_roles : changed;
}

// A good being SET leads the stream - `first_good_mask` is the stream's own
// event 0 mask (AnimBeats.first_good_mask), ANIM_NO_MASK for a step with none.
int wasm_anim_roles_goods_opening(int shown_defender, int shown_first_attacker,
                                  int shown_good_mask, int first_good_mask) {
    const AnimRoles shown = { shown_defender, shown_first_attacker, shown_good_mask };
    return wasm_roles_answer(anim_goods_opening(shown, first_good_mask, &g_anim_roles));
}

// A good being CLEARED runs parallel with the throw-in that cleared it -
// `step_good_mask` is that beat's own mask (AnimBeat.good_mask).
int wasm_anim_roles_goods_cleared(int shown_defender, int shown_first_attacker,
                                  int shown_good_mask, int step_good_mask) {
    const AnimRoles shown = { shown_defender, shown_first_attacker, shown_good_mask };
    return wasm_roles_answer(anim_goods_cleared(shown, step_good_mask, &g_anim_roles));
}

// A PASS hands the shield over WITH the transfer card. `final_defender` is the
// stream's FINAL board's defender, because a pass is snapshotted before the
// hand-over and writes no event for it.
int wasm_anim_roles_pass_hand_off(int shown_defender, int shown_first_attacker,
                                  int shown_good_mask, int attack_pass_seats,
                                  int final_defender) {
    const AnimRoles shown = { shown_defender, shown_first_attacker, shown_good_mask };
    return wasm_roles_answer(anim_pass_hand_off(shown, (unsigned)attack_pass_seats,
                                                final_defender, &g_anim_roles));
}

// MAY THIS WRITER TOUCH THE SHOWN BADGES (anim_plan.h anim_shown_ledger_allows)?
// ANIM_CLAIM_* in, 1 or 0 out.
int wasm_anim_shown_ledger_allows(int claim, int sequencing) {
    return anim_shown_ledger_allows(claim, sequencing);
}

// WHAT A CHANGE OF THE MARKS SOUNDS LIKE (anim_plan.h anim_role_cue): the badges
// going from `shown` to `next`, each as its three scalars. An ANIM_CUE_* out,
// ANIM_CUE_NONE for a change that makes no sound. The host asks it at the one
// place it turns the marks, so no host decides which turn is audible.
int wasm_anim_role_cue(int shown_defender, int shown_first_attacker, int shown_good_mask,
                       int next_defender, int next_first_attacker, int next_good_mask) {
    const AnimRoles shown = { shown_defender, shown_first_attacker, shown_good_mask };
    const AnimRoles next = { next_defender, next_first_attacker, next_good_mask };
    return anim_role_cue(shown, next);
}

// WHAT THE END OF THE GAME SOUNDS LIKE to this viewer (anim_plan.h
// anim_finish_cue): the fool's seat (negative while the game runs) and the
// viewer's (negative for a spectator) in, an ANIM_CUE_* out.
int wasm_anim_finish_cue(int game_over, int my_seat) {
    return anim_finish_cue(game_over, my_seat);
}

// THE FINISH ORDER (anim_plan.h anim_finish_rows), for the end screen, asked as
// a value (AnimFinishAsk: the elimination seats first out first, the fool's
// seat or a negative while the game runs, the seat count, and the viewer or a
// negative for a spectator) and answered as one (AnimFinish), each through its
// generated writer or reader. Returns the row count, or a negative ANIM_E*.
static AnimFinishAsk g_anim_finish_ask;
static AnimFinish    g_anim_finish;
AnimFinishAsk *wasm_anim_finish_ask_ptr(void) { return &g_anim_finish_ask; }
AnimFinish *wasm_anim_finish_ptr(void)        { return &g_anim_finish; }
int wasm_anim_finish_rows(void) {
    return anim_finish_ask(&g_anim_finish_ask, &g_anim_finish);
}


// ---------- legal moves --------------------------------------------------------
// u32 n, then per move: u8 type, u8 n_cards, n_cards x u8 wire-card cards,
// n_cards x u8 wire-card attack_cards (zeroed for non-cover moves).

int wasm_legal_moves(int bot_idx) {
    calculate_legal_moves(&g_game, bot_idx, &g_moves);
    return g_moves.n;
}

// The menu itself (legal.h LegalMoves), which the caller reads through the
// generated accessors (sdk/ts/gen/anim.bots.ts). It replaced a CHUNKED packed
// export: the list can be far larger than the IO buffer, so a host used to loop
// over u32-prefixed chunks of the wire format and unpack each move by hand.
// Reading the struct where it already is needs neither the chunks nor the wire
// (the packed form stays for the hosts that genuinely have to copy it across a
// boundary - legal_menu_write, which iOS's fio_legal_packed uses).
void *wasm_legal_moves_ptr(void) { return &g_moves; }
