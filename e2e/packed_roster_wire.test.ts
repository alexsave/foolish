/* =============================================================================
 * The envelope's roster is packed - and a 1.0(43) client cannot tell
 * =============================================================================
 * The client-server envelope (encodeGameResponse) carried its roster as a JSON
 * island: the last JSON on any path that matters. It is bytes now.
 *
 * THE CONSTRAINT THIS FILE EXISTS FOR. Merging a PR in this repo deploys the
 * server IMMEDIATELY. The iOS client does not deploy that way - it ships through
 * the App Store, and build 1.0(43) is in users' hands. There is no coordinated
 * deploy available, and the same envelope is also STORED (player_views.view,
 * spectator_views.view), so there is not even a request to negotiate a format
 * on. A server that simply switched to emitting a packed roster would break
 * every installed client the moment it landed.
 *
 * So the packed roster is a TRAILER, announced in a flags bit, and this file's
 * first two tests are the ones that matter: a frozen replica of the 1.0(43)
 * encoder and a frozen replica of the 1.0(43) decoder, transcribed from the
 * build that is live. The encoder replica pins that every byte an old client
 * reads is unchanged; the decoder replica pins that it still reads them to the
 * same game. Break either - emit the packed roster in the island's place, drop
 * the island, move the flag - and they fail.
 *
 * The rest of the file is the codec itself: a TS round trip, the cross-language
 * gate against the REAL Swift decoder (the pattern imessage_replay_names.test.ts
 * set - compile the production source, not a copy), and the non-ASCII cases a
 * byte-length codec goes wrong on.
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

import {
    Game, PrivatePlayer, PLAYER_STATUS, GAME_STATUS, STRATEGY_KEY,
} from '../server/api/core/types.ts';
import {
    GAME_RESP_FORMAT, GAME_RESP_FLAG_PACKED_ROSTER,
    VIEW_FORMAT_VERSION, PackedGameRoster,
    encodeGameResponse,
} from '../sdk/ts/wire/view.ts';
import { decodeEnvelope } from './helpers/client_read.ts';
import { wasmViewFromGame } from '../sdk/ts/wasm/bots.ts';
import {
    ROSTER_MAX_NAME_BYTES, ROSTER_WIRE_FORMAT,
    encodePackedRoster, encodeRosterNames, rosterNameBytes,
    PackedRoster,
} from '../sdk/ts/wire/roster.ts';
import { RosterTable, cRosterTrailer, tsRosterFor } from './helpers/roster_kernel.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; }

const REPO = new URL('..', import.meta.url).pathname;
const utf8len = (s: string) => new TextEncoder().encode(s).length;

// ---------------------------------------------------------------------------
// A real envelope, built the way the server builds one for a lobby: the pure-TS
// mirror of view.c's masked state (no wasm boot needed for a WAITING game).
// ---------------------------------------------------------------------------

const mkPlayer = (i: number, name: string, isAi: boolean): PrivatePlayer => ({
    player_id: `00000000-0000-4000-8000-00000000000${i}`, name,
    status: PLAYER_STATUS.READY, is_ai: isAi, hand: [], awaiting_attack: false,
    hand_length: 0, strategy_key: isAi ? STRATEGY_KEY.RANDOM : STRATEGY_KEY.HUMAN,
});

function mkGame(names: string[], status: string = GAME_STATUS.WAITING): Game {
    return {
        id: 'game-abc123', name: "Sveta's Game", status,
        players: names.map((n, i) => mkPlayer(i, n, i % 3 === 2)),
        deck: [], deck_length: 0, discard_pile_length: 0, flipped: null,
        power_suit: 0, first_attacker: 0, defender: 0, table_battles: [],
        elimination_order: [], good_timestamp: null, good_players: [], logs: [],
    };
}

const rosterFor = (game: Game): PackedGameRoster => ({
    id: game.id,
    name: game.name,
    status: game.status,
    players: game.players.map(p => ({ player_id: p.player_id, name: p.name, is_ai: p.is_ai })),
    good_players: game.good_players ?? [],
    good_timestamp: game.good_timestamp ?? null,
});

// The kernel's own writer (wasm_view_serialize), which already emits the
// [VIEW_FORMAT_VERSION | viewer | masked put_state] envelope. It replaced the
// pure-TS mirror this used to call, so what the frozen build-43 encoder is
// compared against is the format's one definition.
function viewBlobFor(game: Game, seat: number): Uint8Array {
    return wasmViewFromGame(game, seat);
}

// A roster trailer read the way the web client reads one: as the trailer of an
// envelope (the view blob of a lobby of that many seats) through the kernel's
// client slot. What it reads is the table's identity - its id, title, status and
// seats; the good ids and their timestamp are checked for shape and not read
// (the goods live in the board, plan Q1). Null when the trailer does not read
// whole.
function readTrailer(trailer: Uint8Array, seats: number) {
    const blob = viewBlobFor(mkGame(Array.from({ length: seats }, (_, i) => `s${i}`)), -1);
    const env = new Uint8Array(11 + blob.length + trailer.length);
    env.set([GAME_RESP_FORMAT, GAME_RESP_FLAG_PACKED_ROSTER, 0xff, 1, 0, 0, 0, 0, 0, blob.length & 0xff, blob.length >> 8]);
    env.set(blob, 11);
    env.set(trailer, 11 + blob.length);
    const d = decodeEnvelope(env);
    return d && {
        id: d.game.id, name: d.game.name, status: d.game.status,
        players: d.game.players.map((p) => ({ player_id: p.player_id, name: p.name, is_ai: p.is_ai })),
    };
}

// ---------------------------------------------------------------------------
// THE FROZEN 1.0(43) ENCODER. Transcribed verbatim from encodeGameResponse as
// it stood in the build that is live in the App Store. Do not "fix" it, ever:
// its whole job is to be what shipped.
// ---------------------------------------------------------------------------
function encodeGameResponse_build43(
    version: number, seat: number, roster: PackedGameRoster, viewBlob: Uint8Array,
): Uint8Array {
    const rosterBytes = new TextEncoder().encode(JSON.stringify(roster));
    const out = new Uint8Array(3 + 4 + 2 + rosterBytes.length + 2 + viewBlob.length);
    let q = 0;
    out[q++] = 1;                              // GAME_RESP_FORMAT
    out[q++] = seat >= 0 ? 1 : 0;              // flags: bit0 only
    out[q++] = seat >= 0 ? seat : 0xff;
    out[q++] = version & 0xff; out[q++] = (version >> 8) & 0xff;
    out[q++] = (version >> 16) & 0xff; out[q++] = (version >> 24) & 0xff;
    out[q++] = rosterBytes.length & 0xff; out[q++] = (rosterBytes.length >> 8) & 0xff;
    out.set(rosterBytes, q); q += rosterBytes.length;
    out[q++] = viewBlob.length & 0xff; out[q++] = (viewBlob.length >> 8) & 0xff;
    out.set(viewBlob, q);
    return out;
}

// ---------------------------------------------------------------------------
// THE FROZEN 1.0(43) DECODER. Transcribed from ios/FoolishNet/PackedGame.swift
// as it stood in the shipped build - the same guards, the same order, the same
// JSON island. `null` here means the installed app shows the game as
// unreadable, which is the failure this file exists to make loud.
// ---------------------------------------------------------------------------
interface Build43Game {
    gameId: string; seat: number; version: number; status: string;
    names: string[]; ids: string[]; isAi: boolean[]; stateBytes: Uint8Array;
}
function decode_build43(buf: Uint8Array): Build43Game | null {
    if (buf.length < 11 || buf[0] !== 1) return null;
    const isPlayer = (buf[1] & 1) !== 0;
    const seat = isPlayer ? buf[2] : -1;
    const version = (buf[3] | (buf[4] << 8) | (buf[5] << 16) | (buf[6] << 24)) >>> 0;
    const rosterLen = buf[7] | (buf[8] << 8);
    if (9 + rosterLen + 2 > buf.length) return null;
    let roster: PackedGameRoster;
    try {
        roster = JSON.parse(new TextDecoder().decode(buf.subarray(9, 9 + rosterLen)));
    } catch { return null; }
    let q = 9 + rosterLen;
    const viewLen = buf[q] | (buf[q + 1] << 8);
    q += 2;
    if (q + viewLen > buf.length || viewLen < 2 || buf[q] !== 1) return null;
    return {
        gameId: roster.id, seat, version, status: roster.status,
        names: roster.players.map(p => p.name),
        ids: roster.players.map(p => p.player_id),
        isAi: roster.players.map(p => p.is_ai),
        stateBytes: buf.subarray(q + 2, q + viewLen),
    };
}

// ===========================================================================
// 1. The envelope, now that the island is gone
// ===========================================================================
//
// This file used to open with a compatibility gate: two sections proving that
// every byte an installed 1.0(43) client read was unchanged, that a reader
// ignoring the new flag bit still landed on the JSON roster island, and that
// the packed trailer won over the island when both were present. All three
// described a transition that is over. The island is not written any more
// (roster_len is 0), the trailer is mandatory, and an envelope without it does
// not decode at all - which is the behaviour this section pins instead.

test('the envelope carries no roster island, and the trailer is the roster', () => {
    const game = mkGame(['Sveta', 'Misha', 'Пётр'], GAME_STATUS.PLAYING);
    game.good_players = [game.players[1].player_id];
    game.good_timestamp = 1723456789012;
    const blob = viewBlobFor(game, 1);
    const out = encodeGameResponse(5, 1, rosterFor(game), blob);

    // roster_len is the u16 at bytes 7-8, and it is zero: nothing rides there.
    assert.equal(out[7] | (out[8] << 8), 0, 'the roster island is still being written');
    assert.ok((out[1] & GAME_RESP_FLAG_PACKED_ROSTER) !== 0, 'the trailer flag must be set');
    assert.equal(out[0], GAME_RESP_FORMAT);

    const dec = decodeEnvelope(out);
    assert.ok(dec, 'the envelope must decode');
    assert.deepEqual(dec!.game.players.map(p => p.name), ['Sveta', 'Misha', 'Пётр'],
                     'the names came from the packed trailer');
    assert.equal(dec!.game.status, GAME_STATUS.PLAYING);
    assert.equal(dec!.game.id, 'game-abc123');
    assert.deepEqual(dec!.game.good_players, [game.players[1].player_id]);
});

// A stored player_views row written before the trailer existed carries neither
// island nor trailer once the island stops being written. It must fail as
// unreadable rather than decode to a game with an empty roster - a table with
// no names is worse than a load error, because the caller cannot tell it went
// wrong. The next commit on that game rewrites the row.
test('an envelope with no packed trailer is unreadable, not an empty table', () => {
    const game = mkGame(['Sveta', 'Misha'], GAME_STATUS.PLAYING);
    const blob = viewBlobFor(game, 0);
    const full = encodeGameResponse(5, 0, rosterFor(game), blob);

    // The same bytes with the trailer lopped off and its flag cleared - what a
    // pre-trailer writer emitted, minus the island it used to put at byte 9.
    const head = 9 + 2 + blob.length;
    const legacy = full.subarray(0, head).slice();
    legacy[1] &= ~GAME_RESP_FLAG_PACKED_ROSTER;
    assert.equal(decodeEnvelope(legacy), null, 'a trailer-less envelope must not decode');
});


// ===========================================================================
// 3. The codec itself
// ===========================================================================

const ROSTERS: PackedRoster[] = [
    {
        id: 'game-1', name: "Sveta's Game", status: 'waiting',
        players: [{ player_id: 'p-0', name: 'Sveta', is_ai: false }],
        good_players: [], good_timestamp: null,
    },
    {
        id: 'game-2', name: 'Игра Володи', status: 'playing',
        players: [
            { player_id: '00000000-0000-4000-8000-000000000000', name: 'Владимир', is_ai: false },
            { player_id: '00000000-0000-4000-8000-000000000001', name: 'さくら', is_ai: false },
            { player_id: '00000000-0000-4000-8000-000000000002', name: '🤡', is_ai: true },
            { player_id: '00000000-0000-4000-8000-000000000003', name: '', is_ai: true },
        ],
        good_players: ['00000000-0000-4000-8000-000000000001'],
        good_timestamp: 1723456789012,
    },
    {
        id: 'game-3', name: 'quote " backslash \\ newline \n tab \t', status: 'game_over',
        players: [
            { player_id: 'p-a', name: 'a"b\\c', is_ai: false },
            { player_id: 'p-b', name: 'line\nbreak', is_ai: false },
        ],
        good_players: ['p-a', 'p-b'], good_timestamp: 0,
    },
];

test('the packed roster round-trips every field a client reads', () => {
    for (const roster of ROSTERS) {
        const bytes = encodePackedRoster(roster);
        assert.equal(bytes[0], ROSTER_WIRE_FORMAT, 'the trailer leads with its version byte');
        // It reads whole or not at all: a trailer with bytes to spare is refused too.
        const back = readTrailer(bytes, roster.players.length);
        assert.ok(back, `roster ${roster.id} did not decode`);
        const { good_players: _g, good_timestamp: _t, ...identity } = roster;
        assert.deepEqual(back, identity, `roster ${roster.id} lost a field`);
    }
});

test('a roster read short is nothing, never half a table', () => {
    const bytes = encodePackedRoster(ROSTERS[1]);
    for (let cut = 0; cut < bytes.length; cut++) {
        assert.equal(readTrailer(bytes.subarray(0, cut), ROSTERS[1].players.length), null,
                     `a ${cut}-byte prefix decoded to a roster`);
    }
    const wrongFormat = Uint8Array.from(bytes);
    wrongFormat[0] = ROSTER_WIRE_FORMAT + 1;
    assert.equal(readTrailer(wrongFormat, ROSTERS[1].players.length), null, 'an unknown trailer version was read anyway');
});

test('a name is trimmed to 64 UTF-8 BYTES, on a scalar boundary, never mid-codepoint', () => {
    const cases = [
        '🤡'.repeat(20),                 // 80 bytes, 20 scalars
        'Владимир'.repeat(9),            // 144 bytes - 2 bytes per char
        'A' + '👍🏽'.repeat(8),          // 65 bytes: the trim line falls INSIDE a cluster
        'さくら'.repeat(8),               // 72 bytes, 3 bytes per char
    ];
    for (const name of cases) {
        assert.ok(utf8len(name) > ROSTER_MAX_NAME_BYTES, `${name} is not over budget - test proves nothing`);
        const roster: PackedRoster = {
            id: 'g', name: 'n', status: 'playing',
            players: [{ player_id: 'p', name, is_ai: false },
                      { player_id: 'q', name: 'Bob', is_ai: false }],
            good_players: [], good_timestamp: null,
        };
        const back = readTrailer(encodePackedRoster(roster), 2);
        assert.ok(back, 'a long name broke the roster');
        const got = back!.players[0].name;
        assert.ok(utf8len(got) <= ROSTER_MAX_NAME_BYTES, `${name}: still over budget at ${utf8len(got)}B`);
        assert.ok(name.startsWith(got), `${name}: the trim did not keep a prefix`);
        assert.ok(got.length > 0, `${name}: the trim ate the whole name`);
        // A severed multi-byte sequence decodes to U+FFFD - the failure a
        // byte-length codec makes when it trims bytes instead of scalars.
        assert.ok(!got.includes('�'), `${name}: the trim cut a codepoint in half`);
        assert.equal(back!.players[1].name, 'Bob', `${name}: the trim desynchronized the next seat`);
    }
});

test('rosterNameBytes is what the trim rule is, and it never emits a partial codepoint', () => {
    // A name whose UTF-8 byte length differs from its character count, at every
    // length around the budget: the decode must be lossless or a clean prefix.
    for (let n = 1; n <= 25; n++) {
        const name = '👍🏽'.repeat(n);
        const bytes = rosterNameBytes(name);
        assert.ok(bytes.length <= ROSTER_MAX_NAME_BYTES, `${n} clusters: ${bytes.length}B over budget`);
        const back = new TextDecoder('utf-8', { fatal: true }).decode(bytes);
        assert.ok(name.startsWith(back), `${n} clusters: not a prefix`);
    }
});

// ===========================================================================
// 4. The cross-language gate: the REAL Swift decoder on this encoder's bytes
// ===========================================================================

const hasSwift = (() => spawnSync('swiftc', ['--version'], { stdio: 'ignore' }).status === 0)();

const DRIVER = `
import Foundation

// Two modes, both driving PRODUCTION sources compiled alongside this file:
//   (default)  hex on stdin -> the decoded roster as JSON on stdout
//   "encode"   a JSON name list on stdin -> RosterWire.encode's bytes as hex
// The second is the byte-for-byte gate: the names block has an encoder on each
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
            `do not delete the test, or the TS encoder and the Swift decoder ` +
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

function swiftDecode(bytes: Uint8Array): any {
    return JSON.parse(execFileSync(swiftDecoder(), [], { input: toHex(bytes), encoding: 'utf8' }));
}

test('the REAL Swift decoder reads this encoder\'s bytes', { skip: !hasSwift && 'no swiftc on this machine' }, () => {
    const STATUS = ['waiting', 'playing', 'game_over'];
    for (const roster of ROSTERS) {
        const bytes = encodePackedRoster(roster);
        const got = swiftDecode(bytes);
        assert.equal(got.next, bytes.length, `${roster.id}: Swift stopped at a different offset`);
        assert.equal(got.id, roster.id);
        assert.equal(got.name, roster.name);
        assert.equal(STATUS[got.status], roster.status, `${roster.id}: status disagrees`);
        assert.deepEqual(got.players, roster.players, `${roster.id}: Swift and TS disagree on the seats`);
        assert.deepEqual(got.good_players, roster.good_players);
        assert.equal(got.good_timestamp ?? null, roster.good_timestamp);
    }
});

const OVER_BUDGET = [
    '🤡'.repeat(20), 'A' + '👍🏽'.repeat(8), 'Владимир'.repeat(9),
    'さくら'.repeat(8), 'x' + '🇺🇦'.repeat(9),
];

test('Swift and TypeScript write the same names block, byte for byte', { skip: !hasSwift && 'no swiftc on this machine' }, () => {
    // The strongest assertion in this file after the 1.0(43) gate, and the one
    // e2e/imessage_replay_names.test.ts set the pattern for: the names block has
    // an encoder on BOTH sides of the language line - RosterWire.encode writes
    // it for FMSG, encodeRosterNames writes it for the envelope - and two tests
    // that each parsed their own output would both pass while the two disagreed
    // about where a 65-byte name gets cut. Same bytes or nothing.
    const swift = (names: string[]) =>
        execFileSync(swiftDecoder(), ['encode'], { input: JSON.stringify(names), encoding: 'utf8' }).trim();

    const tables = [
        ['Sveta', 'Misha'],
        ['Владимир', 'Ольга', 'Пётр', 'Анна'],
        ['さくら', 'Ünïcodé', ''],
        ['🤡', 'A👍🏽B', 'x'],
        ['a"b\\c', 'line\nbreak', 'nul inside'],
        OVER_BUDGET,
        [],
    ];
    for (const names of tables) {
        assert.equal(swift(names), toHex(encodeRosterNames(names)),
                     `Swift and TS disagree on ${JSON.stringify(names)}`);
    }
});

test('Swift and TypeScript agree on the trim, byte for byte', { skip: !hasSwift && 'no swiftc on this machine' }, () => {
    // The only part of this format with any judgement in it. Swift trims
    // unicodeScalars; TS trims Array.from. A grapheme cluster may be split, a
    // code point never - and both sides must split in the SAME place.
    for (const name of OVER_BUDGET) {
        const roster: PackedRoster = {
            id: 'g', name: 'n', status: 'playing',
            players: [{ player_id: 'p', name, is_ai: false },
                      { player_id: 'q', name: 'Bob', is_ai: true }],
            good_players: [], good_timestamp: null,
        };
        const bytes = encodePackedRoster(roster);
        const got = swiftDecode(bytes);
        const mine = readTrailer(bytes, 2)!;
        assert.deepEqual(got.players, mine.players,
                         `Swift and TS disagree about ${JSON.stringify(name)}`);
        assert.equal(got.players[1].name, 'Bob', 'the trim desynchronized the seat after it');
    }
});

// ===========================================================================
// 5. The C writer (c/src/roster.c) through the same REAL Swift decoder
// ===========================================================================
//
// The C Roster takes the trailer over from encodePackedRoster (docs/
// C_GAME_SHAPE_MIGRATION.md Phase 2, 4b). Shipped iOS builds cannot be updated
// with it, so what C writes has to read in the installed EnvelopeRoster.decode
// exactly as the TS bytes do. e2e/roster_c_parity.test.ts holds C to TS byte for
// byte; this closes the loop through the decoder that actually ships.

const C_TABLES: RosterTable[] = [
    ...ROSTERS.map((r): RosterTable => ({
        gid: r.id, title: r.name, status: ['waiting', 'playing', 'game_over'].indexOf(r.status),
        goodMask: r.players.reduce((m, p, i) => m | (r.good_players.includes(p.player_id) ? 1 << i : 0), 0),
        seats: r.players.map(p => ({ id: p.player_id, name: p.name, brain: p.is_ai ? 'cordite' : '' })),
    })),
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

test('the REAL Swift decoder reads C-written trailers exactly as it reads TS-written ones', { skip: !hasSwift && 'no swiftc on this machine' }, () => {
    for (const table of C_TABLES) {
        const c = cRosterTrailer(table);
        assert.ok(c instanceof Uint8Array, `${table.gid}: C refused the table with ${c}`);
        const fromC = swiftDecode(c);
        assert.equal(fromC.next, c.length, `${table.gid}: Swift stopped short of the end of C's trailer`);
        assert.equal(fromC.id, table.gid);
        assert.equal(fromC.status, table.status);
        assert.equal(fromC.players.length, table.seats.length);
        fromC.players.forEach((p: { name: string; player_id: string; is_ai: boolean }, i: number) => {
            assert.ok(table.seats[i].name.startsWith(p.name), `${table.gid}: seat ${i}'s name is not a prefix`);
            assert.equal(p.is_ai, table.seats[i].brain !== '', `${table.gid}: seat ${i}'s is_ai`);
        });
        assert.deepEqual(fromC, swiftDecode(encodePackedRoster(tsRosterFor(table))),
                         `${table.gid}: Swift reads C's trailer differently from TS's`);
    }
});

test('a truncated trailer is nothing to Swift too', { skip: !hasSwift && 'no swiftc on this machine' }, () => {
    const bytes = encodePackedRoster(ROSTERS[1]);
    for (const cut of [1, 5, 12, bytes.length - 1]) {
        const r = spawnSync(swiftDecoder(), [], { input: toHex(bytes.subarray(0, cut)), encoding: 'utf8' });
        assert.notEqual(r.status, 0, `Swift read a ${cut}-byte prefix as a whole roster`);
    }
});

after(() => { if (workdir) rmSync(workdir, { recursive: true, force: true }); });
