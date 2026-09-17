// Replay steps from the kernel (docs/C_CORE_CONSOLIDATION.md F4.2 / A5).
//
// A replay is not a second kind of game. This rebuilds the REAL Game a v6 code
// describes and plays it back through the REAL engine, so the animation events
// a replay renders are produced by evwire_walk from real engine hooks — the
// same one derivation live play uses (F4.1 / A3). There is no replay-side
// projection to keep in step, on any platform.
//
// Why this is possible at all: v6 is hidden-state-lossless. Its decode yields
// every seat's exact opening hand and every stock draw in pop order, and that
// pair IS a deck (start_game_with_deck). v5 is not supported and cannot be:
// it hides the deal, so its "hands" are retrodiction — a tri-state (known /
// unknown-slot / never-surfaces) that a Game's concrete Card hand[] has no way
// to hold. v5 is a dead format here; the decoder refuses it.
#ifndef CNITRO_REPLAY_STEPS_H
#define CNITRO_REPLAY_STEPS_H

#include "game.h"
#include "evwire.h"
#include "replay.h"

// ---------- the deal and the actions, without the playback ----------------
//
// The two halves of what a v6+ code says, handed over as data: the DECK its
// reveals imply and the ACTION stream it records. Exposed because rebuilding a
// game is not the only thing a code is good for - an analyser BRANCHES one,
// replaying the recorded actions up to a decision and then substituting a
// different move - and it must branch the same deal, through the same engine,
// or it is analysing a game nobody played.
// A BUILD PARAMETER, and an analytic ceiling rather than a sampled margin:
// `count_atoms_from_logs` emits at most one atom per log, DEAL and DRAW are not
// actions, and the encoder refuses a game at the log cap, so no code that can
// exist carries more than MAX_LOGS-1 actions. A build may therefore size this
// to its own MAX_LOGS instead of the 4096 that suits the widest one. The
// default is unchanged so every existing consumer keeps today's array.
#ifndef REPLAY_MAX_ACTIONS
#define REPLAY_MAX_ACTIONS 4096
#endif

typedef struct {
    int  kind;                     // REPLAY_ATOM_* (never DEAL or DRAW)
    int  seat;
    Card cards[REPLAY_MAX_PAIRS];
    int  n_cards;
    Card target;                   // COVER only
} ReplayAction;

// Decode `code` for its deal and its actions. `hdr` optional. Returns
// REPLAY_EOK or -REPLAY_E*; -REPLAY_ECAP when the stream outruns a cap.
int replay_deal_v6(const unsigned char *code, int code_len, ReplayHeader *hdr,
                   Card *deck, int deck_cap, int *n_deck,
                   ReplayAction *acts, int acts_cap, int *n_acts);

// Deal `g` from that deck under the rules and opening `hdr` records, then CHECK
// that the rebuilt hands really derive the recorded opener (the check that
// catches a mis-rebuilt deal - see rs_play). Returns REPLAY_EOK or -REPLAY_E*.
// Installs no snapshot hook; a caller that wants the deal's animation events
// installs its own around the call, as replay_steps_v6 does.
int replay_deal_start(Game *g, const ReplayHeader *hdr, const Card *deck, int n_deck);

// Apply one recorded action to `g`. ROUND_END is not a move and has no single
// actor, so it is not something a caller can spell with handle_*; this owns
// that translation, and it is why an analyser must not roll its own.
void replay_action_apply(Game *g, const ReplayAction *a);

// Play a v6 replay code back through the engine, handing every animation event
// to `sink` in play order, masked for `viewer` (a seat, or VIEW_SPECTATOR).
//
// `hdr`, if given, receives the decode header — its `fool` and elimination
// order are the code's own claim, which a caller can hold against the rebuilt
// game (replay_steps_test does).
//
// Returns REPLAY_EOK (0) or -REPLAY_E*. REPLAY_EVERSION for a v5 code.
int replay_steps_v6(const unsigned char *code, int code_len, int viewer,
                    ReplayHeader *hdr, EvwSink sink, void *ctx);

// Serialize a v6 replay as packed evwire FRAMES — one per step (the deal, then
// one per action), which is exactly what live play broadcasts and what the web
// already decodes and renders. Chunked: a whole game's frames (each carrying a
// masked board snapshot) outgrow any single wasm IO buffer, and evwire's
// n_events is a u8, so one frame per game is impossible anyway.
//
// Writes frames for steps [from, ...) into `out`, stopping before the first one
// that would not fit. Each frame is preceded by a u16 LE length. `n_frames`
// receives how many landed and `next_step` the cursor to resume from (== the
// step count when the stream is exhausted). Returns bytes written or -REPLAY_E*.
int replay_steps_frames_v6(const unsigned char *code, int code_len, int viewer,
                           int from, ReplayHeader *hdr,
                           unsigned char *out, int out_cap,
                           int *n_frames, int *next_step);

// How many steps a code replays to (the deal + one per action), or -REPLAY_E*.
// Sizes a scrubber before any frame is pulled.
int replay_steps_count_v6(const unsigned char *code, int code_len,
                          ReplayHeader *hdr);

// What each step IS, one RS_INDEX_STRIDE-byte record per step, in step order:
//
//   u8  kind     REPLAY_ATOM_* — the action this step played. The opening deal
//                reports REPLAY_ATOM_DEAL, which is otherwise a per-seat hand
//                atom and never an action, so the two never collide.
//   u8  seat     the acting seat, or RS_SEAT_NONE for the deal and ROUND_END
//                (nobody in particular closes a bout).
//
// A scrubber needs the kind and the seat to say what just happened, and it
// cannot honestly get them from the frames: an attack and a pass are the same
// evwire event type, told apart only by a reconstructed English message. That
// would be a projection — the exact thing A5 deletes. The kernel knows what it
// played, so it says so.
//
// Deliberately NOT reported: how many log records each step produced. It is
// right there (rs_step clears the log per action), and it is useless, because
// the replayed engine's log stream is not the stream any caller holds. A 3p
// game logs 92 records when it is PLAYED, 79 when replay_decode reconstructs
// it, and 76 when the engine replays it here — they disagree on goods (v6
// trims all but a trailing one, by design) and on how draws are grouped. A
// count against a fourth private stream would only look like a mapping.
//
// Returns bytes written (steps * RS_INDEX_STRIDE) or -REPLAY_E*.
#define RS_INDEX_STRIDE 2
#define RS_SEAT_NONE    0xFF
int replay_steps_index_v6(const unsigned char *code, int code_len,
                          ReplayHeader *hdr, unsigned char *out, int out_cap);

// ---------- a recorded decision, as an analyser reads it -------------------
//
// The Oracle deliberates a decision of a replay as the seat that made it
// (docs/C_GAME_SHAPE_MIGRATION.md Phase 7). What it needs are two things only
// the kernel can say honestly about a code: the board that seat decided on, as
// that seat saw it, and the public history before the move. Both are bytes the
// analyser's module imports unchanged (wasm_import_state masked,
// wasm_import_logs); the host reads no byte of either.

// The most bytes a board in the state_put layout (view.h) can take.
#define RS_BOARD_MAX (17 + MAX_DECK + 1 + 2 * MAX_BATTLES \
                      + MAX_PLAYERS * (3 + MAX_HAND_SIZE) + 1 + MAX_PLAYERS)

// The board action step `step` (1 .. steps - 1) was decided on - the board step
// `step - 1` left - as `viewer` (a seat of the game, or VIEW_SPECTATOR) sees it,
// in the state_put layout a masked import reads: the viewer's hand real, every
// card the viewer cannot see written hidden. Returns bytes written;
// -REPLAY_EINPUT for a step that is not an action or a viewer that is not a
// seat, -REPLAY_ECAP when `out_cap` is below RS_BOARD_MAX, or -REPLAY_E*.
int replay_steps_board_v6(const unsigned char *code, int code_len, int step, int viewer,
                          unsigned char *out, int out_cap);

// The public session log before action step `step`'s move, as an analyser's
// memory: replay_decode's log stream up to the record of that move, in the log
// import layout (u16 LE count, then per record u8 log_type, u8 seat,
// u8 defender_index, u8 n_pairs, n_pairs x (u8 primary, u8 target)), every
// LOG_DRAW's card written hidden (a drawn card's identity is nobody's public
// knowledge, view.h log_record_put) and at most MAX_LOGS records, keep-first.
//
// The decoded log stream is not the step stream (see replay_steps_index_v6), but
// the moves are one record each in both, in the same order: every ATTACK, COVER,
// PASS and PICKUP step is the next record of those four types, and the pair must
// agree on the type and the seat. A step with no record of its own (a GOOD, a
// ROUND_END), a step past the records, or a pair that disagrees has no memory
// this can vouch for: -REPLAY_EINPUT. `out` also holds the decode while this
// runs, so `out_cap` must fit the whole decoded stream. Returns bytes written,
// or -REPLAY_E*.
int replay_steps_memory_v6(const unsigned char *code, int code_len, int step,
                           unsigned char *out, int out_cap);

// The game the last successful replay_steps_v6 rebuilt — the state its code
// decodes TO, valid until the next call. A whole-game code leaves the finished
// game here; a mid-game cut leaves the exact position, which is what a
// continuation (an iMessage turn) plays on from.
const Game *replay_steps_last_game(void);

#endif
