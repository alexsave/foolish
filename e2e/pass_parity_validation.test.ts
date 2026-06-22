// Pass legality VALIDATION — a few handpicked, deterministic scenarios that lock
// in client/server agreement on whether the defender may pass. This is the fast
// CI guard: no Postgres, no fuzzing. The exhaustive search lives in
// pass_parity.test.ts (run locally / on demand); this file pins the specific
// regressions so a future edit can't silently reintroduce them.
//
// Three oracles, all pure functions on an in-memory game:
//   ground truth — calculateLegalMoves (the bot enumerator)
//   SERVER       — validatePass from actions/pass.ts (throw == illegal)
//   CLIENT       — canPass from src/utils/gameValidation.ts (the UI button gate)

// Deno globals the _shared modules read at import time. Set BEFORE importing them.
(globalThis as any).Deno = (globalThis as any).Deno || { env: { get: (k: string) => process.env[k] || 'x' } };
(globalThis as any).EdgeRuntime = (globalThis as any).EdgeRuntime || { waitUntil: () => {} };

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { Game, PersonalGame, PrivatePlayer, Battle, Card, GAME_STATUS, PLAYER_STATUS, STRATEGY_KEY } from '../supabase/functions/_shared/types.ts';
import { personalize_game } from '../supabase/functions/_shared/common_utils.ts';
import { calculateLegalMoves } from '../supabase/functions/_shared/bot_strategy.ts';
import { validatePass as serverValidatePass } from '../supabase/functions/_shared/actions/pass.ts';
import { canPass as clientCanPass } from '../src/utils/gameValidation.ts';

const c = (suit: number, value: number): Card => ({ suit, value });
const cardKey = (x: Card) => `${x.suit}:${x.value}`;

interface PlayerSpec { status: 'in' | 'out'; hand: Card[] }

// Build a minimal but real PLAYING game: validatePass / canPass / calculateLegalMoves
// only read these fields, so no deck/DB is needed.
function makeGame(defender: number, players: PlayerSpec[], table: Battle[], powerSuit = 0): Game {
    const ps: PrivatePlayer[] = players.map((p, i) => ({
        player_id: `P${i}`,
        name: `P${i}`,
        status: p.status === 'out' ? PLAYER_STATUS.OUT : PLAYER_STATUS.IN,
        hand: p.hand,
        hand_length: p.hand.length,
        awaiting_attack: false,
        is_ai: false,
        strategy_key: STRATEGY_KEY.HUMAN,
    }));
    return {
        id: 'g', name: 'g', deck: [], deck_length: 0, discard_pile_length: 0, flipped: null,
        players: ps, status: GAME_STATUS.PLAYING, power_suit: powerSuit,
        first_attacker: 0, defender, table_battles: table,
        elimination_order: players.map((p, i) => p.status === 'out' ? `P${i}` : '').filter(Boolean),
        good_timestamp: null, good_players: [], logs: [],
    };
}

const serverYes = (g: Game, pid: string, cards: Card[]): boolean => {
    try { serverValidatePass(g, pid, cards); return true; } catch { return false; }
};
const truthYes = (g: Game, pid: string, cards: Card[]): boolean => {
    const k = cards.map(cardKey).sort().join('|');
    return calculateLegalMoves(g, pid).some((m) => m.type === 'pass' && (m.cards as Card[]).map(cardKey).sort().join('|') === k);
};
const clientYes = (g: Game, defender: number, cards: Card[]): boolean =>
    clientCanPass(personalize_game(g, g.players[defender].player_id) as PersonalGame, cards);

// One assertion helper: all three oracles must agree with `expected` for this pass.
function expectParity(name: string, g: Game, defender: number, cards: Card[], expected: boolean) {
    const pid = g.players[defender].player_id;
    const s = serverYes(g, pid, cards), t = truthYes(g, pid, cards), cl = clientYes(g, defender, cards);
    assert.equal(s, expected, `${name}: SERVER legality ${s} !== expected ${expected}`);
    assert.equal(t, expected, `${name}: GROUND-TRUTH legality ${t} !== expected ${expected}`);
    assert.equal(cl, expected, `${name}: CLIENT legality ${cl} !== expected ${expected} (client/server disagree)`);
}

// THE REGRESSION: defender at seat 1, seat 2 is OUT (empty hand), the real next
// defender (wrapping past the out seat to seat 0) has plenty of cards. Passing a
// third 8 is legal — the client must not look at the out seat's empty hand and
// hide the Pass button.
test('parity: legal pass when the seat after the defender is eliminated (the reported bug)', () => {
    const g = makeGame(1,
        [
            { status: 'in', hand: [c(0, 5), c(1, 6), c(2, 9), c(3, 10), c(0, 11)] }, // P0 real next defender, 5 cards
            { status: 'in', hand: [c(0, 8), c(1, 12), c(2, 13), c(3, 14)] },          // P1 defender, holds an 8
            { status: 'out', hand: [] },                                              // P2 eliminated, 0 cards
        ],
        [{ attack: c(3, 8), defense: null }, { attack: c(2, 8), defense: null }]);
    expectParity('out-seat-after-defender', g, 1, [c(0, 8)], true);
});

// Don't over-correct: same shape, but the REAL next defender (seat 0) is too
// small to receive the pass — illegal on every oracle.
test('parity: pass blocked when the real next defender (past an out seat) lacks room', () => {
    const g = makeGame(1,
        [
            { status: 'in', hand: [c(0, 5), c(1, 6)] },                       // P0 real next defender, only 2 cards
            { status: 'in', hand: [c(0, 8), c(1, 12), c(2, 13), c(3, 14)] },  // P1 defender
            { status: 'out', hand: [] },                                      // P2 eliminated
        ],
        [{ attack: c(3, 8), defense: null }, { attack: c(2, 8), defense: null }]); // 2 + 1 = 3 > 2
    expectParity('out-seat-real-next-too-small', g, 1, [c(0, 8)], false);
});

// Baseline, no eliminations: a plainly legal pass.
test('parity: ordinary legal pass with all players in', () => {
    const g = makeGame(0,
        [
            { status: 'in', hand: [c(0, 7), c(1, 12), c(2, 13)] },            // P0 defender, holds a 7
            { status: 'in', hand: [c(0, 5), c(1, 6), c(2, 9), c(3, 10)] },    // P1 next defender, 4 cards
            { status: 'in', hand: [c(0, 11), c(2, 11)] },                     // P2
        ],
        [{ attack: c(1, 7), defense: null }]);
    expectParity('ordinary-legal', g, 0, [c(0, 7)], true);
});

// Baseline, no eliminations: next defender too small -> illegal everywhere.
test('parity: ordinary pass blocked when next defender lacks room', () => {
    const g = makeGame(0,
        [
            { status: 'in', hand: [c(0, 7), c(1, 7), c(2, 13)] },             // P0 defender, two 7s
            { status: 'in', hand: [c(0, 5)] },                               // P1 next defender, 1 card
            { status: 'in', hand: [c(0, 11), c(2, 11)] },                     // P2
        ],
        [{ attack: c(1, 7), defense: null }, { attack: c(2, 7), defense: null }]); // 2 + 1 = 3 > 1
    expectParity('ordinary-blocked', g, 0, [c(0, 7)], false);
});

// A covered battle on the table makes any pass illegal (no transfer once defending began).
test('parity: cannot pass once a battle is covered', () => {
    const g = makeGame(0,
        [
            { status: 'in', hand: [c(0, 7), c(1, 12)] },                      // P0 defender, holds a 7
            { status: 'in', hand: [c(0, 5), c(1, 6), c(2, 9), c(3, 10)] },    // P1 next defender
            { status: 'in', hand: [c(0, 11), c(2, 11)] },                     // P2
        ],
        [{ attack: c(1, 7), defense: c(1, 13) }]); // already covered
    expectParity('covered-no-pass', g, 0, [c(0, 7)], false);
});
