// R1 (docs/RULES_GUARDS_WASM_MEMORY_PLAN.md): rules.wasm-only arena overlay.
// The bots round aliased the replay scratch into the solver's solve_ws arena
// (M8/M9, wasm_overlay.h). rules.wasm links no solver — so it gets its OWN
// arena (g_rules_arena, defined in wasm/wasm_api.c), into which the two
// mutually-exclusive buffer families are aliased.
//
// The families never coexist because the wasm instance is single-threaded and
// the two top-level export groups never nest (verified against the TS bridge,
// supabase/functions/_shared/wasm/engine.ts):
//
//   ACTION family — live during an action / marshal / menu export:
//     g_moves (LegalMoves)  the legal-move menu
//     g_snaps (SnapSlot[])  one marshal window's animation snapshots
//     g_io    (IO_CAP)      the marshal / export I/O buffer
//   Every one is written-before-read within a single synchronous handler and
//   fully drained (copied into JS objects) before that handler returns
//   (applyAction, kernelLegalMoves). No index into a resident menu is ever
//   held across calls — wasm_apply_action takes wire bytes, not a menu index.
//
//   REPLAY family — live only inside ONE wasm_replay_encode/decode call:
//     g_rec   (RecChoice[]) the rANS choice log
//     g_bn    (Bn)          the bignum accumulator
//     g_replay_io (REPLAY_IO_CAP) the replay blob in/out
//   kernelReplayRun writes the input, calls the codec, and slice()s the output
//   all within one synchronous function; each buffer is written-before-read
//   (n_rec/bn .n start at 0; g_replay_io is written by the bridge first).
//
// Laying BOTH families from offset 0 aliases them: the arena is sized by the
// larger (action) family, and the two families reuse the same bytes. During a
// replay call the action-family buffers are dead (nothing reads them); the
// next action re-marshals / re-enumerates, overwriting whatever replay bytes
// remained before any read. Pure address reuse, zero behavior change.
//
// rules-ONLY: CD_RULES_OVERLAY is set only on the rules.wasm compile
// (WASM_RULES_FLAGS). wasm_api.c / replay.c are ALSO compiled into bots.wasm
// (with CD_WASM_OVERLAY, which aliases into solve_ws instead) and into native
// tools (plain statics) — the two flavors are mutually exclusive (below) and a
// native build sees neither, so its buffers stay independent statics.
//
// Offsets are 16-aligned; each buffer's fit into its slot is _Static_assert'd
// at its definition site, and wasm_api.c asserts each family END fits the
// arena — so a cap bump (MAX_LEGAL_MOVES, MAX_SNAPS, IO_CAP, REC_CAP,
// REPLAY_IO_CAP) that would overflow fails the LINK loudly (also caught by the
// R0 memory pin) instead of corrupting a live buffer.
#ifndef RULES_OVERLAY_H
#define RULES_OVERLAY_H

#if defined(CD_WASM_OVERLAY) && defined(CD_RULES_OVERLAY)
#error "pick one overlay flavor: CD_WASM_OVERLAY (bots, into solve_ws) XOR CD_RULES_OVERLAY (rules, into g_rules_arena)"
#endif

#ifdef CD_RULES_OVERLAY
extern unsigned char *const rules_overlay;   // == (unsigned char *)&g_rules_arena

// ACTION family, laid out from offset 0. Slot widths (next_off - this_off)
// are 16-aligned around the measured sizes at the rules caps
// (MAX_LEGAL_MOVES=1024, MAX_SNAPS=16, WASM_IO_CAP=24576):
#define RULES_OVL_MOVES_OFF     0u        // g_moves (LegalMoves): <= 59408 B slot
#define RULES_OVL_SNAPS_OFF     59408u    // g_snaps (SnapSlot[MAX_SNAPS]): <= 18560 B slot
#define RULES_OVL_IO_OFF        77968u    // g_io (IO_CAP): <= 24576 B slot
#define RULES_OVL_ACTION_END    102544u   // 77968 + 24576

// REPLAY family, ALSO laid out from offset 0 — it aliases the action family:
#define RULES_OVL_REC_OFF       0u        // g_rec (RecChoice[REC_CAP]): <= 49152 B slot
#define RULES_OVL_BN_OFF        49152u    // g_bn (Bn): <= 10768 B slot
#define RULES_OVL_REPLAY_IO_OFF 59920u    // g_replay_io (REPLAY_IO_CAP): <= 42624 B slot
#define RULES_OVL_REPLAY_END    92688u    // 59920 + 32768

// Sized by the larger family; both ENDs are asserted to fit in wasm_api.c.
#define RULES_ARENA_SIZE        102544u   // max(RULES_OVL_ACTION_END, RULES_OVL_REPLAY_END), 16-aligned
#endif

#endif
