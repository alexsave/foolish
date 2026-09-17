/* =============================================================================
 * The web reads envelopes and pushes through C, and reads them as it did
 * =============================================================================
 * docs/C_GAME_SHAPE_MIGRATION.md Phase 5a. The web client stops reading the
 * wire in TypeScript (sdk/ts/wire/packed_read.ts behind decodePackedGame and
 * decodeEventWire) and reads it through the kernel's client slot
 * (c/src/client_table.c, sdk/ts/table/client_table.ts), mapped onto today's
 * PersonalGame by src/state/snapshotToGame.ts. Two things are held here:
 *
 *   1. EQUALITY. For every envelope and every push a server writes while tables
 *      are created, joined, filled with bots, dealt, played to their end by humans
 *      and bots, and continued - 2, 4 and 8 seats, every human seat and the
 *      spectator - the new read is exactly the old one: the seat, the version,
 *      the board, the names, and each event with its board. The C Table writes
 *      the bytes (sdk/ts/table/server_table.ts on a private bots.wasm). Decided
 *      differences, removed before comparing: event message prose (Q7).
 *
 *   2. REFUSAL. An envelope that does not read whole - no trailer, a truncated
 *      trailer, a count past its capacity, a card byte that is not a card - is
 *      refused, never clamped into a board nobody sent.
 * ========================================================================== */

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { decodePackedGame } from '../sdk/ts/wire/view.ts';
import { decodeEventWire } from '../sdk/ts/wire/evwire.ts';
import { decodeEnvelope, pushToSequence } from '../src/state/snapshotToGame.ts';
import { clientTable } from '../sdk/ts/table/client_table.ts';
import { deserializeGameState, kernelLegalMoves } from '../sdk/ts/wasm/engine.ts';
import { wasmBotEligibleMask } from '../sdk/ts/wasm/bots.ts';
import { encodeAction, AWIRE_KIND } from '../sdk/ts/wire/awire.ts';
import { PLAYER_STATUS, GAME_STATUS } from '../server/api/core/types.ts';
import * as L from '../sdk/ts/gen/game_layout.bots.ts';
import { loadWasmGz } from '../sdk/ts/wasm/wasm_asset.ts';
import { assertLayoutHash } from '../sdk/ts/wasm/layout_hash.ts';
import { LAYOUT_HASH } from '../sdk/ts/gen/layout_hash.bots.ts';
import { ServerTable, type TableExports } from '../sdk/ts/table/server_table.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; }

const NOW = () => 1_760_000_000_000;
const inst = new WebAssembly.Instance(new WebAssembly.Module(loadWasmGz('bots') as BufferSource), {});
const sx = inst.exports as unknown as TableExports & {
    wasm_table_set_deal_seed(len: number): number;
    wasm_table_bot_drive(prefsLen: number, maxActions: number): number;
};
assertLayoutHash('bots.wasm', sx, LAYOUT_HASH, 'sdk/ts/gen/layout_hash.bots.ts');
sx.wasm_init();
const server = new ServerTable(sx);

function setDealSeed(hex: string): void {
    const bytes = new TextEncoder().encode(hex);
    new Uint8Array(sx.memory.buffer).set(bytes, sx.wasm_io_ptr());
    assert.equal(sx.wasm_table_set_deal_seed(bytes.length), L.TABLE_OK);
}

const counts = { envelopes: 0, pushes: 0, events: 0, games: 0, lobbyEdits: 0 };

interface Row { gid: string; state: Uint8Array; roster: Uint8Array; version: number; title: string }

const seed = (k: number) => Uint8Array.from({ length: 32 }, (_, i) => (i * 29 + k * 13 + 1) & 0xff);
const withoutProse = <T extends { events: { message?: string }[] }>(seq: T) => ({ ...seq, events: seq.events.map(({ message: _m, ...e }) => e) });

/** Every envelope and push of the table's last operation, read both ways. */
function compareProducts(label: string, row: Row, p: Exclude<ReturnType<ServerTable['commit']>, number>): void {
    const seats = server.seats();
    const viewers = [...seats.map((s, i) => (s.brain ? -1 : i)).filter((i) => i >= 0), -1];
    for (const viewer of viewers) {
        const env = viewer >= 0 ? p.views[viewer]! : p.spectator;
        const old = decodePackedGame(env, NOW);
        assert.ok(old, `${label}: the old reader reads the envelope`);
        assert.deepEqual(decodeEnvelope(env, NOW), old, `${label}: viewer ${viewer} envelope`);
        counts.envelopes++;
    }
    if (p.nEvents === 0) return;
    const title = decodePackedGame(p.spectator, NOW)!.game.name;
    const roster = { id: row.gid, name: title, players: seats.map((s) => ({ player_id: s.id, name: s.name, is_ai: s.brain !== '' })) };
    for (const viewer of viewers) {
        const as3 = server.push(row.gid, viewer);
        assert.ok(as3 instanceof Uint8Array, `${label}: push ${viewer}`);
        const old = decodeEventWire(as3.subarray(0, as3.length - 1), roster, { preGood: [], prevGoodTs: null, now: NOW });
        assert.ok(old, `${label}: the old reader reads the push`);
        const read = clientTable().readPush(as3, { as3: true, gameId: row.gid, version: row.version });
        assert.ok(read, `${label}: viewer ${viewer} push reads (${JSON.stringify(clientTable().lastRefusal())})`);
        const mine = pushToSequence(read, { now: NOW });
        assert.deepEqual({ viewerSeat: mine.viewerSeat, events: mine.events, game: mine.game },
            { viewerSeat: old.viewerSeat, ...withoutProse({ events: old.events }), game: old.game }, `${label}: viewer ${viewer} push`);
        counts.pushes++;
        counts.events += read.steps.length;
        // The as2 form of a move's push (servers before Phase 5b: the sequence alone) reads the same with the
        // kept identity. A roster change's push carries its roster after the flags byte, so it has no such form.
        if (p.rosterChanged) continue;
        const as2 = clientTable().readPush(as3.subarray(0, as3.length - 1), { as3: false, gameId: row.gid });
        assert.ok(as2 && as2.steps.length === read.steps.length, `${label}: viewer ${viewer} as2 push reads (${JSON.stringify(clientTable().lastRefusal())})`);
        assert.deepEqual(pushToSequence(as2, { now: NOW }).events, mine.events, `${label}: viewer ${viewer} as2 events`);
        counts.pushes++;
    }
}

function commit(label: string, row: Row): void {
    row.version++;
    const p = server.commit(row.gid, row.version, NOW());
    assert.ok(typeof p !== 'number', `${label}: commit (${p})`);
    row.state = p.state;
    row.roster = p.roster;
    compareProducts(label, row, p);
}

function lobby(label: string, row: Row, op: () => number): void {
    assert.equal(server.load(row.state, row.roster), L.TABLE_OK, `${label}: loads`);
    const rc = op();
    assert.ok(rc === L.TABLE_OK, `${label}: applied (${rc})`);
    counts.lobbyEdits++;
    commit(label, row);
}

function play(label: string, gid: string, humans: number, bots: string[], k: number): void {
    const row: Row = { gid, state: new Uint8Array(0), roster: new Uint8Array(0), version: 0, title: '' };
    const ids = Array.from({ length: humans }, (_, i) => `00000000-0000-4000-8000-${String(k * 16 + i).padStart(12, '0')}`);
    const names = ['Дмитрий', 'Zoë 🃏', 'Ann', '田中花子', 'q'.repeat(70), 'Bo', 'Cy', 'Di'];
    assert.equal(server.create(ids[0], names[0]), L.TABLE_OK);
    commit(`${label} create`, row);
    for (let i = 1; i < humans; i++) lobby(`${label} join ${i}`, row, () => server.join(ids[i], names[i]));
    bots.forEach((brain, b) => lobby(`${label} bot ${b}`, row, () => server.addBot(ids[0], `b0000000-0000-4000-8000-${String(k * 16 + b).padStart(12, '0')}`, `Bot ${b}`, brain, seed(k))));
    for (let i = 0; i < humans; i++) lobby(`${label} ready ${i}`, row, () => server.ready(ids[i], seed(k + i)));

    // The good order and timestamp are carried from the template only where the board's mask and flag say so.
    const template = () => ({
        id: gid, name: '', deck_length: 0, good_players: server.seats().map((s) => s.id), good_timestamp: 1,
        players: server.seats().map((s) => ({ player_id: s.id, name: s.name, is_ai: s.brain !== '', strategy_key: s.brain || 'human' })),
    });
    for (let step = 0; step < 4000; step++) {
        assert.equal(server.load(row.state, row.roster), L.TABLE_OK);
        const game = deserializeGameState(row.state, template());
        if (game.status !== GAME_STATUS.PLAYING) break;
        if (wasmBotEligibleMask(game) !== 0) {
            assert.equal(server.load(row.state, row.roster), L.TABLE_OK);
            setDealSeed('ab'.repeat(32));
            assert.ok(sx.wasm_table_bot_drive(0, 0) > 0, `${label} step ${step}: a bot cycle`);
        } else {
            const seat = game.players.findIndex((pl) => pl.status === PLAYER_STATUS.IN && !pl.is_ai
                && kernelLegalMoves(game, pl.player_id).some((m) => m.type !== 'wait'));
            if (seat < 0) break;
            const moves = kernelLegalMoves(game, game.players[seat].player_id).filter((m) => m.type !== 'wait');
            const m = moves[(step * 7) % moves.length];
            const wire = encodeAction({ kind: m.type as keyof typeof AWIRE_KIND, cards: m.cards, attack_cards: m.attack_cards });
            assert.equal(server.load(row.state, row.roster), L.TABLE_OK);
            const rc = server.act(game.players[seat].player_id, wire, null, 0);
            assert.equal(rc, L.TABLE_APPLIED, `${label} step ${step}: the human move ${JSON.stringify(m)} by ${seat} (reject ${server.reject()}; legal ${JSON.stringify(moves)})`);
        }
        commit(`${label} step ${step}`, row);
    }
    assert.equal(server.load(row.state, row.roster), L.TABLE_OK);
    assert.equal(deserializeGameState(row.state, template()).status, GAME_STATUS.GAME_OVER, `${label}: played to its end`);
    lobby(`${label} continue`, row, () => server.continueGame(ids[0]));
    counts.games++;
}

test('every envelope and push of whole games reads through C exactly as the TS readers read it', () => {
    play('2p', 'g2', 2, [], 1);
    play('4p', 'g4', 2, ['random', 'handwritten'], 2);
    play('8p', 'g8-0000-4000-8000-000000000008', 3, ['random', 'random', 'handwritten', 'random', 'simple_heuristic'], 3);
    console.error(`[client_envelope_decode] ${JSON.stringify(counts)}`);
    assert.ok(counts.games === 3 && counts.envelopes > 500 && counts.pushes > 300, `enough was read (${JSON.stringify(counts)})`);
});

// ---- refusals ------------------------------------------------------------------------

function sample(): Uint8Array {
    const row: Row = { gid: 'gr', state: new Uint8Array(0), roster: new Uint8Array(0), version: 0, title: '' };
    server.create('ann', 'Ann');
    const p0 = server.commit('gr', 1, 0) as Exclude<ReturnType<ServerTable['commit']>, number>;
    row.state = p0.state; row.roster = p0.roster;
    server.load(row.state, row.roster);
    server.join('bob', 'Bob');
    server.ready('ann', seed(9));
    server.ready('bob', seed(9));
    const p1 = server.commit('gr', 2, 0) as Exclude<ReturnType<ServerTable['commit']>, number>;
    server.load(p1.state, p1.roster);
    const game = deserializeGameState(p1.state, { id: 'gr', name: '', deck_length: 0, good_players: [], good_timestamp: null,
        players: server.seats().map((s) => ({ player_id: s.id, name: s.name, is_ai: false, strategy_key: 'human' })) });
    const attacker = game.players[game.first_attacker].player_id;
    const m = kernelLegalMoves(game, attacker).find((x) => x.type === 'attack')!;
    assert.equal(server.act(attacker, encodeAction({ kind: 'attack', cards: m.cards }), null, 0), L.TABLE_APPLIED);
    return server.envelope('gr', 0, 3) as Uint8Array;   // seat 0's view, one battle on the table
}

// Offsets of the sample envelope, for doctoring it (this test's knowledge, not the client's).
const VIEW_LEN_AT = 9, STATE_AT = 13;
const viewLen = (e: Uint8Array) => e[VIEW_LEN_AT] | (e[VIEW_LEN_AT + 1] << 8);
const battlesAt = (e: Uint8Array) => STATE_AT + 16 + (e[STATE_AT + 14] | (e[STATE_AT + 15] << 8));

test('an envelope that does not read whole is refused, never clamped into a board', () => {
    const env = sample();
    assert.ok(decodeEnvelope(env), 'the sample reads (the control)');
    const trailerAt = 11 + viewLen(env);

    assert.equal(decodeEnvelope(env.subarray(0, trailerAt)), null, 'no trailer');
    for (const cut of [1, 2, 10, env.length - trailerAt - 1]) {
        assert.equal(decodeEnvelope(env.subarray(0, env.length - cut)), null, `a trailer truncated by ${cut}`);
    }
    const noFlag = env.slice(0, trailerAt); noFlag[1] &= ~2;
    assert.equal(decodeEnvelope(noFlag), null, 'an envelope that announces no trailer');

    const at = battlesAt(env);
    assert.equal(env[at], 1, 'the sample has its one battle where the test expects');
    const badCard = env.slice(); badCard[at + 1] = 0x80;
    assert.equal(decodeEnvelope(badCard), null, 'a card byte 0x80 on the table');

    // A battle count past its capacity (64), with every byte it claims present and the lengths agreeing.
    const extra = 2 * 65 - 2;
    const over = new Uint8Array(env.length + extra);
    over.set(env.subarray(0, at + 1));
    over[at] = 65;
    for (let i = 0; i < 2 * 65; i++) over[at + 1 + i] = i % 36;
    over.set(env.subarray(at + 3), at + 1 + 2 * 65);
    const vl = viewLen(env) + extra;
    over[VIEW_LEN_AT] = vl & 0xff; over[VIEW_LEN_AT + 1] = vl >> 8;
    assert.equal(decodeEnvelope(over), null, 'a battle count over its capacity');
});
