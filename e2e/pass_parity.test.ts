// Pass legality PARITY fuzzer. Drives real games to completion (so players get
// eliminated) and, at every state, asks three independent oracles whether the
// CURRENT DEFENDER may pass a given set of same-valued cards:
//
//   1. ground truth  — calculateLegalMoves (the bot enumerator, the canonical
//                       legal-move set the engine actually plays)
//   2. SERVER         — validatePass from actions/pass.ts (the authoritative
//                       handler the action endpoint runs; throw == illegal)
//   3. CLIENT         — canPass from src/utils/gameValidation.ts (the exact
//                       predicate the UI uses to decide whether to show the
//                       Pass button / accept a drag)
//
// The invariant under test: all three must agree for the defender's own hand.
// A disagreement is the "I could pass legally but the client gave me no option"
// bug (or its dual — the client offering a pass the server rejects).

import './harness.ts';
import { test, before, beforeEach, after } from 'node:test';
import assert from 'node:assert/strict';
import { applySchema, resetDb, seedGame, uuid, pgPool } from './harness.ts';
import { executeWithGameLock } from '../supabase/functions/_shared/utils.ts';
import { start_game, personalize_game } from '../supabase/functions/_shared/common_utils.ts';
import { Game, AnimationEvent, PersonalGame, PrivatePlayer, PLAYER_STATUS, Card } from '../supabase/functions/_shared/types.ts';
import { calculateLegalMoves } from '../supabase/functions/_shared/bot_strategy.ts';
import { validatePass as serverValidatePass } from '../supabase/functions/_shared/actions/pass.ts';
import { canPass as clientCanPass } from '../src/utils/gameValidation.ts';
import { legalMovesFor, applyPlayerMove } from './dispatch.ts';

// Deterministic RNG so a found mismatch reproduces from the printed seed.
let seed = Number(process.env.FUZZ_SEED || 0x1234abcd) >>> 0;
const rnd = () => { seed = (seed * 1664525 + 1013904223) >>> 0; return seed / 0x100000000; };
const ri = (n: number) => Math.floor(rnd() * n);
const pick = <T>(a: T[]): T => a[ri(a.length)];

const cardKey = (c: Card) => `${c.suit}:${c.value}`;

// Every non-empty same-value subset of the defender's hand whose value also
// appears on the table — the only card-sets a pass could conceivably use. This
// is the candidate space we ask all three oracles about (capped per value so a
// pathological hand can't blow up; real decks have <=4 of a value anyway).
function candidatePassSets(defender: PrivatePlayer, game: Game): Card[][] {
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
}

// Does the SERVER accept this pass? (throw == illegal)
function serverAllowsPass(game: Game, playerId: string, cards: Card[]): boolean {
    try { serverValidatePass(game, playerId, cards); return true; } catch { return false; }
}

async function loadGame(gameId: string): Promise<Game> {
    const { loadCompleteGame } = await import('../supabase/functions/_shared/utils.ts');
    return loadCompleteGame(gameId);
}
async function freshGame(): Promise<string> {
    const gameId = `p${uuid().slice(0, 6)}`;
    // Three players so the game actually eliminates someone mid-play, producing
    // the OUT-player-between-defender-and-next states the bug needs.
    await seedGame(gameId, [
        { id: uuid(), name: 'H0', is_ai: false, strategy_key: 'human' },
        { id: uuid(), name: 'H1', is_ai: false, strategy_key: 'human' },
        { id: uuid(), name: 'B0', is_ai: true, strategy_key: 'random' },
    ]);
    await executeWithGameLock(gameId, async (g) => ({ game: g, events: start_game(g) as AnimationEvent[] }), 'start', false);
    return gameId;
}

before(async () => { await applySchema(); });
beforeEach(async () => { await resetDb(); });

test('pass parity: client offers exactly the legal passes the server accepts', async () => {
    const ITER = Number(process.env.FUZZ_ITERS || 8000);
    const seedLabel = '0x' + (Number(process.env.FUZZ_SEED || 0x1234abcd) >>> 0).toString(16);

    let gameId = await freshGame();
    let checkedStates = 0, sawEliminations = 0;
    // Two disagreement buckets, each with a reproducing example.
    const hiddenLegal: string[] = [];   // server YES, client NO  (the reported bug)
    const offeredIllegal: string[] = []; // client YES, server NO  (the dual)

    for (let i = 0; i < ITER; i++) {
        let g = await loadGame(gameId);
        if (g.status !== 'playing') { gameId = await freshGame(); g = await loadGame(gameId); }

        // Inspect the current defender's pass options against all three oracles
        // BEFORE advancing the game.
        const defender = g.players[g.defender];
        if (defender && defender.status === PLAYER_STATUS.IN && g.table_battles.length > 0) {
            const outBetween = g.players.some((p) => p.status === PLAYER_STATUS.OUT);
            if (outBetween) sawEliminations++;

            const personal = personalize_game(g, defender.player_id) as PersonalGame;

            // Ground-truth legal passes (what the engine would actually allow).
            const truthPasses = calculateLegalMoves(g, defender.player_id)
                .filter((m) => m.type === 'pass')
                .map((m) => m.cards as Card[]);
            // Re-key truth passes so we can match candidate sets to them.
            const truthKeys = new Set(truthPasses.map((cs) => cs.map(cardKey).sort().join('|')));

            for (const cards of candidatePassSets(defender, g)) {
                const k = cards.map(cardKey).sort().join('|');
                const truthYes = truthKeys.has(k);
                const serverYes = serverAllowsPass(g, defender.player_id, cards);
                const clientYes = clientCanPass(personal, cards);
                checkedStates++;

                // The bug the user described: a legal pass the client refuses to offer.
                if (serverYes && !clientYes && hiddenLegal.length < 3) {
                    hiddenLegal.push(`seed=${seedLabel} iter=${i} truth=${truthYes} cards=[${cards.map(c => cardKey(c)).join(',')}] `
                        + `defender=#${g.defender}(${defender.name}) players=[${g.players.map((p, idx) => `${idx}:${p.name}:${p.status}:h${p.hand.length}`).join(' ')}] `
                        + `table=[${g.table_battles.map(b => `${cardKey(b.attack)}${b.defense ? '/' + cardKey(b.defense) : ''}`).join(' ')}]`);
                }
                // The dual: a pass the client would offer that the server rejects.
                if (clientYes && !serverYes && offeredIllegal.length < 3) {
                    offeredIllegal.push(`seed=${seedLabel} iter=${i} truth=${truthYes} cards=[${cards.map(c => cardKey(c)).join(',')}] `
                        + `defender=#${g.defender}(${defender.name}) players=[${g.players.map((p, idx) => `${idx}:${p.name}:${p.status}:h${p.hand.length}`).join(' ')}] `
                        + `table=[${g.table_battles.map(b => `${cardKey(b.attack)}${b.defense ? '/' + cardKey(b.defense) : ''}`).join(' ')}]`);
                }
            }
        }

        // Advance with a random legal move to keep the game evolving.
        const moves = legalMovesFor(g);
        if (moves.length) {
            try { await executeWithGameLock(gameId, async (gg) => ({ game: gg, events: applyPlayerMove(gg, pick(moves)) }), `m${i}`, true); } catch { /* race -> no-op */ }
        } else {
            gameId = await freshGame();
        }
    }

    console.error(`[pass-parity] checked=${checkedStates} statesWithEliminations=${sawEliminations} hiddenLegal=${hiddenLegal.length} offeredIllegal=${offeredIllegal.length}`);

    assert.equal(hiddenLegal.length, 0, `CLIENT hides a pass the SERVER accepts (legal pass, no button):\n  ${hiddenLegal.join('\n  ')}`);
    assert.equal(offeredIllegal.length, 0, `CLIENT offers a pass the SERVER rejects:\n  ${offeredIllegal.join('\n  ')}`);
});

after(async () => { await pgPool.end(); });
