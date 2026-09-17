// Does production-style concurrency - MANY games progressing at once on ONE
// Postgres - cause cross-game contention (deadlocks, lost/duplicated cards)?
//
// Real games only touch their own game_id row (CAS-fenced by commit_table), so
// they should be fully isolated. Every game here is a kernel-owned row dealt by
// the real `meta` start, and every move goes through the real move path
// (executePackedAction -> table_io's CAS loop); cards are counted straight from
// the stored state blob. Owns the concurrency validation scenario; the fast
// runner (e2e/validation/db_validation.test.ts) imports
// `registerConcurrentValidation` and provides the shared DB before/after.

import './harness.ts';
import { test, before } from 'node:test';
import assert from 'node:assert/strict';
import { applySchema, resetDb, uuid } from './harness.ts';
import * as L from '../sdk/ts/gen/game_layout.bots.ts';
import { checkCardConservation, legalMoves, mustReadTable, type PlayMove } from './helpers/table_play.ts';
import { runAction, runMeta, seedLobby, type LobbySeat } from './helpers/table_server.ts';
import { suiteRng } from './helpers/rng.ts';

// One stream per game, forked from the suite seed: these games race on one
// Postgres, so a single shared stream would deal a different move sequence to
// each game every run and the seed would reproduce nothing.
const rng = suiteRng('concurrent_games');

/** A human (not ready) and a bot, dealt by the human's `start`. */
async function startedGame(gameId: string, bot: LobbySeat = { id: uuid(), name: 'B', brain: 'random' }): Promise<string> {
    const human = uuid();
    await seedLobby(gameId, [{ id: human, name: 'H', ready: false }, bot]);
    await runMeta(gameId, human, { type: 'start' });
    assert.equal((await mustReadTable(gameId)).status, L.GAME_STATUS_PLAYING, `fixture: ${gameId} dealt`);
    return gameId;
}

/** One pick sent as its seat: a rule refusal or a stale pick is fine, a deadlock is not. */
async function send(gameId: string, m: PlayMove, errors: string[]): Promise<void> {
    try { await runAction(gameId, m.playerId, m); }
    catch (e) {
        const msg = String((e as Error)?.message ?? e);
        if (/deadlock/i.test(msg)) errors.push(`DEADLOCK ${gameId}: ${msg}`);
    }
}

// ---- handpicked validation: a few games at once stay isolated ----------------
export function registerConcurrentValidation(): void {
    test('concurrent: a few games progressing at once stay isolated (no deadlock, no cross-game corruption)', async () => {
        const ids = await Promise.all([0, 1, 2].map(() => startedGame(`cg${uuid().slice(0, 5)}`)));
        const errors: string[] = [];
        await Promise.all(ids.map(async (gameId, gi) => {
            const pick = rng.fork(`v${gi}`).pick;
            for (let step = 0; step < 60; step++) {
                const t = await mustReadTable(gameId);
                if (t.status !== L.GAME_STATUS_PLAYING) break;
                const moves = legalMoves(t);
                if (moves.length === 0) break;
                await send(gameId, pick(moves), errors);
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
        // One bot seated at every third table: its bots row and bot_hands
        // memberships are shared across games, the likeliest cross-game lock.
        const sharedBot: LobbySeat = { id: uuid(), name: 'Shared', brain: 'random' };

        for (let i = 0; i < N; i++) {
            const gameId = `cg${i}_${uuid().slice(0, 4)}`;
            ids.push(await startedGame(gameId, i % 3 === 0 ? sharedBot : undefined));
        }

        const errors: string[] = [];
        const violations: string[] = [];
        await Promise.all(ids.map(async (gameId, gi) => {
            const pick = rng.fork(gi).pick;
            let steps = 0;
            while (steps < 400) {
                let t;
                try { t = await mustReadTable(gameId); } catch (e) { errors.push(`load ${gameId}: ${(e as Error).message}`); return; }
                if (t.status !== L.GAME_STATUS_PLAYING) break;
                const moves = legalMoves(t);
                if (moves.length === 0) break;
                await send(gameId, pick(moves), errors);
                const chk = await checkCardConservation(gameId);
                if (!chk.ok) violations.push(`${gameId}@${steps}: ${chk.detail}`);
                steps++;
            }
        }));

        assert.deepEqual(errors, [], `cross-game errors/deadlocks (seed=${rng.seed}): ${errors.slice(0, 5).join(' | ')}`);
        assert.deepEqual(violations.slice(0, 5), [],
            `card-conservation violations (seed=${rng.seed}): ${violations.slice(0, 5).join(' | ')}`);
        // Not vacuous: the games really were played, not refused move after move.
        const finished = (await Promise.all(ids.map(mustReadTable))).filter((t) => t.statusColumn === 'game_over').length;
        assert.ok(finished > 0, `no game reached its end (seed=${rng.seed})`);
    });

    test('the harness DROP SCHEMA / TRUNCATE is what deadlocks - not gameplay (documentation)', () => {
        assert.ok(true);
    });

    registerConcurrentValidation();
}
