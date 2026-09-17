/* =============================================================================
 * The envelope's roster is packed, written by the kernel and read by every client
 * =============================================================================
 * The client-server envelope carried its roster as a JSON island: the last JSON
 * on any path that matters. It is bytes now, a trailer announced by a flags bit,
 * and since Phase 4b the C Roster writes it (roster_trailer_write, reached
 * through table_envelope; c/src/roster.h).
 *
 * THE CONSTRAINT THIS FILE EXISTS FOR. Merging a PR in this repo deploys the
 * server IMMEDIATELY. The iOS client does not deploy that way - it ships through
 * the App Store - and the same envelope is also STORED (player_views.view,
 * spectator_views.view), so there is no request to negotiate a format on. What
 * the kernel writes must read in the Swift decoder that is installed, exactly.
 *
 * So the file holds the kernel's bytes three ways: the envelope's shape (no
 * island, a mandatory trailer), the web's own reader (the client slot,
 * sdk/ts/table/client_table.ts) round-tripping every field a client reads, and
 * the REAL Swift decoder compiled from the tree (the pattern
 * imessage_replay_names.test.ts set - compile the production source, not a
 * copy), including the non-ASCII cases a byte-length codec goes wrong on. The
 * TypeScript roster encoder these used to exercise is retired; the C writer is
 * the one definition now.
 *
 * Pure test - needs no Postgres. The Swift half skips where there is no
 * toolchain (Linux CI); it is a Mac-side guard on a Mac-side file.
 * ========================================================================== */

import { test, after } from 'node:test';
import assert from 'node:assert/strict';
import { execFileSync, spawnSync } from 'node:child_process';
import { mkdtempSync, writeFileSync, rmSync, existsSync } from 'node:fs';
import { join } from 'node:path';
import { tmpdir } from 'node:os';

import * as L from '../sdk/ts/gen/game_layout.bots.ts';
import { readEnvelopeView } from './helpers/client_read.ts';
import { fixture, fixtureTable, PLAYING } from './helpers/table_fixture.ts';
import { RosterTable, cRosterTrailer } from './helpers/roster_kernel.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; }

const REPO = new URL('..', import.meta.url).pathname;
const utf8len = (s: string) => new TextEncoder().encode(s).length;

// The shipped envelope, as c/src/table.h table_envelope documents it and the
// installed iOS decoder reads it: u8 fmt, u8 flags (bit0 seated viewer, bit1
// roster trailer), u8 seat, u32 version, u16 roster island length (0), u16
// view_len, the view blob, the roster trailer. Frozen here on purpose: this
// file is the gate on that layout.
const ENVELOPE_FORMAT = 1;
const FLAG_ROSTER_TRAILER = 0x02;
const VIEW_AT = 11;
const viewLenOf = (env: Uint8Array) => env[9] | (env[10] << 8);
/** The longest name a seat keeps, in UTF-8 bytes (c/src/roster.h ROSTER_NAME_MAX). */
const ROSTER_NAME_MAX = 64;

/** The kernel's envelope for `viewer` of a table the fixtures seal. */
function kernelEnvelope(gid: string, fx: { state: Uint8Array; roster: Uint8Array }, viewer: number, version: number): Uint8Array {
    const table = fixtureTable();
    assert.equal(table.load(fx.state, fx.roster), L.TABLE_OK, 'the fixture loads');
    const env = table.envelope(gid, viewer, version);
    assert.ok(env instanceof Uint8Array, `table_envelope (${env})`);
    return env;
}

// A roster trailer read the way the web client reads one: as the trailer of a
// spectator envelope of a lobby with that many seats, through the kernel's
// client slot. What it reads is the table's identity - its id, title, status
// and seats; the goods live in the board (plan Q1). Null when the trailer does
// not read whole.
function readTrailer(trailer: Uint8Array, seats: number) {
    const lobby = fixture().title('host').seats(Array.from({ length: seats }, (_, i) => ({ id: `s${i}`, name: `s${i}` }))).build();
    const host = kernelEnvelope('host', lobby, -1, 1);
    const head = VIEW_AT + viewLenOf(host);
    const env = new Uint8Array(head + trailer.length);
    env.set(host.subarray(0, head));
    env.set(trailer, head);
    const v = readEnvelopeView(env);
    return v && {
        id: v.gameId, name: v.title, status: v.status,
        players: v.seats.map((p) => ({ player_id: p.id, name: p.name, is_ai: p.isAi })),
    };
}

const trailerOf = (t: RosterTable): Uint8Array => {
    const c = cRosterTrailer(t);
    assert.ok(c instanceof Uint8Array, `${t.gid}: C refused the table with ${c}`);
    return c;
};

// ===========================================================================
// 1. The envelope: no island, and the trailer is the roster
// ===========================================================================

const SEATS3 = [
    { id: '00000000-0000-4000-8000-000000000000', name: 'Sveta' },
    { id: '00000000-0000-4000-8000-000000000001', name: 'Misha' },
    { id: '00000000-0000-4000-8000-000000000002', name: 'Пётр', brain: 'random' },
];
const dealt3 = () => fixture().title("Sveta's Game").seats(SEATS3).status(PLAYING)
    .hand(0, '6h 7h').hand(1, '8h 9h').hand(2, 'Th Jh').trump('Ks').deck('Qs').good(1).build();

test('the envelope carries no roster island, and the trailer is the roster', () => {
    const out = kernelEnvelope('game-abc123', dealt3(), 1, 5);

    assert.equal(out[0], ENVELOPE_FORMAT);
    // roster_len is the u16 at bytes 7-8, and it is zero: nothing rides there.
    assert.equal(out[7] | (out[8] << 8), 0, 'the roster island is still being written');
    assert.ok((out[1] & FLAG_ROSTER_TRAILER) !== 0, 'the trailer flag must be set');

    const dec = readEnvelopeView(out);
    assert.ok(dec, 'the envelope must decode');
    assert.deepEqual(dec.seats.map((p) => p.name), ['Sveta', 'Misha', 'Пётр'], 'the names came from the packed trailer');
    assert.deepEqual(dec.seats.map((p) => p.id), SEATS3.map((s) => s.id));
    assert.equal(dec.status, L.GAME_STATUS_PLAYING);
    assert.equal(dec.gameId, 'game-abc123');
    assert.equal(dec.title, "Sveta's Game");
    assert.equal(dec.goodMask, 1 << 1, "Misha's good is on the board");
});

// A stored player_views row written before the trailer existed carries neither
// island nor trailer. It must fail as unreadable rather than decode to a game
// with an empty roster - a table with no names is worse than a load error,
// because the caller cannot tell it went wrong. The next commit rewrites the row.
test('an envelope with no packed trailer is unreadable, not an empty table', () => {
    const full = kernelEnvelope('game-abc123', dealt3(), 0, 5);
    assert.ok(readEnvelopeView(full), 'the control reads');
    const legacy = full.subarray(0, VIEW_AT + viewLenOf(full)).slice();
    legacy[1] &= ~FLAG_ROSTER_TRAILER;
    assert.equal(readEnvelopeView(legacy), null, 'a trailer-less envelope must not decode');
});

// ===========================================================================
// 2. The trailer, through the web's reader
// ===========================================================================

const TABLES: RosterTable[] = [
    { gid: 'game-1', title: "Sveta's Game", status: 0, goodMask: 0, seats: [{ id: 'p-0', name: 'Sveta', brain: '' }] },
    {
        gid: 'game-2', title: 'Игра Володи', status: 1, goodMask: 0b10,
        seats: [
            { id: '00000000-0000-4000-8000-000000000000', name: 'Владимир', brain: '' },
            { id: '00000000-0000-4000-8000-000000000001', name: 'さくら', brain: '' },
            { id: '00000000-0000-4000-8000-000000000002', name: '🤡', brain: 'cordite' },
            { id: '00000000-0000-4000-8000-000000000003', name: '', brain: 'random' },
        ],
    },
    {
        gid: 'game-3', title: 'quote " backslash \\ newline \n tab \t', status: 2, goodMask: 0b11,
        seats: [{ id: 'p-a', name: 'a"b\\c', brain: '' }, { id: 'p-b', name: 'line\nbreak', brain: '' }],
    },
];

const identityOf = (t: RosterTable) => ({
    id: t.gid, name: t.title, status: t.status,
    players: t.seats.map((s) => ({ player_id: s.id, name: s.name, is_ai: s.brain !== '' })),
});

test('the packed roster round-trips every field a client reads', () => {
    for (const t of TABLES) {
        const back = readTrailer(trailerOf(t), t.seats.length);
        assert.ok(back, `roster ${t.gid} did not decode`);
        assert.deepEqual(back, identityOf(t), `roster ${t.gid} lost a field`);
    }
});

test('a roster read short is nothing, never half a table', () => {
    const bytes = trailerOf(TABLES[1]);
    for (let cut = 0; cut < bytes.length; cut++) {
        assert.equal(readTrailer(bytes.subarray(0, cut), TABLES[1].seats.length), null,
                     `a ${cut}-byte prefix decoded to a roster`);
    }
    // Its first byte is the trailer's format version: an unknown one is refused, not guessed at.
    const wrongFormat = Uint8Array.from(bytes);
    wrongFormat[0] += 1;
    assert.equal(readTrailer(wrongFormat, TABLES[1].seats.length), null, 'an unknown trailer version was read anyway');
});

test('a name is trimmed to 64 UTF-8 BYTES, on a scalar boundary, never mid-codepoint', () => {
    const cases = [
        '🤡'.repeat(20),                 // 80 bytes, 20 scalars
        'Владимир'.repeat(9),            // 144 bytes - 2 bytes per char
        'A' + '👍🏽'.repeat(8),          // 65 bytes: the trim line falls INSIDE a cluster
        'さくら'.repeat(8),               // 72 bytes, 3 bytes per char
    ];
    for (const name of cases) {
        assert.ok(utf8len(name) > ROSTER_NAME_MAX, `${name} is not over budget - test proves nothing`);
        const back = readTrailer(trailerOf({
            gid: 'g', title: 'n', status: 1, goodMask: 0,
            seats: [{ id: 'p', name, brain: '' }, { id: 'q', name: 'Bob', brain: '' }],
        }), 2);
        assert.ok(back, 'a long name broke the roster');
        const got = back.players[0].name;
        assert.ok(utf8len(got) <= ROSTER_NAME_MAX, `${name}: still over budget at ${utf8len(got)}B`);
        assert.ok(name.startsWith(got), `${name}: the trim did not keep a prefix`);
        assert.ok(got.length > 0, `${name}: the trim ate the whole name`);
        // A severed multi-byte sequence decodes to U+FFFD - the failure a
        // byte-length codec makes when it trims bytes instead of scalars.
        assert.ok(!got.includes('�'), `${name}: the trim cut a codepoint in half`);
        assert.equal(back.players[1].name, 'Bob', `${name}: the trim desynchronized the next seat`);
    }
});

// ===========================================================================
// 3. The cross-language gate: the REAL Swift decoder on the kernel's bytes
// ===========================================================================

const hasSwift = (() => spawnSync('swiftc', ['--version'], { stdio: 'ignore' }).status === 0)();

const DRIVER = `
import Foundation

// Two modes, both driving PRODUCTION sources compiled alongside this file:
//   (default)  hex on stdin -> the decoded roster as JSON on stdout
//   "encode"   a JSON name list on stdin -> RosterWire.encode's bytes as hex
// The second is the byte-for-byte gate: the names block has a writer on each
// side of the language line, and the trim rule is the part with judgement in it.
if CommandLine.arguments.count > 1 && CommandLine.arguments[1] == "encode" {
    let raw = FileHandle.standardInput.readDataToEndOfFile()
    let names = try! JSONDecoder().decode([String].self, from: raw)
    let joins = names.enumerated().map { MessageJoin(seat: $0.offset, name: $0.element) }
    print(RosterWire.encode(joins).map { String(format: "%02x", $0) }.joined())
    exit(0)
}

let hex = String(decoding: FileHandle.standardInput.readDataToEndOfFile(), as: UTF8.self)
    .trimmingCharacters(in: .whitespacesAndNewlines)
var b: [UInt8] = []
var i = hex.startIndex
while i < hex.endIndex {
    let j = hex.index(i, offsetBy: 2)
    b.append(UInt8(hex[i..<j], radix: 16)!)
    i = j
}
guard let (r, next) = EnvelopeRoster.decode(b, at: 0) else {
    FileHandle.standardError.write("decode returned nil\\n".data(using: .utf8)!)
    exit(2)
}
struct OutPlayer: Encodable { let player_id: String; let name: String; let is_ai: Bool }
struct Out: Encodable {
    let id: String; let name: String; let status: Int
    let players: [OutPlayer]; let good_players: [String]
    let good_timestamp: Double?; let next: Int
}
let out = Out(id: r.id, name: r.name, status: r.status,
              players: r.players.map { OutPlayer(player_id: $0.playerId, name: $0.name, is_ai: $0.isAI) },
              good_players: r.goodPlayers, good_timestamp: r.goodTimestamp, next: next)
let enc = JSONEncoder()
enc.outputFormatting = [.sortedKeys]
print(String(decoding: try! enc.encode(out), as: UTF8.self))
`;

let binary: string | null = null;
let workdir: string | null = null;

// The Swift half of the parity, by PATH. These files are compiled straight out
// of the tree rather than through a target, so a move breaks this test - which
// is the point: EnvelopeRoster.swift moved to ios/FoolishNet in the bundle diet
// (its `public` symbols were dead-strip roots in a shipped dylib) and this list
// was not updated, so the whole Swift side failed with a swiftc "no such file"
// that named nothing about parity. `resolve` turns that into a directive.
const SWIFT_SOURCES = [
    'sdk/swift/PackedBytes.swift',
    'sdk/swift/RosterWire.swift',
    'ios/FoolishNet/EnvelopeRoster.swift',
];

function resolve(rel: string): string {
    const abs = join(REPO, rel);
    if (!existsSync(abs)) {
        throw new Error(
            `${rel} is gone. This test compiles the REAL Swift decoder from the ` +
            `tree, so if that file moved, update SWIFT_SOURCES in this file - ` +
            `do not delete the test, or the kernel's writer and the Swift decoder ` +
            `are free to drift.`);
    }
    return abs;
}

function swiftDecoder(): string {
    if (binary) return binary;
    workdir = mkdtempSync(join(tmpdir(), 'foolish_roster_wire_'));
    const main = join(workdir, 'main.swift');
    writeFileSync(main, DRIVER);
    const out = join(workdir, 'decode_roster');
    execFileSync('swiftc', [
        ...SWIFT_SOURCES.map(resolve),
        main, '-o', out,
    ], { stdio: ['ignore', 'pipe', 'pipe'] });
    binary = out;
    return out;
}

const toHex = (b: Uint8Array) => Array.from(b).map(x => x.toString(16).padStart(2, '0')).join('');

// eslint-disable-next-line @typescript-eslint/no-explicit-any
function swiftDecode(bytes: Uint8Array): any {
    return JSON.parse(execFileSync(swiftDecoder(), [], { input: toHex(bytes), encoding: 'utf8' }));
}

const OVER_BUDGET = [
    '🤡'.repeat(20), 'A' + '👍🏽'.repeat(8), 'Владимир'.repeat(9),
    'さくら'.repeat(8), 'x' + '🇺🇦'.repeat(9),
];

const C_TABLES: RosterTable[] = [
    ...TABLES,
    ...OVER_BUDGET.map((name, k): RosterTable => ({
        gid: `g-${k}`, title: 'n', status: 1, goodMask: 0b10,
        seats: [{ id: 'p', name, brain: '' }, { id: 'q', name: 'Bob', brain: 'random' }],
    })),
    {
        gid: '00000000-0000-4000-8000-0000000000aa', title: '🎴'.repeat(50), status: 2, goodMask: 0b10100101,
        seats: Array.from({ length: 8 }, (_, i) => ({
            id: `00000000-0000-4000-8000-00000000000${i}`, name: OVER_BUDGET[i % OVER_BUDGET.length],
            brain: i % 3 === 0 ? 'octogen' : '',
        })),
    },
    { gid: 'empty', title: '', status: 0, goodMask: 0, seats: [] },
];

test('the REAL Swift decoder reads the kernel\'s trailers, every field', { skip: !hasSwift && 'no swiftc on this machine' }, () => {
    for (const table of C_TABLES) {
        const c = trailerOf(table);
        const got = swiftDecode(c);
        assert.equal(got.next, c.length, `${table.gid}: Swift stopped short of the end of C's trailer`);
        assert.equal(got.id, table.gid);
        assert.equal(got.name, table.title);
        assert.equal(got.status, table.status, `${table.gid}: status disagrees`);
        assert.equal(got.players.length, table.seats.length);
        got.players.forEach((p: { name: string; player_id: string; is_ai: boolean }, i: number) => {
            assert.equal(p.player_id, table.seats[i].id, `${table.gid}: seat ${i}'s id`);
            assert.ok(table.seats[i].name.startsWith(p.name), `${table.gid}: seat ${i}'s name is not a prefix`);
            assert.equal(p.is_ai, table.seats[i].brain !== '', `${table.gid}: seat ${i}'s is_ai`);
        });
        assert.deepEqual(got.good_players, table.seats.filter((_, i) => (table.goodMask >> i) & 1).map((s) => s.id),
                         `${table.gid}: the good seats, in seat order`);
        assert.equal(got.good_timestamp ?? null, null, `${table.gid}: the timestamp rides the board, not the trailer`);
        if (utf8len(table.title) <= 200 && table.seats.every((s) => utf8len(s.name) <= ROSTER_NAME_MAX)) {
            // Nothing to trim: Swift reads exactly what the web reads.
            if (table.seats.length > 0) assert.deepEqual(
                { id: got.id, name: got.name, status: got.status, players: got.players },
                readTrailer(c, table.seats.length), `${table.gid}: Swift and the client slot read the same roster`);
        }
    }
});

test('Swift and the kernel write the same names block, byte for byte', { skip: !hasSwift && 'no swiftc on this machine' }, () => {
    // The names block has a writer on BOTH sides of the language line -
    // RosterWire.encode writes it for FMSG, the C Roster writes it inside the
    // envelope's trailer - and two tests that each parsed their own output would
    // both pass while the two disagreed about where a 65-byte name gets cut. The
    // trailer is [format][id][title][status][names block][ids...]: the Swift
    // block must appear in it whole, at a byte boundary, after the header.
    const swift = (names: string[]) =>
        execFileSync(swiftDecoder(), ['encode'], { input: JSON.stringify(names), encoding: 'utf8' }).trim();

    const tables = [
        ['Sveta', 'Misha'],
        ['Владимир', 'Ольга', 'Пётр', 'Анна'],
        ['さくら', 'Ünïcodé', ''],
        ['🤡', 'A👍🏽B', 'x'],
        ['a"b\\c', 'line\nbreak', 'nul inside'],
        OVER_BUDGET,
    ];
    for (const names of tables) {
        const c = toHex(trailerOf({
            gid: 'g', title: 't', status: 1, goodMask: 0,
            seats: names.map((name, i) => ({ id: `p${i}`, name, brain: '' })),
        }));
        const block = swift(names);
        const at = c.indexOf(block);
        assert.ok(at > 0 && at % 2 === 0, `Swift's names block for ${JSON.stringify(names)} is not in the kernel's trailer (${block} in ${c})`);
    }
});

test('Swift and the web agree on the trim, byte for byte', { skip: !hasSwift && 'no swiftc on this machine' }, () => {
    // The only part of this format with any judgement in it. A grapheme cluster
    // may be split, a code point never - and both readers see the SAME cut.
    for (const name of OVER_BUDGET) {
        const bytes = trailerOf({
            gid: 'g', title: 'n', status: 1, goodMask: 0,
            seats: [{ id: 'p', name, brain: '' }, { id: 'q', name: 'Bob', brain: 'random' }],
        });
        const got = swiftDecode(bytes);
        const mine = readTrailer(bytes, 2)!;
        assert.deepEqual(got.players, mine.players, `Swift and the web disagree about ${JSON.stringify(name)}`);
        assert.equal(got.players[1].name, 'Bob', 'the trim desynchronized the seat after it');
    }
});

test('a truncated trailer is nothing to Swift too', { skip: !hasSwift && 'no swiftc on this machine' }, () => {
    const bytes = trailerOf(TABLES[1]);
    for (const cut of [1, 5, 12, bytes.length - 1]) {
        const r = spawnSync(swiftDecoder(), [], { input: toHex(bytes.subarray(0, cut)), encoding: 'utf8' });
        assert.notEqual(r.status, 0, `Swift read a ${cut}-byte prefix as a whole roster`);
    }
});

after(() => { if (workdir) rmSync(workdir, { recursive: true, force: true }); });
