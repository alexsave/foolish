// ask_wire.test.ts - every request that is not a move, and every reply that is
// not an answer, as the web's client writes and reads them.
//
// The lobby requests and the bot nudge used to be JSON objects, a refusal used
// to be `{ error: "<prose>" }`, and a realtime message spelled its format, a
// random sequence id and its version beside the bytes. All of it is the
// kernel's now (c/src/ask.h, c/src/evwire.h the cast), and so is every test of
// what the server does with it: the layout and its refusals
// (c/tests/tests.c test_ask_round_trips), every kind run on a table
// (test_table_ask_runs_every_kind, test_ask_body_runs_for_the_caller) and the
// cast (test_table_cast_carries_its_version). The handlers are c_base's,
// tested there (c_base/test/unit_app.c, the door's 415 included).
//
// What stays here is the TypeScript that wraps the client's halves
// (sdk/ts/table/client_table.ts askBody, readRefusal, readCast, castVersion)
// over web.wasm, the module a visitor runs: it writes the bytes the kernel
// pins, reads the bytes the server writes, and refuses the rest. The goldens
// are ask_encode's and ask_refusal_encode's own output, captured from the
// native kernel.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import * as V from '../sdk/ts/gen/view_layout.bots.ts';
import { clientTable, type Ask } from '../sdk/ts/table/client_table.ts';
import { askBody, askOf } from './helpers/ask.ts';

const A = '11111111-1111-4111-8111-111111111111', B = '22222222-2222-4222-8222-222222222222';
const BOT = '44444444-4444-4444-8444-444444444444';
const hex = (b: Uint8Array) => Buffer.from(b).toString('hex');

const send = (ask: Ask): Uint8Array => {
    const body = clientTable().askBody(ask);
    assert.ok(body, `the client's kernel writes the ask (${JSON.stringify(clientTable().lastRefusal())})`);
    return body;
};

test('an ask: the client writes the kernel\'s bytes for every kind', () => {
    const ids = (s: string) => Buffer.from(s).toString('hex');
    const asks: [Ask, string][] = [
        [{ kind: V.ASK_JOIN, gid: 'g-1' }, '010103672d31'],
        [{ kind: V.ASK_START, gid: 'g-1' }, '010203672d31'],
        [{ kind: V.ASK_ADD_BOT, gid: 'g-1' }, '010303672d3100'],
        [{ kind: V.ASK_ADD_BOT, gid: 'g-1', bot: BOT }, `010303672d3124${ids(BOT)}`],
        [{ kind: V.ASK_EXIT, gid: 'g-1' }, '010403672d310000'],
        [{ kind: V.ASK_EXIT, gid: 'g-1', bot: BOT }, `010403672d3124${ids(BOT)}00`],
        [{ kind: V.ASK_EXIT, gid: 'g-1', player: B }, `010403672d310024${ids(B)}`],
        [{ kind: V.ASK_CONTINUE, gid: 'g-1' }, '010503672d31'],
        [{ kind: V.ASK_REARRANGE_HAND, gid: 'g-1', idx: [5, 0, 4, 1, 3, 2] }, '010603672d3106050004010302'],
        [{ kind: V.ASK_REARRANGE_PLAYERS, gid: 'g-1', order: [{ id: B }, { id: A }] }, `010703672d310224${ids(B)}24${ids(A)}`],
        [{ kind: V.ASK_UPDATE_NAME, gid: 'g-1', title: '  Пятница \u{1F0CF}  ' }, '010803672d3117002020d09fd18fd182d0bdd0b8d186d0b020f09f838f2020'],
        [{ kind: V.ASK_SET_RULES, gid: 'g-1', passing: 1 }, '010903672d3101'],
        [{ kind: V.ASK_SET_RULES, gid: 'g-1', passing: 0 }, '010903672d3100'],
        [{ kind: V.ASK_BUMP, gid: 'g-1' }, '010a03672d31'],
    ];
    for (const [ask, want] of asks) assert.equal(hex(send(ask)), want, `kind ${ask.kind}`);
});

test('an ask the kernel will not write', () => {
    assert.equal(clientTable().askBody({ kind: V.ASK_JOIN, gid: '' }), null, 'an ask names a game');
    assert.equal(clientTable().lastRefusal().code, V.ASK_E_WIRE);
    assert.equal(clientTable().askBody({ kind: 99, gid: 'g' }), null, 'a kind that is not one');
    assert.throws(() => clientTable().askBody({ kind: V.ASK_UPDATE_NAME, gid: 'g', title: 'x'.repeat(500) }), RangeError,
        'a title longer than its slot is the writer\'s refusal');
    // A refusal leaves nothing behind: the next ask is written whole.
    assert.equal(hex(send({ kind: V.ASK_BUMP, gid: 'g-1' })), '010a03672d31');
});

test('the suites\' old request names state the same asks (helpers/ask.ts)', () => {
    assert.deepEqual(askBody({ type: 'exit', game_id: 'g-1', player_id: B, seat: 3, user_id: A }),
        send({ kind: V.ASK_EXIT, gid: 'g-1', player: B }), 'a field no ask carries is dropped');
    assert.deepEqual(askBody({ type: 'rearrange-hand', game_id: 'g-1', card_indices: [1, 0] }),
        send({ kind: V.ASK_REARRANGE_HAND, gid: 'g-1', idx: [1, 0] }));
    assert.deepEqual(askBody({ type: 'set-rules', game_id: 'g-1', passing: true }), send({ kind: V.ASK_SET_RULES, gid: 'g-1', passing: 1 }));
    assert.throws(() => askOf({ type: '__proto__', game_id: 'g' }), /names no ask/);
});

test('a refusal: the client reads a reason and the kernel\'s codes, no prose', () => {
    // ask_refusal_encode's bytes: the format, why, then the two codes as i32 LE.
    assert.deepEqual(clientTable().readRefusal(Buffer.from('01039afffffff7ffffff', 'hex')),
        { why: V.ASK_WHY_REFUSED, code: -102, detail: -9 }, 'a roster refusal: TABLE_E_ROSTER and ROSTER_E_FULL, negative codes whole');
    for (let why = V.ASK_WHY_AUTH; why <= V.ASK_WHY_BUSY; why++) {
        const bytes = Uint8Array.of(1, why, 0, 0, 0, 0, 0, 0, 0, 0);
        assert.equal(bytes.length, V.ASK_REFUSAL_BYTES);
        assert.deepEqual(clientTable().readRefusal(bytes), { why, code: 0, detail: 0 }, `reason ${why}`);
    }
    assert.equal(clientTable().readRefusal(Uint8Array.of(1, V.ASK_WHY_BUSY + 1, 0, 0, 0, 0, 0, 0, 0, 0)), null, 'a reason that is not one');
    assert.equal(clientTable().readRefusal(new TextEncoder().encode('{"error":"Player x not in game g"}')), null,
        'the old JSON error is not a refusal');
    assert.equal(clientTable().readRefusal(new Uint8Array(0)), null);
});

test('a cast: the client reads its version from the header, and nothing else as one', () => {
    // The header (evwire.h): EVW_CAST_FORMAT, then the version u32 LE, then the as3 push.
    const header = Uint8Array.of(V.EVW_CAST_FORMAT, 42, 0, 0, 0);
    const notAPush = Uint8Array.of(...header, 0xff, 0xff);
    assert.equal(clientTable().readCast(notAPush, { identity: 'none' }), null, 'a cast whose push does not read does not read');
    assert.equal(clientTable().castVersion(notAPush), 42, 'but still says which version to fetch instead');
    assert.equal(clientTable().castVersion(header.subarray(0, 3)), null, 'bytes too short for a header name none');
    const bare = Uint8Array.of(1, 0, 0, 0, 0, 0);   // what an old server sent as `b`: an evwire push, format 1
    assert.equal(clientTable().readCast(bare, { identity: 'none' }), null, 'a bare push is not a cast');
    assert.equal(clientTable().castVersion(bare), null, 'nor does it name a version: it is never half-read');
});
