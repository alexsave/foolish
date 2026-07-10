// M8 (docs/BOTS_WASM_MEMORY_PLAN.md): wasm-only overlay of the three
// replay-CALL scratch buffers (g_rec, g_bn, g_replay_io — 90.5 KiB together)
// into the solver's solve_ws arena (272 KiB). The two families are never live
// at the same time: the wasm instance is single-threaded and wasm_choose_move
// (which drives the solver → solve_ws) vs wasm_replay_encode/decode (which use
// the replay scratch) are top-level exports that never nest. Aliasing them
// reclaims the 90.5 KiB outright — pure address reuse, no behavior change,
// because each replay buffer is written-before-read within a single call
// (g_rec/g_bn counters start at 0; g_replay_io is written by the TS bridge
// before the codec call), so whatever solver bytes precede it are irrelevant.
//
// wasm-ONLY: natively solve_ws is _Thread_local (OMP runs games in parallel)
// while the replay buffers are process-global — aliasing them on native is a
// data race by construction. CD_WASM_OVERLAY is set ONLY on the bots-module
// compile (WASM_BOT_CFLAGS), never for rules.wasm (which does not link
// cordite_sim.c, so has no solve_ws) or any native build.
//
// Offsets are 16-aligned; the layout is g_rec [0,49152) | g_bn [49152,60416) |
// g_replay_io [60416,93184). Each block's fit into its slot is _Static_assert'd
// at its definition site, and cordite_sim.c asserts the whole arena fits
// solve_ws — so a cap bump (REC_CAP, BN_CAP, REPLAY_IO_CAP) that would overflow
// fails the link loudly instead of corrupting the solver.
#ifndef WASM_OVERLAY_H
#define WASM_OVERLAY_H

#ifdef CD_WASM_OVERLAY
extern unsigned char *const cd_overlay;   // == (unsigned char *)&solve_ws
#define CD_OVL_REC_OFF 0u        // g_rec:       REC_CAP * 12 B  == 49152
#define CD_OVL_BN_OFF  49152u    // g_bn (Bn):   <= 11264 B slot (16-aligned)
#define CD_OVL_IO_OFF  60416u    // g_replay_io: <= 32768 B slot
#define CD_OVL_END     (CD_OVL_IO_OFF + 32768u)   // 93184
#endif

#endif
