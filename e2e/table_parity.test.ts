/* =============================================================================
 * The C Table writes what the TS pipeline wrote (the cutover gate, frozen)
 * =============================================================================
 * docs/C_GAME_SHAPE_MIGRATION.md Phase 3 built this gate and Phase 4b froze it.
 *
 * Phase 3: before any server path moved onto c/src/table.c, every product the
 * move and lobby paths commit and broadcast had to come out of the C Table byte
 * for byte as it came out of the TS pipeline (runPackedAction ->
 * materializeKernelGame -> logsFromKernelExport -> buildPlayerViewRows /
 * buildSpectatorView -> the as2 event buffers; handleMetaAction on a JS Game),
 * on the same games: every scenario of the pre-migration fixture
 * (e2e/fixtures/pre_table) played on to its end with legal and hostile requests,
 * fresh deals driven by the fuzz generator of e2e/fuzz.test.ts, a scripted lobby
 * from create through every lobby edit, a deal, a played game, continue and an
 * empty table, a bot-completed deal, and every fixture lobby joined, readied and
 * dealt. It passed: 2,242 driven requests (1,336 applied, 652 rejected, 151 not
 * seated, 95 malformed wires, 8 moot, 6 games to their end) and 51 lobby edits
 * (31 applied, 18 refused, 1 moot ready, 1 emptied table, 6 deals), every
 * envelope and push byte-equal, with the decided divergences (Q1 trailer goods,
 * Q7 lobby push prose and the as3 roster block, Q9 policy refusals, the moot
 * ready-after-deal, the lobby state blob) asserted explicitly.
 *
 * Phase 4b deletes that TS pipeline. Before it did, the gate ran once more with
 * a recorder (e2e/helpers/table_golden.ts): every request it drove, every
 * outcome, and a SHA-256 digest of every C product - each asserted equal to the
 * TS product in that same passing run - went into
 * e2e/fixtures/table_parity/golden.json. This file replays those requests
 * through the C Table and holds every outcome, response body and digest. A
 * change to what the table writes for any of those requests is a red here, and a
 * deliberate one needs a plan decision and a re-record (there is nothing left to
 * record against: re-recording means accepting the new bytes as the reference).
 *
 * The TS action request and response codecs (sdk/ts/wire/awire.ts, which the
 * clients still use) and updateEloRatings' arithmetic (calculateEloChange) are
 * still live code, so those two comparisons still run against TS.
 * ========================================================================== */

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { calculateEloChange } from '../server/api/common/common_utils.ts';
import { encodeActionRequest, encodeActionResponse, decodeActionRequest, ACTION_STATUS, REJECT_STALE_ROUND } from '../sdk/ts/wire/awire.ts';
import { createServerTable } from '../sdk/ts/table/server_table.ts';
import {
    GAME_INVALID_LOBBY_CARDS, GAME_STATUS_GAME_OVER, GAME_STATUS_WAITING,
    TABLE_APPLIED, TABLE_E_NOT_SEATED, TABLE_E_WIRE, TABLE_MOOT, TABLE_OK, TABLE_REJECTED, TABLE_STALE_ROUND,
} from '../sdk/ts/gen/game_layout.bots.ts';
import { fuzzRng } from './helpers/fuzz_moves.ts';
import { bytesOf, hexOf as hex, productsDigest, type Golden, type GoldenDrive, type GoldenLobby, type GoldenLobbyEdit, type GoldenMove } from './helpers/table_golden.ts';

const GOLDEN: Golden = JSON.parse(readFileSync(join(process.cwd(), 'e2e', 'fixtures', 'table_parity', 'golden.json'), 'utf8'));
const NOW = GOLDEN.now;
const DEAL_SEED = bytesOf(GOLDEN.dealSeed);
const table = createServerTable();

const counts = { steps: 0, applied: 0, rejected: 0, moot: 0, notSeated: 0, wire: 0, digests: 0, games: 0, ended: 0 };
const lobbyCounts = { edits: 0, applied: 0, refused: 0, deleted: 0, moot: 0, digests: 0 };

function outcomeOf(rc: number): { outcome: GoldenMove['outcome']; reason?: number } {
    switch (rc) {
        case TABLE_APPLIED: return { outcome: 'applied' };
        case TABLE_MOOT: return { outcome: 'moot' };
        case TABLE_E_NOT_SEATED: return { outcome: 'not_seated' };
        case TABLE_E_WIRE: return { outcome: 'wire' };
        case TABLE_REJECTED: return { outcome: 'rejected', reason: table.reject() };
        default: throw new Error(`table_act returned ${rc}`);
    }
}

interface Chain { state: Uint8Array; roster: Uint8Array; version: number }

function replayDrive(d: GoldenDrive, from?: Chain): Chain {
    counts.games++;
    let c: Chain = { state: bytesOf(d.state), roster: bytesOf(d.roster), version: d.version };
    if (from) {
        assert.equal(hex(from.state), d.state, `${d.label}: the drive starts from the lobby's dealt state`);
        assert.equal(from.version, d.version, `${d.label}: at the lobby's version`);
    }
    d.moves.forEach((m, i) => {
        const label = `${d.label} move ${i}`;
        counts.steps++;
        assert.equal(table.load(c.state, c.roster), TABLE_OK, `${label}: the chain loads`);
        const rc = table.act(m.actor, bytesOf(m.wire), null, 0);
        const got = outcomeOf(rc);
        assert.deepEqual(got, m.reason === undefined ? { outcome: m.outcome } : { outcome: m.outcome, reason: m.reason },
            `${label}: outcome (wire ${m.wire}, actor ${m.actor})`);
        const next = m.outcome === 'applied' ? c.version + 1 : c.version;
        if (m.response !== undefined) {
            assert.equal(hex(table.actionResponse(rc, table.reject(), next)), m.response, `${label}: response body`);
        }
        switch (m.outcome) {
            case 'moot': counts.moot++; return;
            case 'not_seated': counts.notSeated++; return;
            case 'wire': counts.wire++; return;
            case 'rejected': counts.rejected++; return;
        }
        counts.applied++;
        assert.equal(m.version, next, `${label}: the version it committed`);
        const p = table.commit(d.gameId, next, NOW);
        assert.ok(typeof p !== 'number', `${label}: commit products (${p})`);
        assert.equal(productsDigest(table, d.gameId, p, m.ended === true), m.digest, `${label}: every product (state, roster, logs, scalars, envelopes, pushes${m.ended ? ', finish order' : ''})`);
        counts.digests++;
        if (m.ended) counts.ended++;
        c = { state: p.state, roster: p.roster, version: next };
    });
    return c;
}

// One lobby edit through the C Table, as the meta dispatcher calls it.
function cMeta(e: GoldenLobbyEdit): number {
    const body = e.body;
    const s = (k: string) => String(body[k] ?? '');
    switch (body.type) {
        case 'join': return table.join(e.actor, e.actorName);
        case 'start': return table.ready(e.actor, DEAL_SEED);
        case 'add-bot': {
            const b = GOLDEN.botRows.find((r) => r.id === s('bot_id'))!;
            return table.addBot(e.actor, b.id, b.nickname, b.strategy_key, DEAL_SEED);
        }
        case 'exit': return body.bot_id ? table.removeBot(e.actor, s('bot_id')) : table.leave(e.actor, body.player_id ? s('player_id') : e.actor);
        case 'continue': return table.continueGame(e.actor);
        case 'rearrange-players': return table.reseat(e.actor, body.new_order as string[]);
        case 'update-name': return table.retitle(e.actor, s('new_name'));
        case 'rearrange-hand': return table.rearrangeHand(e.actor, body.card_indices as number[]);
        default: throw new Error(`no C edit for ${String(body.type)}`);
    }
}

function replayEdits(l: GoldenLobby, edits: GoldenLobbyEdit[], c: Chain): Chain {
    for (const e of edits) {
        const label = `${l.label}: ${e.label}`;
        lobbyCounts.edits++;
        assert.equal(table.load(c.state, c.roster), TABLE_OK, `${label}: the chain loads`);
        const rc = cMeta(e);
        assert.equal(rc, e.rc, `${label}: result (detail ${table.detail()})`);
        if (e.digest === undefined) {
            if (rc < 0) lobbyCounts.refused++;
            else if (rc === TABLE_MOOT) lobbyCounts.moot++;
            else lobbyCounts.deleted++;
            continue;
        }
        lobbyCounts.applied++;
        const p = table.commit(l.gameId, e.version!, NOW);
        assert.ok(typeof p !== 'number', `${label}: commit products (${p})`);
        assert.equal(productsDigest(table, l.gameId, p, false), e.digest, `${label}: every product`);
        lobbyCounts.digests++;
        c = { state: p.state, roster: p.roster, version: e.version! };
    }
    return c;
}

function replayLobby(l: GoldenLobby): void {
    if (l.create) {
        assert.equal(table.create(l.create.actor, l.create.name), TABLE_OK, `${l.label}: create`);
        const p0 = table.commit(l.gameId, 0, NOW);
        assert.ok(typeof p0 !== 'number');
        assert.equal(productsDigest(table, l.gameId, p0, false), l.create.digest, `${l.label}: create's products`);
        assert.equal(hex(p0.state), l.state, `${l.label}: the created lobby blob`);
        assert.equal(hex(p0.roster), l.roster, `${l.label}: the created roster`);
    }
    let c: Chain = { state: bytesOf(l.state), roster: bytesOf(l.roster), version: l.version };
    c = replayEdits(l, l.edits, c);
    if (l.drive) c = replayDrive(l.drive, c);
    if (l.after) replayEdits(l, l.after, c);
}

test('the recorded gate is the one that passed (its request counts)', () => {
    const moves = [...GOLDEN.fixtureDrives, ...GOLDEN.fuzzDrives].flatMap((d) => d.moves);
    assert.equal(moves.length, 2242, 'driven requests on the fixture games and the fuzz deals');
    const by = (o: string) => moves.filter((m) => m.outcome === o).length;
    assert.deepEqual([by('applied'), by('rejected'), by('not_seated'), by('wire'), by('moot')], [1336, 652, 151, 95, 8]);
    assert.equal(GOLDEN.lobbies.flatMap((l) => (l.drive ? l.drive.moves : [])).length, 291, 'and on the dealt scripted lobby');
    assert.equal(GOLDEN.lobbies.flatMap((l) => [...l.edits, ...(l.after ?? [])]).length, 51, 'lobby edits');
    assert.equal(GOLDEN.rows.length, 8, 'the fixture has its eight scenarios');
});

test('every pre-migration row: the C Table loads it and writes the recorded products', () => {
    let dealt = 0, lobbies = 0;
    for (const row of GOLDEN.rows) {
        const state = bytesOf(row.state), roster = bytesOf(row.roster);
        if (row.status === 'waiting') {
            lobbies++;
            if (row.staleState) {
                // The damaged shape: the blob of a finished session under a WAITING
                // column. The blob is authoritative (Q6) and says the game is over,
                // which is why the expand migration rewrites every WAITING row's
                // state; relabelled WAITING, the same board is refused as a lobby
                // with cards.
                const stale = bytesOf(row.staleState);
                assert.equal(table.load(stale, roster), TABLE_OK, `${row.id}: the finished blob itself loads`);
                const sp = table.commit(row.id, row.version, NOW);
                assert.ok(typeof sp !== 'number' && sp.status === GAME_STATUS_GAME_OVER, `${row.id}: as the finished game it is`);
                const relabelled = stale.slice();
                relabelled[2] = GAME_STATUS_WAITING;
                assert.equal(table.load(relabelled, roster), GAME_INVALID_LOBBY_CARDS, `${row.id}: relabelled WAITING it is refused`);
            }
        } else {
            dealt++;
        }
        assert.equal(table.load(state, roster), TABLE_OK, `${row.id}: loads`);
        const p = table.commit(row.id, row.version, NOW);
        assert.ok(typeof p !== 'number', `${row.id}: commit products (${p})`);
        assert.equal(hex(p.state), row.state, `${row.id}: the state blob round-trips`);
        assert.equal(hex(p.roster), row.roster, `${row.id}: the roster blob round-trips`);
        assert.equal(p.logs, null, `${row.id}: a load writes no records`);
        assert.equal(productsDigest(table, row.id, p, false), row.digest, `${row.id}: every product`);
    }
    assert.ok(dealt === 4 && lobbies === 4, `four dealt rows and four lobbies (${dealt}, ${lobbies})`);
});

test('the dealt fixture games and the fuzz deals, replayed request by request', () => {
    for (const d of GOLDEN.fixtureDrives) replayDrive(d);
    for (const d of GOLDEN.fuzzDrives) replayDrive(d);
    assert.ok(counts.ended >= 5, `the running games reached their end (${counts.ended})`);
});

test('lobby edits: create through a deal, a played game, continue and an empty table; a bot deal; the fixture lobbies', () => {
    for (const l of GOLDEN.lobbies) replayLobby(l);
    console.error(`[table_parity] ${JSON.stringify(counts)} lobby ${JSON.stringify(lobbyCounts)}`);
    assert.deepEqual([lobbyCounts.edits, lobbyCounts.applied, lobbyCounts.refused, lobbyCounts.moot, lobbyCounts.deleted], [51, 31, 18, 1, 1]);
});

test('the action request and response codecs are the TS ones, byte for byte', () => {
    const wires = [Uint8Array.of(3, 0), Uint8Array.of(0, 1, 7), Uint8Array.of(1, 2, 8, 9, 10, 11)];
    for (const gid of ['a', 'd79ae3', 'x'.repeat(64)]) {
        for (const wire of wires) {
            for (const intent of [undefined, 0, 5, 0x7fffffff, 0xffffffff]) {
                const body = encodeActionRequest(gid, wire, intent);
                const c = table.requestDecode(body);
                const t = decodeActionRequest(body)!;
                assert.ok(typeof c !== 'number', `${gid}/${hex(wire)}/${intent}: decodes`);
                assert.deepEqual({ gameId: c.gameId, wire: hex(c.wire), intent: c.intent },
                    { gameId: t.gameId, wire: hex(t.wire), intent: t.intentVersion ?? null }, `${gid}/${hex(wire)}/${intent}`);
            }
        }
    }
    for (const bad of [Uint8Array.of(), Uint8Array.of(1), Uint8Array.of(2, 1, 0x67, 1, 0, 0), Uint8Array.of(9, 0, 3, 0)]) {
        assert.equal(decodeActionRequest(bad), null, `TS refuses ${hex(bad)}`);
        assert.equal(typeof table.requestDecode(bad), 'number', `C refuses ${hex(bad)}`);
    }
    for (const version of [0, 1, 256, 0x01020304, 0x7fffffff]) {
        assert.equal(hex(table.actionResponse(TABLE_STALE_ROUND, 0, version)),
            hex(encodeActionResponse(ACTION_STATUS.REJECTED, REJECT_STALE_ROUND, version)), `stale round at ${version}`);
    }
});

test('elo_deltas is updateEloRatings\' arithmetic for every seat count and a sweep of ratings', () => {
    const r = fuzzRng(0xe10);
    let cases = 0;
    for (let n = 2; n <= 8; n++) {
        for (let k = 0; k < 400; k++) {
            const ratings = Array.from({ length: n }, () => (r.rnd() < 0.2 ? 1000 : r.ri(3000)));
            const order = Array.from({ length: n }, (_, i) => i).sort(() => r.rnd() - 0.5);
            // The retired TS updateEloRatings: for each place, the sum of the 1v1 changes against every other place.
            const want = new Array<number>(n).fill(0);
            for (let i = 0; i < n; i++) {
                let total = 0;
                for (let j = 0; j < n; j++) {
                    if (i === j) continue;
                    total += calculateEloChange(ratings[order[i]], ratings[order[j]], i < j ? 1 : 0);
                }
                want[order[i]] = total;
            }
            assert.deepEqual(table.eloDeltas(ratings, order), want.map((v) => v + 0), `n=${n} ratings=${ratings} order=${order}`);
            cases++;
        }
    }
    assert.equal(cases, 7 * 400);
});
