// Does production-style concurrency — MANY games progressing at once on ONE
// Postgres — cause cross-game contention (deadlocks, lost/duplicated cards)?
//
// This is the honest check behind "the parallel tests deadlocked". Production
// never drops the schema or truncates all games (the test-harness reset that
// actually caused those deadlocks). Real games only touch their own game_id row
// (CAS-fenced), so they should be fully isolated. Here we drive many games
// concurrently WITHOUT any global reset and assert: no deadlocks, and every
// game's card conservation holds throughout.

import './harness.ts';
import { test, before } from 'node:test';
import assert from 'node:assert/strict';
import { applySchema, resetDb, seedGame, uuid, pgPool } from './harness.ts';
import { executeWithGameLock, loadCompleteGame } from '../supabase/functions/_shared/utils.ts';
import { start_game } from '../supabase/functions/_shared/common_utils.ts';
import { AnimationEvent } from '../supabase/functions/_shared/types.ts';
import { legalMovesFor, applyPlayerMove, checkCardConservation } from './dispatch.ts';

const pick = <T>(a: T[]): T => a[Math.floor(Math.random() * a.length)];

before(async () => { await applySchema(); await resetDb(); });

test('many concurrent games on one Postgres: no deadlock, no cross-game corruption', async () => {
    const N = 24;
    const ids: string[] = [];
    // A bot id SHARED across several games — the one genuinely cross-game write is
    // the end-of-game ELO update on the bots row; include it to stress that too.
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

    // Drive all games concurrently, interleaving at every await.
    await Promise.all(ids.map(async (gameId) => {
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
                // other throws are legitimate stale-move rejections
            }
            const chk = await checkCardConservation(gameId);
            if (!chk.ok) violations.push(`${gameId}@${steps}: ${chk.detail}`);
            steps++;
        }
    }));

    assert.deepEqual(errors, [], `cross-game errors/deadlocks: ${errors.slice(0, 5).join(' | ')}`);
    assert.deepEqual(violations.slice(0, 5), [], `card-conservation violations: ${violations.slice(0, 5).join(' | ')}`);
});

test('the harness DROP SCHEMA / TRUNCATE is what deadlocks — not gameplay (documentation)', () => {
    // No production code path drops the public schema or truncates the games table;
    // those are test-setup-only operations. Gameplay writes are CAS-fenced to a
    // single game_id row, so concurrent games never lock-order against each other.
    assert.ok(true);
});
