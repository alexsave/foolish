// E2E for the packed action pipeline (docs/C_GAME_SHAPE_MIGRATION.md 2.3, Phase
// 4b): executePackedAction - the kernel's request decode, table_act (the seat of
// the auth id, the finished-game and round guards, the rules, the finish), the
// commit products, the CAS commit of the state blob (commit_table), and the
// {t,s,v,b} realtime broadcast of the kernel's per-viewer pushes - driven against
// a REAL Postgres through the same harness the other server suites use. The wire
// in is the real awire encoding of moves the REAL kernel enumerates; the wire
// out is decoded with the REAL client decoders (never hand-rolled parsing).

import './harness.ts'; // sets Deno globals BEFORE any server module loads
import './helpers/edge.ts'; // bot pacing sleeps resolve on the next tick
import { test, before, beforeEach, after } from 'node:test';
import assert from 'node:assert/strict';

import { applySchema, resetDb, uuid, pgPool, broadcastLog } from './harness.ts';
import * as L from '../sdk/ts/gen/game_layout.bots.ts';
import { ANIMATION_EVENT_TYPE } from '../server/api/core/types.ts';
import { fixture, PLAYING } from './helpers/table_fixture.ts';
import { seedTable } from './helpers/table_db.ts';
import { checkCardConservation, legalMoves, mustReadTable, type PlayMove } from './helpers/table_play.ts';
import { driveBots, runAction, runMeta, seedLobby } from './helpers/table_server.ts';
import { suiteRng } from './helpers/rng.ts';
import { executePackedAction, MalformedActionRequest } from '../server/impls/supabase/functions/_shared/adapter/packed_action.ts';
import { __setTableDealSeedOverride } from '../server/impls/supabase/functions/_shared/adapter/table_io.ts';
import { __clearGameCache } from '../server/impls/supabase/functions/_shared/adapter/game_cache.ts';
import { encodeAction, encodeActionRequest, ACTION_STATUS } from '../sdk/ts/wire/awire.ts';
import { readPush } from './helpers/client_read.ts';
import type { ReadRoster as ViewRoster } from './helpers/client_read.ts';
import { base64ToBytes } from '../sdk/ts/wire/bytes.ts';

// console.error too: a fixture game has no deal seed, so its end logs that no
// replay snapshot could be encoded (expected here, and finalize's own suites own it).
if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; console.error = () => {}; }

// Seeded so a failure reproduces from the printed seed: the deals, the moves and
// the fuzzed wires all draw from it.
const rng = suiteRng('packed_action');
const ri = (n: number) => rng.int(n);
const pick = <T>(a: T[]): T => rng.pick(a);

const KNOWN_EVENT_TYPES = new Set<string>(Object.values(ANIMATION_EVENT_TYPE));
// A pinned clock: a decoded good_timestamp is read from it, and two decodes of
// the same bytes must compare equal.
const CTX = { preGood: [], prevGoodTs: null, now: () => 1 };

// The broadcast is fire-and-forget (executePackedAction does not await it);
// drain the microtask/immediate queue so broadcastLog is settled.
const settle = () => new Promise((r) => setImmediate(() => setImmediate(r)));

const hexOf = (b: Uint8Array) => Buffer.from(b).toString('hex');
const animations = (from: number) => broadcastLog.slice(from).filter((b) => b.event === 'animation_events');

interface Seeded { gameId: string; seats: { id: string; name: string; brain: string }[]; roster: ViewRoster }

const rosterOf = (gameId: string, seats: Seeded['seats']): ViewRoster =>
    ({ id: gameId, name: gameId, players: seats.map((s) => ({ player_id: s.id, name: s.name, is_ai: s.brain !== '' })) });

// A real PLAYING game dealt by the server: a kernel lobby, the first human's
// start through the real meta handler, on a deal seed from the suite stream.
async function newDealtGame(humans: number, bots = 0): Promise<Seeded> {
    const gameId = `pk${uuid().slice(0, 6)}`;
    const lobby = [];
    for (let i = 0; i < humans; i++) lobby.push({ id: uuid(), name: `H${i}`, ready: i > 0 });
    for (let i = 0; i < bots; i++) lobby.push({ id: uuid(), name: `B${i}`, brain: 'random' });
    await seedLobby(gameId, lobby);
    __setTableDealSeedOverride(Uint8Array.from({ length: 32 }, () => ri(256)));
    await runMeta(gameId, lobby[0].id, { type: 'start' });
    const t = await mustReadTable(gameId);
    assert.equal(t.statusColumn, 'playing', 'game started');
    return { gameId, seats: t.seats, roster: rosterOf(gameId, t.seats) };
}

// A dealt two-human board whose next attack ends the game: A (seat 0) attacks
// with its last card and the deck is empty.
async function lastCardGame(): Promise<Seeded> {
    const gameId = `pe${uuid().slice(0, 6)}`;
    const seats = [{ id: uuid(), name: 'A', brain: '' }, { id: uuid(), name: 'B', brain: '' }];
    await seedTable(gameId, fixture().seats(seats).status(PLAYING).attacker(0).defender(1)
        .hand(0, '6h').hand(1, '7s 8s').powerSuit(L.SUIT_CLUBS).discard(33).build(), { version: 4 });
    return { gameId, seats, roster: rosterOf(gameId, seats) };
}

if (!process.env.VALIDATION_ONLY) {
before(async () => { await applySchema(); });
beforeEach(async () => { await resetDb(); __clearGameCache(); });
after(async () => { __setTableDealSeedOverride(null); await pgPool.end(); });

test('packed pipeline: legal awire moves apply, bump the version, rewrite the blob, and broadcast decodable {t,s,v,b} payloads', async () => {
    const { gameId, seats, roster } = await newDealtGame(3);
    // The move-kind -> the actor's own event type in the broadcast stream.
    const ACTOR_EVENT: Record<string, string> = {
        attack: ANIMATION_EVENT_TYPE.ATTACK_PASS,
        pass: ANIMATION_EVENT_TYPE.ATTACK_PASS,
        cover: ANIMATION_EVENT_TYPE.COVER,
        pickup: ANIMATION_EVENT_TYPE.PICKUP,
    };

    let applied = 0, broadcasted = 0;
    for (let step = 0; step < 14; step++) {
        const prev = await mustReadTable(gameId);
        if (prev.status !== L.GAME_STATUS_PLAYING) break;
        const moves = legalMoves(prev);
        if (moves.length === 0) break;
        const pm = pick(moves);
        const logStart = broadcastLog.length;

        const out = await runAction(gameId, pm.playerId, pm);
        await settle();

        assert.equal(out.status, ACTION_STATUS.APPLIED, `legal ${pm.kind} applies (step ${step}, seed=${rng.seed})`);
        assert.equal(out.rejectCode, 0, 'applied moves carry no reject code');
        assert.equal(out.version, prev.version + 1, 'committed version increments');
        const row = await mustReadTable(gameId);
        assert.equal(row.version, out.version, 'games.version matches the returned version');
        assert.notEqual(hexOf(row.state), hexOf(prev.state), `the state blob changed (step ${step} ${pm.kind})`);

        const chk = await checkCardConservation(gameId);
        assert.ok(chk.ok, `card conservation after packed ${pm.kind} at step ${step}: ${chk.detail}`);

        const fresh = animations(logStart);
        if (fresh.length === 0) {
            // A move producing zero events broadcasts nothing - only a plain good.
            assert.equal(pm.kind, 'good', `only a plain good may broadcast nothing (step ${step} ${pm.kind})`);
        } else {
            broadcasted++;
            assert.equal(fresh.length, seats.length + 1, 'one payload per human + one spectator payload');
            const chans = new Set(fresh.map((f) => f.channel));
            for (const p of seats) assert.ok(chans.has(`gu-${gameId}-${p.id}`), `payload for human ${p.name}`);
            assert.ok(chans.has(`game-${gameId}`), 'spectator payload');
            for (const f of fresh) {
                assert.deepEqual(Object.keys(f.payload).sort(), ['b', 's', 't', 'v'], 'exactly {t,s,v,b}: no r / m extras (Q7)');
                assert.equal(f.payload.t, 'as3', 'packed payload tag');
                assert.equal(typeof f.payload.s, 'string', 'sequence id');
                assert.equal(f.payload.v, out.version, 'payload.v is the committed version');
                const bytes = base64ToBytes(f.payload.b);
                const decoded = readPush(bytes, roster, CTX);
                assert.ok(decoded, 'payload.b decodes as event wire');
                // as3: the as2 sequence, then one flags byte (0 for a move: no roster trailer).
                assert.equal(bytes[bytes.length - 1], 0, 'a move\'s push ends in a zero flags byte');
                assert.deepEqual(readPush(bytes.subarray(0, bytes.length - 1), roster, CTX), decoded,
                    'and everything before it is the as2 sequence');
                const expectSeat = f.channel === `game-${gameId}` ? -1 : seats.findIndex((p) => f.channel === `gu-${gameId}-${p.id}`);
                assert.equal(decoded!.viewerSeat, expectSeat, 'stream is personalized to its channel');
                assert.ok(decoded!.events.length > 0, 'broadcast carries events');
                for (const ev of decoded!.events) {
                    assert.ok(KNOWN_EVENT_TYPES.has(ev.type), `known event type ${ev.type}`);
                    if (ev.player_id !== undefined) {
                        assert.ok(seats.some((p) => p.id === ev.player_id), `event player ${ev.player_id} is in the game`);
                    }
                }
                const actorEvent = ACTOR_EVENT[pm.kind];
                if (actorEvent) {
                    assert.ok(decoded!.events.some((ev) => ev.type === actorEvent && ev.player_id === pm.playerId),
                        `a ${pm.kind} broadcasts a ${actorEvent} event by the actor`);
                }
                // The trailer is the committed final state - publicly consistent.
                assert.equal(decoded!.game.status, row.statusColumn, 'decoded final state matches the committed status');
            }
        }
        applied++;
    }
    assert.ok(applied >= 6, `exercised enough packed moves (${applied}, seed=${rng.seed})`);
    assert.ok(broadcasted >= 3, `enough eventful broadcasts (${broadcasted}, seed=${rng.seed})`);
});

test('packed pipeline: an illegal move is REJECTED with a reject code - no version bump, no blob write, no broadcast', async () => {
    const { gameId } = await newDealtGame(2);
    const prev = await mustReadTable(gameId);
    // The defender attacking is always illegal (ENGINE_REJECT_IS_DEFENDER).
    const defender = prev.seats[prev.defender];
    const logStart = broadcastLog.length;

    const out = await runAction(gameId, defender.id, encodeAction({ kind: 'attack', cards: [defender.hand[0]] }));
    await settle();

    assert.equal(out.status, ACTION_STATUS.REJECTED, 'illegal move is rejected');
    assert.equal(out.rejectCode, L.ENGINE_REJECT_IS_DEFENDER, `rejection carries the kernel's reason (got ${out.rejectCode})`);
    assert.equal(out.version, prev.version, 'returned version is the untouched current version');
    assert.equal(out.needsBots, false, 'nothing committed, nothing to wake');
    const row = await mustReadTable(gameId);
    assert.equal(row.version, prev.version, 'games.version did not bump');
    assert.equal(hexOf(row.state), hexOf(prev.state), 'the state blob is untouched');
    assert.equal(animations(logStart).length, 0, 'no broadcast for a rejected move');
    const chk = await checkCardConservation(gameId);
    assert.ok(chk.ok, `conservation after a rejected move: ${chk.detail}`);
});

test('packed pipeline: a move against a finished game is MOOT', async () => {
    const { gameId, seats } = await lastCardGame();
    const t0 = await mustReadTable(gameId);

    // A's last card ends the game (through the real move path and its finalize).
    const last = legalMoves(t0, (_, seat) => seat === 0).find((m) => m.kind === 'attack')!;
    assert.equal((await runAction(gameId, seats[0].id, last)).status, ACTION_STATUS.APPLIED, 'the last card applies');
    const prev = await mustReadTable(gameId);
    assert.equal(prev.statusColumn, 'game_over', 'and ended the game');
    await settle();
    const logStart = broadcastLog.length;

    // B's pickup, composed before the game ended, arrives now.
    const out = await runAction(gameId, seats[1].id, encodeAction({ kind: 'pickup' }), t0.version);
    await settle();

    assert.equal(out.status, ACTION_STATUS.MOOT, 'move against a finished game is a no-op');
    assert.equal(out.rejectCode, 0, 'moot carries no reject code');
    assert.equal(out.version, prev.version, 'version untouched');
    const row = await mustReadTable(gameId);
    assert.equal(row.version, prev.version, 'games.version did not bump');
    assert.equal(hexOf(row.state), hexOf(prev.state), 'state blob untouched');
    assert.equal(animations(logStart).length, 0, 'no broadcast for a moot move');
});

test('packed pipeline: malformed wire throws cleanly, never commits, never breaks conservation', async () => {
    const { gameId, seats } = await newDealtGame(2);
    const actor = seats[0].id;
    const prev = await mustReadTable(gameId);
    const logStart = broadcastLog.length;

    const malformed: { label: string; wire: Uint8Array }[] = [
        { label: 'empty', wire: new Uint8Array([]) },
        { label: 'one byte', wire: new Uint8Array([0]) },
        { label: 'unknown kind', wire: new Uint8Array([9, 0]) },
        { label: 'n > 28', wire: new Uint8Array([0, 29, ...Array(29).fill(7)]) },
        { label: 'pickup with cards', wire: new Uint8Array([3, 1, 5]) },
        { label: 'good with cards', wire: new Uint8Array([4, 2, 5, 6]) },
        { label: 'attack truncated', wire: new Uint8Array([0, 3, 1, 2]) },
        { label: 'cover missing attack half', wire: new Uint8Array([1, 2, 10, 11, 12]) },
        { label: 'trailing garbage', wire: new Uint8Array([2, 1, 7, 7, 7]) },
        { label: 'oversized (>128 bytes)', wire: new Uint8Array(200).fill(1) },
    ];
    for (const { label, wire } of malformed) {
        await assert.rejects(runAction(gameId, actor, wire), MalformedActionRequest,
            `${label}: rejects with the clean malformed-request error`);
        const chk = await checkCardConservation(gameId);
        assert.ok(chk.ok, `conservation after malformed wire (${label}): ${chk.detail}`);
    }
    // A request body the kernel cannot parse at all is the same clean error.
    for (const [label, body] of [
        ['empty body', new Uint8Array([])],
        ['bad format', new Uint8Array([7, 2, 97, 98, 3, 0])],
        ['game id past the end', encodeActionRequest(gameId, encodeAction({ kind: 'pickup' })).subarray(0, 6)],
    ] as const) {
        await assert.rejects(executePackedAction(body, actor, 'mal'), MalformedActionRequest, `${label}: malformed request`);
    }
    await settle();
    let row = await mustReadTable(gameId);
    assert.equal(row.version, prev.version, 'no malformed wire committed anything');
    assert.equal(hexOf(row.state), hexOf(prev.state), 'state blob untouched by malformed wire');
    assert.equal(animations(logStart).length, 0, 'malformed wire never broadcasts');

    // Random-bytes fuzz: any buffer must either throw the clean malformed
    // error or come back with a well-defined status. Half the buffers are
    // pure noise (a valid kind byte is a 5/256 accident, so these virtually
    // always throw); half are shaped - valid kind + consistent length but
    // random card bytes - which decode structurally and get rule-rejected.
    // A shaped buffer CAN even be a real legal move (e.g. [4,0] is a plain
    // good), so applied is a legitimate outcome too. The invariants: no
    // other crash, the version moves only on APPLIED, conservation always.
    let threw = 0, rejected = 0, appliedRandom = 0;
    let lastVersion = row.version;
    for (let i = 0; i < 160; i++) {
        let wire: Uint8Array;
        if (i % 2 === 0) {
            wire = new Uint8Array(ri(40));
            for (let j = 0; j < wire.length; j++) wire[j] = ri(256);
        } else {
            const kind = ri(5);
            const n = kind === 3 || kind === 4 ? 0 : 1 + ri(6);
            const body = new Uint8Array(kind === 1 ? 2 * n : n);
            for (let j = 0; j < body.length; j++) body[j] = ri(256);
            wire = new Uint8Array([kind, n, ...body]);
        }
        const who = pick(seats).id;
        try {
            const out = await runAction(gameId, who, wire);
            assert.ok(([ACTION_STATUS.APPLIED, ACTION_STATUS.REJECTED, ACTION_STATUS.MOOT] as number[]).includes(out.status),
                `random wire returned a defined status (got ${out.status})`);
            if (out.status === ACTION_STATUS.APPLIED) {
                assert.equal(out.version, lastVersion + 1, 'random-but-legal wire bumps the version by one');
                lastVersion = out.version;
                appliedRandom++;
            } else {
                assert.equal(out.version, lastVersion, 'non-applied outcomes never move the version');
                rejected++;
            }
        } catch (e) {
            assert.ok(e instanceof MalformedActionRequest,
                `random wire may only throw the clean malformed-request error (iter ${i}, seed=${rng.seed}): ${e}`);
            threw++;
        }
        const chk = await checkCardConservation(gameId);
        assert.ok(chk.ok, `conservation after random wire iter ${i}: ${chk.detail}`);
    }
    await settle();
    row = await mustReadTable(gameId);
    assert.equal(row.version, lastVersion, 'DB version consistent with the outcomes seen');
    assert.ok(threw > 20 && rejected > 20, `fuzz exercised both outcomes (threw=${threw} rejected=${rejected} applied=${appliedRandom}, seed=${rng.seed})`);
});

test('human moves and the bot loop interleave: same payload shape, strictly increasing versions per channel', async () => {
    // The two writers that emit animations on a game in play: the move path and
    // the bot loop (lockedBotLoop), each through its own commit and broadcast.
    const { gameId, seats, roster } = await newDealtGame(2, 1);
    const humans = seats.filter((s) => !s.brain);
    const logStart = broadcastLog.length;

    let humanApplied = 0, botCommits = 0;
    for (let step = 0; step < 40 && (humanApplied < 3 || botCommits < 3); step++) {
        const t = await mustReadTable(gameId);
        if (t.status !== L.GAME_STATUS_PLAYING) break;
        if (t.needsBotsColumn && step % 2 === 1) {
            await driveBots(gameId);
            if ((await mustReadTable(gameId)).version > t.version) botCommits++;
        } else {
            const moves: PlayMove[] = legalMoves(t, (s) => !s.brain);
            if (moves.length === 0) continue;
            const pm = pick(moves);
            const out = await runAction(gameId, pm.playerId, pm);
            assert.equal(out.status, ACTION_STATUS.APPLIED, `human leg applies (step ${step} ${pm.kind}, seed=${rng.seed})`);
            humanApplied++;
        }
        await settle();
        const chk = await checkCardConservation(gameId);
        assert.ok(chk.ok, `conservation after interleaved step ${step}: ${chk.detail}`);
    }
    assert.ok(humanApplied >= 3 && botCommits >= 3, `both paths exercised (human=${humanApplied} bot=${botCommits}, seed=${rng.seed})`);

    // Every broadcast from EITHER path is the same packed envelope, and each
    // channel's versions are strictly increasing across the mixed paths.
    const evts = animations(logStart);
    assert.ok(evts.length > 0, 'interleaved moves broadcast');
    const perChannel = new Map<string, number[]>();
    for (const e of evts) {
        assert.deepEqual(Object.keys(e.payload).sort(), ['b', 's', 't', 'v'], 'exactly {t,s,v,b}');
        assert.equal(e.payload.t, 'as3', 'payload tag');
        assert.equal(typeof e.payload.s, 'string', 'sequence id');
        assert.equal(typeof e.payload.v, 'number', 'numeric version');
        assert.equal(typeof e.payload.b, 'string', 'base64 event wire');
        assert.ok(readPush(base64ToBytes(e.payload.b), roster, CTX), 'payload decodes regardless of the emitting path');
        if (!perChannel.has(e.channel)) perChannel.set(e.channel, []);
        perChannel.get(e.channel)!.push(e.payload.v);
    }
    assert.equal(perChannel.size, humans.length + 1, 'per-human channels + the spectator channel (a bot has none)');
    for (const [chan, vs] of perChannel) {
        for (let i = 1; i < vs.length; i++) {
            assert.ok(vs[i] > vs[i - 1], `versions not strictly increasing on ${chan}: ${vs.join(',')}`);
        }
    }
});
}
