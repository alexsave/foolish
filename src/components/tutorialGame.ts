/* The tutorial's scripted game — a third "game-state source" alongside the
 * live server and shared replay URLs. It is a real, complete 3-player game
 * produced by the actual engine (tests/gen_tutorial_game.ts) and frozen here as
 * a replay moves code. The tutorial replays it client-side exactly like the
 * replay screen, then narrates it and highlights the learner's moves.
 *
 * Seat 0 is the learner ("You"). The game is selected to contain every gameplay
 * element — lead attack, cover, trump cover, throw-in, perevod/pass, pickup,
 * round-end discard, refills, the deck running out, a player going out, and the
 * fool — with seat 0 performing as many of them as possible. */

// base32 moves code (no extras section). Produced by tests/gen_tutorial_game.ts,
// which searches seeds for a game that teaches everything: this is seed 37 of
// 1500 (172 of which qualified), the shortest complete one — 48 steps, 18 of
// them the learner's.
//
// 3-player game. You (seat 0) hold the lowest trump and lead; over the game you
// personally lead, throw in, cover, trump-cover, pass (perevod), pick up and say
// good, and you are not the one left the fool.
//
// A replay code is only readable by the kernel that cut it: the coder's
// probability model IS the legal-move menu, so changing the menu renumbers every
// choice and orphans this constant. When that happens, do NOT hunt for an old
// kernel to decode it with — re-cut it: `npx tsx tests/gen_tutorial_game.ts`.
// The beats and the learner's prompts are all derived from the game itself, so a
// re-cut needs no other edit. (The previous code was frozen with its generator
// missing, which is how it came to be re-cut by hand once already.)
//
// This is v6, and had to be: the tutorial now replays through the real engine
// (C_CORE_CONSOLIDATION.md A5), which v5 cannot do — v5 hides the deal, so there
// is no deck to rebuild and its hands are retrodiction.
export const TUTORIAL_MOVES_CODE = 'AWYOKP7AJKMWBDPFRTMY5CEP6OM34SS64VICTW2C5VLM7NQW';

// Seat names; index 0 is the learner.
export const TUTORIAL_NAMES = ['You', 'Vera', 'Boris'];
