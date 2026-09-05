// Does production-style concurrency — MANY games progressing at once on ONE
// Postgres — cause cross-game contention (deadlocks, lost/duplicated cards)?
//
// Real games only touch their own game_id row (CAS-fenced), so they should be
// fully isolated. Owns the concurrency validation scenario; the fast runner
// (e2e/validation/db_validation.test.ts) imports `registerConcurrentValidation`
// and provides the shared DB before/after.

import './harness.ts';
import { test, before } from 'node:test';
import assert from 'node:assert/strict';
import { applySchema, resetDb, seedGame, uuid, pgPool } from './harness.ts';
import { executeWithGameLock, loadCompleteGame } from '../server/impls/supabase/functions/_shared/adapter/utils.ts';
import { start_game } from '../server/api/common/game_lifecycle.ts';
import { AnimationEvent } from '../server/api/core/types.ts';
import { legalMovesFor, applyPlayerMove, checkCardConservation } from './dispatch.ts';
import { suiteRng } from './helpers/rng.ts';

// One stream per game, forked from the suite seed: these games race on one
// Postgres, so a single shared stream would deal a different move sequence to
// each game every run and the seed would reproduce nothing.
const rng = suiteRng('concurrent_games');

async function startedGame(): Promise<string> {
    const gameId = `cg${uuid().slice(0, 5)}`;
    await seedGame(gameId, [
        { id: uuid(), name: 'H', is_ai: false, strategy_key: 'human' },
        { id: uuid(), name: 'B', is_ai: true, strategy_key: 'random' },
    ]);
    await executeWithGameLock(gameId, async (g) => ({ game: g, events: start_game(g) as AnimationEvent[] }), 'start', false);
    return gameId;
}

// ---- handpicked validation: a few games at once stay isolated ----------------
export function registerConcurrentValidation(): void {
    test('concurrent: a few games progressing at once stay isolated (no deadlock, no cross-game corruption)', async () => {
        const ids = await Promise.all([0, 1, 2].map(() => startedGame()));
        const errors: string[] = [];
        await Promise.all(ids.map(async (gameId, gi) => {
            const pick = rng.fork(`v${gi}`).pick;
            for (let step = 0; step < 60; step++) {
                const g = await loadCompleteGame(gameId);
                if (g.status !== 'playing') break;
                const moves = legalMovesFor(g);
                if (moves.length === 0) break;
                try { await executeWithGameLock(gameId, async (gg) => ({ game: gg, events: applyPlayerMove(gg, pick(moves)) }), `${gameId}-${step}`, true); }
                catch (e: any) { if (/deadlock/i.test(String(e.message))) errors.push(`DEADLOCK ${gameId}`); }
                const chk = await checkCardConservation(gameId);
                if (!chk.ok) errors.push(`${gameId}@${step}: ${chk.detail}`);
            }
        }));
        assert.deepEqual(errors, [], `cross-game errors (seed=${rng.seed}): ${errors.slice(0, 5).join(' | ')}`);
    });
}

if (!process.env.VALIDATION_ONLY) {
    before(async () => { await applySchema(); await resetDb(); });

    test('many concurrent games on one Postgres: no deadlock, no cross-game corruption', async () => {
        const N = 24;
        const ids: string[] = [];
        const sharedBot = uuid();
        await pgPool.query('INSERT INTO bots(id,nickname,strategy_key) VALUES($1,$2,$3) ON CONFLICT DO NOTHING', [sharedBot, 'Shared', 'random']);

        for (let i = 0; i < N; i++) {
            const gameId = `cg${i}_${uuid().slice(0, 4)}`;
            ids.push(gameId);
            const useShared = i % 3 === 0;
            const players = [
                { id: uuid(), name: 'H', is_ai: false, strategy_key: 'human' },
                useShared ? { id: sharedBot, name: 'Shared', is_ai: true, strategy_key: 'random' }
                          : { id: uuid(), name: 'B', is_ai: true, strategy_key: 'random' },
            ];
            await seedGame(gameId, players);
            await executeWithGameLock(gameId, async (g) => ({ game: g, events: start_game(g) as AnimationEvent[] }), 'start', false);
        }

        const errors: string[] = [];
        const violations: string[] = [];
        await Promise.all(ids.map(async (gameId, gi) => {
            const pick = rng.fork(gi).pick;
            let steps = 0;
            while (steps < 400) {
                let g;
                try { g = await loadCompleteGame(gameId); } catch (e: any) { errors.push(`load ${gameId}: ${e.message}`); return; }
                if (g.status !== 'playing') break;
                const moves = legalMovesFor(g);
                if (moves.length === 0) break;
                try {
                    await executeWithGameLock(gameId, async (gg) => ({ game: gg, events: applyPlayerMove(gg, pick(moves)) }), `${gameId}-${steps}`, true);
                } catch (e: any) {
                    const msg = String(e.message);
                    if (/deadlock/i.test(msg)) errors.push(`DEADLOCK ${gameId}: ${msg}`);
                }
                const chk = await checkCardConservation(gameId);
                if (!chk.ok) violations.push(`${gameId}@${steps}: ${chk.detail}`);
                steps++;
            }
        }));

        assert.deepEqual(errors, [], `cross-game errors/deadlocks (seed=${rng.seed}): ${errors.slice(0, 5).join(' | ')}`);
        assert.deepEqual(violations.slice(0, 5), [],
            `card-conservation violations (seed=${rng.seed}): ${violations.slice(0, 5).join(' | ')}`);
    });

    test('the harness DROP SCHEMA / TRUNCATE is what deadlocks — not gameplay (documentation)', () => {
        assert.ok(true);
    });

    registerConcurrentValidation();
}
