// How a commit's blobs should ride to Postgres through PostgREST: measured, not
// assumed (docs/C_GAME_SHAPE_MIGRATION.md 4.0, "commit transport").
//
// Needs a LOCAL Supabase stack (`supabase start --workdir server/impls`); it
// talks to the real PostgREST behind Kong, the way an edge function does, and
// writes into a scratch table through scratch functions it creates and drops.
//
//   TSX_TSCONFIG_PATH=e2e/tsconfig.json node --import tsx e2e/bench_commit_transport.ts
//
// The blobs are one 4-seat mid-game commit's sizes (state, four envelopes, the
// spectator's, one move's log records; the roster left out, as an unchanged
// roster now is). Three transports, interleaved round by round:
//
//   hex      TEXT parameters holding hex, views as a JSONB array of objects
//            (the interface before this change), decoded to BYTEA in SQL
//   bytea    BYTEA and BYTEA[] parameters, the JSON strings '\x'-hex (PostgREST
//            casts them), views as parallel uuid[] and bytea[]
//   base64   TEXT and TEXT[] parameters holding base64, decode(..., 'base64')
//
// BENCH_RTT_MS adds that much delay to every request (half each way), a stand-in
// for the edge-to-database round trip on hosted.

import { execFileSync } from 'node:child_process';
import { Client } from 'pg';
import { derivedUuid } from '../sdk/ts/wire/detid.ts';
import { suiteRng } from './helpers/rng.ts';

const ROUNDS = Number(process.env.BENCH_ROUNDS || 20);
const PER_ROUND = Number(process.env.BENCH_PER_ROUND || 50);
const RTT = Number(process.env.BENCH_RTT_MS || 0);
const say = (s: string) => process.stdout.write(`${s}\n`);

// The blob bytes and seat ids are drawn from the suite seed: their values change
// nothing measured, and a bench is reproducible like any e2e file.
const rng = suiteRng('commit_transport');
let idSeq = 0;
const seededBytes = (n: number): Uint8Array => Uint8Array.from({ length: n }, () => rng.int(256));
const seededUuid = (): string => derivedUuid(`commit_transport:${rng.seed}`, idSeq++);

const env: Record<string, string> = Object.fromEntries(
    execFileSync('supabase', ['status', '-o', 'env', '--workdir', 'server/impls'], { cwd: process.cwd(), encoding: 'utf8', stdio: ['ignore', 'pipe', 'ignore'] })
        .split('\n').map((l) => l.match(/^([A-Z_]+)="?(.*?)"?$/)).filter(Boolean).map((m) => [m![1], m![2]]));
const url = env.API_URL, key = env.SERVICE_ROLE_KEY, dbUrl = env.DB_URL;
if (!url || !key || !dbUrl) throw new Error('no local Supabase stack: run `supabase start --workdir server/impls`');

const SQL = `
CREATE TABLE IF NOT EXISTS zz_bench_rows (id TEXT PRIMARY KEY, state BYTEA, logs BYTEA, spectator BYTEA, views BYTEA[], players UUID[]);
CREATE OR REPLACE FUNCTION zz_bench_hex(p_id TEXT, p_state TEXT, p_logs TEXT, p_spectator TEXT, p_views JSONB) RETURNS JSONB
LANGUAGE plpgsql AS $$ BEGIN
  INSERT INTO zz_bench_rows VALUES (p_id, decode(substr(p_state, 3), 'hex'), decode(p_logs, 'hex'), decode(p_spectator, 'hex'),
    (SELECT array_agg(decode(v->>'view', 'hex')) FROM jsonb_array_elements(p_views) v),
    (SELECT array_agg((v->>'player_id')::uuid) FROM jsonb_array_elements(p_views) v))
  ON CONFLICT (id) DO UPDATE SET state = EXCLUDED.state, logs = EXCLUDED.logs, spectator = EXCLUDED.spectator, views = EXCLUDED.views, players = EXCLUDED.players;
  RETURN jsonb_build_object('status', 'ok', 'version', 1, 'round_epoch', 0);
END $$;
CREATE OR REPLACE FUNCTION zz_bench_bytea(p_id TEXT, p_state BYTEA, p_logs BYTEA, p_spectator BYTEA, p_view_players UUID[], p_views BYTEA[],
  OUT committed BOOLEAN, OUT new_version BIGINT, OUT new_round_epoch BIGINT)
LANGUAGE plpgsql AS $$ BEGIN
  INSERT INTO zz_bench_rows VALUES (p_id, p_state, p_logs, p_spectator, p_views, p_view_players)
  ON CONFLICT (id) DO UPDATE SET state = EXCLUDED.state, logs = EXCLUDED.logs, spectator = EXCLUDED.spectator, views = EXCLUDED.views, players = EXCLUDED.players;
  committed := TRUE; new_version := 1; new_round_epoch := 0;
END $$;
CREATE OR REPLACE FUNCTION zz_bench_base64(p_id TEXT, p_state TEXT, p_logs TEXT, p_spectator TEXT, p_view_players UUID[], p_views TEXT[],
  OUT committed BOOLEAN, OUT new_version BIGINT, OUT new_round_epoch BIGINT)
LANGUAGE plpgsql AS $$ BEGIN
  INSERT INTO zz_bench_rows VALUES (p_id, decode(p_state, 'base64'), decode(p_logs, 'base64'), decode(p_spectator, 'base64'),
    (SELECT array_agg(decode(v, 'base64')) FROM unnest(p_views) v), p_view_players)
  ON CONFLICT (id) DO UPDATE SET state = EXCLUDED.state, logs = EXCLUDED.logs, spectator = EXCLUDED.spectator, views = EXCLUDED.views, players = EXCLUDED.players;
  committed := TRUE; new_version := 1; new_round_epoch := 0;
END $$;
GRANT ALL ON zz_bench_rows TO service_role;
NOTIFY pgrst, 'reload schema';
`;
const DROP = `DROP FUNCTION IF EXISTS zz_bench_hex; DROP FUNCTION IF EXISTS zz_bench_bytea; DROP FUNCTION IF EXISTS zz_bench_base64;
DROP TABLE IF EXISTS zz_bench_rows; NOTIFY pgrst, 'reload schema';`;

const hex = (b: Uint8Array) => Buffer.from(b).toString('hex');
const b64 = (b: Uint8Array) => Buffer.from(b).toString('base64');
const sizes = { state: 110, logs: 40, spectator: 280, view: 300, seats: 4 };

function payload(kind: 'hex' | 'bytea' | 'base64', id: string): object {
    const state = seededBytes(sizes.state), logs = seededBytes(sizes.logs), spectator = seededBytes(sizes.spectator);
    const players = Array.from({ length: sizes.seats }, () => seededUuid());
    const views = players.map(() => seededBytes(sizes.view));
    if (kind === 'hex') {
        return { p_id: id, p_state: `\\x${hex(state)}`, p_logs: hex(logs), p_spectator: hex(spectator),
            p_views: players.map((p, i) => ({ player_id: p, view: hex(views[i]), status: 'playing' })) };
    }
    if (kind === 'bytea') {
        return { p_id: id, p_state: `\\x${hex(state)}`, p_logs: `\\x${hex(logs)}`, p_spectator: `\\x${hex(spectator)}`,
            p_view_players: players, p_views: views.map((v) => `\\x${hex(v)}`) };
    }
    return { p_id: id, p_state: b64(state), p_logs: b64(logs), p_spectator: b64(spectator), p_view_players: players, p_views: views.map(b64) };
}

const delay = (ms: number) => new Promise((r) => setTimeout(r, ms));

async function call(kind: string, body: object): Promise<number> {
    const text = JSON.stringify(body);
    const t0 = performance.now();
    if (RTT) await delay(RTT / 2);
    const res = await fetch(`${url}/rest/v1/rpc/zz_bench_${kind}`, {
        method: 'POST', body: text,
        headers: { apikey: key, Authorization: `Bearer ${key}`, 'Content-Type': 'application/json' },
    });
    const out = await res.text();
    if (RTT) await delay(RTT / 2);
    const dt = performance.now() - t0;
    if (res.status !== 200) throw new Error(`${kind}: ${res.status} ${out}`);
    return dt;
}

async function main(): Promise<void> {
    const db = new Client({ connectionString: dbUrl });
    await db.connect();
    await db.query(DROP);
    await db.query(SQL);
    await delay(1500);   // PostgREST reloads its schema cache
    const kinds = ['hex', 'bytea', 'base64'] as const;
    const bytes: Record<string, number> = {};
    for (const k of kinds) bytes[k] = JSON.stringify(payload(k, 'x')).length;
    const samples: Record<string, number[]> = { hex: [], bytea: [], base64: [] };
    try {
        for (const k of kinds) for (let i = 0; i < 20; i++) await call(k, payload(k, `warm${i}`));
        for (let r = 0; r < ROUNDS; r++) {
            const order = r % 2 ? [...kinds].reverse() : kinds;
            for (const k of order) {
                for (let i = 0; i < PER_ROUND; i++) samples[k].push(await call(k, payload(k, `g${i}`)));
            }
        }
    } finally {
        await db.query(DROP);
        await db.end();
    }
    say(`commit transport through local PostgREST: ${ROUNDS} rounds x ${PER_ROUND} calls each, rtt ${RTT} ms, 4 seats`);
    for (const k of kinds) {
        const s = samples[k].sort((a, b) => a - b);
        const q = (p: number) => s[Math.min(s.length - 1, Math.floor(p * s.length))].toFixed(2);
        say(`  ${k.padEnd(7)} body ${String(bytes[k]).padStart(5)} B   p50 ${q(0.5)} ms   p95 ${q(0.95)} ms`);
    }
}

main().catch((e) => { process.stderr.write(`${e?.stack ?? e}\n`); process.exit(1); });
