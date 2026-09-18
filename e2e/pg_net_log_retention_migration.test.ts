/* =============================================================================
 * The pg_net response log stops eating the free-tier disk
 * =============================================================================
 * Migration 20260918200000_pg_net_response_log_retention.sql. Measured on hosted
 * (wngpfwmwkltonwosqflx, read-only) on 2026-09-18: the database was 542.5 MB
 * against a 500 MB cap, and 511.1 MB of it was `net._http_response` - a log that
 * held 2,155 rows totalling 2.5 MB. The heap was 63,763 pages and every live row
 * sat in pages 63,403..63,762. The first 63,403 pages - 495 MB - held nothing.
 *
 * So the table is not big, it is append-only in practice, and the fix has two
 * halves, each of which this file holds to an assertion:
 *
 *   TRUNCATE gives the 511 MB back to the OS now. A DELETE would not: it leaves
 *   the file exactly as long as it found it, which is how the table got here.
 *   Asserted as pg_total_relation_size() = 0, so swapping TRUNCATE for DELETE
 *   goes red on the only property that matters.
 *
 *   A scheduled VACUUM stops it coming back. Nothing else does: pg_net's TTL
 *   sweep already deletes the rows (the live window is exactly the 6 hours
 *   pg_net.ttl says), and its own scan prunes them off the page, which holds
 *   n_dead_tup near zero, keeps the table under the autovacuum threshold
 *   forever, and so the free space map is never written and every insert
 *   extends the file. `the scheduled command is what bounds the log` runs the
 *   production cycle twice on a real Postgres table - once running the exact
 *   command string the migration put in cron.job, once not - and holds the two
 *   apart.
 *
 * Nothing in this repository reads net._http_response. The heartbeat calls
 * net.http_post and never collects; `net.http_collect_response` appears nowhere
 * outside pg_net's own function bodies. That is what makes dropping the newest
 * rows as acceptable as dropping the oldest, and it is asserted here too, so a
 * future collector cannot be added without this file objecting.
 * ========================================================================== */

import './harness.ts';
import { describe, test, before } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { applySchema, pgPool } from './harness.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; }

const MIGRATION = join(
    process.cwd(), 'server', 'impls', 'supabase', 'migrations',
    '20260918200000_pg_net_response_log_retention.sql',
);
const EXTENSIONS = join(process.cwd(), 'e2e', 'fixtures', 'platform_extensions.sql');
const migrationSql = readFileSync(MIGRATION, 'utf8');
const extensionsSql = readFileSync(EXTENSIONS, 'utf8');

const JOB_NAME = 'pg-net-response-vacuum';

// The hosted rows are 1,200 bytes each (avg(pg_column_size(t.*)) = max = 1200),
// of which the JSON body is 28. The bulk is the response headers. A row this
// size packs 6 to a page, which is the ratio every page count below rests on.
const HEADERS = { server: 'cloudflare', 'cf-ray': 'x'.repeat(900) };

// One heartbeat every 10 seconds: 360 responses an hour, and pg_net.ttl = 6h.
const PER_HOUR = 360;
const TTL_HOURS = 6;
const WINDOW_ROWS = PER_HOUR * TTL_HOURS;

const scalar = async <T>(sql: string, params: unknown[] = []): Promise<T> =>
    (await pgPool.query(sql, params)).rows[0].v as T;

const responseRows = () => scalar<number>('SELECT count(*)::int AS v FROM net._http_response');
/** The heap alone: what a DELETE leaves behind and a TRUNCATE gives back. */
const responseHeapBytes = () => scalar<number>(`SELECT pg_relation_size('net._http_response')::float8 AS v`);
/** Heap plus the created index plus the forks, which is what the dashboard reports. */
const responseBytes = () => scalar<number>(`SELECT pg_total_relation_size('net._http_response')::float8 AS v`);
const cronHistoryRows = () => scalar<number>('SELECT count(*)::int AS v FROM cron.job_run_details');

/** Seed `n` responses aged `minutesAgo`, the shape pg_net writes. */
async function seedResponses(n: number, minutesAgo: number): Promise<void> {
    await pgPool.query(
        `INSERT INTO net._http_response (id, status_code, content_type, headers, content, timed_out, created)
         SELECT g, 200, 'application/json', $2::jsonb, '{"scanned":0,"dispatched":0}', false,
                now() - make_interval(mins => $3)
         FROM generate_series(1, $1) g`,
        [n, JSON.stringify(HEADERS), minutesAgo],
    );
}

/** The seeded game, as every column reads after the migration ran over it. */
const gameRow = async () =>
    (await pgPool.query(`SELECT *, status::text AS status FROM games WHERE id = 'keepme'`)).rows[0];

const GAME_ID = 'keepme';

describe('pg_net response-log retention', () => {
    let beforeBytes = 0;
    let beforeGame: Record<string, unknown> | null = null;
    let cronHistoryBefore = 0;

    before(async () => {
        await applySchema();
        await pgPool.query(extensionsSql);

        // Product data the migration must not touch.
        await pgPool.query(
            `INSERT INTO games (id, name, players, status) VALUES ($1, 'keep me', '[]'::jsonb, 'waiting')`,
            [GAME_ID],
        );
        beforeGame = await gameRow();

        // A log spanning both sides of any retention window somebody might reach
        // for: a day old, an hour old, and a minute old.
        await seedResponses(PER_HOUR * 24, 24 * 60);
        await seedResponses(PER_HOUR, 60);
        await seedResponses(6, 1);

        // pg_cron's own history, kept at 2 days by the DELETE that migration
        // 20260712120000 folded into the heartbeat job.
        await pgPool.query(
            `INSERT INTO cron.job_run_details (jobid, job_pid, database, username, command, status, start_time, end_time)
             SELECT 2, 100, current_database(), current_user, 'SELECT net.http_post(...)', 'succeeded',
                    now() - make_interval(secs => g * 10), now() - make_interval(secs => g * 10 - 1)
             FROM generate_series(1, 500) g`,
        );
        cronHistoryBefore = await cronHistoryRows();

        beforeBytes = await responseBytes();
        assert.ok(beforeBytes > 8 * 1024 * 1024,
            `the seeded log must be big enough for the reclaim to mean something, got ${beforeBytes} bytes`);

        await pgPool.query(migrationSql);
    });

    test('the response log is empty and its disk is back with the OS', async () => {
        assert.equal(await responseRows(), 0, 'every response row is gone');
        assert.equal(await responseHeapBytes(), 0,
            'net._http_response still occupies disk. A DELETE empties the table and leaves the file '
            + 'exactly as long as it found it - which is how 2,155 live rows came to hold 511 MB on '
            + 'hosted. Only TRUNCATE hands the extents back.');
        // The index keeps a fresh metapage and the unlogged table an init fork,
        // so the total is not zero - it is 32 kB where it was 8.6 MB here and
        // 511 MB on hosted.
        assert.ok(await responseBytes() < 64 * 1024, 'nothing but empty forks is left');
    });

    test('the newest responses go with the oldest, because nothing ever reads one', async () => {
        assert.equal(
            await scalar<number>(
                `SELECT count(*)::int AS v FROM net._http_response WHERE created > now() - interval '1 hour'`),
            0,
            'a response from a minute ago survived. The heartbeat fires net.http_post and never collects, '
            + 'and net.http_collect_response is called nowhere in this repository, so there is no read '
            + 'latency for a retention window to exceed. If that changes, this migration has to become a '
            + 'time-bounded DELETE plus a separate reclaim.',
        );
    });

    test('the game row is untouched', async () => {
        assert.deepEqual(await gameRow(), beforeGame, 'the migration rewrote a games row');
        assert.equal(await scalar<number>('SELECT count(*)::int AS v FROM games'), 1);
    });

    test("pg_cron's history is left alone - it is live rows, not bloat", async () => {
        assert.equal(await cronHistoryRows(), cronHistoryBefore,
            'cron.job_run_details was pruned. Measured on hosted it is 14.5 MB holding 16,832 live rows '
            + 'under the working 2-day DELETE in the heartbeat job, so there is no dead space to reclaim '
            + 'and a truncate would only throw away two days of operator history that comes straight back.');
    });

    test('a VACUUM of the response log is scheduled', async () => {
        const jobs = (await pgPool.query(
            'SELECT jobname, schedule, command, active FROM cron.job WHERE jobname = $1', [JOB_NAME])).rows;
        assert.equal(jobs.length, 1, `exactly one ${JOB_NAME} job`);
        assert.equal(jobs[0].active, true);
        assert.equal(jobs[0].schedule, '*/15 * * * *',
            'the schedule decides the steady-state size: the heap settles at the TTL window plus whatever '
            + 'one interval of inserts extends it by.');
        assert.match(jobs[0].command.trim(), /^VACUUM\s+net\._http_response;?$/,
            'the job must be a single bare VACUUM. cron.use_background_workers is off on hosted, so pg_cron '
            + 'sends the command as one simple query; a second statement would put it in an implicit '
            + 'transaction block and Postgres refuses VACUUM there.');
    });

    test('running the migration a second time changes nothing', async () => {
        await pgPool.query(migrationSql);
        assert.equal(await responseRows(), 0);
        assert.equal(await responseHeapBytes(), 0);
        assert.equal(await cronHistoryRows(), cronHistoryBefore);
        assert.deepEqual(await gameRow(), beforeGame);
        assert.equal(
            await scalar<number>('SELECT count(*)::int AS v FROM cron.job WHERE jobname = $1', [JOB_NAME]), 1,
            'cron.schedule() replaces a job by name rather than adding one, so db push re-running this '
            + 'migration leaves one job, not two.');
    });

    // The reason the job exists, run as Postgres rather than asserted as a string.
    test('the scheduled command is what bounds the log', async () => {
        const scheduled = (await pgPool.query(
            'SELECT command FROM cron.job WHERE jobname = $1', [JOB_NAME])).rows;
        assert.equal(scheduled.length, 1, `there is a ${JOB_NAME} job to run`);
        const command: string = scheduled[0].command;

        // Two days of the production cycle: each hour 360 responses arrive and
        // pg_net's TTL sweep deletes everything older than 6 hours. Two days,
        // because the point is the RATIO - eight windows' worth of traffic has to
        // pass through a table that only ever holds one window.
        const HOURS = 48;
        const run = async (maintain: boolean): Promise<number> => {
            await pgPool.query('TRUNCATE net._http_response');
            for (let hour = 0; hour < HOURS; hour++) {
                await pgPool.query(
                    `INSERT INTO net._http_response (id, status_code, content_type, headers, content, timed_out, created)
                     SELECT g, 200, 'application/json', $2::jsonb, '{"scanned":0,"dispatched":0}', false, now()
                     FROM generate_series(1, $1) g`,
                    [PER_HOUR, JSON.stringify(HEADERS)],
                );
                await pgPool.query(
                    `DELETE FROM net._http_response
                     WHERE ctid IN (SELECT ctid FROM net._http_response ORDER BY ctid LIMIT
                                    greatest(0, (SELECT count(*) FROM net._http_response) - $1))`,
                    [WINDOW_ROWS],
                );
                if (maintain) await pgPool.query(command);
            }
            assert.equal(await responseRows(), WINDOW_ROWS, 'the TTL sweep leaves exactly the live window');
            return responseBytes();
        };

        const maintained = await run(true);
        const neglected = await run(false);

        // The live window is 2,160 rows; six to a page that is ~360 pages, and
        // the index adds its own. Two windows is generous room for the interval's
        // worth of extension the schedule allows for.
        const windowBytes = WINDOW_ROWS * 1400;
        assert.ok(maintained < 2 * windowBytes,
            `with the scheduled command the log stays near its live window: ${maintained} bytes against a `
            + `${windowBytes}-byte window. Postgres only writes the free space map in VACUUM, so this is `
            + 'the whole mechanism - without it every insert extends the file.');
        assert.ok(neglected > 4 * maintained,
            `without it the same day of traffic must bloat: ${neglected} bytes against ${maintained}. If `
            + 'these are close the cycle above is not reproducing the hosted failure and the first '
            + 'assertion proves nothing.');
    });
});
