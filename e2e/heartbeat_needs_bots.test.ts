// The bot heartbeat's scan dispatches exactly the playing games with a bot still
// IN (docs/C_GAME_SHAPE_MIGRATION.md 2.7, Phase 4b).
//
// Which game has bot work is the kernel's verdict (table_needs_bots), stored as
// games.needs_bots by every commit; the scan adds only the staleness window.
// Every row here is kernel-owned (seedTable), aged with raw SQL, and the SCAN
// runs through the real bot-heartbeat entry point. Its self-dispatches are the
// POSTs to /functions/v1/bot-heartbeat, recorded here instead of sent.

import './harness.ts';
import { test, before, after } from 'node:test';
import assert from 'node:assert/strict';
import { applySchema, pgPool, uuid } from './harness.ts';
import { postJson, settle, tokenFor } from './helpers/edge.ts';
import { fixture, GAME_OVER, PLAYING, READY } from './helpers/table_fixture.ts';
import { seedTable } from './helpers/table_db.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; console.error = () => {}; }

// Record the scan's self-dispatches.
const dispatched: string[] = [];
const passThrough = globalThis.fetch;
globalThis.fetch = (async (input: unknown, init?: RequestInit) => {
    const url = typeof input === 'string' ? input : String((input as { url?: string })?.url ?? '');
    if (url.endsWith('/functions/v1/bot-heartbeat')) {
        dispatched.push(JSON.parse(String(init?.body)).game_id);
        return new Response('{}', { status: 200 });
    }
    return passThrough(input as RequestInfo, init);
}) as typeof fetch;

before(async () => { await applySchema(); });
after(async () => { await settle(); });

// The row's last commit, moved into the past. update_games_updated_at would stamp
// the UPDATE itself with now(), so it is paused for the statement.
async function age(gameId: string, seconds: number): Promise<void> {
    const c = await pgPool.connect();
    try {
        await c.query('BEGIN');
        await c.query('ALTER TABLE games DISABLE TRIGGER update_games_updated_at');
        await c.query(`UPDATE games SET updated_at = now() - make_interval(secs => $2) WHERE id = $1`, [gameId, seconds]);
        await c.query('ALTER TABLE games ENABLE TRIGGER update_games_updated_at');
        await c.query('COMMIT');
    } finally {
        c.release();
    }
}

test('SCAN dispatches the stalled playing games with a bot IN, and nothing else', async () => {
    const h1 = uuid(), h2 = uuid(), bot = uuid(), bot2 = uuid();
    const g = (tag: string) => `${tag}${uuid().slice(0, 5)}`;
    const rows: Record<string, string> = {};

    // A bot IN mid-game, untouched for 30 s: the one game to drive.
    rows.botIn = g('bi');
    await seedTable(rows.botIn, fixture().seats([{ id: h1, name: 'Hana' }, { id: bot, name: 'Bolt', brain: 'random' }])
        .status(PLAYING).hand(0, '6h 7h').hand(1, '8h 9h').deck('Th Jh').trump('Qc').build());
    // Every seat human.
    rows.humans = g('hu');
    await seedTable(rows.humans, fixture().seats([{ id: h1, name: 'Hana' }, { id: h2, name: 'Ivo' }])
        .status(PLAYING).hand(0, '6h 7h').hand(1, '8h 9h').deck('Th Jh').trump('Qc').build());
    // The bot is already out; the humans play on.
    rows.botOut = g('bo');
    await seedTable(rows.botOut, fixture().seats([{ id: h1, name: 'Hana' }, { id: bot, name: 'Bolt', brain: 'random' }, { id: h2, name: 'Ivo' }])
        .status(PLAYING).powerSuit(0).hand(0, '6h 7h').hand(2, '8h 9h').eliminated(1).discard(32).build());
    // A lobby with a bot seated.
    rows.lobby = g('lb');
    await seedTable(rows.lobby, fixture().seats([{ id: h1, name: 'Hana' }, { id: bot2, name: 'Bolt2', brain: 'random' }]).seatStatus(1, READY).build());
    // A finished game with a bot.
    rows.over = g('ov');
    await seedTable(rows.over, fixture().seats([{ id: h1, name: 'Hana' }, { id: bot2, name: 'Bolt2', brain: 'random' }])
        .status(GAME_OVER).powerSuit(0).eliminated(0).discard(36).build());
    // A bot IN, but committed a moment ago: not stalled.
    rows.fresh = g('fr');
    await seedTable(rows.fresh, fixture().seats([{ id: h2, name: 'Ivo' }, { id: bot, name: 'Bolt', brain: 'random' }])
        .status(PLAYING).hand(0, '6h 7h').hand(1, '8h 9h').deck('Th Jh').trump('Qc').build());
    // A bot IN, but abandoned for two hours.
    rows.abandoned = g('ab');
    await seedTable(rows.abandoned, fixture().seats([{ id: h2, name: 'Ivo' }, { id: bot2, name: 'Bolt2', brain: 'random' }])
        .status(PLAYING).hand(0, '6h 7h').hand(1, '8h 9h').deck('Th Jh').trump('Qc').build());

    for (const [k, id] of Object.entries(rows)) {
        if (k === 'fresh') continue;
        await age(id, k === 'abandoned' ? 7200 : 30);
    }

    const res = await postJson('bot-heartbeat', null, {});
    assert.equal(res.status, 200, JSON.stringify(res.json));
    assert.deepEqual(dispatched, [rows.botIn], 'exactly the stalled game with a bot IN');
    assert.deepEqual(res.json, { scanned: 1, dispatched: 1 });
});

// The other way a stalled table gets its bots moving: any signed-in client's
// `bump`. It commits nothing itself and wakes the loop exactly when the kernel
// says a bot has work.
test('a bump wakes the bot loop when a bot has work, and does nothing when none does', async () => {
    const h = uuid(), s = uuid(), bot = uuid();
    const stranger = await tokenFor(s, 'stranger');
    const botTurn = `bt${uuid().slice(0, 5)}`;
    // The bot attacks first: it has work.
    await seedTable(botTurn, fixture().seats([{ id: bot, name: 'Bolt', brain: 'random' }, { id: h, name: 'Hana' }])
        .status(PLAYING).attacker(0).defender(1).hand(0, '6h 7h').hand(1, '8s 9s').deck('Th Jh').trump('Qc').build());
    const humans = `hb${uuid().slice(0, 5)}`;
    await seedTable(humans, fixture().seats([{ id: h, name: 'Hana' }, { id: s, name: 'Stan' }])
        .status(PLAYING).hand(0, '6h 7h').hand(1, '8s 9s').deck('Th Jh').trump('Qc').build());
    const versionOf = async (g: string) => Number((await pgPool.query('SELECT version FROM games WHERE id = $1', [g])).rows[0].version);

    for (const g of [botTurn, humans]) {
        const res = await postJson('action', stranger, { type: 'bump', game_id: g });
        assert.equal(res.status, 200, `${g}: bump answers (${JSON.stringify(res.json)})`);
    }
    await settle();
    assert.ok(await versionOf(botTurn) > 0, 'the bot moved after the bump');
    assert.equal(await versionOf(humans), 0, 'a table with no bot work is left alone');
});
