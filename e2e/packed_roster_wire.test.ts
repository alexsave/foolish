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
import { mkdtempSync, writeFileSync, rmSync } from 'node:fs';
import { join } from 'node:path';
import { tmpdir } from 'node:os';

import {
    Game, PrivatePlayer, PLAYER_STATUS, GAME_STATUS, STRATEGY_KEY,
} from '../server/api/core/types.ts';
import {
    GAME_RESP_FORMAT, GAME_RESP_FLAG_PACKED_ROSTER, LEGACY_ROSTER_JSON,
    VIEW_FORMAT_VERSION, PackedGameRoster,
    encodeGameResponse, decodePackedGame, writeMaskedState,
} from '../sdk/ts/wire/view.ts';
import {
    ROSTER_MAX_NAME_BYTES, ROSTER_WIRE_FORMAT,
    encodePackedRoster, decodePackedRoster, encodeRosterNames, rosterNameBytes,
    PackedRoster,
} from '../sdk/ts/wire/roster.ts';

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

function viewBlobFor(game: Game, seat: number): Uint8Array {
    const body: number[] = [];
    writeMaskedState(game, seat, body);
    return Uint8Array.from([VIEW_FORMAT_VERSION, seat < 0 ? 0xff : seat, ...body]);
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
// 1. The gate: an installed 1.0(43) client sees the same bytes it always saw
// ===========================================================================

test('every byte a 1.0(43) client reads is unchanged, and the trailer is all that is new', () => {
    assert.ok(LEGACY_ROSTER_JSON,
        'the island is gone - this gate no longer protects anything, and the deletion ' +
        'condition in view.ts must have been met before it was flipped');

    for (const names of [['Sveta'], ['Sveta', 'Misha', 'Оля'], ['a', 'b', 'c', 'd', 'e', 'f', 'g', 'h']]) {
        const game = mkGame(names, GAME_STATUS.PLAYING);
        game.good_players = [game.players[0].player_id];
        game.good_timestamp = 1723456789012;
        const roster = rosterFor(game);
        for (const seat of [...names.map((_, i) => i), -1]) {
            const blob = viewBlobFor(game, seat);
            const shipped = encodeGameResponse_build43(7, seat, roster, blob);
            const now = encodeGameResponse(7, seat, roster, blob);
            const tag = `${names.length}p seat ${seat}`;

            assert.ok(now.length > shipped.length, `${tag}: no trailer was appended at all`);
            // The flags byte is the ONE place the new bit lives. Assert the
            // difference there is exactly that bit, then hold every other byte
            // of the old reader's span to equality - so nothing can hide behind
            // the exemption.
            assert.equal(now[1] & ~GAME_RESP_FLAG_PACKED_ROSTER, shipped[1],
                         `${tag}: flags byte changed by more than the trailer bit`);
            assert.notEqual(now[1] & GAME_RESP_FLAG_PACKED_ROSTER, 0,
                            `${tag}: the trailer was written without announcing itself`);
            // The load-bearing assertion. Not "it still decodes" - the actual
            // bytes, in order, for the whole span an old reader walks.
            const mask = (b: Uint8Array) => { const c = Array.from(b); c[1] = 0; return c; };
            assert.deepEqual(
                mask(now.subarray(0, shipped.length)), mask(shipped),
                `${tag}: the prefix a 1.0(43) client reads changed`);
        }
    }
});

test('the frozen 1.0(43) decoder reads the new envelope to exactly the game it read before', () => {
    const names = ['Sveta', 'Владимир', '🤡'];
    const game = mkGame(names, GAME_STATUS.PLAYING);
    const roster = rosterFor(game);
    for (const seat of [0, 1, 2, -1]) {
        const blob = viewBlobFor(game, seat);
        const before = decode_build43(encodeGameResponse_build43(11, seat, roster, blob));
        const after = decode_build43(encodeGameResponse(11, seat, roster, blob));
        assert.ok(before, `seat ${seat}: the frozen decoder cannot read its own build's bytes`);
        assert.ok(after, `seat ${seat}: a 1.0(43) client can no longer read this envelope`);
        assert.deepEqual(after, before, `seat ${seat}: a 1.0(43) client decodes a DIFFERENT game now`);
    }
});

// The same guarantee for the OTHER shipped reader: a browser holding a cached
// bundle from before this change. Its decoder is decodePackedGame's JSON
// branch, which is still in this tree, so exercise it directly by clearing the
// flag bit - the payload a pre-trailer reader effectively sees.
test('a reader that ignores the flag bit still lands on the JSON island', () => {
    const game = mkGame(['Sveta', 'Misha'], GAME_STATUS.PLAYING);
    const blob = viewBlobFor(game, 0);
    const bytes = encodeGameResponse(3, 0, rosterFor(game), blob);
    const blind = Uint8Array.from(bytes);
    blind[1] &= ~GAME_RESP_FLAG_PACKED_ROSTER;   // pretend the bit was never set

    const viaTrailer = decodePackedGame(bytes);
    const viaJson = decodePackedGame(blind);
    assert.ok(viaTrailer && viaJson, 'both paths must decode');
    assert.deepEqual(viaJson!.game, viaTrailer!.game,
                     'the packed roster and the JSON island describe different games');
});

// ===========================================================================
// 2. The decoder really reads the trailer (and not the island next to it)
// ===========================================================================

test('the packed trailer wins over the island - the new path is the one being taken', () => {
    const game = mkGame(['Sveta', 'Misha'], GAME_STATUS.PLAYING);
    const blob = viewBlobFor(game, 0);
    // An island that disagrees with the trailer. Only a reader that ignores the
    // island can pass this, which is the point: a decoder that quietly kept
    // parsing JSON would sail through every other test in this file.
    const lying: PackedGameRoster = { ...rosterFor(game), name: 'ISLAND', players: [
        { player_id: 'island-0', name: 'ISLAND-0', is_ai: true },
        { player_id: 'island-1', name: 'ISLAND-1', is_ai: true },
    ] };
    const truth = rosterFor(game);

    const island = new TextEncoder().encode(JSON.stringify(lying));
    const trailer = encodePackedRoster(truth);
    const out = new Uint8Array(9 + island.length + 2 + blob.length + trailer.length);
    let q = 0;
    out[q++] = GAME_RESP_FORMAT;
    out[q++] = 1 | GAME_RESP_FLAG_PACKED_ROSTER;
    out[q++] = 0;
    out[q++] = 1; out[q++] = 0; out[q++] = 0; out[q++] = 0;
    out[q++] = island.length & 0xff; out[q++] = (island.length >> 8) & 0xff;
    out.set(island, q); q += island.length;
    out[q++] = blob.length & 0xff; out[q++] = (blob.length >> 8) & 0xff;
    out.set(blob, q); q += blob.length;
    out.set(trailer, q);

    const dec = decodePackedGame(out);
    assert.ok(dec, 'the mixed envelope must decode');
    assert.equal(dec!.game.name, "Sveta's Game", 'the JSON island was read instead of the trailer');
    assert.deepEqual(dec!.game.players.map(p => p.name), ['Sveta', 'Misha'],
                     'the names came from the island, not the trailer');
});

test('an envelope with NO island at all decodes - the one-commit deletion is already reachable', () => {
    // What encodeGameResponse emits with LEGACY_ROSTER_JSON flipped to false:
    // roster_len 0, trailer present. If this ever fails, the deletion commit is
    // not one commit.
    const game = mkGame(['Sveta', 'Misha', 'Пётр'], GAME_STATUS.PLAYING);
    game.good_players = [game.players[1].player_id];
    game.good_timestamp = 1723456789012;
    const blob = viewBlobFor(game, 1);
    const full = encodeGameResponse(5, 1, rosterFor(game), blob);
    const trailer = encodePackedRoster(rosterFor(game));

    const out = new Uint8Array(9 + 2 + blob.length + trailer.length);
    out.set(full.subarray(0, 7), 0);
    out[7] = 0; out[8] = 0;                       // rosterLen = 0
    out[9] = blob.length & 0xff; out[10] = (blob.length >> 8) & 0xff;
    out.set(blob, 11);
    out.set(trailer, 11 + blob.length);

    const dec = decodePackedGame(out);
    assert.ok(dec, 'an island-free envelope must decode');
    assert.deepEqual(dec!.game.players.map(p => p.name), ['Sveta', 'Misha', 'Пётр']);
    assert.equal(dec!.game.status, GAME_STATUS.PLAYING);
    assert.equal(dec!.game.id, 'game-abc123');
    assert.deepEqual(dec!.game.good_players, [game.players[1].player_id]);
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

test('the packed roster round-trips every field the JSON island carried', () => {
    for (const roster of ROSTERS) {
        const bytes = encodePackedRoster(roster);
        assert.equal(bytes[0], ROSTER_WIRE_FORMAT, 'the trailer leads with its version byte');
        const back = decodePackedRoster(bytes, 0);
        assert.ok(back, `roster ${roster.id} did not decode`);
        assert.equal(back!.next, bytes.length, 'the reader stopped somewhere other than the end');
        assert.deepEqual(back!.roster, roster, `roster ${roster.id} lost a field`);
    }
});

test('a roster read short is nothing, never half a table', () => {
    const bytes = encodePackedRoster(ROSTERS[1]);
    for (let cut = 0; cut < bytes.length; cut++) {
        assert.equal(decodePackedRoster(bytes.subarray(0, cut), 0), null,
                     `a ${cut}-byte prefix decoded to a roster`);
    }
    const wrongFormat = Uint8Array.from(bytes);
    wrongFormat[0] = ROSTER_WIRE_FORMAT + 1;
    assert.equal(decodePackedRoster(wrongFormat, 0), null, 'an unknown trailer version was read anyway');
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
        const back = decodePackedRoster(encodePackedRoster(roster), 0);
        assert.ok(back, 'a long name broke the roster');
        const got = back!.roster.players[0].name;
        assert.ok(utf8len(got) <= ROSTER_MAX_NAME_BYTES, `${name}: still over budget at ${utf8len(got)}B`);
        assert.ok(name.startsWith(got), `${name}: the trim did not keep a prefix`);
        assert.ok(got.length > 0, `${name}: the trim ate the whole name`);
        // A severed multi-byte sequence decodes to U+FFFD - the failure a
        // byte-length codec makes when it trims bytes instead of scalars.
        assert.ok(!got.includes('�'), `${name}: the trim cut a codepoint in half`);
        assert.equal(back!.roster.players[1].name, 'Bob', `${name}: the trim desynchronized the next seat`);
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

function swiftDecoder(): string {
    if (binary) return binary;
    workdir = mkdtempSync(join(tmpdir(), 'foolish_roster_wire_'));
    const main = join(workdir, 'main.swift');
    writeFileSync(main, DRIVER);
    const out = join(workdir, 'decode_roster');
    execFileSync('swiftc', [
        join(REPO, 'sdk/swift/PackedBytes.swift'),
        join(REPO, 'sdk/swift/RosterWire.swift'),
        join(REPO, 'sdk/swift/EnvelopeRoster.swift'),
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
        const mine = decodePackedRoster(bytes, 0)!.roster;
        assert.deepEqual(got.players, mine.players,
                         `Swift and TS disagree about ${JSON.stringify(name)}`);
        assert.equal(got.players[1].name, 'Bob', 'the trim desynchronized the seat after it');
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
