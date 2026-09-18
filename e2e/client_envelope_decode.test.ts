/* =============================================================================
 * The web reads envelopes and pushes through C, and reads them as it did
 * =============================================================================
 * docs/C_GAME_SHAPE_MIGRATION.md Phase 5a. The web client stops reading the
 * wire in TypeScript (sdk/ts/wire/packed_read.ts behind decodePackedGame and
 * decodeEventWire) and reads it through the kernel's client slot
 * (c/src/client_table.c, sdk/ts/table/client_table.ts) into TableView boards.
 * Two things are held here:
 *
 *   1. EQUALITY. For every envelope and every push a server writes while tables
 *      are created, joined, filled with bots, dealt, played to their end by humans
 *      and bots, and continued - 2, 4 and 8 seats, every human seat and the
 *      spectator - the client reads the board the table holds, as that viewer
 *      may see it: the seat, the version, the names, the counts, the table, its
 *      own hand. The C Table writes the bytes (sdk/ts/table/server_table.ts on the
 *      fixtures' bots.wasm); the truth is the whole board the table loaded, read
 *      through the generated accessors (e2e/helpers/table_mem.ts), with every
 *      hand but the viewer's cut to its count. A push reads to the same board
 *      its envelope gives, in every form a server sends it (as3, as3 labelled
 *      as2, as2).
 *
 *      Until the TS readers were deleted this compared against them instead:
 *      2,066 envelopes and 2,579 pushes read identically (commit e6a295fd),
 *      message prose aside (Q7). Until Phase 8 the truth was the state blob
 *      personalized by the server's TS oracle (personalize_game).
 *
 *   2. REFUSAL. An envelope that does not read whole - no trailer, a truncated
 *      trailer, a count past its capacity, a card byte that is not a card - is
 *      refused, never clamped into a board nobody sent.
 * ========================================================================== */

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { clientTable, type TableView } from '../sdk/ts/table/client_table.ts';
import { pushToSequence } from '../src/state/pushSequence.ts';
import { encodeAction } from '../sdk/ts/wire/awire.ts';
import * as L from '../sdk/ts/gen/game_layout.bots.ts';
import * as V from '../sdk/ts/gen/view_layout.bots.ts';
import type { ServerTable } from '../sdk/ts/table/server_table.ts';
import { fixtureTable } from './helpers/table_fixture.ts';
import { residentBoard, residentMoves, type MemCard } from './helpers/table_mem.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; }

const NOW = () => 1_760_000_000_000;
const server = fixtureTable();

const counts = { envelopes: 0, pushes: 0, events: 0, games: 0, lobbyEdits: 0 };

interface Row { gid: string; state: Uint8Array; roster: Uint8Array; version: number; title: string }

const seed = (k: number) => Uint8Array.from({ length: 32 }, (_, i) => (i * 29 + k * 13 + 1) & 0xff);
const NO_CARD = { suit: V.CARD_NONE_SUIT, value: V.CARD_NONE_VALUE };
const readEnvelope = (bytes: Uint8Array): TableView | null => clientTable().adoptEnvelope(bytes);

/** The table's board as `viewer` may see it: the whole board, every other hand cut to its count. */
function truth(row: Row, viewer: number) {
    assert.equal(server.load(row.state, row.roster), L.TABLE_OK);
    const b = residentBoard();
    const card = (c: MemCard) => ({ suit: c.suit, value: c.value });
    return {
        status: b.status, powerSuit: b.powerSuit, firstAttacker: b.firstAttacker, defender: b.defender,
        deckCount: b.deck.length, discardPileLength: b.discard,
        hasFlipped: b.trump !== null, hasGoodTimestamp: b.hasGoodTimestamp, goodMask: b.goodMask,
        battles: b.battles.map((x) => ({ attack: card(x.attack), defense: x.defense ? card(x.defense) : NO_CARD })),
        seats: b.seats.map((s) => ({ id: s.id, name: s.name, isAi: s.brain !== '', status: s.status, handCount: s.hand.length })),
        myHand: viewer >= 0 ? b.seats[viewer].hand.map(card) : [],
        awaiting: viewer >= 0 ? b.seats[viewer].awaiting : false,
        elimination: b.eliminated, trump: b.trump,
    };
}

/** Every envelope and push of the table's last operation, read by the client and held to the table. */
function compareProducts(label: string, row: Row, p: Exclude<ReturnType<ServerTable['commit']>, number>): void {
    const seats = server.seats();
    const viewers = [...seats.map((s, i) => (s.brain ? -1 : i)).filter((i) => i >= 0), -1];
    // The pushes are the last operation's, so they are copied out before the truth reloads the table.
    const pushes = new Map(viewers.map((viewer) => [viewer, p.nEvents > 0 ? server.push(row.gid, viewer) : null]));
    const envelopeViews = new Map<number, TableView>();
    for (const viewer of viewers) {
        const env = viewer >= 0 ? p.views[viewer]! : p.spectator;
        const v = readEnvelope(env);
        assert.ok(v, `${label}: viewer ${viewer} envelope reads`);
        const want = truth(row, viewer);
        const w = `${label}: viewer ${viewer}`;
        assert.equal(v.mySeat, viewer, `${w} seat`);
        assert.equal(v.version, row.version, `${w} version`);
        assert.equal(v.gameId, row.gid, `${w} id`);
        assert.equal(v.title, row.title, `${w} title`);
        assert.deepEqual(v.seats.map(({ id, name, isAi, status, handCount }) => ({ id, name, isAi, status, handCount })), want.seats,
            `${w} seats (ids, names, bots, statuses, counts)`);
        assert.deepEqual(v.battles, want.battles, `${w} table`);
        assert.equal(v.deckCount, want.deckCount, `${w} deck`);
        assert.equal(v.discardPileLength, want.discardPileLength, `${w} discard`);
        assert.equal(v.hasFlipped, want.hasFlipped, `${w} trump face up`);
        if (want.trump) assert.deepEqual(v.flipped, want.trump, `${w} trump`);
        assert.equal(v.status, want.status, `${w} status`);
        assert.equal(v.powerSuit, want.powerSuit, `${w} power suit`);
        assert.equal(v.firstAttacker, want.firstAttacker, `${w} first attacker`);
        assert.equal(v.defender, want.defender, `${w} defender`);
        assert.deepEqual(v.elimination, want.elimination, `${w} elimination`);
        assert.equal(v.goodMask, want.goodMask, `${w} goods`);
        assert.equal(v.hasGoodTimestamp, want.hasGoodTimestamp, `${w} good clock`);
        assert.deepEqual(v.myHand, want.myHand, `${w} own hand, in order${viewer < 0 ? ' (a spectator holds none)' : ''}`);
        if (viewer >= 0) assert.equal(v.seats[viewer].awaitingAttack, want.awaiting, `${w} awaiting attack`);
        envelopeViews.set(viewer, v);
        counts.envelopes++;
    }
    if (p.nEvents === 0) return;
    for (const viewer of viewers) {
        const as3 = pushes.get(viewer);
        assert.ok(as3 instanceof Uint8Array, `${label}: push ${viewer}`);
        const read = clientTable().readPush(as3, { as3: true, gameId: row.gid, version: row.version });
        assert.ok(read, `${label}: viewer ${viewer} push reads (${JSON.stringify(clientTable().lastRefusal())})`);
        const mine = pushToSequence(read);
        assert.equal(mine.viewerSeat, viewer, `${label}: viewer ${viewer} push is theirs`);
        assert.equal(mine.events.length, p.nEvents, `${label}: viewer ${viewer} push carries every event`);
        assert.deepEqual(mine.game, envelopeViews.get(viewer), `${label}: viewer ${viewer} push ends on the envelope's board`);
        mine.events.forEach((e, i) => {
            const raw = read.steps[i].event, w = `${label}: viewer ${viewer} event ${i}`;
            assert.deepEqual(e.game_state.seats.map((s) => s.id), seats.map((s) => s.id), `${w} names the seats`);
            assert.equal(e.seat, raw.seat >= 0 ? raw.seat : undefined, `${w} actor`);
            assert.ok(typeof e.type === 'string' && e.type.length > 0, `${w} type`);
            assert.deepEqual(e.cards ?? [], raw.cards, `${w} cards`);
            assert.equal(e.battle_index, raw.battle >= 0 ? raw.battle : undefined, `${w} battle`);
            assert.deepEqual(e.target_card, raw.hasTarget ? raw.target : undefined, `${w} target`);
        });
        counts.pushes++;
        counts.events += read.steps.length;
        // What a server since Phase 4b sends until Phase 5b: these very bytes, labelled as2.
        const labelled = clientTable().readPush(as3, { as3: false, gameId: row.gid, version: row.version });
        assert.ok(labelled, `${label}: viewer ${viewer} as3 bytes labelled as2 read (${JSON.stringify(clientTable().lastRefusal())})`);
        assert.deepEqual(pushToSequence(labelled), mine, `${label}: viewer ${viewer} as3 bytes labelled as2`);
        counts.pushes++;
        // The as2 form of a move's push (servers before Phase 4b: the sequence alone) reads the same with the
        // kept identity. A roster change's push carries its roster after the flags byte, so it has no such form.
        if (p.rosterChanged) continue;
        const as2 = clientTable().readPush(as3.subarray(0, as3.length - 1), { as3: false, gameId: row.gid, version: row.version });
        assert.ok(as2 && as2.steps.length === read.steps.length, `${label}: viewer ${viewer} as2 push reads (${JSON.stringify(clientTable().lastRefusal())})`);
        assert.deepEqual(pushToSequence(as2).events, mine.events, `${label}: viewer ${viewer} as2 events`);
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
    row.title = `${names[0]}'s Game`;
    assert.equal(server.create(ids[0], names[0]), L.TABLE_OK);
    commit(`${label} create`, row);
    for (let i = 1; i < humans; i++) lobby(`${label} join ${i}`, row, () => server.join(ids[i], names[i]));
    bots.forEach((brain, b) => lobby(`${label} bot ${b}`, row, () => server.addBot(ids[0], `b0000000-0000-4000-8000-${String(k * 16 + b).padStart(12, '0')}`, `Bot ${b}`, brain, seed(k))));
    for (let i = 0; i < humans; i++) lobby(`${label} ready ${i}`, row, () => server.ready(ids[i], seed(k + i)));

    for (let step = 0; step < 4000; step++) {
        assert.equal(server.load(row.state, row.roster), L.TABLE_OK);
        const board = residentBoard();
        if (board.status !== L.GAME_STATUS_PLAYING) break;
        // A bot seat still in may have no move yet (the drive says so by applying nothing): then a human moves.
        let drove = false;
        if (server.needsBots()) {
            assert.equal(server.setDealSeed('ab'.repeat(32)), L.TABLE_OK);
            const d = server.botDrive(null);
            assert.ok(typeof d !== 'number', `${label} step ${step}: a bot cycle (${d})`);
            drove = d.n > 0;
        }
        if (!drove) {
            assert.equal(server.load(row.state, row.roster), L.TABLE_OK);
            const seat = board.seats.findIndex((s, i) => s.status === L.PLAYER_STATUS_IN && !s.brain && residentMoves(i).length > 0);
            assert.ok(seat >= 0, `${label} step ${step}: a human has a move when no bot does`);
            const moves = residentMoves(seat);
            const m = moves[(step * 7) % moves.length];
            assert.equal(server.load(row.state, row.roster), L.TABLE_OK);
            const rc = server.act(board.seats[seat].id, m.wire, null, 0);
            assert.equal(rc, L.TABLE_APPLIED, `${label} step ${step}: the human move ${m.kind} by ${seat} (reject ${server.reject()})`);
        }
        commit(`${label} step ${step}`, row);
    }
    assert.equal(server.load(row.state, row.roster), L.TABLE_OK);
    assert.equal(residentBoard().status, L.GAME_STATUS_GAME_OVER, `${label}: played to its end`);
    lobby(`${label} continue`, row, () => server.continueGame(ids[0]));
    counts.games++;
}

test('every envelope and push of whole games reads through C as the board the table holds', () => {
    play('2p', 'g2', 2, [], 1);
    play('4p', 'g4', 2, ['random', 'handwritten'], 2);
    play('8p', 'g8-0000-4000-8000-000000000008', 3, ['random', 'random', 'handwritten', 'random', 'simple_heuristic'], 3);
    console.error(`[client_envelope_decode] ${JSON.stringify(counts)}`);
    assert.ok(counts.games === 3 && counts.envelopes > 500 && counts.pushes > 300, `enough was read (${JSON.stringify(counts)})`);
});

// ---- refusals ------------------------------------------------------------------------

function sample(): Uint8Array {
    server.create('ann', 'Ann');
    const p0 = server.commit('gr', 1, 0) as Exclude<ReturnType<ServerTable['commit']>, number>;
    server.load(p0.state, p0.roster);
    server.join('bob', 'Bob');
    server.ready('ann', seed(9));
    server.ready('bob', seed(9));
    const p1 = server.commit('gr', 2, 0) as Exclude<ReturnType<ServerTable['commit']>, number>;
    server.load(p1.state, p1.roster);
    const attacker = residentBoard().firstAttacker;
    const m = residentMoves(attacker).find((x) => x.kind === 'attack')!;
    assert.equal(server.act(server.seats()[attacker].id, encodeAction({ kind: 'attack', cards: m.cards }), null, 0), L.TABLE_APPLIED);
    return server.envelope('gr', 0, 3) as Uint8Array;   // seat 0's view, one battle on the table
}

// Offsets of the sample envelope, for doctoring it (this test's knowledge, not the client's).
const VIEW_LEN_AT = 9, STATE_AT = 13;
const viewLen = (e: Uint8Array) => e[VIEW_LEN_AT] | (e[VIEW_LEN_AT + 1] << 8);
const battlesAt = (e: Uint8Array) => STATE_AT + 16 + (e[STATE_AT + 14] | (e[STATE_AT + 15] << 8));

test('an envelope that does not read whole is refused, never clamped into a board', () => {
    const env = sample();
    assert.ok(readEnvelope(env), 'the sample reads (the control)');
    const trailerAt = 11 + viewLen(env);

    assert.equal(readEnvelope(env.subarray(0, trailerAt)), null, 'no trailer');
    for (const cut of [1, 2, 10, env.length - trailerAt - 1]) {
        assert.equal(readEnvelope(env.subarray(0, env.length - cut)), null, `a trailer truncated by ${cut}`);
    }
    const noFlag = env.slice(0, trailerAt); noFlag[1] &= ~2;
    assert.equal(readEnvelope(noFlag), null, 'an envelope that announces no trailer');

    const at = battlesAt(env);
    assert.equal(env[at], 1, 'the sample has its one battle where the test expects');
    const badCard = env.slice(); badCard[at + 1] = 0x80;
    assert.equal(readEnvelope(badCard), null, 'a card byte 0x80 on the table');

    // A battle count past its capacity (64), with every byte it claims present and the lengths agreeing.
    const extra = 2 * 65 - 2;
    const over = new Uint8Array(env.length + extra);
    over.set(env.subarray(0, at + 1));
    over[at] = 65;
    for (let i = 0; i < 2 * 65; i++) over[at + 1 + i] = i % 36;
    over.set(env.subarray(at + 3), at + 1 + 2 * 65);
    const vl = viewLen(env) + extra;
    over[VIEW_LEN_AT] = vl & 0xff; over[VIEW_LEN_AT + 1] = vl >> 8;
    assert.equal(readEnvelope(over), null, 'a battle count over its capacity');
});
