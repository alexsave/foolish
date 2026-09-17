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
// Kicking a lobby member IS a product feature (any seated lobby player may
// remove another - Lobby.tsx, pinned in adversarial_ts_layer.test.ts). What is
// pinned here is the boundary around it: only somebody SEATED at that lobby.

import './harness.ts';
import { test, before, beforeEach, after } from 'node:test';
import assert from 'node:assert/strict';
import { applySchema, resetDb, seedGame, uuid, pgPool, broadcastLog } from './harness.ts';
import { settle, tokenFor, postJson, postPacked } from './helpers/edge.ts';
import { loadCompleteGame } from '../server/impls/supabase/functions/_shared/adapter/utils.ts';
import { __clearGameCache } from '../server/impls/supabase/functions/_shared/adapter/game_cache.ts';
import { __setDealSeedOverride } from '../sdk/ts/wasm/engine.ts';
import { ACTION_STATUS, AwireMove, decodeActionResponse, encodeAction, encodeActionRequest } from '../sdk/ts/wire/awire.ts';
import { decodePackedGame } from '../sdk/ts/wire/view.ts';
import { legalMovesFor, PlayerMove } from './dispatch.ts';
import { GAME_STATUS, PLAYER_STATUS } from '../server/api/core/types.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; console.error = () => {}; }

// A pinned deal: the tests pick moves by rule (the seat to move, its first
// legal move), so a fixed deal makes a red run replay exactly.
__setDealSeedOverride(Uint8Array.from({ length: 32 }, (_, i) => (i * 37 + 11) & 0xff));

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

// ---- fixtures ----------------------------------------------------------------

interface Cast { A: string; B: string; C: string; S: string; bot: string; bot2: string; tok: Record<string, string> }

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

async function setStatus(gameId: string, playerId: string, status: string) {
    await pgPool.query(
        `UPDATE games SET players = (SELECT jsonb_agg(CASE WHEN p->>'player_id' = $2 THEN jsonb_set(p, '{status}', to_jsonb($3::text)) ELSE p END)
                                     FROM jsonb_array_elements(players) p) WHERE id=$1`, [gameId, playerId, status]);
    __clearGameCache();
}

// A lobby that cannot deal by accident: A, B (humans) and a bot are seated, and
// B is not ready, so nothing a test does short of B readying starts it.
async function lobby(c: Cast): Promise<string> {
    const gameId = `l${uuid().slice(0, 5)}`;
    await seedGame(gameId, [
        { id: c.A, name: 'alice', is_ai: false, strategy_key: 'human' },
        { id: c.B, name: 'bob', is_ai: false, strategy_key: 'human' },
        { id: c.bot, name: 'Botty', is_ai: true, strategy_key: 'random' },
    ]);
    await setStatus(gameId, c.B, PLAYER_STATUS.IDLE);
    return gameId;
}

// A dealt two-human game, started through the real meta endpoint.
async function dealt(c: Cast): Promise<string> {
    const gameId = `d${uuid().slice(0, 5)}`;
    await seedGame(gameId, [
        { id: c.A, name: 'alice', is_ai: false, strategy_key: 'human' },
        { id: c.B, name: 'bob', is_ai: false, strategy_key: 'human' },
    ]);
    await setStatus(gameId, c.A, PLAYER_STATUS.IDLE);
    await setStatus(gameId, c.B, PLAYER_STATUS.IDLE);
    for (const who of ['A', 'B'] as const) {
        const res = await postJson('meta', c.tok[who], { type: 'start', game_id: gameId });
        assert.equal(res.status, 200, `fixture: ${who} readies (${JSON.stringify(res.json)})`);
    }
    await settle();
    const g = await loadCompleteGame(gameId);
    assert.equal(g.status, GAME_STATUS.PLAYING, 'fixture: the game dealt');
    return gameId;
}

const toAwire = (pm: PlayerMove): AwireMove =>
    ({ kind: pm.move.type, cards: pm.move.cards, attack_cards: pm.move.attack_cards } as AwireMove);

// The first legal move of whoever may move, and the id of a human who may NOT
// make that move (the other seat).
async function moverAndBystander(gameId: string, c: Cast) {
    const g = await loadCompleteGame(gameId);
    const moves = legalMovesFor(g).filter(m => m.move.type !== 'good');
    assert.ok(moves.length > 0, 'fixture: somebody can move');
    const pm = moves[0];
    const mover = pm.playerId === c.A ? 'A' : 'B';
    const bystander = mover === 'A' ? 'B' : 'A';
    // The bystander must not hold a move with those cards, or "A sends B's move"
    // would be a legal move of A's and prove nothing.
    const own = legalMovesFor(g, id => id === c[bystander])
        .some(m => m.move.type === pm.move.type && JSON.stringify(m.move.cards) === JSON.stringify(pm.move.cards));
    assert.ok(!own, 'fixture: the move is only legal for the mover');
    return { pm, mover, bystander, g } as const;
}

before(async () => { await applySchema(); });
beforeEach(async () => { await resetDb(); __clearGameCache(); });
after(async () => { await settle(); });

// =============================================================================
// action (packed moves)
// =============================================================================

test('action: the seat to move, acting as itself, applies (positive control)', async () => {
    const c = await cast();
    const gameId = await dealt(c);
    const { pm, mover } = await moverAndBystander(gameId, c);
    const v0 = Number((await loadCompleteGame(gameId)).version ?? 0);
    const res = await postPacked(c.tok[mover], encodeActionRequest(gameId, encodeAction(toAwire(pm))));
    assert.equal(res.status, 200);
    const out = decodeActionResponse(res.bytes)!;
    assert.equal(out.status, ACTION_STATUS.APPLIED, 'the move applied');
    assert.equal(out.version, v0 + 1, 'and committed');
});

test('action: a seated player sending the OTHER seat\'s legal move is rejected and changes nothing', async () => {
    const c = await cast();
    const gameId = await dealt(c);
    const { pm, bystander } = await moverAndBystander(gameId, c);
    const before = await world(gameId);
    const res = await postPacked(c.tok[bystander], encodeActionRequest(gameId, encodeAction(toAwire(pm))));
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
    const res = await postPacked(c.tok.S, encodeActionRequest(gameId, encodeAction(toAwire(pm))));
    assertRefused(res, /not in game/i, 'stranger plays the mover\'s move');
    await assertUnchanged(gameId, before, 'stranger plays the mover\'s move');
});

test('action: the JSON body cannot name the actor (player_id / user_id / seat)', async () => {
    const c = await cast();
    const gameId = await dealt(c);
    const { pm, mover, bystander, g } = await moverAndBystander(gameId, c);
    const moverSeat = g.players.findIndex(p => p.player_id === pm.playerId);
    const before = await world(gameId);
    for (const [who, label] of [[bystander, 'seated bystander'], ['S', 'stranger']] as const) {
        const res = await postJson('action', c.tok[who], {
            type: pm.move.type, game_id: gameId, cards: pm.move.cards, attack_cards: pm.move.attack_cards,
            player_id: c[mover], user_id: c[mover], seat: moverSeat, player_index: moverSeat,
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
    const res = await postPacked(`${h}.${moverPayload}.${s}`, encodeActionRequest(gameId, encodeAction(toAwire(pm))));
    assertRefused(res, /invalid token/i, 'a token re-pointed at the mover');
    const none = await postPacked(null, encodeActionRequest(gameId, encodeAction(toAwire(pm))));
    assertRefused(none, /authorization/i, 'no token at all');
    await assertUnchanged(gameId, before, 'forged / missing token');
});

test('action: a move on a game the caller does not sit at is refused and changes nothing', async () => {
    const c = await cast();
    const mine = await dealt(c);
    // A second table with B and C: A is not there.
    const other = `o${uuid().slice(0, 5)}`;
    await seedGame(other, [
        { id: c.B, name: 'bob', is_ai: false, strategy_key: 'human' },
        { id: c.C, name: 'carol', is_ai: false, strategy_key: 'human' },
    ]);
    await setStatus(other, c.B, PLAYER_STATUS.IDLE);
    await setStatus(other, c.C, PLAYER_STATUS.IDLE);
    for (const who of ['B', 'C'] as const) await postJson('meta', c.tok[who], { type: 'start', game_id: other });
    await settle();
    const og = await loadCompleteGame(other);
    const pm = legalMovesFor(og).filter(m => m.move.type !== 'good')[0];
    const before = await world(other);
    const beforeMine = await world(mine);
    const res = await postPacked(c.tok.A, encodeActionRequest(other, encodeAction(toAwire(pm))));
    assertRefused(res, /not in game/i, 'A plays at a table A is not at');
    await assertUnchanged(other, before, 'A plays at a table A is not at');
    await assertUnchanged(mine, beforeMine, 'A\'s own table is untouched too');
});

// =============================================================================
// meta
// =============================================================================

test('meta start: readies the CALLER only, whoever the body names; a stranger is refused', async () => {
    const c = await cast();
    const gameId = await lobby(c);
    await setStatus(gameId, c.A, PLAYER_STATUS.IDLE);

    const before = await world(gameId);
    assertRefused(await postJson('meta', c.tok.S, { type: 'start', game_id: gameId, player_id: c.B }),
        /not in game/i, 'stranger readies B');
    await assertUnchanged(gameId, before, 'stranger readies B');

    const res = await postJson('meta', c.tok.A, { type: 'start', game_id: gameId, player_id: c.B, user_id: c.B, seat: 1 });
    assert.equal(res.status, 200, 'A readies (positive control)');
    const g = await loadCompleteGame(gameId);
    assert.equal(g.players.find(p => p.player_id === c.A)!.status, PLAYER_STATUS.READY, 'A is ready');
    assert.equal(g.players.find(p => p.player_id === c.B)!.status, PLAYER_STATUS.IDLE, 'B, named in the body, is not');
    assert.equal(g.status, GAME_STATUS.WAITING, 'so nothing dealt');
});

test('meta join: seats the CALLER, never the user the body names', async () => {
    const c = await cast();
    const gameId = await lobby(c);
    const res = await postJson('meta', c.tok.C, { type: 'join', game_id: gameId, player_id: c.S, user_id: c.S });
    assert.equal(res.status, 200, 'C joins (positive control)');
    const ids = (await loadCompleteGame(gameId)).players.map(p => p.player_id);
    assert.ok(ids.includes(c.C), 'the caller is seated');
    assert.ok(!ids.includes(c.S), 'the user named in the body is not');
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
    assert.ok((await loadCompleteGame(gameId)).players.some(p => p.player_id === c.bot2), 'the bot is seated');
});

test('meta add-bot: a stranger cannot deal somebody else\'s all-ready lobby by adding its last bot', async () => {
    const c = await cast();
    const gameId = await lobby(c);
    await setStatus(gameId, c.B, PLAYER_STATUS.READY); // every seat ready: one more bot deals
    const before = await world(gameId);
    assertRefused(await postJson('meta', c.tok.S, { type: 'add-bot', game_id: gameId, bot_id: c.bot2 }),
        /not in game/i, 'stranger starts the lobby');
    await assertUnchanged(gameId, before, 'stranger starts the lobby');
    assert.equal((await loadCompleteGame(gameId)).status, GAME_STATUS.WAITING, 'the lobby did not deal');
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
    let ids = (await loadCompleteGame(gameId)).players.map(p => p.player_id);
    assert.deepEqual(ids, [c.A], 'only A remains');
    const g2 = await lobby({ ...c });
    assert.equal((await postJson('meta', c.tok.B, { type: 'exit', game_id: g2 })).status, 200, 'B leaves');
    ids = (await loadCompleteGame(g2)).players.map(p => p.player_id);
    assert.ok(!ids.includes(c.B) && ids.includes(c.A), 'B left, A stayed');
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
    const g = await loadCompleteGame(gameId);
    assert.equal(g.name, 'ours');
    assert.deepEqual(g.players.map(p => p.player_id), [c.bot, c.B, c.A]);
});

test('meta rearrange-hand: reorders the CALLER\'s hand only; a stranger or a named seat is refused or ignored', async () => {
    const c = await cast();
    const gameId = await dealt(c);
    const g0 = await loadCompleteGame(gameId);
    const handOf = (g: typeof g0, id: string) => g.players.find(p => p.player_id === id)!.hand;
    const n = handOf(g0, c.A).length;
    const reversed = Array.from({ length: n }, (_, i) => n - 1 - i);

    const before = await world(gameId);
    assertRefused(await postJson('meta', c.tok.S, { type: 'rearrange-hand', game_id: gameId, card_indices: reversed, player_id: c.A }),
        /not in this game/i, 'stranger reorders A\'s hand');
    await assertUnchanged(gameId, before, 'stranger reorders A\'s hand');

    // B names A (by id and by seat, and with a seat off the table); only B's own hand may move.
    const nB = handOf(g0, c.B).length;
    const reversedB = Array.from({ length: nB }, (_, i) => nB - 1 - i);
    for (const seat of [0, 1, 99, -1]) {
        const res = await postJson('meta', c.tok.B, {
            type: 'rearrange-hand', game_id: gameId, card_indices: reversedB, player_id: c.A, user_id: c.A, seat,
        });
        assert.equal(res.status, 200, `B reorders (seat field ${seat}): ${JSON.stringify(res.json)}`);
        const g = await loadCompleteGame(gameId);
        assert.deepEqual(handOf(g, c.A), handOf(g0, c.A), `A's hand never moved (seat field ${seat})`);
    }
    const g = await loadCompleteGame(gameId);
    assert.deepEqual(handOf(g, c.B), handOf(g0, c.B), 'four reversals of B\'s own hand bring it back');
});

test('action bump: open to anyone by design, and it changes no game content', async () => {
    const c = await cast();
    const gameId = await dealt(c);
    const before = await world(gameId);
    const res = await postJson('action', c.tok.S, { type: 'bump', game_id: gameId, player_id: c.A });
    assert.equal(res.status, 200, 'a spectator may nudge a stalled game');
    const view = decodePackedGame(res.bytes);
    assert.ok(view, 'the response is a packed view');
    assert.equal(view!.seat, -1, 'and it is the SPECTATOR view, not the view of the player the body named');
    // A bump is a commit of the unchanged game (executeWithGameLock always
    // commits), so the version token and the views' version header move. The
    // content - state blob, roster, board columns, every view's board - may not.
    const now = await world(gameId);
    const content = (w: typeof now) => ({
        games: w.games.map(({ version: _v, updated_at: _u, bot_lease_token: _t, bot_lease_until: _l, ...rest }) => rest),
        playerHands: w.playerHands, botHands: w.botHands,
        // envelope bytes 3..6 are the u32 version (view.ts encodeGameResponse)
        playerViews: w.playerViews.map(r => ({ player_id: r.player_id, board: r.view.slice(0, 6) + r.view.slice(14) })),
        spectatorViews: w.spectatorViews.map(r => ({ board: r.view.slice(0, 6) + r.view.slice(14) })),
    });
    assert.deepEqual(content(now), content(before), 'stranger bump: no game content changed');
});

// =============================================================================
// create
// =============================================================================

test('create: the new lobby seats the CALLER, whoever the body names', async () => {
    const c = await cast();
    const res = await postJson('create', c.tok.A, { player_id: c.B, user_id: c.B, name: 'x' });
    assert.equal(res.status, 200, `create succeeds: ${JSON.stringify(res.json)}`);
    const view = decodePackedGame(res.bytes);
    assert.ok(view, 'the response is a packed view');
    await settle();
    const { rows } = await pgPool.query('SELECT players FROM games WHERE id=$1', [view!.game.id]);
    assert.equal(rows.length, 1, 'the lobby persisted');
    assert.deepEqual(rows[0].players.map((p: { player_id: string }) => p.player_id), [c.A], 'seated: the caller alone');
    const { rows: views } = await pgPool.query('SELECT player_id FROM player_views WHERE game_id=$1', [view!.game.id]);
    assert.deepEqual(views.map(r => r.player_id), [c.A], 'the only view row is the caller\'s');
});
