/* =============================================================================
 * The finish screen shows what THIS game did to each rating
 * =============================================================================
 * The bug, as a player met it: finish a game, and the finish screen says
 * "1000 -> 1005, +5". Come back to that same finished table after playing
 * another game, and it says something else - because the screen did not show
 * what the game did. It read each player's "rating now" and "rating before my
 * last game" and subtracted, which is that PLAYER's last change, in whatever
 * game it came from. A bot sits at many tables at once, so its row was wrong
 * within seconds; and the write behind those two columns was an unfenced
 * read-modify-write, so two games that shared a seat could lose one of them.
 *
 * This file is that walk, end to end: the deployed page (ServerProvider,
 * GameView, WinScreen) against the real edge handlers and a real Postgres
 * (e2e/helpers/live_page.ts).
 *
 *   1. Ana and a bot finish game A. Ana's page shows A's changes.
 *   2. The same bot finishes game B with Bo, and Ana finishes game C with
 *      another bot. Every rating row A's screen used to read has now moved on.
 *   3. Ana opens A again. It shows exactly what it showed in step 1 - and so
 *      does a spectator who was never at the table.
 *
 * And the totals: the bot's rating row carries BOTH its games, each one's change
 * as that game's screen shows it.
 * ========================================================================== */

import { LivePage } from './helpers/live_page.ts';
import { test, before } from 'node:test';
import assert from 'node:assert/strict';
import { applySchema, pgPool, resetDb, uuid } from './harness.ts';
import * as L from '../sdk/ts/gen/game_layout.bots.ts';
import { legalMoves, mustReadTable } from './helpers/table_play.ts';
import { runAction, runMeta, seedLobby, type LobbySeat } from './helpers/table_server.ts';
import { __setTableDealSeedOverride } from '../server/impls/supabase/functions/_shared/adapter/table_io.ts';
import { __clearGameCache } from '../server/impls/supabase/functions/_shared/adapter/game_cache.ts';

before(async () => { await applySchema(); });

/** Deals `seats` at a new table and plays it out through the real move path. Returns the game id. */
async function playOut(tag: string, seats: LobbySeat[], seed: number): Promise<string> {
    const id = `${tag}${uuid().slice(0, 6)}`;
    await seedLobby(id, seats);
    __setTableDealSeedOverride(Uint8Array.from({ length: 32 }, (_, i) => (i * 71 + seed) & 0xff));
    await runMeta(id, seats[0].id, { type: 'start' });
    for (let step = 0; step < 600; step++) {
        const t = await mustReadTable(id);
        if (t.status !== L.GAME_STATUS_PLAYING) break;
        const mv = legalMoves(t)[0];
        assert.ok(mv, `${id}: somebody can move (step ${step})`);
        await runAction(id, mv.playerId, mv);
    }
    assert.equal((await mustReadTable(id)).statusColumn, 'game_over', `${id}: the game ended`);
    return id;
}

/** What the finish screen of `gameId` says about each seat's rating, as `viewer` reads it: "name: before -> after change". */
async function finishScreen(gameId: string, viewer: string): Promise<string[]> {
    const page = new LivePage();
    await page.mount(gameId, viewer);
    try {
        for (let t = 0; t < 3000 && !page.host.querySelector('.result-card'); t += 50) await page.advance(50);
        const cards = Array.from(page.host.querySelectorAll('.result-card'));
        assert.ok(cards.length > 0, `the finish screen of ${gameId} is up (the page drew: ${page.host.textContent?.slice(0, 160)})`);
        return cards.map((card) => {
            const name = card.querySelector('.result-card__name')?.textContent ?? '?';
            const elo = card.querySelector('.result-card__elo');
            return `${name}: ${elo ? Array.from(elo.children).map((c) => c.textContent).join(' ') : 'no rating'}`;
        });
    } finally { await page.unmount(); }
}

const ratingOf = async (table: 'bots' | 'user_elo_ratings', id: string): Promise<{ elo: number; played: number }> => {
    const { rows } = await pgPool.query(`SELECT elo_rating, games_played FROM ${table} WHERE ${table === 'bots' ? 'id' : 'user_id'} = $1`, [id]);
    return { elo: rows[0].elo_rating, played: rows[0].games_played };
};
/** The signed change a finish-screen row shows ("Ana: 1000 → 1005 +5" -> 5). */
const changeIn = (rows: string[], name: string): number => {
    const row = rows.find((r) => r.startsWith(`${name}:`));
    const m = row?.match(/([+-]?\d+)$/);
    assert.ok(m, `a rating row for ${name} (${rows.join(' | ')})`);
    return Number(m[1]);
};

test('a finished game keeps showing its own rating changes after its players finish other games', async () => {
    await resetDb();
    __clearGameCache();
    const [ana, bo, sam] = [uuid(), uuid(), uuid()];
    const [miami, vienna] = [uuid(), uuid()];
    await pgPool.query('INSERT INTO auth.users(id) VALUES ($1) ON CONFLICT DO NOTHING', [sam]);
    try {
        // 1. Game A: Ana and the Miami bot.
        const a = await playOut('a', [{ id: ana, name: 'Ana' }, { id: miami, name: '%Miami 1', brain: 'random' }], 5);
        const first = await finishScreen(a, ana);
        assert.equal(first.length, 2, 'two rows');
        assert.ok(first.every((r) => /: 1000 → \d+ [+-]?\d+$/.test(r)), `each row shows a rating that started at 1000 and how it changed (${first.join(' | ')})`);
        const [anaInA, miamiInA] = [changeIn(first, 'Ana'), changeIn(first, 'Miami 1')];
        assert.ok(anaInA !== 0 && anaInA === -miamiInA, `an even pair: one gains what the other loses (${first.join(' | ')})`);
        assert.deepEqual(await ratingOf('user_elo_ratings', ana), { elo: 1000 + anaInA, played: 1 }, 'Ana\'s rating row carries game A');
        assert.deepEqual(await ratingOf('bots', miami), { elo: 1000 + miamiInA, played: 1 }, 'and so does the bot\'s');

        // 2. Every rating A's screen used to read moves on: the bot finishes
        //    game B with Bo, and Ana finishes game C with another bot.
        const b = await playOut('b', [{ id: bo, name: 'Bo' }, { id: miami, name: '%Miami 1', brain: 'random' }], 9);
        const c = await playOut('c', [{ id: ana, name: 'Ana' }, { id: vienna, name: '%Vienna 1', brain: 'random' }], 13);
        const miamiInB = changeIn(await finishScreen(b, bo), 'Miami 1');
        const anaInC = changeIn(await finishScreen(c, ana), 'Ana');
        assert.deepEqual(await ratingOf('bots', miami), { elo: 1000 + miamiInA + miamiInB, played: 2 },
            'the bot\'s row carries BOTH of its games, each as its own finish screen shows it');
        assert.deepEqual(await ratingOf('user_elo_ratings', ana), { elo: 1000 + anaInA + anaInC, played: 2 }, 'and Ana\'s carries both of hers');

        // 3. Game A's screen, opened again, is game A's.
        assert.deepEqual(await finishScreen(a, ana), first, 'Ana reopens game A: the same rows, to the character');
        assert.deepEqual(await finishScreen(a, sam), first, 'and a spectator who was never at the table reads the same');
    } finally { __setTableDealSeedOverride(null); }
});
