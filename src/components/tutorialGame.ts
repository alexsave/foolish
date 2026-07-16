/* The tutorial's scripted game — a third "game-state source" alongside the
 * live server and shared replay URLs. It is a real, complete 3-player game
 * produced by the actual server engine (tests/gen_tutorial_game.ts) and frozen
 * here as a replay moves code. The tutorial decodes it client-side exactly like
 * the replay screen, then narrates it and highlights the learner's moves.
 *
 * Seat 0 is the learner ("You"). The game was selected to contain every
 * gameplay element — lead attack, cover, trump cover, throw-in, perevod/pass,
 * pickup, round-end discard, refills, the deck running out, a player going out,
 * and the fool — with seat 0 performing as many of them as possible. */

// base32 moves code (no extras section). Produced by tests/gen_tutorial_game.ts.
// 3-player game, Hearts trump (7♥). You (seat 0) hold the lowest trump and
// lead; over the game you personally lead, throw in, cover, trump-cover, pass
// (perevod), pick up, say "good", and finally go out safely — Vera is left the
// fool. Every gameplay element appears at least once.
//
// A replay code is only readable by the kernel that cut it: the coder's
// probability model IS the legal-move menu, so changing the menu renumbers
// every choice. This code was re-cut when v6 gained the pending-good atom
// (cnitro/src/replay.c) — same 70-log game, new integer. If a menu change ever
// orphans it again, decode it with the kernel of the commit before the change
// and re-encode; there is no other way back in.
export const TUTORIAL_MOVES_CODE = 'WQBIG5WZTXKMTQRRWK4LLV5UTRRQ2CZFVWYAK';

// Seat names; index 0 is the learner.
export const TUTORIAL_NAMES = ['You', 'Vera', 'Boris'];
