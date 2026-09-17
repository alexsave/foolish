/* =============================================================================
 * foolish.cards - whole-game replay format: the version a code carries
 * =============================================================================
 * The replay codec is the C kernel's (c/src/replay.c, replay_steps.c); hosts
 * read a code through bots.wasm (sdk/ts/wasm/bots.ts replaySummary and the
 * step readers). What is left here is the version number tests hold a code's
 * summary to.
 *
 * Never change the wire format in one place: bump the version in replay.h AND
 * decide what happens to every code already cut.
 * ========================================================================== */

// The ONE format: inline reveals, hidden-state-lossless, partial-game
// (c/src/replay.h REPLAY_FORMAT_VERSION_V10,
// docs/REPLAY_FORMAT6_HIDDEN_STATE.md). Was 6, then 7 (pass-mode bit), then 8
// (forced-opening bit), and is now 10 for a reason that is not a wire change at
// all: the bytes did not move, the deal order under them did. A code carrying
// any other version is refused, never re-read.
export const FORMAT_VERSION_V6 = 10;
