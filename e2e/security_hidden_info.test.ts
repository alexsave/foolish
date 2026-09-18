// SECURITY S1: hidden information never leaves the server for the wrong viewer.
//
// Real games - 2, 4 and 6 seats, humans and bots interleaved - are created,
// joined, dealt and played through the REAL edge entry points (action / meta /
// create index.ts handlers, signed tokens, the real bot loop), and after every
// step every payload a viewer can receive is collected:
//
//   - the HTTP response to the request (create / meta view envelope, awire reply)
//   - every realtime broadcast on the viewer's own `gu-{game}-{user}` topic and
//     on the public `game-{game}` topic
//   - the viewer's player_views row, read as that user under RLS
//   - the spectator_views row, read as an authenticated user under RLS
//   - the games row, read AS anon (refused outright: since the contract
//     migration no client role holds any privilege on games)
//
// Viewers: every seated human, a signed-in stranger (spectator), and anon.
//
// Ground truth is the kernel's UNMASKED state (games.state and games.roster,
// loaded into the fixtures' C Table and read through the generated accessors,
// e2e/helpers/table_play.ts) plus the set of cards that have ever been public
// (every card named by a committed non-DRAW session log, archived by a test-only
// trigger so the end-of-game log wipe cannot hide one).
//
// Each payload is checked three independent ways, because a decoder only proves
// what the decoder reads - the client's reader keeps no hand but the viewer's
// and no deck byte, so a leak sitting there is invisible to it:
//
//   1. DECODE. The client's reader (sdk/ts/table/client_table.ts) must show
//      the viewer exactly their own hand, the public board, and nothing else.
//
//   2. KERNEL MASKED RE-ENCODE. Every board inside a payload (a view blob, each
//      per-event snapshot, the stream's trailer) is fed to the kernel's MASKED
//      importer (wasm_import_state(masked=1), a separate bots.wasm instance) and
//      re-serialized for the same viewer (wasm_view_serialize). A masked board
//      is a fixed point of that round trip; a real card in a hidden slot comes
//      back as 0xFE, a stray trailing byte is not re-emitted, a hidden
//      awaiting-attack flag comes back 0. Every envelope must also be byte for
//      byte the envelope the kernel writes for that viewer from the stored row
//      at the envelope's version (table_envelope), and every realtime push (the
//      kernel's as3 payload: the event sequence, a flags byte, the roster
//      trailer when the roster changed) is walked to its last byte, so no byte
//      goes unaccounted for. Event card bytes must be 0xFE or a card the viewer
//      may know; another seat's DEAL/REFILL may name only the face-up trump.
//
//   3. NONINTERFERENCE (the raw scan). A card-byte value scan would false-
//      positive (a count, a seat or a status is also a byte 0..51), so instead:
//      before a step, the cards the viewer cannot see are rotated among their
//      slots (other seats' hands and the deck; the viewer's own hand, the cards
//      the viewer will draw, the acting seats' hands, the table and the trump
//      are pinned so the step is the same step), the rotated state (sealed by the
//      kernel, table_seal) is committed in place of the real one, the IDENTICAL
//      request is replayed, and every
//      payload that viewer receives must be byte-identical to the real run's.
//      Any byte - in any field, of any format, decoded by anything or nothing -
//      that depends on a hidden card differs between the two worlds. The only
//      bytes excluded are the ones that are random or wall-clock by
//      construction: the broadcast dedup id `s` and games.updated_at.
//
// Replays after a game ends reveal every hand on purpose (game_snapshots); that
// is not a live payload and is not checked here.

import './harness.ts';
import { test, before, after } from 'node:test';
import assert from 'node:assert/strict';
import { applySchema, uuid, pgPool, broadcastLog } from './harness.ts';
import { settle, tokenFor, postJson, postPacked, EdgeResponse } from './helpers/edge.ts';
import { lockedBotLoop } from '../server/impls/supabase/functions/_shared/adapter/bot_actions.ts';
import { __clearGameCache } from '../server/impls/supabase/functions/_shared/adapter/game_cache.ts';
import { __setTableDealSeedOverride } from '../server/impls/supabase/functions/_shared/adapter/table_io.ts';
import { botsTestWasm } from './helpers/bots_test_wasm.ts';
import { encodeActionRequest, wireCard } from '../sdk/ts/wire/awire.ts';
import { readEnvelopeView, readPushSequence } from './helpers/client_read.ts';
import { cRosterTrailerRead } from './helpers/roster_kernel.ts';
import { clientTable } from '../sdk/ts/table/client_table.ts';
import { base64ToBytes } from '../sdk/ts/wire/bytes.ts';
import * as L from '../sdk/ts/gen/game_layout.bots.ts';
import { fixtureTable } from './helpers/table_fixture.ts';
import { legalMoves, readTable, rebuild, type BoardState, type PlayCard } from './helpers/table_play.ts';
import { suiteRng } from './helpers/rng.ts';
import { isCard } from '../src/state/view.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; console.error = () => {}; }

const rng = suiteRng('security_hidden_info');
const HIDDEN = 0xfe;
const NONE = 0xff;
const id = (c: PlayCard) => wireCard(c);
const hex = (b: Uint8Array) => Buffer.from(b).toString('hex');
const hexToBytes = (h: string) => Uint8Array.from(Buffer.from(h.replace(/^\\x/, ''), 'hex'));

// ---- the kernel's masked importer, in its own instance ------------------------
// A separate instance of the test build of bots.wasm (every shipped export plus the
// engine probes this needs, same C and layout hash), so probing a payload never disturbs the game the
// server's instance holds resident.
interface ProbeExports {
    memory: WebAssembly.Memory;
    wasm_init(): void; wasm_io_ptr(): number;
    wasm_import_state(masked: number): number;
    wasm_view_serialize(viewer: number): number;
}
const probe = new WebAssembly.Instance(new WebAssembly.Module(botsTestWasm() as BufferSource), {}).exports as unknown as ProbeExports;
probe.wasm_init();

/** The board, imported MASKED and re-serialized for the same viewer. */
function maskedReencode(board: Uint8Array, viewer: number): Uint8Array {
    const io = probe.wasm_io_ptr();
    new Uint8Array(probe.memory.buffer).set(board, io);
    const r = probe.wasm_import_state(1);
    assert.ok(r >= 0, `the kernel's masked importer refused the board (reason ${r})`);
    const len = probe.wasm_view_serialize(viewer);
    return new Uint8Array(probe.memory.buffer).slice(io + 2, io + len);
}

function assertMaskedFixedPoint(board: Uint8Array, viewer: number, what: string) {
    const again = maskedReencode(board, viewer);
    assert.equal(hex(board), hex(again), `${what}: the board is not a masked fixed point for viewer ${viewer} - it carries bytes masking removes`);
}

// ---- viewers ------------------------------------------------------------------

interface Human { id: string; name: string; tok: string }
interface Table {
    gameId: string;
    humans: Human[];
    bots: string[];
    spectator: Human;
    trumpId: number | null;
    everPublic: Set<number>;
    counts: Record<string, number>;
}

// A viewer is a seated human (seat >= 0), the signed-in spectator (seat -1,
// `spectator`), or anon (seat -1, whose read of games is refused).
type Viewer = { kind: 'human'; h: Human } | { kind: 'spectator'; h: Human } | { kind: 'anon' };
const viewerName = (v: Viewer) => v.kind === 'anon' ? 'anon' : `${v.kind}:${v.h.name}`;

// ---- DB-side reads, as the client roles ---------------------------------------

async function asRole<T>(role: 'anon' | 'authenticated', sub: string | null, fn: (c: import('pg').PoolClient) => Promise<T>): Promise<T> {
    const c = await pgPool.connect();
    try {
        await c.query('BEGIN');
        await c.query(`SET LOCAL ROLE ${role}`);
        await c.query(`SELECT set_config('request.jwt.claims', $1, true)`,
            [JSON.stringify(sub ? { sub, role } : { role })]);
        return await fn(c);
    } finally {
        try { await c.query('ROLLBACK'); } catch { /* */ }
        c.release();
    }
}

/** The games row as anon reads it: 'denied' when Postgres refuses the read (42501), else what it returned. */
async function anonGamesRow(gameId: string): Promise<'denied' | Record<string, unknown> | null> {
    return asRole('anon', null, async (c) => {
        try {
            return (await c.query('SELECT * FROM games WHERE id=$1', [gameId])).rows[0] ?? null;
        } catch (e) {
            if ((e as { code?: string }).code === '42501') return 'denied';
            throw e;
        }
    });
}
const ownViewRow = (gameId: string, userId: string) => asRole('authenticated', userId, async (c) =>
    (await c.query('SELECT view, version, status FROM player_views WHERE game_id=$1', [gameId])).rows);
const spectatorRow = (gameId: string, userId: string) => asRole('authenticated', userId, async (c) =>
    (await c.query('SELECT view, version, status FROM spectator_views WHERE game_id=$1', [gameId])).rows);

// ---- ground truth -----------------------------------------------------------------

interface Truth {
    /** The status column. */
    status: string;
    /** The dealt board, read by the kernel; null for a lobby (or no row). */
    board: BoardState | null;
    /** The stored row as the kernel reads it (lobby or dealt); null when the row is gone. */
    row: BoardState | null;
    seatOf: Map<string, number>;
    version: number;
}

async function truthOf(t: Table): Promise<Truth> {
    const row = await readTable(t.gameId);
    if (!row) return { status: 'deleted', board: null, row: null, seatOf: new Map(), version: 0 };
    const seatOf = new Map(row.seats.map((s, i) => [s.id, i] as const));
    const dealt = row.status !== L.GAME_STATUS_WAITING;
    return { status: row.statusColumn, board: dealt ? row : null, row, seatOf, version: row.version };
}

// The session log (games.logs_packed), walked record by record like the pushes
// below: [u48 ms][u8 type][u8 seat][u8 defender][u8 n_pairs][n_pairs x (primary,
// target)] wire card bytes. Walked here, not decoded by a reader under test: the
// set of cards that have been public is this suite's ground truth. The record
// types are c/src/game.h's LOG_* numbers, from the generated module.
const { LOG_PICKUP, LOG_DISCARD, LOG_DRAW } = L;
function sessionLogRecords(log: Uint8Array, what: string): { type: number; cards: number[] }[] {
    const out: { type: number; cards: number[] }[] = [];
    let q = 0;
    while (q < log.length) {
        assert.ok(q + 10 <= log.length, `${what}: a session log record is truncated`);
        const type = log[q + 6], nPairs = log[q + 9];
        assert.ok(q + 10 + 2 * nPairs <= log.length, `${what}: a session log record's pairs are truncated`);
        out.push({ type, cards: [...log.subarray(q + 10, q + 10 + 2 * nPairs)] });
        q += 10 + 2 * nPairs;
    }
    return out;
}

async function sessionLogs(gameId: string): Promise<Uint8Array[]> {
    const { rows } = await pgPool.query(
        `SELECT logs FROM e2e_log_archive WHERE game_id=$1
         UNION ALL SELECT logs_packed FROM games WHERE id=$1 AND logs_packed IS NOT NULL`, [gameId]);
    return rows.filter((r) => r.logs).map((r) => hexToBytes(r.logs));
}

// Every card a committed session log has named face up, except DRAW records
// (a draw is a private event; its only public card is the trump, added apart).
async function refreshEverPublic(t: Table): Promise<void> {
    for (const log of await sessionLogs(t.gameId)) {
        for (const r of sessionLogRecords(log, `${t.gameId} session log`)) {
            if (r.type === LOG_DRAW) continue;
            for (const c of r.cards) if (c !== HIDDEN && c !== NONE) t.everPublic.add(c);
        }
    }
}

// ---- step capture -------------------------------------------------------------------

interface Capture {
    requester: string | null;
    response: EdgeResponse | null;
    broadcasts: { channel: string; payload: Record<string, unknown> }[];
}

// Everything `v` can receive from one step (and the standing rows after it), as
// canonical strings, for the noninterference comparison.
async function receivedBy(t: Table, v: Viewer, cap: Capture): Promise<Record<string, string>> {
    const out: Record<string, string> = {};
    if (v.kind !== 'anon') {
        if (cap.requester === v.h.id && cap.response) {
            out.response = `${cap.response.status}:${hex(cap.response.bytes)}`;
        }
        const topics = new Set([`game-${t.gameId}`, ...(v.kind === 'human' ? [`gu-${t.gameId}-${v.h.id}`] : [])]);
        out.broadcasts = JSON.stringify(cap.broadcasts.filter(b => topics.has(b.channel))
            .map(b => ({ channel: b.channel, payload: { ...b.payload, s: undefined } })));
        if (v.kind === 'human') out.ownView = JSON.stringify(await ownViewRow(t.gameId, v.h.id));
        out.spectatorView = JSON.stringify(await spectatorRow(t.gameId, v.h.id));
    }
    out.anonGames = JSON.stringify(await anonGamesRow(t.gameId));
    return out;
}

// ---- the three checks for one viewer --------------------------------------------------

const cardsOf = (b: BoardState | null, seat: number) => (b && seat >= 0 ? b.seats[seat].hand.map(id) : []);

function checkViewEnvelope(t: Table, bytes: Uint8Array, v: Viewer, seat: number, pre: Truth, post: Truth, what: string) {
    t.counts.views++;
    const d = readEnvelopeView(bytes);
    assert.ok(d, `${what}: the view envelope decodes`);
    assert.equal(d!.mySeat, seat, `${what}: the envelope is ${viewerName(v)}'s own (seat ${seat})`);

    // 2. byte accounting: [9-byte header][u16 view_len][view blob][packed roster], nothing else.
    const viewLen = bytes[9] | (bytes[10] << 8);
    const blob = bytes.subarray(11, 11 + viewLen);
    const roster = cRosterTrailerRead(bytes.subarray(11 + viewLen));
    assert.ok(typeof roster !== 'number', `${what}: the roster trailer decodes (${roster})`);
    assert.equal(11 + viewLen + roster.consumed, bytes.length, `${what}: no bytes after the roster trailer`);
    assert.equal(blob[1], seat < 0 ? 0xff : seat, `${what}: the view blob is written for seat ${seat}`);
    assertMaskedFixedPoint(blob.subarray(2), seat, what);
    // Every byte is the kernel's own envelope for this viewer, from the stored row,
    // at the version the envelope carries (the rows and responses are the final
    // commit's: a step's post state).
    if (post.row && d!.version === post.version) {
        const table = fixtureTable();
        assert.equal(table.load(post.row.state, post.row.roster), L.TABLE_OK, `${what}: the stored row loads`);
        const want = table.envelope(t.gameId, seat, d!.version);
        assert.ok(want instanceof Uint8Array, `${what}: table_envelope`);
        assert.equal(hex(bytes), hex(want), `${what}: the envelope is byte for byte the kernel's for ${viewerName(v)}`);
        t.counts.exactEnvelopes++;
    }

    // 1. decode: exactly the viewer's own hand, the public board, nothing more.
    const g = d!;
    const truth = post.board;
    if (seat < 0) assert.deepEqual(g.myHand, [], `${what}: a spectator view holds no hand`);
    if (!truth) {
        assert.equal(g.deckCount, 0, `${what}: a lobby has no deck`);
        assert.equal(g.battles.length, 0, `${what}: a lobby has no table`);
        assert.equal(g.myHand.length, 0, `${what}: a lobby hand is empty`);
        return;
    }
    if (seat >= 0) {
        assert.deepEqual(g.myHand.map(id), cardsOf(truth, seat), `${what}: own hand is the real hand`);
        t.counts.ownCards += g.myHand.length;
    }
    g.seats.forEach((p, i) => assert.equal(p.handCount, truth.seats[i].hand.length, `${what}: seat ${i} hand count`));
    assert.equal(g.deckCount, truth.deck.length, `${what}: deck count`);
    assert.deepEqual(g.hasFlipped ? g.flipped : null, truth.trump, `${what}: face-up trump`);
    assert.deepEqual(g.battles.map((b) => ({ attack: b.attack, defense: isCard(b.defense) ? b.defense : null })), truth.battles, `${what}: table`);
}

// Walk an evwire stream to its last byte, handing back each board and each
// event's raw card bytes.
function walkEvwire(b: Uint8Array, what: string) {
    assert.ok(b.length >= 4 && b[0] === 1, `${what}: an evwire v1 stream`);
    const viewer = b[1];
    const n = b[3];
    let q = 4;
    const events: { cards: number[]; target: number | null; board: Uint8Array }[] = [];
    const take = (k: number) => { assert.ok(q + k <= b.length, `${what}: stream truncated`); const s = b.subarray(q, q + k); q += k; return s; };
    for (let i = 0; i < n; i++) {
        const [, , , , , flags, nCards] = take(7);
        const cards = [...take(nCards)];
        const target = flags & 1 ? take(1)[0] : null;
        if (flags & 2) take(1);
        assert.equal(flags & ~3, 0, `${what}: no unknown event flag bits`);
        const lenB = take(2);
        events.push({ cards, target, board: take(lenB[0] | (lenB[1] << 8)) });
    }
    const lenB = take(2);
    const final = take(lenB[0] | (lenB[1] << 8));
    // as3: one flags byte, then the roster trailer when the operation changed the roster.
    const [flags] = take(1);
    assert.equal(flags & ~1, 0, `${what}: no unknown as3 flag bits`);
    let roster: string[] | null = null;
    if (flags & 1) {
        const r = cRosterTrailerRead(b.subarray(q));
        assert.ok(typeof r !== 'number', `${what}: the announced roster trailer decodes (${r})`);
        q += r.consumed;
        // The seat ids it announces, as the client reads them from the push itself.
        roster = clientTable().readPush(b, { as3: true, identity: 'none' })!.final.seats.map((s) => s.id);
    }
    assert.equal(q, b.length, `${what}: the payload ends where its last block ends`);
    return { viewer, events, final, roster };
}

function checkEventPayload(t: Table, payload: Record<string, unknown>, v: Viewer, seat: number,
                           pre: Truth, post: Truth, roster: { id: string; name: string; players: { player_id: string; name: string; is_ai: boolean }[] }, what: string) {
    t.counts.streams++;
    // The JSON envelope carries the kernel's bytes and nothing else (plan Q7: no r / m extras).
    assert.deepEqual(Object.keys(payload).sort(), ['b', 's', 't', 'v'], `${what}: envelope keys`);
    // Phase 5b: labelled for what it is, so the client reads the roster block from the push itself.
    assert.equal(payload.t, 'as3', `${what}: an as3 push`);

    const b = base64ToBytes(payload.b as string);
    const walked = walkEvwire(b, what);
    assert.equal(walked.viewer, seat < 0 ? NONE : seat, `${what}: the stream is written for seat ${seat}`);
    for (const [i, e] of walked.events.entries()) assertMaskedFixedPoint(e.board, seat, `${what} event ${i} board`);
    assertMaskedFixedPoint(walked.final, seat, `${what} trailer board`);

    // Which cards may this viewer see named?
    const may = new Set<number>([...t.everPublic, ...cardsOf(pre.board, seat), ...cardsOf(post.board, seat)]);
    if (walked.roster) {
        // A roster block announces seats and names, never a card or a hand.
        assert.deepEqual(walked.roster, roster.players.map((p) => p.player_id), `${what}: the announced roster is the table's`);
        t.counts.rosterBlocks++;
    }
    if (t.trumpId !== null) may.add(t.trumpId);
    const decoded = readPushSequence(b, roster);
    assert.ok(decoded, `${what}: the client decodes the stream`);
    assert.equal(decoded!.events.length, walked.events.length, `${what}: decoder and walk agree on the event count`);
    for (const [i, e] of walked.events.entries()) {
        const de = decoded!.events[i];
        const named = [...e.cards, ...(e.target === null ? [] : [e.target])].filter(c => c !== HIDDEN);
        for (const c of named) {
            assert.ok(c <= 51, `${what} event ${i}: a card byte that is no card (${c})`);
            assert.ok(may.has(c), `${what} event ${i} (${de.type}): names card ${c}, which ${viewerName(v)} may not know`);
            t.counts.namedCards++;
        }
        const drawSeat = de.seat ?? -1;
        if ((de.type === 'deal' || de.type === 'refill') && drawSeat !== seat) {
            for (const c of e.cards) {
                assert.ok(c === HIDDEN || c === t.trumpId, `${what} event ${i}: seat ${drawSeat}'s ${de.type} names card ${c} to ${viewerName(v)}`);
            }
            t.counts.otherDraws++;
        }
    }
}

function checkAnonRow(t: Table, row: 'denied' | Record<string, unknown> | null, what: string) {
    t.counts.anonRows++;
    // Not a filtered row, not an empty one: the read itself is refused, so no
    // column of the unmasked board or the roster can ever reach anon.
    assert.equal(row, 'denied', `${what}: anon's read of games is refused (got ${JSON.stringify(row)})`);
}

async function checkAllViewers(t: Table, cap: Capture, pre: Truth, post: Truth, label: string) {
    if (!post.row) return;
    const roster = {
        id: t.gameId, name: post.row.title,
        players: post.row.seats.map((s) => ({ player_id: s.id, name: s.name, is_ai: s.brain !== '' })),
    };
    const viewers: Viewer[] = [...t.humans.map(h => ({ kind: 'human', h }) as Viewer), { kind: 'spectator', h: t.spectator }, { kind: 'anon' }];
    for (const v of viewers) {
        const seat = v.kind === 'human' ? (post.seatOf.get(v.h.id) ?? -1) : -1;
        const what = `${label} -> ${viewerName(v)}`;
        if (v.kind !== 'anon') {
            if (cap.requester === v.h.id && cap.response && cap.response.status === 200
                && cap.response.type.includes('octet-stream') && cap.response.bytes.length > 7) {
                checkViewEnvelope(t, cap.response.bytes, v, seat, pre, post, `${what} response`);
            }
            const topics = new Set([`game-${t.gameId}`, ...(v.kind === 'human' ? [`gu-${t.gameId}-${v.h.id}`] : [])]);
            for (const b of cap.broadcasts.filter(x => topics.has(x.channel))) {
                checkEventPayload(t, b.payload, v, b.channel.startsWith('game-') ? -1 : seat, pre, post, roster, `${what} ${b.channel}`);
            }
            if (v.kind === 'human') {
                for (const r of await ownViewRow(t.gameId, v.h.id)) checkViewEnvelope(t, hexToBytes(r.view), v, seat, pre, post, `${what} player_views`);
            }
            for (const r of await spectatorRow(t.gameId, v.h.id)) checkViewEnvelope(t, hexToBytes(r.view), v, -1, pre, post, `${what} spectator_views`);
        } else {
            checkAnonRow(t, await anonGamesRow(t.gameId), what);
        }
    }
}

// ---- world snapshots, for the noninterference replays ---------------------------------

let jsonbGamesColumns = new Set<string>();

async function snapshotRows(gameId: string) {
    return {
        games: (await pgPool.query('SELECT * FROM games WHERE id=$1', [gameId])).rows[0],
        pv: (await pgPool.query('SELECT * FROM player_views WHERE game_id=$1', [gameId])).rows,
        sv: (await pgPool.query('SELECT * FROM spectator_views WHERE game_id=$1', [gameId])).rows,
    };
}
type Rows = Awaited<ReturnType<typeof snapshotRows>>;

async function restoreRows(gameId: string, snap: Rows, stateOverride?: string) {
    const cols = Object.keys(snap.games).filter(c => c !== 'id');
    const vals = cols.map(c => {
        const v = c === 'state' && stateOverride !== undefined ? stateOverride : snap.games[c];
        return jsonbGamesColumns.has(c) && v !== null ? JSON.stringify(v) : v;
    });
    await pgPool.query(`UPDATE games SET ${cols.map((c, i) => `${c}=$${i + 1}`).join(', ')} WHERE id=$${cols.length + 1}`, [...vals, gameId]);
    for (const [table, rows] of [['player_views', snap.pv], ['spectator_views', snap.sv]] as const) {
        await pgPool.query(`DELETE FROM ${table} WHERE game_id=$1`, [gameId]);
        for (const r of rows) {
            const keys = Object.keys(r);
            await pgPool.query(`INSERT INTO ${table} (${keys.join(',')}) VALUES (${keys.map((_, i) => `$${i + 1}`).join(',')})`, keys.map(k => r[k]));
        }
    }
    __clearGameCache();
}

// The hidden cards of `truth` for a viewer, rotated one slot along. Pinned: the
// hands of `pinSeats` (the viewer and whoever acts), the cards in `pinCards`
// (what the viewer draws this step), and the whole deck if `pinDeck` (a bot
// cycle may draw and then act on what it drew). Table and trump are never in a
// hand or the deck, so they cannot move.
function rotateHidden(truth: BoardState, pinSeats: Set<number>, pinCards: Set<number>, pinDeck: boolean): { board: BoardState; moved: number } {
    const board: BoardState = structuredClone(truth);
    const slots: { get: () => PlayCard; set: (c: PlayCard) => void }[] = [];
    board.seats.forEach((p, s) => {
        if (pinSeats.has(s)) return;
        p.hand.forEach((c, j) => { if (!pinCards.has(id(c))) slots.push({ get: () => p.hand[j], set: (x) => { p.hand[j] = x; } }); });
    });
    if (!pinDeck) {
        board.deck.forEach((c, j) => { if (!pinCards.has(id(c))) slots.push({ get: () => board.deck[j], set: (x) => { board.deck[j] = x; } }); });
    }
    const cards = slots.map(s => s.get());
    if (cards.length < 2) return { board, moved: 0 };
    slots.forEach((s, i) => s.set(cards[(i + 1) % cards.length]));
    return { board, moved: cards.length };
}

// ---- steps ----------------------------------------------------------------------

interface Step {
    label: string;
    requester: Human | null;
    run: () => Promise<EdgeResponse | null>;
    // seats whose whole hand is fixed across the twin worlds, and whether the deck is
    actors: (post: Truth) => number[];
    pinDeck: boolean;
}

async function runCaptured(step: Step): Promise<Capture> {
    const from = broadcastLog.length;
    const response = await step.run();
    await settle();
    return {
        requester: step.requester?.id ?? null,
        response,
        broadcasts: broadcastLog.slice(from).map(b => ({ channel: b.channel, payload: b.payload })),
    };
}

async function holdBotLease(gameId: string) {
    await pgPool.query('UPDATE games SET bot_lease_token = gen_random_uuid(), bot_lease_until = now() + interval \'1 hour\' WHERE id=$1', [gameId]);
}
async function dropBotLease(gameId: string) {
    await pgPool.query('UPDATE games SET bot_lease_until = now() - interval \'1 second\' WHERE id=$1', [gameId]);
}

async function doStep(t: Table, step: Step, noninterference: boolean): Promise<Truth> {
    const pre = await truthOf(t);
    const s0 = await snapshotRows(t.gameId);
    const capA = await runCaptured(step);
    const post = await truthOf(t);
    await refreshEverPublic(t);
    if (post.board?.trump && t.trumpId === null) t.trumpId = id(post.board.trump);
    await checkAllViewers(t, capA, pre, post, `${t.gameId} ${step.label}`);

    if (!noninterference || !pre.board || post.status !== 'playing') return post;

    // Replay the step in a world where every card this viewer cannot see sits in
    // a different slot; everything the viewer receives must be byte-identical.
    const s1 = await snapshotRows(t.gameId);
    const viewers: Viewer[] = [...t.humans.map(h => ({ kind: 'human', h }) as Viewer), { kind: 'spectator', h: t.spectator }];
    const actors = step.actors(post);
    const real = new Map<Viewer, Record<string, string>>();
    for (const v of viewers) real.set(v, await receivedBy(t, v, capA));
    for (const v of viewers) {
        const seat = v.kind === 'human' ? (pre.seatOf.get(v.h.id) ?? -1) : -1;
        const pinSeats = new Set<number>([...actors, ...(seat >= 0 ? [seat] : [])]);
        const drawn = new Set<number>(seat >= 0 ? cardsOf(post.board, seat).filter(c => !cardsOf(pre.board, seat).includes(c)) : []);
        // A fresh copy of the PRE state to rotate.
        await restoreRows(t.gameId, s0);
        const fresh = (await truthOf(t)).board!;
        // The twin is sealed by the kernel (table_seal); unrotated it must be the real blob.
        const realHex = String(s0.games.state).replace(/^\\x/, '');
        assert.equal(hex(rebuild(fresh).build().state), realHex, 'the twin builder is faithful');
        const twin = rotateHidden(fresh, pinSeats, drawn, step.pinDeck);
        if (twin.moved < 2) continue;
        const twinBlob = rebuild(twin.board).build().state;
        assert.notEqual(hex(twinBlob), realHex, 'the twin world really differs');
        await restoreRows(t.gameId, s0, `\\x${hex(twinBlob)}`);
        const capB = await runCaptured(step);
        assert.deepEqual(await receivedBy(t, v, capB), real.get(v),
            `${t.gameId} ${step.label}: what ${viewerName(v)} receives depends on ${twin.moved} cards it cannot see (noninterference)`);
        t.counts.twins++;
        t.counts.rotated += twin.moved;
    }
    await restoreRows(t.gameId, s1);
    return post;
}

// ---- the game ----------------------------------------------------------------------

async function seatBots(n: number): Promise<string[]> {
    const ids = Array.from({ length: n }, () => uuid());
    for (const [i, b] of ids.entries()) {
        await pgPool.query('INSERT INTO bots(id,nickname,strategy_key) VALUES($1,$2,$3)', [b, `Bot${i}-${b.slice(0, 4)}`, 'random']);
    }
    return ids;
}

async function human(name: string): Promise<Human> {
    const hid = uuid();
    await pgPool.query('INSERT INTO auth.users(id) VALUES($1) ON CONFLICT DO NOTHING', [hid]);
    return { id: hid, name, tok: await tokenFor(hid, name) };
}

async function playTable(nHumans: number, nBots: number, dealSeed: number): Promise<Table> {
    const humans: Human[] = [];
    for (let i = 0; i < nHumans; i++) humans.push(await human(`h${i}x${nHumans + nBots}`));
    const bots = await seatBots(nBots);
    const t: Table = {
        gameId: '', humans, bots, spectator: await human(`spec${nHumans + nBots}`), trumpId: null, everPublic: new Set(),
        counts: { views: 0, exactEnvelopes: 0, streams: 0, rosterBlocks: 0, anonRows: 0, ownCards: 0, namedCards: 0, otherDraws: 0, twins: 0, rotated: 0, humanSteps: 0, botSteps: 0 },
    };

    // create - the response is the creator's first payload
    const noActors = () => [];
    const created = await runCaptured({ label: 'create', requester: humans[0], run: () => postJson('create', humans[0].tok, {}), actors: noActors, pinDeck: false });
    const d = readEnvelopeView(created.response!.bytes);
    assert.ok(d, 'create answered with a view');
    t.gameId = d!.gameId;
    await holdBotLease(t.gameId); // bots act only in explicit bot steps
    const lobbyTruth = await truthOf(t);
    await checkAllViewers(t, created, lobbyTruth, lobbyTruth, `${t.gameId} create`);

    const meta = (who: Human, body: object, label: string): Step =>
        ({ label, requester: who, run: () => postJson('meta', who.tok, { ...body, game_id: t.gameId }), actors: noActors, pinDeck: false });
    for (const h of humans.slice(1)) await doStep(t, meta(h, { type: 'join' }, `join ${h.name}`), false);
    for (const b of bots) await doStep(t, meta(humans[0], { type: 'add-bot', bot_id: b }, 'add-bot'), false);
    // interleave humans and bots around the table
    const order: string[] = [];
    for (let i = 0; i < Math.max(nHumans, nBots); i++) { if (humans[i]) order.push(humans[i].id); if (bots[i]) order.push(bots[i]); }
    await doStep(t, meta(humans[0], { type: 'rearrange-players', new_order: order }, 'rearrange-players'), false);

    __setTableDealSeedOverride(Uint8Array.from({ length: 32 }, (_, i) => (dealSeed * 131 + i * 17) & 0xff));
    for (const h of humans) await doStep(t, meta(h, { type: 'start' }, `start ${h.name}`), false);
    let truth = await truthOf(t);
    assert.equal(truth.status, 'playing', `${t.gameId}: the table dealt`);

    const isBot = new Set(bots);
    for (let n = 0; n < 600 && truth.status === 'playing'; n++) {
        const g = truth.board!;
        const humanMoves = legalMoves(g, s => !isBot.has(s.id));
        const botMoves = legalMoves(g, s => isBot.has(s.id));
        const humanFirst = humanMoves.length > 0 && (botMoves.length === 0 || rng.next() < 0.6);
        if (humanFirst) {
            // Prefer real moves; `good` only now and then, or when it is all there is.
            const real = humanMoves.filter(m => m.kind !== 'good');
            const pool = real.length > 0 && rng.next() < 0.85 ? real : humanMoves;
            const pm = rng.pick(pool);
            const who = humans.find(h => h.id === pm.playerId)!;
            const version = truth.version;
            t.counts.humanSteps++;
            truth = await doStep(t, {
                label: `#${n} ${who.name} ${pm.kind}`, requester: who,
                run: () => postPacked(who.tok, encodeActionRequest(t.gameId, pm.wire, version)),
                actors: () => [pm.seat], pinDeck: false,
            }, true);
        } else if (botMoves.length > 0) {
            await dropBotLease(t.gameId);
            const botSeats = g.seats.map((p, i) => (p.brain ? i : -1)).filter(i => i >= 0);
            t.counts.botSteps++;
            truth = await doStep(t, {
                label: `#${n} bot cycle`, requester: null,
                run: async () => { await dropBotLease(t.gameId); await lockedBotLoop(t.gameId); return null; },
                actors: () => botSeats, pinDeck: true,
            }, true);
            await holdBotLease(t.gameId);
        } else {
            break;
        }
    }
    return t;
}

function assertExercised(t: Table, label: string) {
    const kinds = new Set<number>();
    return (async () => {
        for (const log of await sessionLogs(t.gameId)) for (const r of sessionLogRecords(log, label)) kinds.add(r.type);
        const c = t.counts;
        const { rows: st } = await pgPool.query('SELECT status FROM games WHERE id=$1', [t.gameId]);
        assert.equal(st[0].status, 'game_over', `${label}: played to the end, so the final payloads were checked too`);
        assert.ok(kinds.has(LOG_PICKUP), `${label}: the game included a pickup (${[...kinds]})`);
        assert.ok(kinds.has(LOG_DISCARD), `${label}: the game included a discard (${[...kinds]})`);
        assert.ok(kinds.has(LOG_DRAW), `${label}: the game included draws`);
        assert.ok(c.humanSteps >= 10, `${label}: humans acted (${c.humanSteps})`);
        assert.ok(c.ownCards > 50, `${label}: own hands were really compared (${c.ownCards} cards)`);
        assert.ok(c.otherDraws > 0, `${label}: another seat's draws reached a viewer (${c.otherDraws})`);
        assert.ok(c.twins >= c.humanSteps, `${label}: noninterference replays ran (${c.twins})`);
        assert.ok(c.rotated > c.twins * 2, `${label}: and moved real cards (${c.rotated})`);
        assert.ok(c.exactEnvelopes > c.views / 2, `${label}: most envelopes were held to the kernel's bytes (${c.exactEnvelopes} of ${c.views})`);
        assert.ok(c.rosterBlocks > 0, `${label}: as3 roster blocks were walked (${c.rosterBlocks})`);
    })();
}

// =============================================================================

before(async () => {
    await applySchema();
    // Faithful stand-ins for Supabase's own auth.uid()/auth.role()/realtime.topic(),
    // so RLS and the realtime channel policies are evaluated for real.
    await pgPool.query(`
        CREATE OR REPLACE FUNCTION auth.uid() RETURNS uuid LANGUAGE sql STABLE AS $$
          SELECT NULLIF(current_setting('request.jwt.claims', true)::jsonb->>'sub','')::uuid $$;
        CREATE OR REPLACE FUNCTION auth.role() RETURNS text LANGUAGE sql STABLE AS $$
          SELECT COALESCE(NULLIF(current_setting('request.jwt.claims', true)::jsonb->>'role',''), 'service_role') $$;
        CREATE OR REPLACE FUNCTION realtime.topic() RETURNS text LANGUAGE sql STABLE AS $$
          SELECT NULLIF(current_setting('realtime.topic', true), '') $$;
        GRANT USAGE ON SCHEMA realtime TO anon, authenticated;
        GRANT SELECT ON realtime.messages TO anon, authenticated;
        -- Supabase's default privileges grant client roles table access in public;
        -- RLS is what narrows it. The gu- channel policy reads membership here.
        GRANT SELECT ON public.player_hands TO authenticated;
        -- Test-only: keep every session log the game ever committed, so the set of
        -- cards that have been public survives the end-of-game log wipe.
        CREATE TABLE e2e_log_archive (game_id TEXT, logs BYTEA);
        CREATE FUNCTION e2e_archive_logs() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN
          IF COALESCE(OLD.logs_packed, '') <> '' AND NEW.logs_packed IS DISTINCT FROM OLD.logs_packed THEN
            INSERT INTO e2e_log_archive VALUES (OLD.id, OLD.logs_packed);
          END IF; RETURN NEW; END $$;
        CREATE TRIGGER e2e_archive_logs BEFORE UPDATE ON games FOR EACH ROW EXECUTE FUNCTION e2e_archive_logs();
    `);
    const anonColumns = (await pgPool.query(
        `SELECT column_name FROM information_schema.column_privileges
         WHERE table_schema='public' AND table_name='games' AND grantee IN ('anon', 'PUBLIC') AND privilege_type='SELECT'`)).rows.map(r => r.column_name);
    assert.deepEqual(anonColumns, [], 'anon holds no column of games (docs/C_GAME_SHAPE_MIGRATION.md 3.3)');
    jsonbGamesColumns = new Set((await pgPool.query(
        `SELECT column_name FROM information_schema.columns WHERE table_schema='public' AND table_name='games' AND data_type='jsonb'`)).rows.map(r => r.column_name));
});
after(async () => { await settle(); });

test('2 seats (two humans): every payload each viewer receives hides what it must', async (tc) => {
    const t = await playTable(2, 0, 2);
    tc.diagnostic(`${t.gameId} ${JSON.stringify(t.counts)}`);
    await assertExercised(t, '2p');
});

test('4 seats (two humans, two bots): every payload each viewer receives hides what it must', async (tc) => {
    const t = await playTable(2, 2, 4);
    tc.diagnostic(`${t.gameId} ${JSON.stringify(t.counts)}`);
    await assertExercised(t, '4p');
});

test('6 seats (three humans, three bots): every payload each viewer receives hides what it must', async (tc) => {
    const t = await playTable(3, 3, 6);
    tc.diagnostic(`${t.gameId} ${JSON.stringify(t.counts)}`);
    await assertExercised(t, '6p');
});

// Channel authorization, as Realtime evaluates it: a private channel join is a
// SELECT on realtime.messages with realtime.topic() set to the channel.
async function channelFixture() {
    // A lobby is enough: channel authorization reads membership, not cards.
    const a = await human('rt-a'), b = await human('rt-b'), spectator = await human('rt-spec');
    const created = await postJson('create', a.tok, {});
    await settle();
    const gameId = readEnvelopeView(created.bytes)!.gameId;
    assert.equal((await postJson('meta', b.tok, { type: 'join', game_id: gameId })).status, 200, 'b joins');
    await settle();
    await pgPool.query(`INSERT INTO realtime.messages(topic, extension) SELECT 'x', 'broadcast' WHERE NOT EXISTS (SELECT 1 FROM realtime.messages)`);
    const canJoin = (role: 'anon' | 'authenticated', sub: string | null, topic: string) => asRole(role, sub, async (c) => {
        await c.query(`SELECT set_config('realtime.topic', $1, true)`, [topic]);
        return (await c.query(`SELECT count(*)::int AS n FROM realtime.messages WHERE extension='broadcast'`)).rows[0].n > 0;
    });
    return { a, b, spectator, gameId, canJoin };
}

test('realtime: nobody but its owner can join a gu- topic; anon cannot join game-; a signed-in spectator can', async () => {
    const { a, b, spectator, gameId, canJoin } = await channelFixture();
    const guA = `gu-${gameId}-${a.id}`;
    assert.equal(await canJoin('authenticated', b.id, guA), false, 'another seated player cannot join A\'s gu- topic');
    assert.equal(await canJoin('authenticated', spectator.id, guA), false, 'a spectator cannot join A\'s gu- topic');
    assert.equal(await canJoin('anon', null, guA), false, 'anon cannot join A\'s gu- topic');
    assert.equal(await canJoin('anon', null, `game-${gameId}`), false, 'anon cannot join the game- topic');
    assert.equal(await canJoin('authenticated', spectator.id, `game-${gameId}`), true, 'a signed-in spectator can join the game- topic');
});

// The owner half of the matrix above. seed.sql's gu- policy used to compare
// split_part(topic, '-', 3) with auth.uid()::text; a user id is a hyphenated
// UUID, so that field is only its first 8 hex digits and every gu- join was
// refused, the owner's included (fail-closed, not a leak). The policy now
// rebuilds the exact topic from the player_hands row
// (seed.sql's realtime policies; migration 20260917120000 carried the same
// change to hosted).
test('realtime: a seated player can join their OWN gu- topic', async () => {
    const { a, b, gameId, canJoin } = await channelFixture();
    assert.equal(await canJoin('authenticated', a.id, `gu-${gameId}-${a.id}`), true, 'A can join A\'s own gu- topic');
    assert.equal(await canJoin('authenticated', b.id, `gu-${gameId}-${b.id}`), true, 'B, who joined the lobby, can join B\'s own gu- topic');
});
