// One full online game against a LOCAL Supabase stack, over HTTP, through the
// real edge functions (docs/C_GAME_SHAPE_MIGRATION.md Phase 4b).
//
// Prerequisites (from server/impls/supabase):
//   supabase start                 # Postgres, GoTrue, PostgREST, Realtime
//   supabase functions serve       # the edge functions on :54321/functions/v1
// Then, from the repo root:
//   TSX_TSCONFIG_PATH=e2e/tsconfig.json node --import tsx e2e/live_online_smoke.ts
//
// Credentials come from `supabase status -o env` (run in server/impls/supabase),
// or from SUPABASE_URL / SUPABASE_ANON_KEY / SUPABASE_SERVICE_ROLE_KEY.
//
// What it does, each step a real request with a real user token:
//   two users signed up (GoTrue admin API) and signed in; alice creates a table;
//   bob joins; alice adds a bot; both ready (the deal); then the humans play legal
//   moves through `action` (packed) until the game is over, nudging the bot loop
//   with `bump` when only the bot can move. Checked on the way: every response is
//   a packed body, the version only grows, the bot moves on its own, the finished
//   game has a replay snapshot, and each human's player_views row is terminal.
//
// Choosing a move needs the full board, which no client may read, so the script
// reads games.state and games.roster with the service role and asks the kernel
// (e2e/helpers/table_play.ts legalMoves) - exactly what a client's own hand and
// the table would allow it.

import { execFileSync } from 'node:child_process';
import { join } from 'node:path';
import * as L from '../sdk/ts/gen/game_layout.bots.ts';
import { fixtureTable } from './helpers/table_fixture.ts';
import { legalMoves, residentBoard, type BoardState } from './helpers/table_play.ts';
import { encodeActionRequest, decodeActionResponse, ACTION_STATUS } from '../sdk/ts/wire/awire.ts';

const say = (s: string) => process.stdout.write(`${s}\n`);

function credentials(): { url: string; anon: string; service: string } {
    let env: Record<string, string> = {};
    if (!process.env.SUPABASE_ANON_KEY || !process.env.SUPABASE_SERVICE_ROLE_KEY) {
        const out = execFileSync('supabase', ['status', '-o', 'env'], { cwd: join(process.cwd(), 'server', 'impls', 'supabase'), encoding: 'utf8' });
        env = Object.fromEntries(out.split('\n').map((l) => l.match(/^([A-Z_]+)="?(.*?)"?$/)).filter(Boolean).map((m) => [m![1], m![2]]));
    }
    return {
        url: process.env.SUPABASE_URL || env.API_URL || 'http://127.0.0.1:54321',
        anon: process.env.SUPABASE_ANON_KEY || env.ANON_KEY,
        service: process.env.SUPABASE_SERVICE_ROLE_KEY || env.SERVICE_ROLE_KEY,
    };
}

const { url, anon, service } = credentials();
if (!anon || !service) throw new Error('no local Supabase credentials: is `supabase start` running?');

async function json(method: string, path: string, key: string, bearer: string, body?: unknown): Promise<any> {
    const res = await fetch(`${url}${path}`, {
        method, headers: { apikey: key, Authorization: `Bearer ${bearer}`, 'Content-Type': 'application/json' },
        body: body === undefined ? undefined : JSON.stringify(body),
    });
    const text = await res.text();
    if (!res.ok) throw new Error(`${method} ${path}: ${res.status} ${text}`);
    return text ? JSON.parse(text) : null;
}

interface User { id: string; name: string; token: string }

async function user(name: string, stamp: string): Promise<User> {
    const email = `${name}-${stamp}@smoke.local`;
    const password = `smoke-${stamp}`;
    const created = await json('POST', '/auth/v1/admin/users', service, service,
        { email, password, email_confirm: true, user_metadata: { username: `${name}${stamp.slice(-4)}` } });
    const session = await json('POST', '/auth/v1/token?grant_type=password', anon, anon, { email, password });
    return { id: created.id, name, token: session.access_token };
}

async function edge(fn: string, u: User, body: unknown): Promise<Uint8Array> {
    const packed = body instanceof Uint8Array;
    const res = await fetch(`${url}/functions/v1/${fn}`, {
        method: 'POST',
        headers: { apikey: anon, Authorization: `Bearer ${u.token}`, 'Content-Type': packed ? 'application/octet-stream' : 'application/json' },
        body: packed ? (body as unknown as BodyInit) : JSON.stringify(body),
    });
    const bytes = new Uint8Array(await res.arrayBuffer());
    if (res.status !== 200) throw new Error(`${fn} ${JSON.stringify(body instanceof Uint8Array ? '<packed>' : body)}: ${res.status} ${new TextDecoder().decode(bytes)}`);
    if (!(res.headers.get('content-type') ?? '').includes('octet-stream')) throw new Error(`${fn}: not a packed response`);
    return bytes;
}

interface Row { board: BoardState; version: number; status: string; needsBots: boolean }

async function row(gameId: string): Promise<Row> {
    const [r] = await json('GET', `/rest/v1/games?id=eq.${gameId}&select=state,roster,version,status,needs_bots`, service, service);
    const bytes = (h: string) => Uint8Array.from(Buffer.from(h.replace(/^\\x/, ''), 'hex'));
    const state = bytes(r.state), roster = bytes(r.roster);
    const rc = fixtureTable().load(state, roster);
    if (rc < 0) throw new Error(`the stored row does not load (${rc})`);
    return { board: residentBoard(gameId, state, roster), version: Number(r.version), status: r.status, needsBots: r.needs_bots };
}

const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));

async function main(): Promise<void> {
    const stamp = String(Date.now());
    const alice = await user('alice', stamp), bob = await user('bob', stamp);
    say(`users: ${alice.id} ${bob.id}`);

    await edge('create', alice, {});
    let gameId = '';
    for (let i = 0; i < 20 && !gameId; i++) {   // create persists after its response
        const views = await json('GET', `/rest/v1/player_views?select=game_id&player_id=eq.${alice.id}`, anon, alice.token);
        gameId = views[0]?.game_id ?? '';
        if (!gameId) await sleep(250);
    }
    if (!gameId) throw new Error('create: no player_views row appeared');
    say(`created ${gameId}`);

    await edge('meta', bob, { type: 'join', game_id: gameId });
    await edge('meta', alice, { type: 'add-bot', game_id: gameId });
    await edge('meta', alice, { type: 'start', game_id: gameId });
    await edge('meta', bob, { type: 'start', game_id: gameId });
    let r = await row(gameId);
    if (r.status !== 'playing') throw new Error(`the table did not deal (${r.status})`);
    say(`dealt: ${r.board.seats.map((s) => `${s.name}${s.brain ? `(${s.brain})` : ''}`).join(', ')}`);

    const humans = new Map([[alice.id, alice], [bob.id, bob]]);
    const dealtAt = r.version;
    let lastVersion = r.version, moves = 0, pick = 0;
    for (let step = 0; step < 3000; step++) {
        r = await row(gameId);
        if (r.version < lastVersion) throw new Error(`the version went back: ${lastVersion} -> ${r.version}`);
        if (r.status !== 'playing') break;
        const mine = legalMoves(r.board, (s) => humans.has(s.id));
        // Prefer a real move over Good, but say Good when it is all the humans
        // have: two attackers each holding only Good is a stall otherwise, since
        // the bot defender waits on them.
        const notGood = mine.filter((m) => m.kind !== 'good');
        const playable = notGood.length > 0 ? notGood : mine;
        if (playable.length === 0) {
            // Only the bot can move: the loop was scheduled by the last human
            // move; nudge it the way a spectator's client does if it stalls.
            if (r.needsBots) await edge('action', alice, { type: 'bump', game_id: gameId });
            await sleep(400);
            lastVersion = (await row(gameId)).version;
            continue;
        }
        const m = playable[pick++ % playable.length];
        const out = decodeActionResponse(await edge('action', humans.get(m.playerId)!, encodeActionRequest(gameId, m.wire, r.version)));
        if (!out) throw new Error('action: the response does not decode');
        if (out.status === ACTION_STATUS.APPLIED) moves++;
        lastVersion = Math.max(lastVersion, out.version);
    }
    r = await row(gameId);
    if (r.status !== 'game_over') throw new Error(`the game did not finish (${r.status} at version ${r.version})`);
    if (r.board.status !== L.GAME_STATUS_GAME_OVER) throw new Error('the blob is not a finished game');

    // Every commit after the deal is a human move or a bot cycle.
    const botCommits = r.version - dealtAt - moves;
    await sleep(500);
    const snaps = await json('GET', `/rest/v1/game_snapshots?game_id=eq.${gameId}&select=id`, service, service);
    const views = await json('GET', `/rest/v1/player_views?game_id=eq.${gameId}&select=player_id,status,version`, service, service);
    say(`finished at version ${r.version}: ${moves} human moves, ${botCommits} bot cycles, ${snaps.length} replay snapshot(s)`);
    say(`player_views: ${JSON.stringify(views)}`);
    if (snaps.length !== 1) throw new Error('no replay snapshot for the finished game');
    if (views.length !== 2 || views.some((v: { status: string }) => v.status !== 'game_over')) throw new Error('a human has no terminal view row');
    if (botCommits === 0) throw new Error('the bot never moved on its own');
    say('live online smoke: OK');
}

main().catch((e) => { process.stderr.write(`live online smoke FAILED: ${e?.stack ?? e}\n`); process.exit(1); });
