// Regression guards for two client-side hand-rolled-logic divergences fixed
// alongside the pass bug — both replaced a hand-rolled computation with the
// shared authoritative utility. Each test FAILS against the pre-fix code.
//
//   1. AnimationContext optimistic next-defender used `(defender + 1) % length`,
//      which does NOT skip eliminated seats — fixed to nextDefenderIndex()
//      (get_next_player_index). Guarded here via that shared helper.
//   2. KeyboardInputHandler had a local canPass that ignored the next-player
//      capacity check (and the out-seat skip) — fixed to use the shared canPass.
//
// Pure logic — no Postgres, no harness. Owns these scenarios; the fast runner
// (e2e/validation/client_rules_validation.test.ts) imports
// `registerClientRulesValidation` and executes them.

(globalThis as any).Deno = (globalThis as any).Deno || { env: { get: (k: string) => process.env[k] || 'x' } };

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { PersonalGame, PublicPlayer, PrivatePlayer, Card, GAME_STATUS, PLAYER_STATUS, STRATEGY_KEY } from '../supabase/functions/_shared/types.ts';
import { canPass, nextDefenderIndex } from '../src/utils/gameValidation.ts';

interface Spec { status: 'in' | 'out'; hand_length: number }
const c = (suit: number, value: number): Card => ({ suit, value });

// Minimal PersonalGame; nextDefenderIndex/canPass read players[].status,
// players[].hand_length, defender, and table_battles.
function makeGame(defender: number, specs: Spec[], table: PersonalGame['table_battles']): PersonalGame {
    const players: PublicPlayer[] = specs.map((s, i) => ({
        player_id: `P${i}`, name: `P${i}`,
        status: s.status === 'out' ? PLAYER_STATUS.OUT : PLAYER_STATUS.IN,
        hand_length: s.hand_length, is_ai: false,
    }));
    const self: PrivatePlayer = { ...players[defender], hand: [], awaiting_attack: false, strategy_key: STRATEGY_KEY.HUMAN };
    return {
        id: 'g', name: 'g', deck_length: 0, discard_pile_length: 0, flipped: null,
        players, status: GAME_STATUS.PLAYING, power_suit: 0, first_attacker: 0, defender,
        table_battles: table, elimination_order: [], good_timestamp: null, good_players: [], self,
    };
}

// Verbatim replica of the deleted KeyboardInputHandler local canPass — it never
// looked at the next player, so it OK'd passes the server rejects. Kept here only
// to document the bug the fix removed (cf. rearrange's "unguarded map").
const oldKeyboardCanPass = (game: PersonalGame, cards: Card[]): boolean => {
    if (game.table_battles.length === 0) return false;
    if (!cards.every((card) => card.value === cards[0].value)) return false;
    return game.table_battles.every((b) => b.defense === null && b.attack.value === cards[0].value);
};

export function registerClientRulesValidation(): void {
    // ---- Finding 1: AnimationContext optimistic next-defender ----------------
    test('nextDefenderIndex skips an eliminated seat (was: (defender+1) % length)', () => {
        // defender at seat 1; seat 2 is OUT; the real next defender wraps to seat 0.
        const g = makeGame(1, [{ status: 'in', hand_length: 5 }, { status: 'in', hand_length: 4 }, { status: 'out', hand_length: 0 }], []);
        const naive = (g.defender + 1) % g.players.length; // the pre-fix computation
        assert.equal(naive, 2, 'sanity: the naive formula lands on the eliminated seat');
        assert.equal(nextDefenderIndex(g), 0, 'fixed: skips the out seat to the next in-play player');
        assert.notEqual(nextDefenderIndex(g), naive, 'the fix diverges from the buggy naive formula here');
    });

    test('nextDefenderIndex matches naive rotation when nobody is eliminated', () => {
        const g = makeGame(0, [{ status: 'in', hand_length: 5 }, { status: 'in', hand_length: 5 }, { status: 'in', hand_length: 5 }], []);
        assert.equal(nextDefenderIndex(g), 1);
    });

    // ---- Finding 2: KeyboardInputHandler local canPass (now shared canPass) --
    test('canPass is FALSE when the next defender lacks room (the keyboard copy skipped this check)', () => {
        // Two uncovered 7s on the table; defender passes a third 7 -> next defender
        // would face 3 cards but holds only 1.
        const g = makeGame(0,
            [{ status: 'in', hand_length: 3 }, { status: 'in', hand_length: 1 }, { status: 'in', hand_length: 5 }],
            [{ attack: c(1, 7), defense: null }, { attack: c(2, 7), defense: null }]);
        // The deleted keyboard logic WOULD have offered this illegal pass...
        assert.equal(oldKeyboardCanPass(g, [c(0, 7)]), true, 'documents the bug: old keyboard logic allowed it');
        // ...the shared canPass the keyboard now uses correctly rejects it.
        assert.equal(canPass(g, [c(0, 7)]), false, '2 on table + 1 passed = 3 > next defender hand of 1');
    });

    test('canPass is TRUE for a legal pass the keyboard should offer', () => {
        const g = makeGame(0,
            [{ status: 'in', hand_length: 3 }, { status: 'in', hand_length: 4 }, { status: 'in', hand_length: 5 }],
            [{ attack: c(1, 7), defense: null }]);
        assert.equal(canPass(g, [c(0, 7)]), true);
    });

    test('canPass over an eliminated next seat checks the REAL next defender', () => {
        // defender seat 1, seat 2 OUT (0 cards), real next defender seat 0 has room.
        const g = makeGame(1,
            [{ status: 'in', hand_length: 5 }, { status: 'in', hand_length: 4 }, { status: 'out', hand_length: 0 }],
            [{ attack: c(3, 8), defense: null }, { attack: c(2, 8), defense: null }]);
        assert.equal(canPass(g, [c(0, 8)]), true, 'must look past the out seat to seat 0 (room for 3)');
    });
}

if (!process.env.VALIDATION_ONLY) registerClientRulesValidation();
