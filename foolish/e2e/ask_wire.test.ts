// ask_wire.test.ts - every request that is not a move, and every reply that is
// not an answer, as bytes through the kernel in both directions.
//
// The lobby requests and the bot nudge used to be JSON objects, a refusal used
// to be `{ error: "<prose>" }`, and a realtime message spelled its format, a
// random sequence id and its version beside the bytes. All of it is the
// kernel's now (c/src/ask.h, c/src/evwire.h the cast). This suite holds the
// crossing itself, with no database: what the CLIENT's kernel writes
// (client_table.ts askBody) is what the SERVER's kernel reads and runs
// (server_table.ts askDecode / ask), what the server's writes as a refusal or a
// cast is what the client's reads, and a body that is not bytes is turned away
// at the door before anything parses it.
//
// The C side of each layout is c/tests/tests.c (test_ask_round_trips,
// test_table_ask_runs_every_kind, test_table_cast_carries_its_version). The
// handlers are c_base's, tested there (c_base/test/unit_app.c, the door's 415
// included).
import { test } from 'node:test';
import assert from 'node:assert/strict';
import * as L from '../sdk/ts/gen/game_layout.bots.ts';
import * as V from '../sdk/ts/gen/view_layout.bots.ts';
import { clientTable, type Ask } from '../sdk/ts/table/client_table.ts';
import { fixture, fixtureTable } from './helpers/table_fixture.ts';
import { askBody, askOf } from './helpers/ask.ts';
import { castParts } from './helpers/cast.ts';
import { bytesToBase64 } from '../sdk/ts/wire/bytes.ts';

const SEED = Uint8Array.from({ length: 32 }, (_, i) => (i * 13 + 5) & 0xff);
const A = '11111111-1111-4111-8111-111111111111', B = '22222222-2222-4222-8222-222222222222';
const C = '33333333-3333-4333-8333-333333333333', BOT = '44444444-4444-4444-8444-444444444444';

/** A two-human lobby, nobody ready, loaded into the server's table. */
function lobby() {
    const row = fixture().title('Ask').seats([{ id: A, name: 'Ana' }, { id: B, name: 'Bo' }]).build();
    const table = fixtureTable();
    assert.equal(table.load(row.state, row.roster), L.TABLE_OK, 'the lobby loads');
    return table;
}

const send = (ask: Ask): Uint8Array => {
    const body = clientTable().askBody(ask);
    assert.ok(body, `the client's kernel writes the ask (${JSON.stringify(clientTable().lastRefusal())})`);
    return body;
};

test('an ask: what the client writes is what the server reads, field for field, for every kind', () => {
    const asks: Ask[] = [
        { kind: V.ASK_JOIN, gid: 'g-1' },
        { kind: V.ASK_START, gid: 'g-1' },
        { kind: V.ASK_ADD_BOT, gid: 'g-1' },
        { kind: V.ASK_ADD_BOT, gid: 'g-1', bot: BOT },
        { kind: V.ASK_EXIT, gid: 'g-1' },
        { kind: V.ASK_EXIT, gid: 'g-1', bot: BOT },
        { kind: V.ASK_EXIT, gid: 'g-1', player: B },
        { kind: V.ASK_CONTINUE, gid: 'g-1' },
        { kind: V.ASK_REARRANGE_HAND, gid: 'g-1', idx: [5, 0, 4, 1, 3, 2] },
        { kind: V.ASK_REARRANGE_PLAYERS, gid: 'g-1', order: [{ id: B }, { id: A }] },
        { kind: V.ASK_UPDATE_NAME, gid: 'g-1', title: '  Пятница \u{1F0CF}  ' },
        { kind: V.ASK_SET_RULES, gid: 'g-1', passing: 1 },
        { kind: V.ASK_SET_RULES, gid: 'g-1', passing: 0 },
        { kind: V.ASK_BUMP, gid: 'g-1' },
    ];
    const none = { passing: 0, bot: '', player: '', idx: [], order: [], title: '' };
    for (const ask of asks) {
        const read = fixtureTable().askDecode(send(ask));
        assert.ok(typeof read !== 'number', `kind ${ask.kind}: the server's kernel reads it (${read})`);
        assert.deepEqual(read, { ...none, ...ask }, `kind ${ask.kind}: every field arrives, and the rest read empty`);
    }
});

test('an ask the kernel will not write, and a body it will not read', () => {
    assert.equal(clientTable().askBody({ kind: V.ASK_JOIN, gid: '' }), null, 'an ask names a game');
    assert.equal(clientTable().lastRefusal().code, V.ASK_E_WIRE);
    assert.equal(clientTable().askBody({ kind: 99, gid: 'g' }), null, 'a kind that is not one');
    assert.throws(() => clientTable().askBody({ kind: V.ASK_UPDATE_NAME, gid: 'g', title: 'x'.repeat(500) }), RangeError,
        'a title longer than its slot is the writer\'s refusal');

    const server = fixtureTable();
    const json = new TextEncoder().encode(JSON.stringify({ type: 'join', game_id: 'g-1' }));
    assert.equal(server.askDecode(json), L.TABLE_E_WIRE, 'the JSON an old client sends is not an ask');
    const good = send({ kind: V.ASK_EXIT, gid: 'g-1', player: B });
    assert.equal(server.askDecode(good.subarray(0, good.length - 1)), L.TABLE_E_WIRE, 'a body cut short');
    assert.equal(server.askDecode(Uint8Array.of(...good, 0)), L.TABLE_E_WIRE, 'a body with a byte to spare');
    assert.equal(server.askDecode(new Uint8Array(0)), L.TABLE_E_WIRE, 'an empty body');
});

test('an ask runs on the table as the edit it names, for the caller the server names', () => {
    let table = lobby();
    // A join by C, under the name the SERVER holds for C: the ask carries no name and no actor.
    assert.equal(table.ask(send({ kind: V.ASK_JOIN, gid: 'g-1' }), C, 'Cy', SEED), L.TABLE_OK);
    assert.deepEqual(table.seats().map((s) => [s.id, s.name]), [[A, 'Ana'], [B, 'Bo'], [C, 'Cy']], 'C is seated as Cy');

    // exit naming B, sent by A: B leaves, A stays.
    assert.equal(table.ask(send({ kind: V.ASK_EXIT, gid: 'g-1', player: B }), A, 'Ana', SEED), L.TABLE_OK);
    assert.deepEqual(table.seats().map((s) => s.id), [A, C]);
    // exit naming nobody: the caller leaves.
    assert.equal(table.ask(send({ kind: V.ASK_EXIT, gid: 'g-1' }), C, 'Cy', SEED), L.TABLE_OK);
    assert.deepEqual(table.seats().map((s) => s.id), [A]);

    table = lobby();
    // add-bot seats the row the host hands over; without one the ask is malformed.
    const addBot = send({ kind: V.ASK_ADD_BOT, gid: 'g-1' });
    assert.equal(table.ask(addBot, A, 'Ana', SEED), L.TABLE_E_WIRE, 'no bot row');
    assert.equal(table.ask(addBot, A, 'Ana', SEED, { id: BOT, nickname: 'Rando', brain: 'random' }), L.TABLE_OK);
    assert.deepEqual(table.seats()[2], { id: BOT, name: 'Rando', brain: 'random' });
    assert.equal(table.ask(send({ kind: V.ASK_EXIT, gid: 'g-1', bot: BOT }), B, 'Bo', SEED), L.TABLE_OK, 'and exit naming a bot removes it');
    assert.equal(table.seats().length, 2);

    assert.equal(table.ask(send({ kind: V.ASK_REARRANGE_PLAYERS, gid: 'g-1', order: [{ id: B }, { id: A }] }), A, 'Ana', SEED), L.TABLE_OK);
    assert.deepEqual(table.seats().map((s) => s.id), [B, A], 'reseated in the order asked');
    assert.equal(table.ask(send({ kind: V.ASK_REARRANGE_PLAYERS, gid: 'g-1', order: [{ id: B }] }), A, 'Ana', SEED), L.TABLE_E_ROSTER);
    assert.equal(table.detail(), L.ROSTER_E_PERM, 'an order missing a seat is the roster\'s refusal');

    assert.equal(table.ask(send({ kind: V.ASK_UPDATE_NAME, gid: 'g-1', title: '  Friday  ' }), A, 'Ana', SEED), L.TABLE_OK);
    assert.equal(table.ask(send({ kind: V.ASK_SET_RULES, gid: 'g-1', passing: 0 }), B, 'Bo', SEED), L.TABLE_OK);
    assert.equal(table.ask(send({ kind: V.ASK_SET_RULES, gid: 'g-1', passing: 0 }), B, 'Bo', SEED), L.TABLE_MOOT, 'the rules it already has');

    // A stranger: every edit is refused, a bump is not (a spectator's client sends it).
    const stranger = '99999999-9999-4999-8999-999999999999';
    assert.equal(table.ask(send({ kind: V.ASK_START, gid: 'g-1' }), stranger, 'S', SEED), L.TABLE_E_NOT_SEATED);
    assert.equal(table.ask(send({ kind: V.ASK_BUMP, gid: 'g-1' }), stranger, 'S', SEED), L.TABLE_MOOT);

    // start readies the caller; the last seat's start deals, from the seed the server drew.
    assert.equal(table.ask(send({ kind: V.ASK_START, gid: 'g-1' }), A, 'Ana', SEED), L.TABLE_OK);
    assert.equal(table.ask(send({ kind: V.ASK_START, gid: 'g-1' }), B, 'Bo', SEED), L.TABLE_OK);
    const p = table.commit('g-1', 1, 1000);
    assert.ok(typeof p !== 'number' && p.dealtNow && p.status === L.GAME_STATUS_PLAYING, 'the table dealt');
    // The response is the caller's envelope, which the client's kernel reads: retitled, podkidnoy, dealt.
    const view = clientTable().adoptEnvelope((p as { views: (Uint8Array | null)[] }).views[0]!);
    assert.ok(view, 'the envelope reads');
    assert.equal(view.title, 'Friday', 'the title, trimmed by the kernel');
    assert.equal(view.passing, false);
    assert.equal(view.myHand.length, 6);
});

test('the suites\' old request names state the same asks (helpers/ask.ts)', () => {
    assert.deepEqual(askBody({ type: 'exit', game_id: 'g-1', player_id: B, seat: 3, user_id: A }),
        send({ kind: V.ASK_EXIT, gid: 'g-1', player: B }), 'a field no ask carries is dropped');
    assert.deepEqual(askBody({ type: 'rearrange-hand', game_id: 'g-1', card_indices: [1, 0] }),
        send({ kind: V.ASK_REARRANGE_HAND, gid: 'g-1', idx: [1, 0] }));
    assert.deepEqual(askBody({ type: 'set-rules', game_id: 'g-1', passing: true }), send({ kind: V.ASK_SET_RULES, gid: 'g-1', passing: 1 }));
    assert.throws(() => askOf({ type: '__proto__', game_id: 'g' }), /names no ask/);
});

test('a refusal: what the server writes, the client reads - a reason and the kernel\'s codes, no prose', () => {
    const server = fixtureTable();
    const cases: [number, number, number][] = [
        [L.ASK_WHY_AUTH, 0, 0],
        [L.ASK_WHY_MALFORMED, 0, 0],
        [L.ASK_WHY_REFUSED, L.TABLE_E_NOT_SEATED, 0],
        [L.ASK_WHY_REFUSED, L.TABLE_E_ROSTER, L.ROSTER_E_FULL],
        [L.ASK_WHY_NOT_FOUND, 0, 0],
        [L.ASK_WHY_NO_BOT, 0, 0],
        [L.ASK_WHY_INTERNAL, 0, 0],
        [L.ASK_WHY_METHOD, 0, 0],
    ];
    for (const [why, code, detail] of cases) {
        const bytes = server.refusal(why, code, detail);
        assert.equal(bytes.length, L.ASK_REFUSAL_BYTES, 'a refusal is ten bytes');
        assert.deepEqual(clientTable().readRefusal(bytes), { why, code, detail });
    }
    assert.throws(() => server.refusal(0), /refusal refused/, 'a reason that is not one is not written');
    assert.equal(clientTable().readRefusal(new TextEncoder().encode('{"error":"Player x not in game g"}')), null,
        'the old JSON error is not a refusal');
    assert.equal(clientTable().readRefusal(new Uint8Array(0)), null);
});

test('a cast: the realtime message is { b }, and the client reads its version and boards from the bytes alone', () => {
    const table = lobby();
    assert.equal(table.ask(send({ kind: V.ASK_JOIN, gid: 'g-1' }), C, 'Cy', SEED), L.TABLE_OK);
    assert.ok(typeof table.commit('g-1', 42, 1000) !== 'number');
    const cast = table.cast('g-1', 0, 42), push = table.push('g-1', 0);
    assert.ok(cast instanceof Uint8Array && push instanceof Uint8Array);

    // What table_io.ts broadcastPushes puts in Realtime's envelope.
    const parts = castParts({ b: bytesToBase64(cast) });
    assert.equal(parts.version, 42, 'the version is in the bytes');
    assert.deepEqual(parts.push, push, 'and behind the header is exactly the as3 push');

    const read = clientTable().readCast(parts.cast, { identity: 'none' });
    assert.ok(read, `the client reads the cast (${JSON.stringify(clientTable().lastRefusal())})`);
    assert.equal(read.final.version, 42, 'the boards carry the version the cast names');
    assert.equal(read.final.gameId, 'g-1');
    assert.deepEqual(read.final.seats.map((s) => [s.id, s.name]), [[A, 'Ana'], [B, 'Bo'], [C, 'Cy']], 'the join\'s roster rides the cast');
    // The sequence id the page derives (AnimationContext decodeCast): one push per topic per version.
    assert.equal(`${read.final.gameId}:${read.final.version}`, 'g-1:42');
    // The same boards as the bare push read at that version.
    assert.deepEqual(read, clientTable().readPush(push, { as3: true, version: 42, identity: 'none' }));

    assert.equal(clientTable().readCast(push, { identity: 'none' }), null, 'a bare push is not a cast');
    assert.equal(clientTable().castVersion(push), null, 'nor does it name a version: what an old server sent as `b` is never half-read');
    assert.equal(clientTable().castVersion(parts.cast.subarray(0, 3)), null, 'and bytes too short for a header name none');
    const cut = parts.cast.subarray(0, parts.cast.length >> 1);
    assert.equal(clientTable().readCast(cut, { identity: 'none' }), null, 'a cast cut short does not read');
    assert.equal(clientTable().castVersion(cut), 42, 'but still says which version to fetch instead');
});
