/* =============================================================================
 * The pg_net response log stops eating the free-tier disk
 * =============================================================================
 * seed.sql's SCHEDULED JOBS section, the `pg-net-response-vacuum` half. Measured
 * on hosted (wngpfwmwkltonwosqflx, read-only) on 2026-09-18: the database was
 * 542.5 MB against a 500 MB cap, and 511.1 MB of it was `net._http_response` - a
 * log that held 2,155 rows totalling 2.5 MB. The heap was 63,763 pages and every
 * live row sat in pages 63,403..63,762. The first 63,403 pages - 495 MB - held
 * nothing.
 *
 * So the table is not big, it is append-only in practice, and the fix had two
 * halves. One was a one-time `TRUNCATE net._http_response`, which handed 511 MB
 * back to the OS where a DELETE would have left the file exactly as long as it
 * found it. That reclaim has happened, on the one database it could ever happen
 * to, and it is not in seed.sql: a database built from that file has nothing to
 * reclaim, and a seed that truncates a running extension's table is a wrecking
 * ball aimed at whatever pg_net has in flight.
 *
 * The other half is the one that has to keep existing, and it is what this file
 * holds to an assertion. Nothing but a scheduled VACUUM stops the bloat coming
 * back: pg_net's TTL sweep already deletes the rows (the live window is exactly
 * the 6 hours pg_net.ttl says), and its own scan prunes them off the page, which
 * holds n_dead_tup near zero, keeps the table under the autovacuum threshold
 * forever, and so the free space map is never written and every insert extends
 * the file. `the scheduled command is what bounds the log` runs the production
 * cycle twice on a real Postgres table - once running the exact command string
 * seed.sql put in cron.job, once not - and holds the two apart.
 * ========================================================================== */

import './harness.ts';
import { describe, test, before } from 'node:test';
import assert from 'node:assert/strict';
import { applySchema, pgPool } from './harness.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; }

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
/** Heap plus the created index plus the forks, which is what the dashboard reports. */
const responseBytes = () => scalar<number>(`SELECT pg_total_relation_size('net._http_response')::float8 AS v`);

describe('pg_net response-log retention', () => {
    before(async () => {
        // applySchema() is e2e/schema.sql + e2e/fixtures/platform_extensions.sql
        // + seed.sql, in that order. The extensions come FIRST so seed.sql really
        // schedules the job below - a shim applied afterwards would leave
        // cron.job empty and this file would assert nothing.
        await applySchema();
    });

    test('a VACUUM of the response log is scheduled', async () => {
        const jobs = (await pgPool.query(
            'SELECT jobname, schedule, command, active FROM cron.job WHERE jobname = $1', [JOB_NAME])).rows;
        assert.equal(jobs.length, 1, `exactly one ${JOB_NAME} job. seed.sql is the only thing that `
            + 'schedules it now, so a database with none has no retention at all and goes back to '
            + 'growing ~11.8 MB a day until it hits the storage cap.');
        assert.equal(jobs[0].active, true);
        assert.equal(jobs[0].schedule, '*/15 * * * *',
            'the schedule decides the steady-state size: the heap settles at the TTL window plus whatever '
            + 'one interval of inserts extends it by.');
        assert.match(jobs[0].command.trim(), /^VACUUM\s+net\._http_response;?$/,
            'the job must be a single bare VACUUM. cron.use_background_workers is off on hosted, so pg_cron '
            + 'sends the command as one simple query; a second statement would put it in an implicit '
            + 'transaction block and Postgres refuses VACUUM there.');
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
