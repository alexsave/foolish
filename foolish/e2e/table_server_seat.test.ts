// SECURITY S3, on the C Table (docs/C_GAME_SHAPE_MIGRATION.md Phase 4b): the seat
// a request acts as is the auth user's roster seat, resolved by the kernel, and
// nothing in a request body selects the actor.
//
// Every row here is the kernel's (e2e/helpers/table_db.ts seedTable: state and
// roster blobs) and every request goes through the real edge entry
// point with a real signed token. For `action` and for every `meta` type the
// test sends the request as one user while the body names another (player_id,
// user_id, seat), and reads the stored table back through the kernel
// (e2e/helpers/table_play.ts) to see whose seat the effect landed on.

import './harness.ts';
import { test, before, beforeEach, after } from 'node:test';
import assert from 'node:assert/strict';
import { applySchema, resetDb, uuid, pgPool } from './harness.ts';
import { settle, tokenFor, postJson, postPacked } from './helpers/edge.ts';
import { fixture, PLAYING, GAME_OVER, READY, IDLE } from './helpers/table_fixture.ts';
import { seedTable } from './helpers/table_db.ts';
import { actionRequest, legalMoves, mustReadTable } from './helpers/table_play.ts';
import { __clearGameCache } from '../server/impls/supabase/functions/_shared/adapter/game_cache.ts';
import { ACTION_STATUS, decodeActionResponse } from '../sdk/ts/wire/awire.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; console.error = () => {}; }

interface Cast { A: string; B: string; C: string; S: string; bot: string; tok: Record<'A' | 'B' | 'C' | 'S', string> }

async function cast(): Promise<Cast> {
    const A = uuid(), B = uuid(), C = uuid(), S = uuid(), bot = uuid();
    await pgPool.query('INSERT INTO auth.users(id) VALUES ($1),($2),($3),($4) ON CONFLICT DO NOTHING', [A, B, C, S]);
    await pgPool.query('INSERT INTO bots(id, nickname, strategy_key) VALUES ($1, $2, $3)', [bot, 'Botty', 'random']);
    return {
        A, B, C, S, bot,
        tok: { A: await tokenFor(A, 'alice'), B: await tokenFor(B, 'bob'), C: await tokenFor(C, 'carol'), S: await tokenFor(S, 'stranger') },
    };
}

const gid = (p: string) => `${p}${uuid().slice(0, 5)}`;
const decoded = (r: { status: number; bytes: Uint8Array; json: unknown }) => {
    assert.equal(r.status, 200, `a packed action response (${JSON.stringify(r.json)})`);
    return decodeActionResponse(r.bytes)!;
};
const assertOk = (r: { status: number; json: unknown }) => assert.equal(r.status, 200, JSON.stringify(r.json));

// A lobby: A and B seated, neither ready.
async function lobby(c: Cast): Promise<string> {
    const g = gid('l');
    await seedTable(g, fixture().seats([{ id: c.A, name: 'alice' }, { id: c.B, name: 'bob' }]).build());
    return g;
}

// A dealt two-human game: A (seat 0) attacks, B (seat 1) defends, hands known.
async function dealt(c: Cast): Promise<string> {
    const g = gid('d');
    await seedTable(g, fixture()
        .seats([{ id: c.A, name: 'alice' }, { id: c.B, name: 'bob' }])
        .status(PLAYING).attacker(0).defender(1)
        .hand(0, '6h 7h 8h 9h Th Jh').hand(1, '6s 7s 8s 9s Ts Js')
        .deck('6d 7d 8d 9d Td Jd Qd Kd').trump('Ac')
        .build());
    return g;
}

before(async () => { await applySchema(); });
beforeEach(async () => { await resetDb(); __clearGameCache(); });
after(async () => { await settle(); });

test('action: a move applies for the auth user\'s seat, and another seat\'s move sent by them is judged as theirs', async () => {
    const c = await cast();
    const g = await dealt(c);
    const t0 = await mustReadTable(g);
    const attack = legalMoves(t0, (_, seat) => seat === 0).find((m) => m.kind === 'attack' && m.cards.length === 1)!;
    assert.ok(attack, 'fixture: A can attack with one card');

    // B sends A's attack: the kernel judges it as B's (the defender, holding none of those cards).
    const asB = decoded(await postPacked(c.tok.B, actionRequest(g, attack)));
    assert.equal(asB.status, ACTION_STATUS.REJECTED, 'B cannot play A\'s cards');
    assert.equal((await mustReadTable(g)).version, t0.version, 'nothing committed');

    // A stranger sends it: not seated.
    const asS = await postPacked(c.tok.S, actionRequest(g, attack));
    assert.equal(asS.status, 400);
    assert.match(String(asS.json?.error), /not in game/i);

    // A sends it: applied, from seat 0's hand.
    const asA = decoded(await postPacked(c.tok.A, actionRequest(g, attack)));
    assert.equal(asA.status, ACTION_STATUS.APPLIED);
    const t1 = await mustReadTable(g);
    assert.equal(t1.version, t0.version + 1);
    assert.equal(t1.seats[0].hand.length, 5, 'the card left seat 0');
    assert.equal(t1.seats[1].hand.length, 6, 'seat 1 is untouched');
    assert.deepEqual(t1.battles.map((b) => b.attack), attack.cards);
});

test('action: a JSON body naming another player acts for nobody', async () => {
    const c = await cast();
    const g = await dealt(c);
    const t0 = await mustReadTable(g);
    for (const [who, re] of [['B', /packed/i], ['S', /not in game/i]] as const) {
        const res = await postJson('action', c.tok[who], { type: 'attack', game_id: g, cards: [{ suit: 2, value: 5 }], player_id: c.A, user_id: c.A, seat: 0 });
        assert.equal(res.status, 400);
        assert.match(String(res.json?.error), re, `${who} names A in a JSON move`);
    }
    assert.equal((await mustReadTable(g)).version, t0.version, 'nothing committed');
});

test('meta start: readies the caller\'s seat, never the one the body names', async () => {
    const c = await cast();
    const g = await lobby(c);
    const refused = await postJson('meta', c.tok.S, { type: 'start', game_id: g, player_id: c.A });
    assert.match(String(refused.json?.error), /not in game/i, 'a stranger readies nobody');
    const res = await postJson('meta', c.tok.A, { type: 'start', game_id: g, player_id: c.B, user_id: c.B, seat: 1 });
    assert.equal(res.status, 200, JSON.stringify(res.json));
    const t = await mustReadTable(g);
    assert.deepEqual(t.seats.map((s) => s.status), [READY, IDLE], 'A (seat 0) is ready, B is not');
});

test('meta join: seats the caller, never the user the body names', async () => {
    const c = await cast();
    const g = await lobby(c);
    assertOk(await postJson('meta', c.tok.C, { type: 'join', game_id: g, player_id: c.S, user_id: c.S }));
    const ids = (await mustReadTable(g)).seats.map((s) => s.id);
    assert.deepEqual(ids, [c.A, c.B, c.C]);
});

test('meta add-bot / exit: only a seated caller edits; a body player_id is the target, never the actor', async () => {
    const c = await cast();
    const g = await lobby(c);
    const v0 = (await mustReadTable(g)).version;
    assert.match(String((await postJson('meta', c.tok.S, { type: 'add-bot', game_id: g, bot_id: c.bot, player_id: c.A })).json?.error), /not in game/i);
    assert.match(String((await postJson('meta', c.tok.S, { type: 'exit', game_id: g, player_id: c.B })).json?.error), /not in game/i);
    assert.match(String((await postJson('meta', c.tok.S, { type: 'exit', game_id: g, player_id: c.S })).json?.error), /not in game/i);
    assert.equal((await mustReadTable(g)).version, v0, 'refusals commit nothing');

    assertOk(await postJson('meta', c.tok.A, { type: 'add-bot', game_id: g, bot_id: c.bot }));
    assert.deepEqual((await mustReadTable(g)).seats.map((s) => s.id), [c.A, c.B, c.bot]);
    // B names A as the actor fields, and nobody as the target: B leaves.
    assertOk(await postJson('meta', c.tok.B, { type: 'exit', game_id: g, user_id: c.A, seat: 0 }));
    assert.deepEqual((await mustReadTable(g)).seats.map((s) => s.id), [c.A, c.bot]);
});

test('meta continue / rearrange-players / update-name: a stranger naming a seated player is refused', async () => {
    const c = await cast();
    const g = await lobby(c);
    const v0 = (await mustReadTable(g)).version;
    for (const body of [{ type: 'continue' }, { type: 'rearrange-players', new_order: [c.B, c.A] }, { type: 'update-name', new_name: 'owned' }]) {
        const res = await postJson('meta', c.tok.S, { ...body, game_id: g, player_id: c.A, user_id: c.A });
        assert.match(String(res.json?.error), /not in game/i, `stranger ${body.type}`);
    }
    assert.equal((await mustReadTable(g)).version, v0);
    assertOk(await postJson('meta', c.tok.B, { type: 'rearrange-players', game_id: g, new_order: [c.B, c.A], player_id: c.A }));
    assertOk(await postJson('meta', c.tok.A, { type: 'update-name', game_id: g, new_name: 'ours', player_id: c.B }));
    const t = await mustReadTable(g);
    assert.deepEqual(t.seats.map((s) => s.id), [c.B, c.A]);
    assert.equal(t.title, 'ours');
});

test('meta continue: the caller\'s seat must be at the finished table', async () => {
    const c = await cast();
    const g = gid('o');
    await seedTable(g, fixture().seats([{ id: c.A, name: 'alice' }, { id: c.B, name: 'bob' }])
        .status(GAME_OVER).eliminated(0).discard(36).seatStatus(0, IDLE).seatStatus(1, IDLE).build());
    assert.match(String((await postJson('meta', c.tok.S, { type: 'continue', game_id: g, player_id: c.A })).json?.error), /not in game/i);
    assertOk(await postJson('meta', c.tok.B, { type: 'continue', game_id: g, player_id: c.A }));
    assert.equal((await mustReadTable(g)).statusColumn, 'waiting');
});

test('meta rearrange-hand: reorders the caller\'s hand only, whatever seat the body names', async () => {
    const c = await cast();
    const g = await dealt(c);
    const t0 = await mustReadTable(g);
    for (const seat of [0, 1, 99, -1]) {
        const res = await postJson('meta', c.tok.B, { type: 'rearrange-hand', game_id: g, card_indices: [5, 4, 3, 2, 1, 0], player_id: c.A, user_id: c.A, seat });
        assert.equal(res.status, 200, JSON.stringify(res.json));
    }
    const t = await mustReadTable(g);
    assert.deepEqual(t.seats[0].hand, t0.seats[0].hand, 'A\'s hand never moved');
    assert.deepEqual(t.seats[1].hand, t0.seats[1].hand, 'four reversals of B\'s own hand bring it back');
    const once = await postJson('meta', c.tok.B, { type: 'rearrange-hand', game_id: g, card_indices: [5, 4, 3, 2, 1, 0], seat: 0 });
    assert.equal(once.status, 200);
    assert.deepEqual((await mustReadTable(g)).seats[1].hand, [...t0.seats[1].hand].reverse(), 'one reversal lands on B');
    assert.match(String((await postJson('meta', c.tok.S, { type: 'rearrange-hand', game_id: g, card_indices: [0], player_id: c.A })).json?.error), /not in game/i);
});
