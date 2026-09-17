// SECURITY S3: the seat a request acts as comes from the auth token, and only
// from the auth token.
//
// Every request here goes through the REAL edge entry point - the handler
// functions/<name>/index.ts registers with serve() - carrying a bearer token the
// real verifier (auth.ts verifyJwtLocal) accepts. The caller is user A. Each
// hostile request tries to make A's request land as somebody else, or on a game
// A does not sit at:
//
//   - a body naming another user's player_id / user_id / seat
//   - a legal move that belongs to another seat, sent by A
//   - a request against a game A is not in
//   - a seat index outside the table
//
// and asserts two things: the request is refused (or, where the API has no such
// field, the effect lands on A and only A), and nothing observable changed - the
// games row (version, state blob, roster, columns), the membership rows, every
// player_views / spectator_views row, and the realtime broadcast log. Each group
// also has the positive control: A acting as A goes through.
//
// Every row is kernel-owned (e2e/helpers/table_db.ts) and read back through the
// C Table (e2e/helpers/table_play.ts): the seat of an auth id is the kernel's
// roster lookup. e2e/table_server_seat.test.ts pins the same boundary on
// hand-built boards; this file adds the whole-world "nothing changed" check, on
// games dealt by the real `meta` start.
//
// Kicking a lobby member IS a product feature (any seated lobby player may
// remove another - Lobby.tsx, pinned in adversarial_ts_layer.test.ts). What is
// pinned here is the boundary around it: only somebody SEATED at that lobby.

import './harness.ts';
import { test, before, beforeEach, after } from 'node:test';
import assert from 'node:assert/strict';
import { applySchema, resetDb, uuid, pgPool, broadcastLog } from './harness.ts';
import { settle, tokenFor, postJson, postPacked } from './helpers/edge.ts';
import { __clearGameCache } from '../server/impls/supabase/functions/_shared/adapter/game_cache.ts';
import { __setTableDealSeedOverride } from '../server/impls/supabase/functions/_shared/adapter/table_io.ts';
import { ACTION_STATUS, decodeActionResponse } from '../sdk/ts/wire/awire.ts';
import { readEnvelopeView } from './helpers/client_read.ts';
import { IDLE, PLAYING, READY, WAITING } from './helpers/table_fixture.ts';
import { actionRequest, legalMoves, mustReadTable } from './helpers/table_play.ts';
import { seedLobby } from './helpers/table_server.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; console.error = () => {}; }

// A pinned deal: the tests pick moves by rule (the seat to move, its first
// legal move), so a fixed deal makes a red run replay exactly.
__setTableDealSeedOverride(Uint8Array.from({ length: 32 }, (_, i) => (i * 37 + 11) & 0xff));

// ---- the observable world ----------------------------------------------------

// Everything a request could change that anyone can observe. Timestamps that a
// no-op never touches are included on purpose: a refused request must not even
// bump updated_at.
async function world(gameId: string) {
    const q = async (sql: string) => (await pgPool.query(sql, [gameId])).rows;
    await settle();
    return {
        games: await q('SELECT * FROM games WHERE id=$1'),
        playerHands: await q('SELECT game_id, player_id FROM player_hands WHERE game_id=$1 ORDER BY player_id'),
        botHands: await q('SELECT game_id, bot_id FROM bot_hands WHERE game_id=$1 ORDER BY bot_id'),
        playerViews: await q('SELECT player_id, view, version, status FROM player_views WHERE game_id=$1 ORDER BY player_id'),
        spectatorViews: await q('SELECT view, version, status FROM spectator_views WHERE game_id=$1'),
        broadcasts: broadcastLog.length,
    };
}

async function assertUnchanged(gameId: string, before: Awaited<ReturnType<typeof world>>, what: string) {
    const now = await world(gameId);
    assert.equal(now.broadcasts, before.broadcasts, `${what}: no realtime broadcast`);
    assert.deepEqual(now.games, before.games, `${what}: games row unchanged`);
    assert.deepEqual(now.playerHands, before.playerHands, `${what}: player membership unchanged`);
    assert.deepEqual(now.botHands, before.botHands, `${what}: bot membership unchanged`);
    assert.deepEqual(now.playerViews, before.playerViews, `${what}: player_views unchanged`);
    assert.deepEqual(now.spectatorViews, before.spectatorViews, `${what}: spectator_views unchanged`);
}

// A refusal is a 400 carrying the reason. The reason is matched loosely: the
// point is that the request was refused for the identity, not which sentence.
function assertRefused(res: { status: number; json: any }, why: RegExp, what: string) {
    assert.equal(res.status, 400, `${what}: refused with 400 (got ${res.status} ${JSON.stringify(res.json)})`);
    assert.match(String(res.json?.error ?? ''), why, `${what}: names the reason`);
}

const ids = async (gameId: string) => (await mustReadTable(gameId)).seats.map((s) => s.id);

// ---- fixtures ----------------------------------------------------------------

type Who = 'A' | 'B' | 'C' | 'S';
interface Cast { A: string; B: string; C: string; S: string; bot: string; bot2: string; tok: Record<Who, string> }

async function cast(): Promise<Cast> {
    const A = uuid(), B = uuid(), C = uuid(), S = uuid(), bot = uuid(), bot2 = uuid();
    await pgPool.query('INSERT INTO auth.users(id) VALUES ($1),($2),($3),($4) ON CONFLICT DO NOTHING', [A, B, C, S]);
    await pgPool.query('INSERT INTO bots(id,nickname,strategy_key) VALUES ($1,$2,$3),($4,$5,$6)',
        [bot, 'Botty', 'random', bot2, 'Botto', 'random']);
    return {
        A, B, C, S, bot, bot2,
        tok: {
            A: await tokenFor(A, 'alice'), B: await tokenFor(B, 'bob'),
            C: await tokenFor(C, 'carol'), S: await tokenFor(S, 'stranger'),
        },
    };
}

// A lobby that cannot deal by accident: A, B (humans) and a bot are seated, and
// B is not ready, so nothing a test does short of B readying starts it.
async function lobby(c: Cast, ready: { A?: boolean; B?: boolean } = {}): Promise<string> {
    const gameId = `l${uuid().slice(0, 5)}`;
    await seedLobby(gameId, [
        { id: c.A, name: 'alice', ready: ready.A ?? true },
        { id: c.B, name: 'bob', ready: ready.B ?? false },
        { id: c.bot, name: 'Botty', brain: 'random' },
    ]);
    return gameId;
}

// A dealt game of two humans, started through the real meta endpoint by each.
async function dealtBy(c: Cast, who: ['A' | 'B' | 'C', 'A' | 'B' | 'C'], prefix: string): Promise<string> {
    const gameId = `${prefix}${uuid().slice(0, 5)}`;
    await seedLobby(gameId, who.map((w) => ({ id: c[w], name: w, ready: false })));
    for (const w of who) {
        const res = await postJson('meta', c.tok[w], { type: 'start', game_id: gameId });
        assert.equal(res.status, 200, `fixture: ${w} readies (${JSON.stringify(res.json)})`);
    }
    await settle();
    assert.equal((await mustReadTable(gameId)).status, PLAYING, 'fixture: the game dealt');
    return gameId;
}
const dealt = (c: Cast) => dealtBy(c, ['A', 'B'], 'd');

// The first legal move of whoever may move, and the human who may NOT make that
// move (the other seat).
async function moverAndBystander(gameId: string, c: Cast) {
    const t = await mustReadTable(gameId);
    const moves = legalMoves(t).filter((m) => m.kind !== 'good');
    assert.ok(moves.length > 0, 'fixture: somebody can move');
    const pm = moves[0];
    const mover = pm.playerId === c.A ? 'A' : 'B';
    const bystander = mover === 'A' ? 'B' : 'A';
    // The bystander must not hold a move with those cards, or "A sends B's move"
    // would be a legal move of A's and prove nothing.
    const own = legalMoves(t, (s) => s.id === c[bystander])
        .some((m) => m.kind === pm.kind && JSON.stringify(m.cards) === JSON.stringify(pm.cards));
    assert.ok(!own, 'fixture: the move is only legal for the mover');
    return { pm, mover, bystander, t } as const;
}

before(async () => { await applySchema(); });
beforeEach(async () => { await settle(); await resetDb(); __clearGameCache(); });
after(async () => { await settle(); });

// =============================================================================
// action (packed moves)
// =============================================================================

test('action: the seat to move, acting as itself, applies (positive control)', async () => {
    const c = await cast();
    const gameId = await dealt(c);
    const { pm, mover, t } = await moverAndBystander(gameId, c);
    const res = await postPacked(c.tok[mover], actionRequest(gameId, pm));
    assert.equal(res.status, 200);
    const out = decodeActionResponse(res.bytes)!;
    assert.equal(out.status, ACTION_STATUS.APPLIED, 'the move applied');
    assert.equal(out.version, t.version + 1, 'and committed');
});

test('action: a seated player sending the OTHER seat\'s legal move is rejected and changes nothing', async () => {
    const c = await cast();
    const gameId = await dealt(c);
    const { pm, bystander } = await moverAndBystander(gameId, c);
    const before = await world(gameId);
    const res = await postPacked(c.tok[bystander], actionRequest(gameId, pm));
    assert.equal(res.status, 200, 'a rules rejection is a packed response, not an HTTP error');
    const out = decodeActionResponse(res.bytes)!;
    assert.equal(out.status, ACTION_STATUS.REJECTED, `the kernel judged the move as ${bystander}'s, and refused it`);
    await assertUnchanged(gameId, before, `${bystander} plays the mover's cards`);
});

test('action: a stranger sending the seat-to-move\'s legal move is refused and changes nothing', async () => {
    const c = await cast();
    const gameId = await dealt(c);
    const { pm } = await moverAndBystander(gameId, c);
    const before = await world(gameId);
    const res = await postPacked(c.tok.S, actionRequest(gameId, pm));
    assertRefused(res, /not in game/i, 'stranger plays the mover\'s move');
    await assertUnchanged(gameId, before, 'stranger plays the mover\'s move');
});

test('action: the JSON body cannot name the actor (player_id / user_id / seat)', async () => {
    const c = await cast();
    const gameId = await dealt(c);
    const { pm, mover, bystander } = await moverAndBystander(gameId, c);
    const before = await world(gameId);
    for (const [who, label] of [[bystander, 'seated bystander'], ['S', 'stranger']] as const) {
        const res = await postJson('action', c.tok[who], {
            type: pm.kind, game_id: gameId, cards: pm.cards, attack_cards: pm.attack_cards,
            player_id: c[mover], user_id: c[mover], seat: pm.seat, player_index: pm.seat,
        });
        assertRefused(res, label === 'stranger' ? /not in game/i : /packed/i, `${label} names the mover in a JSON move`);
        await assertUnchanged(gameId, before, `${label} names the mover in a JSON move`);
    }
});

test('action: a forged token naming another user is refused before any game is read', async () => {
    const c = await cast();
    const gameId = await dealt(c);
    const { pm, mover, bystander } = await moverAndBystander(gameId, c);
    const before = await world(gameId);
    // The bystander's genuine token, with the payload swapped for one naming
    // the mover: the signature no longer matches.
    const [h, , s] = c.tok[bystander].split('.');
    const moverPayload = c.tok[mover].split('.')[1];
    const res = await postPacked(`${h}.${moverPayload}.${s}`, actionRequest(gameId, pm));
    assertRefused(res, /invalid token/i, 'a token re-pointed at the mover');
    const none = await postPacked(null, actionRequest(gameId, pm));
    assertRefused(none, /authorization/i, 'no token at all');
    await assertUnchanged(gameId, before, 'forged / missing token');
});

test('action: a move on a game the caller does not sit at is refused and changes nothing', async () => {
    const c = await cast();
    const mine = await dealt(c);
    // A second table with B and C: A is not there.
    const other = await dealtBy(c, ['B', 'C'], 'o');
    const pm = legalMoves(await mustReadTable(other)).filter((m) => m.kind !== 'good')[0];
    const before = await world(other);
    const beforeMine = await world(mine);
    const res = await postPacked(c.tok.A, actionRequest(other, pm));
    assertRefused(res, /not in game/i, 'A plays at a table A is not at');
    await assertUnchanged(other, before, 'A plays at a table A is not at');
    await assertUnchanged(mine, beforeMine, 'A\'s own table is untouched too');
});

// =============================================================================
// meta
// =============================================================================

test('meta start: readies the CALLER only, whoever the body names; a stranger is refused', async () => {
    const c = await cast();
    const gameId = await lobby(c, { A: false });

    const before = await world(gameId);
    assertRefused(await postJson('meta', c.tok.S, { type: 'start', game_id: gameId, player_id: c.B }),
        /not in game/i, 'stranger readies B');
    await assertUnchanged(gameId, before, 'stranger readies B');

    const res = await postJson('meta', c.tok.A, { type: 'start', game_id: gameId, player_id: c.B, user_id: c.B, seat: 1 });
    assert.equal(res.status, 200, 'A readies (positive control)');
    const t = await mustReadTable(gameId);
    assert.equal(t.seats.find((p) => p.id === c.A)!.status, READY, 'A is ready');
    assert.equal(t.seats.find((p) => p.id === c.B)!.status, IDLE, 'B, named in the body, is not');
    assert.equal(t.status, WAITING, 'so nothing dealt');
});

test('meta join: seats the CALLER, never the user the body names', async () => {
    const c = await cast();
    const gameId = await lobby(c);
    const res = await postJson('meta', c.tok.C, { type: 'join', game_id: gameId, player_id: c.S, user_id: c.S });
    assert.equal(res.status, 200, 'C joins (positive control)');
    const seated = await ids(gameId);
    assert.ok(seated.includes(c.C), 'the caller is seated');
    assert.ok(!seated.includes(c.S), 'the user named in the body is not');
});

test('meta add-bot: a caller who is not seated at the lobby is refused and changes nothing', async () => {
    const c = await cast();
    const gameId = await lobby(c);
    const before = await world(gameId);
    assertRefused(await postJson('meta', c.tok.S, { type: 'add-bot', game_id: gameId, bot_id: c.bot2 }),
        /not in game/i, 'stranger adds a bot');
    await assertUnchanged(gameId, before, 'stranger adds a bot');

    const res = await postJson('meta', c.tok.A, { type: 'add-bot', game_id: gameId, bot_id: c.bot2 });
    assert.equal(res.status, 200, `a seated player adds a bot (positive control): ${JSON.stringify(res.json)}`);
    assert.ok((await ids(gameId)).includes(c.bot2), 'the bot is seated');
});

test('meta add-bot: a stranger cannot deal somebody else\'s all-ready lobby by adding its last bot', async () => {
    const c = await cast();
    const gameId = await lobby(c, { B: true }); // every seat ready: one more bot deals
    const before = await world(gameId);
    assertRefused(await postJson('meta', c.tok.S, { type: 'add-bot', game_id: gameId, bot_id: c.bot2 }),
        /not in game/i, 'stranger starts the lobby');
    await assertUnchanged(gameId, before, 'stranger starts the lobby');
    assert.equal((await mustReadTable(gameId)).status, WAITING, 'the lobby did not deal');
});

test('meta exit: a caller who is not seated cannot remove a player, a bot, or "themselves"', async () => {
    const c = await cast();
    const gameId = await lobby(c);
    const before = await world(gameId);
    for (const [body, what] of [
        [{ player_id: c.B }, 'stranger kicks B'],
        [{ bot_id: c.bot }, 'stranger removes the bot'],
        [{}, 'stranger exits a game they are not in'],
        [{ player_id: c.S }, 'stranger names themselves'],
    ] as const) {
        assertRefused(await postJson('meta', c.tok.S, { type: 'exit', game_id: gameId, ...body }), /not in game/i, what);
        await assertUnchanged(gameId, before, what);
    }
});

test('meta exit: a seated player may leave, remove a bot, or kick (the lobby feature, positive control)', async () => {
    const c = await cast();
    const gameId = await lobby(c);
    assert.equal((await postJson('meta', c.tok.A, { type: 'exit', game_id: gameId, bot_id: c.bot })).status, 200, 'A removes the bot');
    assert.equal((await postJson('meta', c.tok.A, { type: 'exit', game_id: gameId, player_id: c.B })).status, 200, 'A kicks B');
    assert.deepEqual(await ids(gameId), [c.A], 'only A remains');
    const g2 = await lobby(c);
    assert.equal((await postJson('meta', c.tok.B, { type: 'exit', game_id: g2 })).status, 200, 'B leaves');
    const seated = await ids(g2);
    assert.ok(!seated.includes(c.B) && seated.includes(c.A), 'B left, A stayed');
});

test('meta continue / rearrange-players / update-name: a stranger is refused and changes nothing', async () => {
    const c = await cast();
    const gameId = await lobby(c);
    const before = await world(gameId);
    for (const body of [
        { type: 'continue' },
        { type: 'rearrange-players', new_order: [c.bot, c.B, c.A] },
        { type: 'update-name', new_name: 'owned' },
    ]) {
        assertRefused(await postJson('meta', c.tok.S, { ...body, game_id: gameId, player_id: c.A }), /not in game/i, `stranger ${body.type}`);
        await assertUnchanged(gameId, before, `stranger ${body.type}`);
    }
    // Positive control for the two lobby edits.
    assert.equal((await postJson('meta', c.tok.A, { type: 'update-name', game_id: gameId, new_name: 'ours' })).status, 200);
    assert.equal((await postJson('meta', c.tok.B, { type: 'rearrange-players', game_id: gameId, new_order: [c.bot, c.B, c.A] })).status, 200);
    const t = await mustReadTable(gameId);
    assert.equal(t.title, 'ours');
    assert.deepEqual(t.seats.map((p) => p.id), [c.bot, c.B, c.A]);
});

test('meta rearrange-hand: reorders the CALLER\'s hand only; a stranger or a named seat is refused or ignored', async () => {
    const c = await cast();
    const gameId = await dealt(c);
    const t0 = await mustReadTable(gameId);
    const handOf = (t: typeof t0, id: string) => t.seats.find((p) => p.id === id)!.hand;
    const n = handOf(t0, c.A).length;
    const reversed = Array.from({ length: n }, (_, i) => n - 1 - i);

    const before = await world(gameId);
    assertRefused(await postJson('meta', c.tok.S, { type: 'rearrange-hand', game_id: gameId, card_indices: reversed, player_id: c.A }),
        /not in game/i, 'stranger reorders A\'s hand');
    await assertUnchanged(gameId, before, 'stranger reorders A\'s hand');

    // B names A (by id and by seat, and with a seat off the table); only B's own hand may move.
    const nB = handOf(t0, c.B).length;
    const reversedB = Array.from({ length: nB }, (_, i) => nB - 1 - i);
    for (const seat of [0, 1, 99, -1]) {
        const res = await postJson('meta', c.tok.B, {
            type: 'rearrange-hand', game_id: gameId, card_indices: reversedB, player_id: c.A, user_id: c.A, seat,
        });
        assert.equal(res.status, 200, `B reorders (seat field ${seat}): ${JSON.stringify(res.json)}`);
        assert.deepEqual(handOf(await mustReadTable(gameId), c.A), handOf(t0, c.A), `A's hand never moved (seat field ${seat})`);
    }
    const t = await mustReadTable(gameId);
    assert.deepEqual(handOf(t, c.B), handOf(t0, c.B), 'four reversals of B\'s own hand bring it back');
    assert.equal(t.version, t0.version + 4, 'and each one was B\'s own commit');
});

test('action bump: open to anyone by design, and it changes nothing', async () => {
    const c = await cast();
    const gameId = await dealt(c);
    const before = await world(gameId);
    const res = await postJson('action', c.tok.S, { type: 'bump', game_id: gameId, player_id: c.A });
    assert.equal(res.status, 200, 'a spectator may nudge a stalled game');
    const view = readEnvelopeView(res.bytes);
    assert.ok(view, 'the response is a packed view');
    assert.equal(view!.mySeat, -1, 'and it is the SPECTATOR view, not the view of the player the body named');
    // A bump commits nothing on the C Table (it only reads the row and wakes the
    // bots the kernel says have work), so not even the version token moves.
    await assertUnchanged(gameId, before, 'stranger bump');
});

// =============================================================================
// create
// =============================================================================

test('create: the new lobby seats the CALLER, whoever the body names', async () => {
    const c = await cast();
    const res = await postJson('create', c.tok.A, { player_id: c.B, user_id: c.B, name: 'x' });
    assert.equal(res.status, 200, `create succeeds: ${JSON.stringify(res.json)}`);
    const view = readEnvelopeView(res.bytes);
    assert.ok(view, 'the response is a packed view');
    await settle();
    assert.deepEqual(await ids(view!.gameId), [c.A], 'seated: the caller alone');
    const { rows: views } = await pgPool.query('SELECT player_id FROM player_views WHERE game_id=$1', [view!.gameId]);
    assert.deepEqual(views.map((r) => r.player_id), [c.A], 'the only view row is the caller\'s');
});
