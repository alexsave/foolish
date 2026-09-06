// E2E for the packed action pipeline (docs/PACKED_WIRE_CUTOVER.md):
// executePackedAction — auth-to-seat mapping, the one-call kernel section
// (apply + finalize + per-viewer event streams), the CAS commit of the state
// blob, and the {t,s,v,b} realtime broadcast — driven against a REAL Postgres
// through the same harness the JSON-path tests use. The wire in is the real
// awire encoding of moves enumerated by the REAL legal-move code; the wire
// out is decoded with the REAL client decoders (never hand-rolled parsing).

import './harness.ts'; // sets Deno globals BEFORE any server module loads
import { test, before, beforeEach, after } from 'node:test';
import assert from 'node:assert/strict';

import { applySchema, resetDb, seedGame, uuid, pgPool, broadcastLog } from './harness.ts';
import { executeWithGameLock, loadCompleteGame } from '../server/impls/supabase/functions/_shared/adapter/utils.ts';
import { packedProducts, start_game_packed } from '../server/api/common/game_lifecycle.ts';
import { verify_player_in_game } from '../server/api/common/common_utils.ts';
import { AnimationEvent, ANIMATION_EVENT_TYPE, Game } from '../server/api/core/types.ts';
import { handleAttack } from '../server/api/common/actions/attack.ts';
import { handleCover } from '../server/api/common/actions/cover.ts';
import { handlePass } from '../server/api/common/actions/pass.ts';
import { handlePickup } from '../server/api/common/actions/pickup.ts';
import { handleGood } from '../server/api/common/actions/good.ts';
import { executePackedAction } from '../server/impls/supabase/functions/_shared/adapter/packed_action.ts';
import { encodeAction, ACTION_STATUS, AwireKindName } from '../sdk/ts/wire/awire.ts';
import { decodeEventWire } from '../sdk/ts/wire/evwire.ts';
import { ViewRoster } from '../sdk/ts/wire/view.ts';
import { base64ToBytes } from '../sdk/ts/wire/bytes.ts';
import { legalMovesFor, checkCardConservation, PlayerMove } from './dispatch.ts';

// Deterministic RNG so a failure reproduces from the printed seed.
let seed = Number(process.env.FUZZ_SEED || 0x9e3779b9) >>> 0;
const rnd = () => { seed = (seed * 1664525 + 1013904223) >>> 0; return seed / 0x100000000; };
const ri = (n: number) => Math.floor(rnd() * n);
const pick = <T>(a: T[]): T => a[ri(a.length)];

const KNOWN_EVENT_TYPES = new Set<string>(Object.values(ANIMATION_EVENT_TYPE));

// The broadcast is fire-and-forget (executePackedAction does not await it);
// drain the microtask/immediate queue so broadcastLog is settled.
const settle = () => new Promise((r) => setImmediate(() => setImmediate(r)));

interface Seeded { gameId: string; players: { id: string; name: string; is_ai: boolean; strategy_key: string }[]; roster: ViewRoster }

// Seed + start a real PLAYING game with a committed state blob (start_game
// through the real lock/commit path, exactly like server.test.ts).
async function newPackedGame(humans: number, bots = 0): Promise<Seeded> {
    const gameId = `pk${uuid().slice(0, 6)}`;
    const players = [] as Seeded['players'];
    for (let i = 0; i < humans; i++) players.push({ id: uuid(), name: `H${i}`, is_ai: false, strategy_key: 'human' });
    for (let i = 0; i < bots; i++) players.push({ id: uuid(), name: `B${i}`, is_ai: true, strategy_key: 'random' });
    await seedGame(gameId, players);
    await executeWithGameLock(gameId, async (g) => ({ game: g, events: [], packed: packedProducts(start_game_packed(g)) }), 'start', false);
    const row = (await pgPool.query('SELECT state, status FROM games WHERE id=$1', [gameId])).rows[0];
    assert.equal(row.status, 'playing', 'game started');
    assert.ok(row.state, 'started game has a committed state blob (packed path precondition)');
    const roster: ViewRoster = { id: gameId, name: gameId, players: players.map((p) => ({ player_id: p.id, name: p.name, is_ai: p.is_ai })) };
    return { gameId, players, roster };
}

// pg returns the version column as a string; normalize for arithmetic.
const gamesRow = async (gameId: string) => {
    const r = (await pgPool.query('SELECT state, version, status FROM games WHERE id=$1', [gameId])).rows[0];
    return { state: r.state as string | null, version: Number(r.version ?? 0), status: r.status as string };
};

const wireFor = (pm: PlayerMove) =>
    encodeAction({ kind: pm.move.type as AwireKindName, cards: pm.move.cards, attack_cards: pm.move.attack_cards });

// The fuzz.test.ts-style legacy dispatch (the JSON/JS-Game path).
function applyLegacy(game: Game, pm: PlayerMove): AnimationEvent[] {
    verify_player_in_game(game, pm.playerId);
    switch (pm.move.type) {
        case 'attack': return handleAttack(game, pm.playerId, pm.move.cards!);
        case 'cover': return handleCover(game, pm.playerId, pm.move.cards!, pm.move.attack_cards!);
        case 'pass': return handlePass(game, pm.playerId, pm.move.cards!);
        case 'pickup': return handlePickup(game, pm.playerId);
        case 'good': return handleGood(game, pm.playerId);
        default: throw new Error(`unknown move type ${pm.move.type}`);
    }
}

if (!process.env.VALIDATION_ONLY) {
before(async () => { await applySchema(); });
beforeEach(async () => { await resetDb(); });

test('packed pipeline: legal awire moves apply, bump the version, rewrite the blob, and broadcast decodable {t,s,v,b} payloads', async () => {
    const { gameId, players, roster } = await newPackedGame(3);
    // The move-kind -> the actor's own event type in the broadcast stream.
    const ACTOR_EVENT: Record<string, string> = {
        attack: ANIMATION_EVENT_TYPE.ATTACK_PASS,
        pass: ANIMATION_EVENT_TYPE.ATTACK_PASS,
        cover: ANIMATION_EVENT_TYPE.COVER,
        pickup: ANIMATION_EVENT_TYPE.PICKUP,
    };

    let applied = 0, broadcasted = 0;
    for (let step = 0; step < 14; step++) {
        const g = await loadCompleteGame(gameId);
        if (g.status !== 'playing') break;
        const moves = legalMovesFor(g);
        if (moves.length === 0) break;
        const pm = pick(moves);
        const kind = pm.move.type as AwireKindName;
        const preGood = [...(g.good_players ?? [])];
        const prevGoodTs = g.good_timestamp ?? null;
        const prev = await gamesRow(gameId);
        const logStart = broadcastLog.length;

        const out = await executePackedAction(gameId, pm.playerId, wireFor(pm), `pk${step}`);
        await settle();

        assert.equal(out.status, ACTION_STATUS.APPLIED, `legal ${kind} applies (step ${step})`);
        assert.equal(out.rejectCode, 0, 'applied moves carry no reject code');
        assert.equal(Number(out.version), prev.version + 1, 'committed version increments');
        const row = await gamesRow(gameId);
        assert.equal(row.version, Number(out.version), 'games.version matches the returned version');
        assert.notEqual(row.state, prev.state, `the state blob changed (step ${step} ${kind})`);

        const chk = await checkCardConservation(gameId);
        assert.ok(chk.ok, `card conservation after packed ${kind} at step ${step}: ${chk.detail}`);

        const fresh = broadcastLog.slice(logStart).filter((b) => b.event === 'animation_events');
        if (fresh.length === 0) {
            // A move producing zero events broadcasts nothing — only a plain good.
            assert.equal(kind, 'good', `only a plain good may broadcast nothing (step ${step} ${kind})`);
        } else {
            broadcasted++;
            assert.equal(fresh.length, players.length + 1, 'one payload per human + one spectator payload');
            const chans = new Set(fresh.map((f) => f.channel));
            for (const p of players) assert.ok(chans.has(`gu-${gameId}-${p.id}`), `payload for human ${p.name}`);
            assert.ok(chans.has(`game-${gameId}`), 'spectator payload');
            for (const f of fresh) {
                assert.equal(f.payload.t, 'as2', 'packed payload tag');
                assert.equal(typeof f.payload.s, 'string', 'sequence id');
                assert.equal(f.payload.v, out.version, 'payload.v is the committed version');
                const decoded = decodeEventWire(base64ToBytes(f.payload.b), roster, { preGood, prevGoodTs });
                assert.ok(decoded, 'payload.b decodes as event wire');
                const expectSeat = f.channel === `game-${gameId}` ? -1 : players.findIndex((p) => f.channel === `gu-${gameId}-${p.id}`);
                assert.equal(decoded!.viewerSeat, expectSeat, 'stream is personalized to its channel');
                assert.ok(decoded!.events.length > 0, 'broadcast carries events');
                for (const ev of decoded!.events) {
                    assert.ok(KNOWN_EVENT_TYPES.has(ev.type), `known event type ${ev.type}`);
                    if (ev.player_id !== undefined) {
                        assert.ok(players.some((p) => p.id === ev.player_id), `event player ${ev.player_id} is in the game`);
                    }
                }
                const actorEvent = ACTOR_EVENT[kind];
                if (actorEvent) {
                    assert.ok(decoded!.events.some((ev) => ev.type === actorEvent && ev.player_id === pm.playerId),
                        `a ${kind} broadcasts a ${actorEvent} event by the actor`);
                }
                // The trailer is the committed final state — publicly consistent.
                assert.equal(decoded!.game.status, out.gameStatus, 'decoded final state matches the committed status');
            }
        }
        applied++;
    }
    assert.ok(applied >= 6, `exercised enough packed moves (${applied})`);
    assert.ok(broadcasted >= 3, `enough eventful broadcasts (${broadcasted})`);
});

test('packed pipeline: an illegal move is REJECTED with a reject code — no version bump, no blob write, no broadcast', async () => {
    const { gameId } = await newPackedGame(2);
    const g = await loadCompleteGame(gameId);
    // The defender attacking is always illegal (ENGINE_REJECT_IS_DEFENDER).
    const defender = g.players[g.defender];
    const prev = await gamesRow(gameId);
    const logStart = broadcastLog.length;

    const out = await executePackedAction(gameId, defender.player_id,
        encodeAction({ kind: 'attack', cards: [defender.hand[0]] }), 'rej');
    await settle();

    assert.equal(out.status, ACTION_STATUS.REJECTED, 'illegal move is rejected');
    assert.ok(out.rejectCode > 0, `rejection carries an ENGINE_REJECT_* code (got ${out.rejectCode})`);
    assert.equal(Number(out.version), prev.version, 'returned version is the untouched current version');
    const row = await gamesRow(gameId);
    assert.equal(row.version, prev.version, 'games.version did not bump');
    assert.equal(row.state, prev.state, 'the state blob is untouched');
    assert.equal(broadcastLog.slice(logStart).filter((b) => b.event === 'animation_events').length, 0, 'no broadcast for a rejected move');
    const chk = await checkCardConservation(gameId);
    assert.ok(chk.ok, `conservation after a rejected move: ${chk.detail}`);
});

test('packed pipeline: a move against a finished game is MOOT', async () => {
    const { gameId, players } = await newPackedGame(2);
    const prev = await gamesRow(gameId);
    await pgPool.query(`UPDATE games SET status='game_over' WHERE id=$1`, [gameId]);
    const logStart = broadcastLog.length;

    const out = await executePackedAction(gameId, players[0].id, encodeAction({ kind: 'good' }), 'moot');
    await settle();

    assert.equal(out.status, ACTION_STATUS.MOOT, 'move against a game_over row is a no-op');
    assert.equal(out.rejectCode, 0, 'moot carries no reject code');
    assert.equal(Number(out.version), prev.version, 'version untouched');
    const row = await gamesRow(gameId);
    assert.equal(row.version, prev.version, 'games.version did not bump');
    assert.equal(row.state, prev.state, 'state blob untouched');
    assert.equal(broadcastLog.slice(logStart).filter((b) => b.event === 'animation_events').length, 0, 'no broadcast for a moot move');
});

test('packed pipeline: malformed wire throws cleanly, never commits, never breaks conservation', async () => {
    const { gameId, players } = await newPackedGame(2);
    const actor = players[0].id;
    const prev = await gamesRow(gameId);
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
        await assert.rejects(
            executePackedAction(gameId, actor, wire, 'mal'),
            /malformed action wire/,
            `${label}: rejects with the clean malformed-wire error`);
        const chk = await checkCardConservation(gameId);
        assert.ok(chk.ok, `conservation after malformed wire (${label}): ${chk.detail}`);
    }
    await settle();
    let row = await gamesRow(gameId);
    assert.equal(row.version, prev.version, 'no malformed wire committed anything');
    assert.equal(row.state, prev.state, 'state blob untouched by malformed wire');
    assert.equal(broadcastLog.slice(logStart).filter((b) => b.event === 'animation_events').length, 0, 'malformed wire never broadcasts');

    // Random-bytes fuzz: any buffer must either throw the clean malformed
    // error or come back with a well-defined status. Half the buffers are
    // pure noise (a valid kind byte is a 5/256 accident, so these virtually
    // always throw); half are shaped — valid kind + consistent length but
    // random card bytes — which decode structurally and get rule-rejected.
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
        const who = pick(players).id;
        try {
            const out = await executePackedAction(gameId, who, wire, `rf${i}`);
            assert.ok([ACTION_STATUS.APPLIED, ACTION_STATUS.REJECTED, ACTION_STATUS.MOOT].includes(out.status as 0 | 1 | 2),
                `random wire returned a defined status (got ${out.status})`);
            if (out.status === ACTION_STATUS.APPLIED) {
                assert.equal(Number(out.version), lastVersion + 1, 'random-but-legal wire bumps the version by one');
                lastVersion = Number(out.version);
                appliedRandom++;
            } else {
                assert.equal(Number(out.version), lastVersion, 'non-applied outcomes never move the version');
                rejected++;
            }
        } catch (e) {
            assert.match(String((e as Error).message), /malformed action wire/,
                `random wire may only throw the clean malformed-wire error (iter ${i}, seed ${process.env.FUZZ_SEED || '0x9e3779b9'}): ${e}`);
            threw++;
        }
        const chk = await checkCardConservation(gameId);
        assert.ok(chk.ok, `conservation after random wire iter ${i}: ${chk.detail}`);
    }
    await settle();
    row = await gamesRow(gameId);
    assert.equal(row.version, lastVersion, 'DB version consistent with the outcomes seen');
    assert.ok(threw > 20 && rejected > 20, `fuzz exercised both outcomes (threw=${threw} rejected=${rejected} applied=${appliedRandom})`);
});

test('packed and legacy JS-path moves interleave: same payload shape, strictly increasing versions per channel', async () => {
    const { gameId, players, roster } = await newPackedGame(2);
    const logStart = broadcastLog.length;

    let packedApplied = 0, legacyApplied = 0;
    for (let step = 0; step < 16; step++) {
        const g = await loadCompleteGame(gameId);
        if (g.status !== 'playing') break;
        const moves = legalMovesFor(g);
        if (moves.length === 0) break;
        const pm = pick(moves);
        if (step % 2 === 0) {
            const out = await executePackedAction(gameId, pm.playerId, wireFor(pm), `il${step}`);
            assert.equal(out.status, ACTION_STATUS.APPLIED, `packed leg applies (step ${step} ${pm.move.type})`);
            packedApplied++;
        } else {
            await executeWithGameLock(gameId, async (gg) => ({ game: gg, events: applyLegacy(gg, pm) }), `il${step}`, true);
            legacyApplied++;
        }
        await settle();
        const chk = await checkCardConservation(gameId);
        assert.ok(chk.ok, `conservation after interleaved step ${step}: ${chk.detail}`);
    }
    assert.ok(packedApplied >= 3 && legacyApplied >= 3, `both paths exercised (packed=${packedApplied} legacy=${legacyApplied})`);

    // Every broadcast from EITHER path is the same packed envelope, and each
    // channel's versions are strictly increasing across the mixed paths.
    const evts = broadcastLog.slice(logStart).filter((b) => b.event === 'animation_events');
    assert.ok(evts.length > 0, 'interleaved moves broadcast');
    const perChannel = new Map<string, number[]>();
    for (const e of evts) {
        assert.equal(e.payload.t, 'as2', 'payload tag');
        assert.equal(typeof e.payload.s, 'string', 'sequence id');
        assert.equal(typeof e.payload.v, 'number', 'numeric version');
        assert.equal(typeof e.payload.b, 'string', 'base64 event wire');
        assert.ok(!('events' in e.payload) && !('game' in e.payload) && !('version' in e.payload),
            'no legacy JSON fields ride along');
        assert.ok(decodeEventWire(base64ToBytes(e.payload.b), roster, { preGood: [], prevGoodTs: null }),
            'payload decodes regardless of the emitting path');
        if (!perChannel.has(e.channel)) perChannel.set(e.channel, []);
        perChannel.get(e.channel)!.push(e.payload.v);
    }
    assert.equal(perChannel.size, players.length + 1, 'per-human channels + the spectator channel');
    for (const [chan, vs] of perChannel) {
        for (let i = 1; i < vs.length; i++) {
            assert.ok(vs[i] > vs[i - 1], `versions not strictly increasing on ${chan}: ${vs.join(',')}`);
        }
    }
});

after(async () => { await pgPool.end(); });
}
