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

// The game the last successful replay_steps_v6 rebuilt — the state its code
// decodes TO, valid until the next call. A whole-game code leaves the finished
// game here; a mid-game cut leaves the exact position, which is what a
// continuation (an iMessage turn) plays on from.
const Game *replay_steps_last_game(void);

#endif
