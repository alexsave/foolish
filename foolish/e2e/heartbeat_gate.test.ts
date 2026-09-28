/* =============================================================================
 * The bot heartbeat stops paying for games nobody is playing
 * =============================================================================
 * seed.sql's SCHEDULED JOBS section - the bot-heartbeat cron entry and the
 * games.last_commit_at column and trigger the gate reads - and the scan in
 * functions/bot-heartbeat/index.ts that reads what it writes.
 *
 * Measured on hosted (wngpfwmwkltonwosqflx, read-only) on 2026-09-18: one
 * bot-heartbeat job on '10 seconds', 8,765 net.http_post calls a day, and 90
 * games of which exactly ONE is playing - 24a407, created 2026-07-13, version
 * and round_epoch frozen at 323,658 / 321,086 across two dumps 410 s apart while
 * updated_at advanced the whole time and sat at bot_lease_until + 1 second in
 * both. That is release_bot_lease: the drive the heartbeat dispatches UPDATEs
 * games, update_games_updated_at stamps updated_at, and the scan's one-hour
 * ABANDON bound reads updated_at. The guard resets itself every ten seconds and
 * can never fire.
 *
 * So there are two properties here and the second rests on the first:
 *
 *   games.last_commit_at moves when `version` moves and at no other time, so a
 *   lease cycle cannot wind it. `the drive does not wind the clock it is judged
 *   by` runs the REAL lease RPCs and holds updated_at and last_commit_at apart;
 *   `a game nobody has moved in an hour stops being driven` then runs the REAL
 *   scan over a row in exactly hosted's state and asserts nothing is dispatched.
 *   Point the scan back at updated_at and that assertion fails, which is the
 *   whole bug in one line.
 *
 *   The cron tick posts only when there is something to drive. Every tick here
 *   is the production command string, taken out of cron.job and run as pg_cron
 *   runs it - one simple query, two statements, one implicit transaction - over
 *   a pg_net whose http_post appends to net.http_request_queue instead of
 *   sending. A queued row is a post is an edge invocation.
 *
 * The acceptance criterion is that a game somebody is playing sees no change at
 * all, so `a live game is posted for on every tick` and `a live game is
 * dispatched exactly as before` come first, and the gate is deliberately built
 * weaker than the scan - it knows the abandon window and not STALE_MS - so that
 * it cannot take a tick the scan wanted. `the gate can only be weaker than the
 * scan` holds the two windows equal by reading both out of the source.
 * ========================================================================== */

import './harness.ts';
import { describe, test, before, beforeEach } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { applySchema, pgPool, uuid } from './harness.ts';
import { postJson, settle } from './helpers/edge.ts';
import { fixture, GAME_OVER, PLAYING } from './helpers/table_fixture.ts';
import { seedTable } from './helpers/table_db.ts';
import { runMeta, seedLobby } from './helpers/table_server.ts';
import { lockedBotLoop } from '../server/impls/supabase/functions/_shared/adapter/bot_actions.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; console.error = () => {}; }

const SUPABASE = join(process.cwd(), 'server', 'impls', 'supabase');
const SEED = join(SUPABASE, 'seed.sql');
const HEARTBEAT_TS = join(SUPABASE, 'functions', 'bot-heartbeat', 'index.ts');

const JOB_NAME = 'bot-heartbeat';

// The scan's self-dispatches, recorded rather than sent (as in
// e2e/heartbeat_needs_bots.test.ts).
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

const scalar = async <T>(sql: string, params: unknown[] = []): Promise<T> =>
    (await pgPool.query(sql, params)).rows[0].v as T;

/** The command pg_cron holds for the job, which is the only thing a tick may run. */
const scheduledCommand = (): Promise<string> =>
    scalar<string>('SELECT command AS v FROM cron.job WHERE jobname = $1', [JOB_NAME]);

/**
 * One cron tick: the production command string, sent the way pg_cron sends it
 * on this instance (cron.use_background_workers is off, so it arrives as a
 * simple query - node-pg's parameterless query() is the same protocol, which is
 * what makes the two statements one implicit transaction here too).
 *
 * Returns the number of HTTP posts the tick made, which is the number of edge
 * invocations it cost.
 */
async function tick(): Promise<number> {
    await pgPool.query('TRUNCATE net.http_request_queue');
    await pgPool.query(await scheduledCommand());
    return scalar<number>('SELECT count(*)::int AS v FROM net.http_request_queue');
}

/** The SCAN, through the real bot-heartbeat entry point. Returns what it dispatched. */
async function scan(): Promise<string[]> {
    dispatched.length = 0;
    const res = await postJson('bot-heartbeat', null, {});
    assert.equal(res.status, 200, JSON.stringify(res.json));
    return [...dispatched];
}

/**
 * Move a row's whole history into the past - its last write AND its last commit.
 * Both triggers are held off for the statement, which is the only way to write
 * either column: one stamps updated_at on any update, the other owns
 * last_commit_at outright.
 */
async function age(gameId: string, seconds: number): Promise<void> {
    await withoutTriggers(`UPDATE games
        SET updated_at = now() - make_interval(secs => $2),
            last_commit_at = now() - make_interval(secs => $2)
        WHERE id = $1`, [gameId, seconds]);
}

/** Move only the last WRITE into the past, leaving the last commit where it is. */
async function ageWriteOnly(gameId: string, seconds: number): Promise<void> {
    await withoutTriggers(
        `UPDATE games SET updated_at = now() - make_interval(secs => $2) WHERE id = $1`, [gameId, seconds]);
}

async function withoutTriggers(sql: string, params: unknown[]): Promise<void> {
    const c = await pgPool.connect();
    try {
        await c.query('BEGIN');
        await c.query('ALTER TABLE games DISABLE TRIGGER update_games_updated_at');
        await c.query('ALTER TABLE games DISABLE TRIGGER games_stamp_last_commit');
        await c.query(sql, params);
        await c.query('ALTER TABLE games ENABLE TRIGGER update_games_updated_at');
        await c.query('ALTER TABLE games ENABLE TRIGGER games_stamp_last_commit');
        await c.query('COMMIT');
    } finally {
        c.release();
    }
}

/**
 * What a drive does to the row when the kernel gives it no work: take the bot
 * lease and give it back. The REAL RPCs, the ones bot_actions.ts calls - and on
 * hosted, 67 days after the last move, the only writes game 24a407 has seen.
 */
async function leaseCycle(gameId: string): Promise<void> {
    const token = await scalar<string>('SELECT try_acquire_bot_lease($1, 25000) AS v', [gameId]);
    assert.ok(token, 'the drive took the lease');
    await pgPool.query('SELECT release_bot_lease($1, $2)', [gameId, token]);
}

const clocks = (gameId: string) => pgPool.query(
    `SELECT version, updated_at, last_commit_at FROM games WHERE id = $1`, [gameId],
).then(r => r.rows[0] as { version: string; updated_at: Date; last_commit_at: Date });

/**
 * A playing table with a bot still IN, which is games.needs_bots.
 *
 * The default is hosted 24a407's shape: the human is on turn, so the kernel
 * gives a drive nothing to do and the whole drive is the lease cycle. That is
 * the case that matters here - a heartbeat that commits nothing and stamps
 * updated_at anyway. `botToMove` hands the first attack to the bot instead, for
 * the one test that needs a real commit out of the real loop.
 */
const liveGame = async (tag: string, botToMove = false): Promise<string> => {
    const id = `${tag}${uuid().slice(0, 5)}`;
    const human = { id: uuid(), name: 'Hana' };
    const bot = { id: uuid(), name: 'Bolt', brain: 'random' };
    const b = fixture()
        .seats(botToMove ? [bot, human] : [human, bot])
        .status(PLAYING).hand(0, '6h 7h').hand(1, '8s 9s').deck('Th Jh').trump('Qc');
    await seedTable(id, (botToMove ? b.attacker(0).defender(1) : b).build());
    return id;
};

describe('the heartbeat only fires when there is something to drive', () => {
    before(async () => {
        // applySchema() is e2e/schema.sql + e2e/fixtures/platform_extensions.sql
        // + seed.sql, in that order, and the job below comes out of seed.sql -
        // nothing here schedules anything. A shim applied AFTER seed.sql would
        // leave cron.job empty and every assertion in this file vacuous, which
        // is why the harness stands the extensions up first.
        await applySchema();
        assert.ok(await scheduledCommand(), 'seed.sql scheduled the bot-heartbeat job');
    });

    beforeEach(async () => {
        // The gate asks about the whole table, so every scenario owns it.
        await pgPool.query('TRUNCATE games, player_hands, bot_hands, bots CASCADE');
        await pgPool.query('TRUNCATE auth.users CASCADE');
        dispatched.length = 0;
    });

    // ---- the acceptance criterion: real play is untouched --------------------

    test('a live game is posted for on every tick, ten seconds apart as before', async () => {
        const g = await liveGame('lv');
        await age(g, 30);

        for (let t = 0; t < 3; t++) {
            assert.equal(await tick(), 1,
                `tick ${t} did not post. A game with a bot to move and a commit inside the abandon window is `
                + 'the case the heartbeat exists for; if the gate can close over one of these the bots stop '
                + 'moving for somebody who is sitting there playing.');
        }
    });

    test('a live game is dispatched by the scan exactly as before', async () => {
        const g = await liveGame('sc');
        await age(g, 30);
        assert.deepEqual(await scan(), [g], 'the stalled game with a bot IN is driven');
    });

    test('a game committed a moment ago is posted for but not yet dispatched', async () => {
        // The gate does not know about STALE_MS on purpose: it must never close
        // over a tick the scan would have used, and the price of that is the
        // ticks the scan itself declines - which is what used to happen anyway.
        const g = await liveGame('fr');
        assert.equal(await tick(), 1, 'the gate stays out of the staleness decision');
        assert.deepEqual(await scan(), [], 'the scan holds it: committed under STALE_MS ago');
    });

    // ---- the guard that could never fire ------------------------------------

    test('the drive does not wind the clock it is judged by', async () => {
        const g = await liveGame('ls');
        await age(g, 7200);
        const before = await clocks(g);

        await leaseCycle(g);

        const after = await clocks(g);
        assert.ok(after.updated_at > before.updated_at,
            'the lease cycle is an UPDATE and update_games_updated_at stamps it - that much is unchanged');
        assert.equal(after.last_commit_at.getTime(), before.last_commit_at.getTime(),
            'the bot lease moved last_commit_at. It is the abandon guard, and the heartbeat is what takes '
            + 'the lease, so a guard this write can touch is a guard that resets itself every ten seconds - '
            + 'which is how hosted game 24a407 was still being driven 67 days after its last move.');
        assert.equal(after.version, before.version, 'and no commit happened, which is why');
    });

    test('a game nobody has moved in an hour stops being driven', async () => {
        const g = await liveGame('ab');
        // Hosted's state exactly: last move two hours ago, then nothing but the
        // heartbeat's own drives, the most recent one a moment before this tick.
        await age(g, 7200);
        await leaseCycle(g);
        await ageWriteOnly(g, 30);

        assert.deepEqual(await scan(), [],
            'the scan dispatched a game whose last move was two hours ago. Its ABANDON bound is reading a '
            + 'column the drive it dispatches writes, so the bound never bites and the game is driven '
            + 'forever - 8,640 edge invocations a day for a table nobody is sitting at.');
        assert.equal(await tick(), 0,
            'and the tick posted for it, which is the same bug one level up: the gate must ask the same '
            + 'question the scan does, or it opens for games the scan will refuse.');
    });

    test('a move is what brings an abandoned game back', async () => {
        const g = await liveGame('bk', true);
        await age(g, 7200);
        assert.equal(await tick(), 0, 'abandoned');

        // The real bot loop, committing for real.
        await lockedBotLoop(g);
        await settle();

        const after = await clocks(g);
        assert.ok(Number(after.version) > 0, 'the loop committed');
        // Asked of Postgres, against the same now() the trigger wrote from,
        // rather than of this process's clock - the determinism gate is right
        // that a test verdict must not turn on a local clock read.
        assert.equal(
            await scalar<boolean>(
                `SELECT last_commit_at > now() - interval '1 minute' AS v FROM games WHERE id = $1`, [g]),
            true,
            'a commit is exactly what last_commit_at is for, and the commit just happened');
        await ageWriteOnly(g, 30);
        assert.equal(await tick(), 1, 'and the game is live again');
        assert.deepEqual(await scan(), [g], 'and driven again');
    });

    // ---- the idle database ---------------------------------------------------

    test('an idle database posts nothing at all', async () => {
        // What hosted actually holds: 53 lobbies and 36 finished games, none of
        // them the heartbeat's business, and not one playing.
        const h = uuid();
        await pgPool.query('INSERT INTO auth.users(id) VALUES ($1) ON CONFLICT DO NOTHING', [h]);
        for (let i = 0; i < 4; i++) {
            const lobby = `lb${uuid().slice(0, 5)}`;
            await seedLobby(lobby, [{ id: uuid(), name: 'Hana' }, { id: uuid(), name: 'Bolt', brain: 'random' }]);
            await age(lobby, 60 * 24 * 3600);
        }
        for (let i = 0; i < 4; i++) {
            const over = `ov${uuid().slice(0, 5)}`;
            await seedTable(over, fixture()
                .seats([{ id: uuid(), name: 'Hana' }, { id: uuid(), name: 'Bolt', brain: 'random' }])
                .status(GAME_OVER).powerSuit(0).eliminated(0).discard(36).build());
            await age(over, 60 * 24 * 3600);
        }

        assert.equal(await tick(), 0,
            'an idle tick posted. This is 8,640 of them a day against a 500,000-invocation month, which is '
            + 'what the whole change is for.');
        assert.deepEqual(await scan(), [], 'and there was nothing for it to have found anyway');
    });

    // ---- lobbies --------------------------------------------------------------

    test('a lobby is not the heartbeat\'s business, and the commit that starts one opens the gate', async () => {
        const host = uuid();
        const lobby = `st${uuid().slice(0, 5)}`;
        await seedLobby(lobby, [{ id: host, name: 'Hana' }, { id: uuid(), name: 'Bolt', brain: 'random' }]);
        await age(lobby, 30);

        assert.equal(await tick(), 0,
            'a waiting lobby drew a post. needs_bots is the kernel\'s table_needs_bots and is false for '
            + 'every status but playing, so the heartbeat has never driven a lobby; a gate that opens for '
            + 'one is paying for all 53 of hosted\'s, most of them a year old.');
        assert.deepEqual(await scan(), [], 'and the scan would have found nothing to drive in it either');
    });

    test('the tick that starts a game is the tick it is driven on', async () => {
        const host = uuid();
        const lobby = `go${uuid().slice(0, 5)}`;
        await seedLobby(lobby, [{ id: host, name: 'Hana' }, { id: uuid(), name: 'Bolt', brain: 'random' }]);
        await age(lobby, 30);
        assert.equal(await tick(), 0, 'closed while it is a lobby');

        const out = await runMeta(lobby, host, { type: 'start' }, 'Hana');
        assert.ok(out.runBots, 'starting a table with a bot in it wakes the loop inline - the heartbeat is '
            + 'the backstop, not the trigger');

        const row = await clocks(lobby);
        assert.ok(Number(row.version) > 0, 'the start committed');
        await ageWriteOnly(lobby, 30);
        assert.equal(await tick(), 1,
            'the gate did not open for a table that had just started. A new game commits on its way out of '
            + 'the lobby, so last_commit_at is now, and nothing about the gate may delay the first drive.');
    });

    // ---- the two windows have to agree ---------------------------------------

    test('the gate can only be weaker than the scan, never narrower', async () => {
        const command = await scheduledCommand();
        const ts = readFileSync(HEARTBEAT_TS, 'utf8');

        const abandon = /const ABANDON_MS = ([^;]+);/.exec(ts);
        assert.ok(abandon, 'ABANDON_MS is still declared in the scan');
        const abandonMs = Number(new Function(`return (${abandon[1]})`)());
        assert.equal(abandonMs, 60 * 60 * 1000);

        const gate = /last_commit_at > now\(\) - interval '([^']+)'/.exec(command);
        assert.ok(gate, 'the scheduled command gates on last_commit_at');
        const gateMs = await scalar<number>(
            `SELECT (extract(epoch from $1::interval) * 1000)::int AS v`, [gate[1]]);
        assert.equal(gateMs, abandonMs,
            `the gate's window (${gate[1]}) and the scan's ABANDON_MS (${abandonMs} ms) have drifted. A gate `
            + 'NARROWER than the scan strands a game the scan was willing to drive, which is a bot that '
            + 'stops moving mid-game. Change both or neither.');

        assert.ok(!/interval '10 seconds'|STALE/.test(command),
            'the gate has taken on the scan\'s staleness test. It must not: the two clocks are read a '
            + 'moment apart, and a gate that can decline a tick the scan would have used is a dropped '
            + 'drive for somebody who is playing.');
    });

    // ---- the shape pg_cron has to be able to send -----------------------------

    test('the job is one simple query pg_cron can send, and still prunes its own history', async () => {
        const jobs = (await pgPool.query(
            'SELECT jobname, schedule, command, active FROM cron.job WHERE jobname = $1', [JOB_NAME])).rows;
        assert.equal(jobs.length, 1, 'exactly one bot-heartbeat job');
        assert.equal(jobs[0].active, true);
        assert.equal(jobs[0].schedule, '10 seconds',
            'the cadence is the one thing that must not move: gating costs no latency, widening the '
            + 'interval buys the same saving by making the bots slower for the people who are playing.');
        assert.match(jobs[0].command, /DELETE FROM cron\.job_run_details WHERE end_time < now\(\) - interval '2 days'/,
            'the cron-history prune from 20260712120000 is still folded in - it is free, and without it '
            + 'cron.job_run_details grows without bound');
        assert.equal((jobs[0].command.match(/;/g) || []).length, 2,
            'two statements, no more: pg_cron sends this as a simple query on an instance with '
            + 'cron.use_background_workers off, which makes the whole command one implicit transaction '
            + 'block, and statements like VACUUM are refused there.');

        // Not asserted about the string - run it. A stalled live game and a post.
        const g = await liveGame('sh');
        await age(g, 30);
        await pgPool.query(
            `INSERT INTO cron.job_run_details (jobid, job_pid, database, username, command, status, start_time, end_time)
             VALUES (1, 100, current_database(), current_user, 'x', 'succeeded', now() - interval '9 days', now() - interval '9 days')`);
        assert.equal(await tick(), 1, 'the command runs and posts');
        assert.equal(await scalar<number>('SELECT count(*)::int AS v FROM cron.job_run_details'), 0,
            'and the same tick pruned the nine-day-old history row');
    });

    // ---- the gate runs every ten seconds, so it had better be cheap ----------

    test('the gate is an index lookup, not a scan of games', async () => {
        // The shape that hurts, and the one that only ever grows: nothing clears
        // needs_bots on a game that was abandoned mid-play, so every table
        // somebody walks away from stays in the partial index forever. A gate
        // that leads on needs_bots alone walks all of them before answering
        // "no", every ten seconds, for the life of the project.
        //
        // Copies of one real kernel-owned row, so games holds what it holds in
        // production - state and roster are NOT NULL blobs the kernel wrote.
        const seed = await liveGame('bk');
        await pgPool.query(
            `INSERT INTO games (id, status, state, roster, needs_bots, version)
             SELECT 'bulk' || g, status, state, roster, true, 1
             FROM generate_series(1, 5000) g, games WHERE games.id = $1`, [seed]);
        assert.equal(
            await scalar<number>(`SELECT count(*)::int AS v FROM games WHERE last_commit_at > now() - interval '1 minute'`),
            5001,
            'the trigger stamped every inserted row, whatever the INSERT said. That is the point of it - a '
            + 'writer cannot hand itself a last_commit_at - and it is why the ageing below runs with the '
            + 'triggers held off rather than by writing the column.');
        await withoutTriggers(
            `UPDATE games SET last_commit_at = now() - interval '30 days', updated_at = now() - interval '30 days'`, []);
        await pgPool.query('ANALYZE games');

        const command = await scheduledCommand();
        const gate = /WHERE EXISTS \(([\s\S]+?)\n  \);/.exec(command);
        assert.ok(gate, 'the command carries an EXISTS gate');
        const plan = (await pgPool.query(
            `EXPLAIN (FORMAT TEXT) SELECT 1 WHERE EXISTS (${gate[1]})`)).rows.map(r => r['QUERY PLAN']).join('\n');

        assert.match(plan, /idx_games_bot_gate/,
            `the gate stopped using its index. It runs every ten seconds forever, so it has to be a lookup. `
            + `Plan was:\n${plan}`);
        assert.doesNotMatch(plan, /Seq Scan on games/,
            `the gate reads every row of games. Plan was:\n${plan}`);
        assert.equal(await tick(), 0, 'and 5,001 stalled bot games are still 5,001 reasons not to post');
    });

    // ---- seed.sql is replayable ----------------------------------------------

    test('re-applying seed.sql leaves one heartbeat job, not two', async () => {
        // seed.sql is what `supabase start` and `supabase db reset` run, and they
        // run it against whatever is already there. cron.schedule() upserts on
        // (jobname, username), so a second application must REPLACE the job. Two
        // bot-heartbeat entries would be two POSTs every ten seconds - the edge
        // bill this file exists to halve, doubled instead.
        //
        // Last in the file: seed.sql drops and rebuilds the gameplay tables, so
        // it takes the scenarios' rows with it.
        await pgPool.query(readFileSync(SEED, 'utf8'));

        for (const job of [JOB_NAME, 'pg-net-response-vacuum']) {
            assert.equal(
                await scalar<number>('SELECT count(*)::int AS v FROM cron.job WHERE jobname = $1', [job]), 1,
                `a second seed.sql left two ${job} jobs`);
        }

        const g = await liveGame('id');
        await age(g, 30);
        assert.equal(await tick(), 1, 'and the gate still opens for a live game after the replay');
    });
});
