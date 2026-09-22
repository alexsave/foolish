/* =============================================================================
 * A fake Supabase, so a LIVE game runs in a real browser
 * =============================================================================
 *
 * WHAT THIS IS. `.env.local` points the web app at http://127.0.0.1:54321.
 * `supabase start` cannot run here (no Docker, no Postgres, no Deno), so the
 * whole backend the browser talks to is this one Node process: GoTrue, PostgREST,
 * the edge functions and - the part that actually matters - Realtime, because a
 * push over a websocket is the only way another player's move reaches a client.
 *
 * NOTHING ABOUT GAMEPLAY IS MOCKED. The server is the C Table itself: the same
 * kernel the real edge functions run (sdk/ts/table/server_table.ts over
 * bots.wasm), driven exactly as server/impls/supabase/functions/_shared/adapter/
 * does it - load the row, one synchronous kernel section, copy the products out,
 * bump the version, push one masked envelope per human seat. Boards are built by
 * e2e/helpers/table_fixture.ts, which is the same builder the jsdom goldens in
 * e2e/ui_animation_trace.test.ts use, so a scenario here and a case there are
 * the same board. Bots are the kernel's own drive loop (table_bot_drive).
 *
 * WHAT IS FAKED, and it is all I/O: there is no database (rows live in a Map and
 * die with the process), no RLS (the bearer token names the caller and the
 * handlers scope by it themselves), no JWT signature (the token is well formed
 * and never verified), no lease, no CAS - one process, one thread, no contention
 * to lose. Realtime has no presence and no replay. Passwords are not checked.
 * `game_snapshots`, `user_elo_ratings` and `chat_messages` answer empty.
 * THIS IS A TEST HARNESS. It must never be reachable from anything shipped.
 *
 * ---------------------------------------------------------------------------
 * RUN IT
 * ---------------------------------------------------------------------------
 *   # one terminal - the backend
 *   TSX_TSCONFIG_PATH=e2e/tsconfig.json node --import tsx e2e/fake_supabase.mts
 *
 *   # another - the app (it already reads .env.local)
 *   npx next dev -p 3000
 *
 * Then open http://localhost:3000, sign in as ME / ME (any password works), and
 * play. `--scenario <name>` seats you in a game that is already under way and
 * prints its URL; `--scenario list` names them.
 *
 * ---------------------------------------------------------------------------
 * DRIVE IT FROM A TEST OR A BROWSER SCRIPT
 * ---------------------------------------------------------------------------
 * Everything the HTTP surface does is also callable in-process:
 *
 *   import { startFakeSupabase } from './e2e/fake_supabase.mts';
 *   const be = await startFakeSupabase({ port: 54321 });
 *   const game = be.scenario('throw_in_race');       // -> { gameId, users }
 *   be.hold('u-me', true);                           // stop delivering my pushes
 *   be.act('u-boris', game.gameId, { kind: 'attack', cards: [{suit:0,value:6}] });
 *   be.release('u-me', 1);                           // deliver exactly one
 *
 * and over HTTP, for a Playwright script that has no Node import of the repo:
 *
 *   POST /__control/scenario   {name}            -> {gameId, users, url}
 *   POST /__control/act        {user, gameId, move:{kind,cards,attack_cards}}
 *   POST /__control/hold       {user, on}
 *   POST /__control/release    {user, n}
 *   GET  /__control/state?gameId=...             -> version, the table, hands
 *   GET  /__control/log                          -> every request and push, timed
 *   POST /__control/reset
 *
 * A move's `cards` are kernel card text ("7d", "Ts"), parsed by the kernel's own
 * card_list_parse through the fixture builder - no card layout is written here.
 * ========================================================================== */

import { createHash, randomUUID, webcrypto } from 'node:crypto';
import { createServer, type IncomingMessage, type ServerResponse } from 'node:http';
import * as L from '../sdk/ts/gen/game_layout.bots.ts';
import { encodeAction, type AwireMove } from '../sdk/ts/wire/awire.ts';
import { bytesToBase64, bytesToBareHex } from '../sdk/ts/wire/bytes.ts';
import { TABLE_DEAL_SEED_BYTES, type TableProducts, type TableSeat } from '../sdk/ts/table/server_table.ts';
import { clientTable } from '../sdk/ts/table/client_table.ts';
import { fixture, fixtureTable, parseCardText, PLAYING, type FixtureSeat, type TableFixture } from './helpers/table_fixture.ts';
import { attachPhoenix, type PhxHub, type PhxSocket } from './helpers/ws_phoenix.ts';

const WEBSITE_DOMAIN = 'foolish.cards';   // src/constants/constants.ts

// ---- the people ---------------------------------------------------------------
//
// The web signs in with a username, which AuthContext turns into an email:
// sha256(UPPERCASE name), the first 16 hex characters, at the site's domain. The
// same derivation here means a person can type "ME" into the real login form and
// land on the seat a scenario built for them.

export interface FakeUser { id: string; name: string; email: string }

const emailFor = (name: string): string =>
    `${createHash('sha256').update(name.toUpperCase(), 'utf8').digest('hex').slice(0, 16)}@${WEBSITE_DOMAIN}`;

const userFor = (name: string): FakeUser => ({ id: `u-${name.toLowerCase()}`, name: name.toUpperCase(), email: emailFor(name) });

/** A JWT nobody verifies: three base64url parts, a far-off expiry, the user in `sub`. */
const b64url = (s: string) => Buffer.from(s, 'utf8').toString('base64url');
function fakeJwt(user: FakeUser): string {
    const now = Math.floor(Date.now() / 1000);
    const head = b64url(JSON.stringify({ alg: 'HS256', typ: 'JWT' }));
    const body = b64url(JSON.stringify({
        sub: user.id, email: user.email, role: 'authenticated', aud: 'authenticated',
        iat: now, exp: now + 24 * 3600, session_id: 'fake-session',
        user_metadata: { username: user.name }, app_metadata: { provider: 'email' },
    }));
    return `${head}.${body}.${b64url('fake-signature')}`;
}

const sessionFor = (user: FakeUser) => ({
    access_token: fakeJwt(user),
    token_type: 'bearer',
    expires_in: 24 * 3600,
    expires_at: Math.floor(Date.now() / 1000) + 24 * 3600,
    refresh_token: `refresh-${user.id}`,
    user: userJson(user),
});

const userJson = (user: FakeUser) => ({
    id: user.id, aud: 'authenticated', role: 'authenticated', email: user.email,
    email_confirmed_at: new Date(0).toISOString(), phone: '',
    confirmed_at: new Date(0).toISOString(), last_sign_in_at: new Date().toISOString(),
    app_metadata: { provider: 'email', providers: ['email'] },
    user_metadata: { username: user.name },
    identities: [], created_at: new Date(0).toISOString(), updated_at: new Date().toISOString(),
    is_anonymous: false,
});

// ---- a game row ---------------------------------------------------------------

interface Row {
    id: string;
    state: Uint8Array;
    roster: Uint8Array;
    version: number;
    roundEpoch: number;
    /** games.game_seed: the bare-hex deal seed, once a deal has happened. */
    seedHex: string | null;
    log: Uint8Array;
}

/** One push, waiting for its channel (or held by the control API). */
interface Push { topic: string; owner: string | null; seq: string; version: number; bytes: Uint8Array }

interface Trace { t: number; what: string }

export interface ScenarioResult { gameId: string; users: string[]; url: string }

export interface FakeBackend {
    readonly url: string;
    readonly hub: PhxHub;
    /** Build a named board and seat its players. */
    scenario(name: string): ScenarioResult;
    /** One move by `userId`, straight into the kernel, pushes and all. */
    act(userId: string, gameId: string, move: MoveSpec): { rc: number; reject: number; version: number };
    /** Stop delivering pushes bound for `userId` (they queue instead). */
    hold(userId: string, on: boolean): void;
    /** Deliver `n` held pushes for `userId` (default: all). Returns how many went out. */
    release(userId: string, n?: number): number;
    /** The row as the kernel reads it: version, the table in card text, hand sizes. */
    state(gameId: string): unknown;
    /** Every request and push, with a millisecond since start. */
    log(): Trace[];
    reset(): void;
    stop(): Promise<void>;
}

/** A move as the control API names it: kernel card text, never card bytes. */
export interface MoveSpec { kind: AwireMove['kind']; cards?: string; attack_cards?: string }

export interface FakeOptions {
    port?: number;
    /** Print a line per request and push. */
    verbose?: boolean;
}

// ---- the server ---------------------------------------------------------------

export async function startFakeSupabase(opts: FakeOptions = {}): Promise<FakeBackend> {
    const port = opts.port ?? 54321;
    const started = Date.now();
    const trace: Trace[] = [];
    const note = (what: string) => {
        trace.push({ t: Date.now() - started, what });
        if (opts.verbose) process.stdout.write(`${String(Date.now() - started).padStart(7)}ms  ${what}\n`);
    };

    const users = new Map<string, FakeUser>();        // by id
    const byEmail = new Map<string, FakeUser>();
    const rows = new Map<string, Row>();
    const held = new Map<string, Push[]>();           // user id -> undelivered
    const holding = new Set<string>();
    const botTimers = new Map<string, ReturnType<typeof setTimeout>>();

    const register = (name: string): FakeUser => {
        const u = userFor(name);
        users.set(u.id, u);
        byEmail.set(u.email, u);
        return u;
    };
    for (const n of ['ME', 'ANNA', 'BORIS']) register(n);

    const server = createServer((req, res) => { void handle(req, res); });
    const hub = attachPhoenix(server, '/realtime/v1/websocket', {
        onJoin: (sock, msg) => { note(`ws join ${msg.topic}`); void sock; return null; },
        onClose: () => note('ws close'),
    });

    // ---- the kernel section --------------------------------------------------
    //
    // The same shape table_io.ts's `section` has, minus the parts a database
    // forces on it: load the row, run ONE operation, copy every product out
    // before anything else happens. Nothing below reads inside a blob.

    interface Outcome { rc: number; reject: number; version: number; envelope: Uint8Array | null; pushes: Push[] }

    function run(row: Row, viewerId: string | null, act: (t: ReturnType<typeof fixtureTable>, seed: Uint8Array) => number): Outcome {
        const t = fixtureTable();
        const loaded = t.load(row.state, row.roster);
        if (loaded < 0) throw new Error(`game ${row.id} does not load (${loaded})`);
        const seeded = t.setDealSeed(row.seedHex);
        if (seeded < 0) throw new Error(`game ${row.id}: deal seed refused (${seeded})`);
        const seed = new Uint8Array(TABLE_DEAL_SEED_BYTES);
        webcrypto.getRandomValues(seed);

        const rc = act(t, seed);
        const seat = viewerId === null ? -1 : t.seatOf(viewerId);
        if (rc < 0) return { rc, reject: 0, version: row.version, envelope: null, pushes: [] };
        const reject = rc === L.TABLE_REJECTED ? t.reject() : 0;
        if (rc === L.TABLE_REJECTED || rc === L.TABLE_MOOT || rc === L.TABLE_STALE_ROUND) {
            const env = t.envelope(row.id, seat, row.version);
            return { rc, reject, version: row.version, envelope: typeof env === 'number' ? null : env, pushes: [] };
        }
        const p = t.commit(row.id, row.version + 1, Date.now());
        if (typeof p === 'number') throw new Error(`game ${row.id}: no commit products (${p})`);
        const seats = t.seats();
        const pushes: Push[] = [];
        if (p.nEvents > 0) {
            for (let s = 0; s < seats.length; s++) {
                if (seats[s].brain) continue;
                const b = t.push(row.id, s);
                if (typeof b === 'number') throw new Error(`game ${row.id}: push refused (${b})`);
                pushes.push({ topic: `gu-${row.id}-${seats[s].id}`, owner: seats[s].id, seq: randomUUID(), version: row.version + 1, bytes: b });
            }
            const spec = t.push(row.id, -1);
            if (typeof spec !== 'number') pushes.push({ topic: `game-${row.id}`, owner: null, seq: randomUUID(), version: row.version + 1, bytes: spec });
        }
        const envelope = (seat >= 0 ? p.views[seat] : null) ?? p.spectator;
        commitRow(row, p, seats, p.dealtNow ? seed : null);
        return { rc, reject, version: row.version, envelope, pushes };
    }

    function commitRow(row: Row, p: TableProducts, seats: TableSeat[], dealSeed: Uint8Array | null): void {
        row.state = p.state;
        row.roster = p.roster;
        row.version += 1;
        if (p.closedRound) row.roundEpoch += 1;
        if (p.logsReset) row.log = new Uint8Array(0);
        if (p.logs) row.log = p.logs;
        if (dealSeed) row.seedHex = bytesToBareHex(dealSeed);
        views.set(row.id, { spectator: p.spectator, perSeat: p.views.map((v, i) => ({ seat: i, id: seats[i]?.id ?? '', bytes: v })) });
    }

    // The player_views / spectator_views caches, the two plain RLS SELECTs the
    // web reads a board from (docs/PLAYER_VIEWS.md).
    const views = new Map<string, { spectator: Uint8Array; perSeat: { seat: number; id: string; bytes: Uint8Array | null }[] }>();

    function refreshViews(row: Row): void {
        const t = fixtureTable();
        if (t.load(row.state, row.roster) < 0) return;
        const seats = t.seats();
        const spec = t.envelope(row.id, -1, row.version);
        const perSeat = seats.map((s, i) => {
            const e = t.envelope(row.id, i, row.version);
            return { seat: i, id: s.id, bytes: typeof e === 'number' ? null : e };
        });
        views.set(row.id, { spectator: typeof spec === 'number' ? new Uint8Array(0) : spec, perSeat });
    }

    /** Send, or queue, every push an operation produced. */
    function dispatch(pushes: Push[]): void {
        for (const p of pushes) {
            if (p.owner && holding.has(p.owner)) {
                const q = held.get(p.owner) ?? [];
                q.push(p);
                held.set(p.owner, q);
                note(`HELD push v${p.version} for ${p.owner}`);
                continue;
            }
            send(p);
        }
    }

    function send(p: Push): void {
        const n = hub.broadcast(p.topic, 'animation_events', { t: 'as3', s: p.seq, v: p.version, b: bytesToBase64(p.bytes) });
        note(`push v${p.version} -> ${p.topic} (${n} listener${n === 1 ? '' : 's'})`);
    }

    // ---- the bot loop --------------------------------------------------------
    //
    // bot_actions.ts's runCycle with the lease, the CAS and the CPU budget taken
    // out: one drive per timer, the kernel's own delay between cycles.

    function wakeBots(gameId: string): void {
        if (botTimers.has(gameId)) return;
        botTimers.set(gameId, setTimeout(() => { botTimers.delete(gameId); driveBots(gameId); }, 10));
    }

    function driveBots(gameId: string): void {
        const row = rows.get(gameId);
        if (!row) return;
        const t = fixtureTable();
        if (t.load(row.state, row.roster) < 0) return;
        if (!t.needsBots()) return;
        t.setDealSeed(row.seedHex);
        t.setSessionLog(row.log);
        const drive = t.botDrive(null);
        if (typeof drive === 'number' || drive.n === 0) return;
        const p = t.commit(gameId, row.version + 1, Date.now());
        if (typeof p === 'number') return;
        const seats = t.seats();
        const pushes: Push[] = [];
        if (p.nEvents > 0) {
            for (let s = 0; s < seats.length; s++) {
                if (seats[s].brain) continue;
                const b = t.push(gameId, s);
                if (typeof b !== 'number') pushes.push({ topic: `gu-${gameId}-${seats[s].id}`, owner: seats[s].id, seq: randomUUID(), version: row.version + 1, bytes: b });
            }
            const spec = t.push(gameId, -1);
            if (typeof spec !== 'number') pushes.push({ topic: `game-${gameId}`, owner: null, seq: randomUUID(), version: row.version + 1, bytes: spec });
        }
        const delay = t.cycleDelayMs();
        commitRow(row, p, seats, null);
        note(`bots drove ${drive.n} action(s) by seat(s) [${drive.seats.join(', ')}] -> v${row.version}`);
        dispatch(pushes);
        if (p.ended) return;
        botTimers.set(gameId, setTimeout(() => { botTimers.delete(gameId); driveBots(gameId); }, Math.max(10, delay)));
    }

    /** Does the loaded row still want a bot? Read in its own tiny section. */
    function needsBots(row: Row): boolean {
        const t = fixtureTable();
        if (t.load(row.state, row.roster) < 0) return false;
        return t.needsBots();
    }

    // ---- HTTP ----------------------------------------------------------------

    const CORS = {
        'Access-Control-Allow-Origin': '*',
        'Access-Control-Allow-Methods': 'GET, POST, PUT, PATCH, DELETE, OPTIONS, HEAD',
        'Access-Control-Allow-Headers': 'authorization, x-client-info, apikey, content-type, accept, accept-profile, content-profile, prefer, range, x-supabase-api-version, x-region',
        'Access-Control-Expose-Headers': 'content-range, x-relay-error',
        'Access-Control-Max-Age': '86400',
    };

    const json = (res: ServerResponse, status: number, body: unknown) => {
        const text = JSON.stringify(body);
        res.writeHead(status, { ...CORS, 'Content-Type': 'application/json', 'Content-Length': Buffer.byteLength(text) });
        res.end(text);
    };
    const packed = (res: ServerResponse, bytes: Uint8Array) => {
        res.writeHead(200, { ...CORS, 'Content-Type': 'application/octet-stream', 'Content-Length': bytes.byteLength });
        res.end(Buffer.from(bytes));
    };

    const bodyOf = (req: IncomingMessage): Promise<Buffer> => new Promise((resolve, reject) => {
        const chunks: Buffer[] = [];
        req.on('data', (c: Buffer) => chunks.push(c));
        req.on('end', () => resolve(Buffer.concat(chunks)));
        req.on('error', reject);
    });

    /** The caller, from the bearer token's `sub`. */
    function callerOf(req: IncomingMessage): FakeUser | null {
        const auth = String(req.headers.authorization ?? '');
        const token = auth.startsWith('Bearer ') ? auth.slice(7) : '';
        const part = token.split('.')[1];
        if (!part) return null;
        try {
            const sub = JSON.parse(Buffer.from(part, 'base64url').toString('utf8')).sub;
            return users.get(String(sub)) ?? null;
        } catch { return null; }
    }

    async function handle(req: IncomingMessage, res: ServerResponse): Promise<void> {
        const url = new URL(req.url ?? '/', `http://127.0.0.1:${port}`);
        if (req.method === 'OPTIONS') { res.writeHead(204, CORS); res.end(); return; }
        note(`${req.method} ${url.pathname}${url.search}`);
        try {
            if (url.pathname.startsWith('/auth/v1/')) return await auth(req, res, url);
            if (url.pathname.startsWith('/rest/v1/')) return await rest(req, res, url);
            if (url.pathname.startsWith('/functions/v1/')) return await fn(req, res, url);
            if (url.pathname.startsWith('/realtime/v1/api/broadcast')) { res.writeHead(202, CORS); res.end('{}'); return; }
            if (url.pathname.startsWith('/__control/')) return await control(req, res, url);
            json(res, 404, { error: `no route ${url.pathname}` });
        } catch (e) {
            json(res, 400, { error: (e as Error).message, stack: (e as Error).stack });
        }
    }

    // GoTrue. Every password is right; an unknown name is signed up on the spot,
    // because the point of this server is the game, not the gate.
    async function auth(req: IncomingMessage, res: ServerResponse, url: URL): Promise<void> {
        const raw = (await bodyOf(req)).toString('utf8');
        const body = raw ? JSON.parse(raw) : {};
        const path = url.pathname.replace('/auth/v1/', '');
        if (path === 'settings') return json(res, 200, { external: {}, disable_signup: false, mailer_autoconfirm: true });
        if (path === 'token') {
            const grant = url.searchParams.get('grant_type');
            if (grant === 'refresh_token') {
                const id = String(body.refresh_token ?? '').replace(/^refresh-/, '');
                const u = users.get(id);
                return u ? json(res, 200, sessionFor(u)) : json(res, 400, { error: 'invalid_grant', error_description: 'unknown refresh token' });
            }
            const email = String(body.email ?? '');
            const u = byEmail.get(email) ?? registerByEmail(email);
            return json(res, 200, sessionFor(u));
        }
        if (path === 'signup') {
            const email = String(body.email ?? '');
            const name = String(body.data?.username ?? body.options?.data?.username ?? '') || 'PLAYER';
            let u = byEmail.get(email);
            if (!u) { u = register(name); byEmail.delete(u.email); u = { ...u, email }; users.set(u.id, u); byEmail.set(email, u); }
            return json(res, 200, sessionFor(u));
        }
        if (path === 'logout') { res.writeHead(204, CORS); res.end(); return; }
        if (path === 'user') {
            const me = callerOf(req);
            if (!me) return json(res, 401, { error: 'unauthenticated' });
            if (req.method === 'PUT') {
                const name = body.data?.username;
                if (typeof name === 'string') { const next = { ...me, name }; users.set(me.id, next); byEmail.set(next.email, next); return json(res, 200, userJson(next)); }
                return json(res, 200, userJson(me));
            }
            return json(res, 200, userJson(me));
        }
        return json(res, 404, { error: `auth: no ${path}` });
    }

    /** An email nobody registered: give it a seat anyway, named after its own hash. */
    function registerByEmail(email: string): FakeUser {
        for (const name of ['ME', 'ANNA', 'BORIS']) if (emailFor(name) === email) return register(name);
        const u: FakeUser = { id: `u-${email.slice(0, 6)}`, name: email.slice(0, 6).toUpperCase(), email };
        users.set(u.id, u);
        byEmail.set(email, u);
        return u;
    }

    // PostgREST, for the five tables the web reads.
    async function rest(req: IncomingMessage, res: ServerResponse, url: URL): Promise<void> {
        const table = url.pathname.replace('/rest/v1/', '');
        const me = callerOf(req);
        const eq = (col: string) => {
            const v = url.searchParams.get(col);
            return v?.startsWith('eq.') ? v.slice(3) : null;
        };
        const single = String(req.headers.accept ?? '').includes('pgrst.object');
        const answer = (list: unknown[]) => {
            if (!single) return json(res, 200, list);
            if (list.length === 1) return json(res, 200, list[0]);
            return json(res, 406, { code: 'PGRST116', message: 'JSON object requested, multiple (or no) rows returned', details: `Results contain ${list.length} rows`, hint: null });
        };

        if (req.method === 'POST') { res.writeHead(201, { ...CORS, 'Content-Type': 'application/json' }); res.end('[]'); return; }

        if (table === 'player_views') {
            if (!me) return answer([]);
            const gid = eq('game_id'), pid = eq('player_id');
            const out: unknown[] = [];
            for (const [id, v] of views) {
                if (gid && id !== gid) continue;
                if (pid && pid !== me.id) continue;
                const mine = v.perSeat.find((s) => s.id === me.id);
                if (!mine?.bytes) continue;
                const row = rows.get(id);
                out.push({ game_id: id, player_id: me.id, view: `\\x${bytesToBareHex(mine.bytes)}`, status: 'playing', version: row?.version ?? 0, updated_at: new Date().toISOString() });
            }
            return answer(out);
        }
        if (table === 'spectator_views') {
            const gid = eq('game_id');
            const out: unknown[] = [];
            for (const [id, v] of views) {
                if (gid && id !== gid) continue;
                if (v.spectator.length === 0) continue;
                out.push({ game_id: id, view: `\\x${bytesToBareHex(v.spectator)}` });
            }
            return answer(out);
        }
        if (table === 'bots') return answer(BOTS);
        if (table === 'game_snapshots' || table === 'user_elo_ratings' || table === 'chat_messages') return answer([]);
        return json(res, 404, { message: `relation "${table}" does not exist` });
    }

    // The edge functions. `action` is the one that matters: its body is the
    // kernel's own action request and its answer the kernel's action response.
    async function fn(req: IncomingMessage, res: ServerResponse, url: URL): Promise<void> {
        const name = url.pathname.replace('/functions/v1/', '');
        const me = callerOf(req);
        if (!me) return json(res, 401, { error: 'unauthenticated' });
        const raw = await bodyOf(req);
        const isJson = String(req.headers['content-type'] ?? '').includes('json');

        if (name === 'create') {
            const gameId = randomUUID().slice(0, 6);
            const t = fixtureTable();
            const rc = t.create(me.id, me.name);
            if (rc < 0) return json(res, 400, { error: `create refused (${rc})` });
            const p = t.commit(gameId, 0, Date.now());
            if (typeof p === 'number') return json(res, 400, { error: `create: no products (${p})` });
            const row: Row = { id: gameId, state: p.state, roster: p.roster, version: 0, roundEpoch: 0, seedHex: null, log: new Uint8Array(0) };
            rows.set(gameId, row);
            views.set(gameId, { spectator: p.spectator, perSeat: t.seats().map((s, i) => ({ seat: i, id: s.id, bytes: p.views[i] })) });
            const mine = p.views[0];
            if (!mine) return json(res, 400, { error: 'create: no envelope' });
            return packed(res, mine);
        }

        if (name === 'action' && !isJson) {
            const t0 = fixtureTable();
            const decoded = t0.requestDecode(new Uint8Array(raw));
            if (typeof decoded === 'number') return json(res, 400, { error: 'malformed action request' });
            const row = rows.get(decoded.gameId);
            if (!row) return json(res, 400, { error: `unknown game ${decoded.gameId}` });
            const out = run(row, me.id, (t) => t.act(me.id, decoded.wire, decoded.intent, row.roundEpoch));
            note(`action by ${me.id}: rc=${out.rc} reject=${out.reject} -> v${row.version}`);
            dispatch(out.pushes);
            if (needsBots(row)) wakeBots(row.id);
            const t = fixtureTable();
            t.load(row.state, row.roster);
            return packed(res, t.actionResponse(out.rc, out.reject, row.version));
        }

        const body = raw.length > 0 ? JSON.parse(raw.toString('utf8')) : {};
        if (name === 'action') {
            // The JSON side of `action` is one request: `bump`, the bot nudge.
            const row = rows.get(String(body.game_id ?? ''));
            if (!row) return json(res, 400, { error: `unknown game ${body.game_id}` });
            if (needsBots(row)) wakeBots(row.id);
            const t = fixtureTable();
            t.load(row.state, row.roster);
            const env = t.envelope(row.id, t.seatOf(me.id), row.version);
            return typeof env === 'number' ? json(res, 400, { error: `no envelope (${env})` }) : packed(res, env);
        }

        if (name === 'meta') {
            const row = rows.get(String(body.game_id ?? ''));
            if (!row) return json(res, 400, { error: `unknown game ${body.game_id}` });
            const out = run(row, me.id, (t, seed) => metaOp(t, me, body, seed));
            dispatch(out.pushes);
            if (needsBots(row)) wakeBots(row.id);
            return out.envelope ? packed(res, out.envelope) : json(res, 400, { error: `meta ${body.type} refused (${out.rc})` });
        }

        if (name === 'delete-account') return json(res, 200, { ok: true });
        return json(res, 404, { error: `no function ${name}` });
    }

    function metaOp(t: ReturnType<typeof fixtureTable>, me: FakeUser, body: any, seed: Uint8Array): number {
        switch (body?.type) {
            case 'start': return t.ready(me.id, seed);
            case 'join': return t.join(me.id, me.name);
            case 'exit': return body.bot_id ? t.removeBot(me.id, String(body.bot_id)) : t.leave(me.id, String(body.player_id ?? me.id));
            case 'add-bot': {
                const seated = new Set(t.seats().map((s) => s.id));
                const pool = body.bot_id ? BOTS.filter((b) => b.id === body.bot_id) : BOTS.filter((b) => !seated.has(b.id));
                const bot = pool[0];
                if (!bot) throw new Error('no available bots');
                return t.addBot(me.id, bot.id, bot.nickname, bot.strategy_key, seed);
            }
            case 'continue': return t.continueGame(me.id);
            case 'rearrange-hand': return t.rearrangeHand(me.id, body.card_indices ?? []);
            case 'rearrange-players': return t.reseat(me.id, body.new_order ?? []);
            case 'update-name': return t.retitle(me.id, String(body.new_name ?? ''));
            default: throw new Error(`unknown meta type ${body?.type}`);
        }
    }

    // ---- the control API -----------------------------------------------------

    const api: FakeBackend = {
        url: `http://127.0.0.1:${port}`,
        hub,
        scenario(name) {
            const built = buildScenario(name);
            const row: Row = { id: built.gameId, state: built.board.state, roster: built.board.roster, version: 10, roundEpoch: 0, seedHex: null, log: new Uint8Array(0) };
            rows.set(row.id, row);
            for (const n of built.users) register(n);
            refreshViews(row);
            note(`scenario ${name} -> ${row.id}`);
            if (needsBots(row)) wakeBots(row.id);
            return { gameId: row.id, users: built.users, url: `http://localhost:3000/${row.id}` };
        },
        act(userId, gameId, move) {
            const row = rows.get(gameId);
            if (!row) throw new Error(`unknown game ${gameId}`);
            const wire = encodeAction({
                kind: move.kind,
                cards: move.cards ? parseCardText(move.cards) : undefined,
                attack_cards: move.attack_cards ? parseCardText(move.attack_cards) : undefined,
            } as AwireMove);
            const out = run(row, userId, (t) => t.act(userId, wire, null, row.roundEpoch));
            note(`control act ${userId} ${move.kind} ${move.cards ?? ''}: rc=${out.rc} -> v${row.version}`);
            dispatch(out.pushes);
            if (needsBots(row)) wakeBots(row.id);
            return { rc: out.rc, reject: out.reject, version: row.version };
        },
        hold(userId, on) {
            if (on) holding.add(userId); else holding.delete(userId);
            note(`hold ${userId} = ${on}`);
        },
        release(userId, n) {
            const q = held.get(userId) ?? [];
            const take = n === undefined ? q.length : Math.min(n, q.length);
            for (let i = 0; i < take; i++) send(q.shift()!);
            held.set(userId, q);
            return take;
        },
        state(gameId) {
            const row = rows.get(gameId);
            if (!row) return null;
            const t = fixtureTable();
            if (t.load(row.state, row.roster) < 0) return null;
            const seats = t.seats().map((s, i) => ({ seat: i, id: s.id, brain: s.brain }));
            return { gameId, version: row.version, roundEpoch: row.roundEpoch, table: tableText(row), seats };
        },
        log: () => trace.slice(),
        reset() {
            rows.clear(); views.clear(); held.clear(); holding.clear();
            for (const timer of botTimers.values()) clearTimeout(timer);
            botTimers.clear();
            trace.length = 0;
        },
        stop: () => new Promise<void>((resolve) => {
            for (const timer of botTimers.values()) clearTimeout(timer);
            botTimers.clear();
            hub.closeAll();
            server.closeAllConnections?.();
            server.close(() => resolve());
        }),
    };

    /**
     * The row's table as card text, for a trace line. Read off the SPECTATOR
     * envelope through the client slot - the same reader a browser uses - so
     * nothing here needs the game's own byte layout.
     */
    function tableText(row: Row): string[] {
        const v = views.get(row.id);
        if (!v || v.spectator.length === 0) return [];
        const board = clientTable().adoptEnvelope(v.spectator);
        if (!board) return [];
        return board.battles.flatMap((b) => (b.defense.value > 0 ? [cardText(b.attack), cardText(b.defense)] : [cardText(b.attack)]));
    }

    async function control(req: IncomingMessage, res: ServerResponse, url: URL): Promise<void> {
        const path = url.pathname.replace('/__control/', '');
        const raw = (await bodyOf(req)).toString('utf8');
        const body = raw ? JSON.parse(raw) : {};
        switch (path) {
            case 'scenario': return json(res, 200, api.scenario(String(body.name ?? 'throw_in_race')));
            case 'act': return json(res, 200, api.act(String(body.user), String(body.gameId), body.move as MoveSpec));
            case 'hold': api.hold(String(body.user), body.on !== false); return json(res, 200, { ok: true });
            case 'release': return json(res, 200, { released: api.release(String(body.user), body.n) });
            case 'state': return json(res, 200, api.state(String(url.searchParams.get('gameId') ?? '')));
            case 'log': return json(res, 200, api.log());
            case 'reset': api.reset(); return json(res, 200, { ok: true });
            case 'ping': return json(res, 200, { ok: true, users: [...users.values()].map((u) => u.name) });
            default: return json(res, 404, { error: `no control ${path}` });
        }
    }

    await new Promise<void>((resolve) => server.listen(port, '127.0.0.1', resolve));
    return api;
}

// ---- the bots table -----------------------------------------------------------

const BOTS = [
    { id: 'bot-cordite-1', nickname: '\u{1F916}Cordite', strategy_key: 'cordite', elo_rating: 1500 },
    { id: 'bot-espresso-1', nickname: '\u{1F916}Espresso', strategy_key: 'espresso', elo_rating: 1200 },
    { id: 'bot-random-1', nickname: '\u{1F916}Random', strategy_key: 'random', elo_rating: 900 },
];

// ---- the scenarios ------------------------------------------------------------
//
// A scenario is a board and the people at it. Each one is the SAME builder
// e2e/ui_animation_trace.test.ts uses, so a case there and a run here start from
// the same table.

const seat = (name: string): FixtureSeat => ({ id: `u-${name.toLowerCase()}`, name: name.toUpperCase() });

interface Scenario { gameId: string; board: TableFixture; users: string[] }

const SCENARIOS: Record<string, () => Scenario> = {
    /**
     * THE HELD PILE, staged: three seats, one attack down, and two throw-ins to
     * come. ME leads (seat 0), ANNA defends (seat 1), BORIS is the third
     * attacker. Everyone holds a seven, so either attacker may throw one in, and
     * ANNA has enough hand to be attacked three times.
     *
     * The race the browser trace drives on it:
     *   1. BORIS throws in 7s          (the server commits it at v+1)
     *   2. ME taps 7d in the browser   (v+2) - both pushes are HELD
     *   3. release BORIS's push        - 7s flies, and MY pending 7d must keep its cell
     *   4. release MY OWN push mid-flight - every one of its events is a
     *      confirmation of a motion this client already animated, so
     *      AnimationContext commits its board AT ONCE: the board now holds 7s
     *      while 7s is still in the air, which is the only thing `heldPiles`
     *      exists for, and 7s must get NO grid cell until it lands.
     */
    throw_in_race: () => ({
        gameId: 'race01',
        users: ['ME', 'ANNA', 'BORIS'],
        board: fixture().title('The race').seats([seat('ME'), seat('ANNA'), seat('BORIS')])
            .status(PLAYING).deterministic().trump('Kc').deck('8s 9s Ts Js Qs')
            .hand(0, '7d Tc Jd Ad').hand(1, '8h 9h Th Jh Qh').hand(2, '7s Qd 6d 6c')
            .table('7h').attacker(0).defender(1).build(),
    }),

    /** A plain two-hander against a bot, for playing by hand. */
    versus_bot: () => ({
        gameId: 'bot001',
        users: ['ME'],
        board: fixture().title('Against a bot')
            .seats([seat('ME'), { id: 'bot-cordite-1', name: '\u{1F916}Cordite', brain: 'cordite' }])
            .status(PLAYING).deterministic().trump('Kc').deck('8s 9s Ts Js Qs 6s 7s')
            .hand(0, '6h 7d Tc Jd Ad Qc').hand(1, '8h 9h Th Jh Qh Ah')
            .attacker(0).defender(1).build(),
    }),

    /** Three humans, nothing on the table: sign in as any of them in three tabs. */
    open_table: () => ({
        gameId: 'open01',
        users: ['ME', 'ANNA', 'BORIS'],
        board: fixture().title('Open table').seats([seat('ME'), seat('ANNA'), seat('BORIS')])
            .status(PLAYING).deterministic().trump('Kc').deck('8s 9s Ts Js Qs 6s 7s 8d 9d')
            .hand(0, '6h 7d Tc Jd Ad Qc').hand(1, '8h 9h Th Jh Qh Ah').hand(2, '7s Qd 6d 6c Kd Ks')
            .attacker(0).defender(1).build(),
    }),
};

function buildScenario(name: string): Scenario {
    const make = SCENARIOS[name];
    if (!make) throw new Error(`no scenario "${name}"; try ${Object.keys(SCENARIOS).join(', ')}`);
    return make();
}

/** A card as the kernel prints it ("7d"): c/src/card.c's own grammar, for a trace line. */
const cardText = (c: { suit: number; value: number }): string =>
    `${'23456789TJQKA'[c.value - 1] ?? '?'}${'shcd'[c.suit] ?? '?'}`;

// ---- the command line ---------------------------------------------------------

const isEntry = process.argv[1] !== undefined && /fake_supabase\.mts$/.test(process.argv[1]);
if (isEntry) {
    const argv = process.argv.slice(2);
    const flag = (name: string, fallback?: string) => {
        const i = argv.indexOf(`--${name}`);
        return i >= 0 ? argv[i + 1] : fallback;
    };
    if (flag('scenario') === 'list') {
        process.stdout.write(`scenarios: ${Object.keys(SCENARIOS).join(', ')}\n`);
    } else {
        const port = Number(flag('port', '54321'));
        const be = await startFakeSupabase({ port, verbose: !argv.includes('--quiet') });
        process.stdout.write(`fake supabase on ${be.url}  (auth, rest, functions, realtime)\n`);
        const want = flag('scenario');
        if (want) {
            const g = be.scenario(want);
            process.stdout.write(`scenario "${want}": game ${g.gameId}, seats ${g.users.join(', ')}\n  open ${g.url} and sign in as ${g.users[0]} (any password)\n`);
        } else {
            process.stdout.write('no scenario: sign in at http://localhost:3000 and create a table\n');
        }
        process.on('SIGINT', () => { void be.stop().then(() => process.exit(0)); });
    }
}
