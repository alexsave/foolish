// create's time to response, in process against real Postgres: the real create
// edge handler (auth, table_create, table_commit_products, the create_table RPC)
// for distinct users, each answered only after its row is stored
// (docs/C_GAME_SHAPE_MIGRATION.md 4.0, the create gate).
//
//   E2E_DB_PREFIX=db2_ BENCH_CREATES=201 TSX_TSCONFIG_PATH=e2e/tsconfig.json node --import tsx e2e/bench_create.ts
//
// E2E_DB_RTT_MS=<ms> adds that round trip to every database request (the
// hosted-like variant). Prints the first call, p50 and p95 over the rest, and
// the JSON body bytes create_table received per call.
import './harness.ts';
import { applySchema, resetDb, uuid, pgPool } from './harness.ts';
import { rpcBodyBytes } from './adapters/supabase.ts';
import { postJson, settle, tokenFor } from './helpers/edge.ts';

const N = Number(process.env.BENCH_CREATES || 201);
const say = (line: string) => process.stdout.write(line + '\n');

(async () => {
    await applySchema();
    await resetDb();
    const users = Array.from({ length: N }, () => uuid());
    const tokens: string[] = [];
    for (let i = 0; i < N; i++) tokens.push(await tokenFor(users[i], `creator${i}`));   // one signing key: not in parallel
    await pgPool.query('INSERT INTO auth.users (id) SELECT unnest($1::uuid[])', [users]);
    const samples: number[] = [];
    for (let i = 0; i < N; i++) {
        const t0 = performance.now();
        const res = await postJson('create', tokens[i], {});
        samples.push(performance.now() - t0);
        if (res.status !== 200) throw new Error(`create ${i}: ${res.status} ${new TextDecoder().decode(res.bytes)}`);
    }
    await settle();
    const first = samples.shift()!;
    samples.sort((a, b) => a - b);
    const pick = (q: number) => samples[Math.min(samples.length - 1, Math.floor(q * samples.length))];
    const stat = rpcBodyBytes.get('create_table');
    say(`create, ${N} distinct users, rtt ${process.env.E2E_DB_RTT_MS || 0} ms: first ${first.toFixed(2)} ms, p50 ${pick(0.5).toFixed(2)} ms, p95 ${pick(0.95).toFixed(2)} ms; create_table body ${stat ? Math.round(stat.bytes / stat.calls) : 0} B/call`);
    await pgPool.end();
})();
