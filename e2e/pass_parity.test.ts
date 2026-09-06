// Pass legality — owns BOTH the exhaustive fuzzer AND the handpicked validation
// scenarios for passing. Per project convention, anything pass-related lives in
// this file; the fast validation runner (e2e/validation/pass_parity_validation.test.ts)
// imports `registerPassValidation` and executes just those deterministic cases.
//
// Three independent oracles decide whether the CURRENT DEFENDER may pass a set of
// same-valued cards:
//   1. ground truth — calculateLegalMoves (the bot enumerator the engine plays)
//   2. SERVER       — validatePass from actions/pass.ts (throw == illegal)
//   3. CLIENT       — canPass from src/utils/gameValidation.ts (the UI button gate)
// The invariant: all three must agree for the defender's own hand. A disagreement
// is the "I could pass legally but the client gave me no option" bug (or its dual).

import './harness.ts';
import { test, before, beforeEach, after } from 'node:test';
import assert from 'node:assert/strict';
import { applySchema, resetDb, seedGame, uuid, pgPool } from './harness.ts';
import { executeWithGameLock, loadCompleteGame } from '../server/impls/supabase/functions/_shared/adapter/utils.ts';
import { personalize_game } from '../server/api/common/common_utils.ts';
import { packedProducts, start_game_packed } from '../server/api/common/game_lifecycle.ts';
import { Game, AnimationEvent, PersonalGame, PrivatePlayer, GAME_STATUS, PLAYER_STATUS, STRATEGY_KEY, Card } from '../server/api/core/types.ts';
import { calculateLegalMoves } from '../server/api/common/bot_strategy.ts';
import { validatePass as serverValidatePass } from '../server/api/common/actions/pass.ts';
import { canPass as clientCanPass } from '../src/utils/gameValidation.ts';
import { legalMovesFor, applyPlayerMove } from './dispatch.ts';

const cardKey = (c: Card) => `${c.suit}:${c.value}`;

// Does the SERVER accept this pass? (throw == illegal)
function serverAllowsPass(game: Game, playerId: string, cards: Card[]): boolean {
    try { serverValidatePass(game, playerId, cards); return true; } catch { return false; }
}

// ===========================================================================
// Handpicked, deterministic validation scenarios (no fuzzing). Exported so the
// fast validation runner can execute exactly these.
// ===========================================================================
export function registerPassValidation(): void {
    const c = (suit: number, value: number): Card => ({ suit, value });
    interface PlayerSpec { status: 'in' | 'out'; hand: Card[] }

    // Minimal PLAYING game; validatePass / canPass / calculateLegalMoves read only these fields.
    const makeGame = (defender: number, players: PlayerSpec[], table: Game['table_battles'], powerSuit = 0): Game => ({
        id: 'g', name: 'g', deck_length: 0, discard_pile_length: 0, flipped: null,
        status: GAME_STATUS.PLAYING, power_suit: powerSuit, first_attacker: 0, defender,
        table_battles: table, good_timestamp: null, good_players: [], deck: [], logs: [],
        elimination_order: players.map((p, i) => (p.status === 'out' ? `P${i}` : '')).filter(Boolean),
        players: players.map((p, i): PrivatePlayer => ({
            player_id: `P${i}`, name: `P${i}`, status: p.status === 'out' ? PLAYER_STATUS.OUT : PLAYER_STATUS.IN,
            hand: p.hand, hand_length: p.hand.length, awaiting_attack: false, is_ai: false, strategy_key: STRATEGY_KEY.HUMAN,
        })),
    });

    const truthYes = (g: Game, pid: string, cards: Card[]): boolean => {
        const k = cards.map(cardKey).sort().join('|');
        return calculateLegalMoves(g, pid).some((m) => m.type === 'pass' && (m.cards as Card[]).map(cardKey).sort().join('|') === k);
    };
    const clientYes = (g: Game, defender: number, cards: Card[]): boolean =>
        clientCanPass(personalize_game(g, g.players[defender].player_id) as PersonalGame, cards);

    // All three oracles must agree with `expected` for this pass.
    const expectParity = (name: string, g: Game, defender: number, cards: Card[], expected: boolean) => {
        const pid = g.players[defender].player_id;
        const s = serverAllowsPass(g, pid, cards), t = truthYes(g, pid, cards), cl = clientYes(g, defender, cards);
        assert.equal(s, expected, `${name}: SERVER legality ${s} !== expected ${expected}`);
        assert.equal(t, expected, `${name}: GROUND-TRUTH legality ${t} !== expected ${expected}`);
        assert.equal(cl, expected, `${name}: CLIENT legality ${cl} !== expected ${expected} (client/server disagree)`);
    };

    // THE REGRESSION: defender at seat 1, seat 2 is OUT (empty hand), the real next
    // defender (wrapping past the out seat to seat 0) has room. Passing a third 8 is
    // legal — the client must not look at the out seat's empty hand and hide it.
    test('pass parity: legal pass when the seat after the defender is eliminated (the reported bug)', () => {
        const g = makeGame(1, [
            { status: 'in', hand: [c(0, 5), c(1, 6), c(2, 9), c(3, 10), c(0, 11)] }, // P0 real next defender, 5 cards
            { status: 'in', hand: [c(0, 8), c(1, 12), c(2, 13), c(3, 14)] },          // P1 defender, holds an 8
            { status: 'out', hand: [] },                                              // P2 eliminated
        ], [{ attack: c(3, 8), defense: null }, { attack: c(2, 8), defense: null }]);
        expectParity('out-seat-after-defender', g, 1, [c(0, 8)], true);
    });

    test('pass parity: blocked when the real next defender (past an out seat) lacks room', () => {
        const g = makeGame(1, [
            { status: 'in', hand: [c(0, 5), c(1, 6)] },                       // P0 real next defender, only 2 cards
            { status: 'in', hand: [c(0, 8), c(1, 12), c(2, 13), c(3, 14)] },  // P1 defender
            { status: 'out', hand: [] },                                      // P2 eliminated
        ], [{ attack: c(3, 8), defense: null }, { attack: c(2, 8), defense: null }]); // 2 + 1 = 3 > 2
        expectParity('out-seat-real-next-too-small', g, 1, [c(0, 8)], false);
    });

    test('pass parity: ordinary legal pass with all players in', () => {
        const g = makeGame(0, [
            { status: 'in', hand: [c(0, 7), c(1, 12), c(2, 13)] },            // P0 defender, holds a 7
            { status: 'in', hand: [c(0, 5), c(1, 6), c(2, 9), c(3, 10)] },    // P1 next defender, 4 cards
            { status: 'in', hand: [c(0, 11), c(2, 11)] },                     // P2
        ], [{ attack: c(1, 7), defense: null }]);
        expectParity('ordinary-legal', g, 0, [c(0, 7)], true);
    });

    test('pass parity: ordinary pass blocked when next defender lacks room', () => {
        const g = makeGame(0, [
            { status: 'in', hand: [c(0, 7), c(1, 7), c(2, 13)] },             // P0 defender, two 7s
            { status: 'in', hand: [c(0, 5)] },                               // P1 next defender, 1 card
            { status: 'in', hand: [c(0, 11), c(2, 11)] },                     // P2
        ], [{ attack: c(1, 7), defense: null }, { attack: c(2, 7), defense: null }]); // 2 + 1 = 3 > 1
        expectParity('ordinary-blocked', g, 0, [c(0, 7)], false);
    });

    test('pass parity: cannot pass once a battle is covered', () => {
        const g = makeGame(0, [
            { status: 'in', hand: [c(0, 7), c(1, 12)] },                      // P0 defender, holds a 7
            { status: 'in', hand: [c(0, 5), c(1, 6), c(2, 9), c(3, 10)] },    // P1 next defender
            { status: 'in', hand: [c(0, 11), c(2, 11)] },                     // P2
        ], [{ attack: c(1, 7), defense: c(1, 13) }]); // already covered
        expectParity('covered-no-pass', g, 0, [c(0, 7)], false);
    });
}

// ===========================================================================
// Exhaustive fuzzer + its validation cases — only when run directly (full e2e),
// NOT when the validation runner imports this file for registerPassValidation.
// ===========================================================================
if (!process.env.VALIDATION_ONLY) {
    // Every non-empty same-value subset of the defender's hand whose value also
    // appears on the table — the candidate space we ask all three oracles about.
    const candidatePassSets = (defender: PrivatePlayer, game: Game): Card[][] => {
        if (game.table_battles.length === 0) return [];
        const tableValues = new Set(game.table_battles.map((b) => b.attack.value));
        const byValue = new Map<number, Card[]>();
        for (const c of defender.hand) {
            if (!tableValues.has(c.value)) continue;
            (byValue.get(c.value) ?? byValue.set(c.value, []).get(c.value)!).push(c);
        }
        const out: Card[][] = [];
        for (const cards of byValue.values()) {
            const n = Math.min(cards.length, 6);
            for (let mask = 1; mask < (1 << n); mask++) {
                const set: Card[] = [];
                for (let i = 0; i < n; i++) if (mask & (1 << i)) set.push(cards[i]);
                out.push(set);
            }
        }
        return out;
    };

    let seed = Number(process.env.FUZZ_SEED || 0x1234abcd) >>> 0;
    const rnd = () => { seed = (seed * 1664525 + 1013904223) >>> 0; return seed / 0x100000000; };
    const ri = (n: number) => Math.floor(rnd() * n);
    const pick = <T>(a: T[]): T => a[ri(a.length)];

    // Top-level import, not an `await import` per iteration: the e2e runner's TS
    // loader re-resolves a dynamic import on every call (~1.9ms) even for a module
    // already in the registry, and this fuzzer calls it 8000 times.
    const loadGame = (gameId: string): Promise<Game> => loadCompleteGame(gameId);
    const freshGame = async (): Promise<string> => {
        const gameId = `p${uuid().slice(0, 6)}`;
        // Three players so the game eliminates someone mid-play, producing the
        // OUT-player-between-defender-and-next states the bug needs.
        await seedGame(gameId, [
            { id: uuid(), name: 'H0', is_ai: false, strategy_key: 'human' },
            { id: uuid(), name: 'H1', is_ai: false, strategy_key: 'human' },
            { id: uuid(), name: 'B0', is_ai: true, strategy_key: 'random' },
        ]);
        await executeWithGameLock(gameId, async (g) => ({ game: g, events: [], packed: packedProducts(start_game_packed(g)) }), 'start', false);
        return gameId;
    };

    before(async () => { await applySchema(); });
    beforeEach(async () => { await resetDb(); });

    test('pass parity fuzz: client offers exactly the legal passes the server accepts', async () => {
        const ITER = Number(process.env.FUZZ_ITERS || 8000);
        const seedLabel = '0x' + (Number(process.env.FUZZ_SEED || 0x1234abcd) >>> 0).toString(16);
        let gameId = await freshGame();
        let checkedStates = 0, sawEliminations = 0;
        const hiddenLegal: string[] = [];    // server YES, client NO (the reported bug)
        const offeredIllegal: string[] = []; // client YES, server NO (the dual)

        for (let i = 0; i < ITER; i++) {
            let g = await loadGame(gameId);
            if (g.status !== 'playing') { gameId = await freshGame(); g = await loadGame(gameId); }

            const defender = g.players[g.defender];
            if (defender && defender.status === PLAYER_STATUS.IN && g.table_battles.length > 0) {
                if (g.players.some((p) => p.status === PLAYER_STATUS.OUT)) sawEliminations++;
                const personal = personalize_game(g, defender.player_id) as PersonalGame;
                const truthKeys = new Set(calculateLegalMoves(g, defender.player_id)
                    .filter((m) => m.type === 'pass').map((m) => (m.cards as Card[]).map(cardKey).sort().join('|')));

                for (const cards of candidatePassSets(defender, g)) {
                    const k = cards.map(cardKey).sort().join('|');
                    const truthYes = truthKeys.has(k);
                    const serverYes = serverAllowsPass(g, defender.player_id, cards);
                    const clientYes = clientCanPass(personal, cards);
                    checkedStates++;
                    const detail = `seed=${seedLabel} iter=${i} truth=${truthYes} cards=[${cards.map(cardKey).join(',')}] `
                        + `defender=#${g.defender}(${defender.name}) players=[${g.players.map((p, idx) => `${idx}:${p.name}:${p.status}:h${p.hand.length}`).join(' ')}] `
                        + `table=[${g.table_battles.map((b) => `${cardKey(b.attack)}${b.defense ? '/' + cardKey(b.defense) : ''}`).join(' ')}]`;
                    if (serverYes && !clientYes && hiddenLegal.length < 3) hiddenLegal.push(detail);
                    if (clientYes && !serverYes && offeredIllegal.length < 3) offeredIllegal.push(detail);
                }
            }

            const moves = legalMovesFor(g);
            if (moves.length) {
                try { await executeWithGameLock(gameId, async (gg) => ({ game: gg, ...applyPlayerMove(gg, pick(moves)) }), `m${i}`, true); } catch { /* race -> no-op */ }
            } else { gameId = await freshGame(); }
        }

        console.error(`[pass-parity] checked=${checkedStates} statesWithEliminations=${sawEliminations} hiddenLegal=${hiddenLegal.length} offeredIllegal=${offeredIllegal.length}`);
        assert.equal(hiddenLegal.length, 0, `CLIENT hides a pass the SERVER accepts (legal pass, no button):\n  ${hiddenLegal.join('\n  ')}`);
        assert.equal(offeredIllegal.length, 0, `CLIENT offers a pass the SERVER rejects:\n  ${offeredIllegal.join('\n  ')}`);
    });

    // The handpicked cases also run as part of the full e2e suite.
    registerPassValidation();

    after(async () => { await pgPool.end(); });
}
