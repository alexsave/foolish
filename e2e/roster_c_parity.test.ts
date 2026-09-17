/* =============================================================================
 * The C Roster writes the envelope trailer byte for byte like the TS encoder
 * =============================================================================
 * c/src/roster.c takes over from sdk/ts/wire/roster.ts (docs/
 * C_GAME_SHAPE_MIGRATION.md Phase 2). Until the server switches (Phase 4b) the
 * TS encoder is the oracle, because shipped iOS builds and the web decode what
 * it writes. So for generated rosters - ASCII, Cyrillic, CJK, emoji names that
 * straddle the 64-byte budget, 0 to 8 seats, bots and humans, every good mask -
 * this asserts:
 *
 *   1. C's trailer bytes EQUAL encodePackedRoster's bytes (the C writer puts
 *      good ids in seat order and has_ts 0, so the TS side is given that).
 *   2. C reads a TS-written trailer - including one with a timestamp and good
 *      ids in insertion order naming a player who left - to the same roster.
 *   3. The durable encoding round-trips, and malformed input is refused.
 *
 * It talks to bots.wasm's test-only wasm_roster_* exports through
 * e2e/helpers/roster_kernel.ts (a private instance). Pure test - no Postgres.
 * ========================================================================== */

import { test } from 'node:test';
import assert from 'node:assert/strict';

import { encodePackedRoster } from '../sdk/ts/wire/roster.ts';
import {
    ROSTER_BYTES, ROSTER_E_ID, ROSTER_E_FULL, ROSTER_E_PADDING, ROSTER_STATUS_NAMES as STATUS,
    RosterSeatSpec as Seat, RosterTable as Table,
    cRosterEncode as cEncode, cRosterDecodeReencode, cRosterTrailer as cTrailer,
    cRosterTrailerRead as cTrailerRead, tsRosterFor as tsRoster,
} from './helpers/roster_kernel.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; }

const enc = new TextEncoder();

// ---------------------------------------------------------------------------
// Generated tables. A fixed-seed PRNG, so a failure names a reproducible case.
// ---------------------------------------------------------------------------

function rng(seed: number) {
    let s = seed >>> 0;
    return (n: number) => { s = (Math.imul(s, 1664525) + 1013904223) >>> 0; return s % n; };
}

const NAME_POOL = [
    'Sveta', 'Misha', '', 'a"b\\c', 'line\nbreak', 'Владимир', 'Пётр', 'さくら', '🤡', 'A👍🏽B', 'Ünïcodé',
    '🤡'.repeat(20),                  // 80 B
    'Владимир'.repeat(9),             // 144 B
    'A' + '👍🏽'.repeat(8),           // 65 B: the cut falls inside a cluster
    'さくら'.repeat(8),                // 72 B
    'x' + '🇺🇦'.repeat(9),            // 73 B
    'q'.repeat(64), 'q'.repeat(65),
];
// Names whose last scalar straddles byte 64 by every offset a 2, 3 and 4 byte
// scalar can.
for (let pad = 58; pad <= 64; pad++) {
    NAME_POOL.push('x'.repeat(pad) + 'Ж' + 'y', 'x'.repeat(pad) + 'ら', 'x'.repeat(pad) + '🤡🤡');
}
const TITLE_POOL = ["Sveta's Game", '', 'Игра Володи', '🎴'.repeat(50), 'T'.repeat(50), 'quote " backslash \\'];
const BRAINS = ['random', 'simple_heuristic', 'handwritten', 'firecracker', 'blackpowder', 'cordite', 'octogen'];

function generate(count: number, seed: number): Table[] {
    const r = rng(seed);
    const out: Table[] = [];
    for (let k = 0; k < count; k++) {
        const n = k % 9;                               // 0..8 seats, all of them
        const seats: Seat[] = [];
        for (let i = 0; i < n; i++) {
            const uuid = r(2) === 0;
            seats.push({
                id: uuid ? `${k.toString(16).padStart(8, '0')}-0000-4000-8000-${i.toString().padStart(12, '0')}` : `p-${k}-${i}`,
                name: NAME_POOL[r(NAME_POOL.length)],
                brain: r(3) === 0 ? BRAINS[r(BRAINS.length)] : '',
            });
        }
        out.push({
            gid: `game-${k}-${r(1 << 20).toString(36)}`,
            title: TITLE_POOL[r(TITLE_POOL.length)],
            status: r(3),
            goodMask: n === 0 ? 0 : r(1 << n),
            seats,
        });
    }
    return out;
}

const TABLES = generate(400, 0x5eed);

test('the generator covers what it claims to', () => {
    const counts = new Set(TABLES.map(t => t.seats.length));
    assert.deepEqual([...counts].sort(), [0, 1, 2, 3, 4, 5, 6, 7, 8]);
    const names = TABLES.flatMap(t => t.seats.map(s => enc.encode(s.name).length));
    assert.ok(names.some(n => n > 64), 'no over-budget name was generated');
    assert.ok(names.some(n => n === 64), 'no exactly-64-byte name was generated');
    assert.ok(TABLES.some(t => t.goodMask !== 0 && t.goodMask !== (1 << t.seats.length) - 1), 'no partial good mask');
    assert.ok(TABLES.some(t => t.seats.some(s => s.brain !== '')), 'no bot seat');
});

test('the C trailer is encodePackedRoster\'s bytes, for every generated table', () => {
    for (const [i, t] of TABLES.entries()) {
        const c = cTrailer(t);
        assert.ok(c instanceof Uint8Array, `table ${i}: C refused it with ${c}`);
        assert.deepEqual(Buffer.from(c).toString('hex'), Buffer.from(encodePackedRoster(tsRoster(t))).toString('hex'),
                         `table ${i} (${t.seats.length} seats) differs`);
    }
});

test('C reads a TS-written trailer to the same roster', () => {
    for (const [i, t] of TABLES.entries()) {
        const roster = tsRoster(t);
        // What an older TS server wrote: good ids in insertion order, one of them
        // for a player who has since left, and a timestamp.
        if (i % 2 === 1 && t.seats.length > 0) {
            roster.good_players = [...roster.good_players].reverse().concat('00000000-0000-4000-8000-00000000dead');
            roster.good_timestamp = 1723456789012.5;
        }
        const trailer = encodePackedRoster(roster);
        const tail = new Uint8Array(trailer.length + 3);
        tail.set(trailer, 0);
        tail.set([0xde, 0xad, 0xbe], trailer.length);   // whatever follows is not the trailer's
        const got = cTrailerRead(tail);
        assert.ok(typeof got !== 'number', `table ${i}: C refused a TS trailer with ${got}`);
        assert.equal(got.consumed, trailer.length, `table ${i}: C stopped at a different offset`);
        assert.equal(got.gid, t.gid);
        assert.equal(STATUS[got.status], roster.status);
        const aiMask = t.seats.reduce((m, s, k) => m | (s.brain !== '' ? 1 << k : 0), 0);
        assert.equal(got.aiMask, aiMask, `table ${i}: the AI seats differ`);
        // The trailer carries is_ai, not a brain: the same seats with no brain.
        const want = cEncode(t.title, t.seats.map(s => ({ ...s, brain: '' })));
        assert.ok(want instanceof Uint8Array);
        assert.deepEqual(Buffer.from(got.durable).toString('hex'), Buffer.from(want).toString('hex'),
                         `table ${i}: C read different ids, names or title`);
    }
});

test('the durable roster round-trips and is refused when it is not canonical', () => {
    for (const [i, t] of TABLES.entries()) {
        const durable = cEncode(t.title, t.seats);
        assert.ok(durable instanceof Uint8Array && durable.length === ROSTER_BYTES, `table ${i}: ${durable}`);
        assert.deepEqual(cRosterDecodeReencode(durable), durable, `table ${i}: decode -> encode is not the identity`);
        // The last byte is always a reserved one: any non-zero value is refused.
        const bad = Uint8Array.from(durable);
        bad[ROSTER_BYTES - 1] = 1;
        assert.equal(cRosterDecodeReencode(bad), ROSTER_E_PADDING, `table ${i}: a dirty reserved byte loaded`);
    }
});

test('C refuses what the TS encoder would have let through', () => {
    assert.equal(cEncode('t', [{ id: 'x'.repeat(37), name: 'n', brain: '' }]), ROSTER_E_ID,
                 'a 37-byte id is not a roster id');
    const nine = Array.from({ length: 9 }, (_, i) => ({ id: `p-${i}`, name: 'n', brain: '' }));
    assert.equal(cEncode('t', nine), ROSTER_E_FULL, 'a ninth seat was seated');
    const trailer = encodePackedRoster(tsRoster(TABLES[13]));
    for (let cut = 0; cut < trailer.length; cut++) {
        const got = cTrailerRead(trailer.subarray(0, cut));
        assert.ok(typeof got === 'number' && got < 0, `a ${cut}-byte prefix of a trailer was read as a roster`);
    }
});
