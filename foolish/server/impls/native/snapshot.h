// snapshot.h - the durable form of a game and a user.
//
// STAGE 2 (DURABILITY.md): SQLite WAL write-behind persistence + crash
// recovery. This is the ONLY place that knows how a GameSlot or a User turns
// into bytes on disk and back; everything else in the server holds the live
// object and calls game_mark_dirty (registry.h) when it changes. The blob
// layouts, their version bytes, the round-trip gate that refuses to let the
// server start on a lossy codec, and the four persist.c callbacks all live
// here together, because they are one decision: what survives a restart.
#ifndef FOOLISH_SNAPSHOT_H
#define FOOLISH_SNAPSHOT_H

#include "persist.h"

// Registered by snapshot_register_tables() before persist_start (see main()'s
// "Stage 2" block) - NULL until then, which is fine: nothing calls
// game_mark_dirty or marks a user dirty before main() finishes setup and
// starts the worker pools / accept loop.
extern PersistTable *g_game_table;
extern PersistTable *g_user_table;

// Correctness gate: serialize -> deserialize -> serialize again must be
// byte-identical. Runs once at startup on a small synthetic game (populated
// with non-default values in every field family - deck, battles, hands,
// lobby roster - so a field silently dropped from either direction shows up
// as a mismatch, not a coincidental pass) - a real regression test, not
// decoration: a mismatch here means the wire format and the round-trip code
// have drifted, and the server refuses to start rather than silently
// persisting (or recovering) corrupt/lossy snapshots. Calls exit(1) on a
// failure, so a caller that returns has passed.
void persist_self_test(void);

// Registers the two durable tables ("games", "users") with persist.c, wiring
// each to its snapshot/load callback pair below. MUST run before
// persist_start, which is what actually opens the DB and runs the synchronous
// crash-recovery pass through those load callbacks.
void snapshot_register_tables(void);

#endif
