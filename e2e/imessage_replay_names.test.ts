/* =============================================================================
 * The iMessage replay link carries the table's nicknames - proved end to end
 * =============================================================================
 * Owner, 1.0(38): "Replay code in iMessage does not save nicknames! It should".
 *
 * The names channel is not new. A replay code is `<base32 moves>` optionally
 * followed by `-<base32 extras>`, and the extras blob has carried seat names
 * since it was invented (server/api/common/replay/extras.ts); the website's own
 * finished games have written it forever (finalizeEndedGame). The phone was the
 * one producer that emitted a bare moves code, so a game between friends
 * replayed on foolish.cards as "P1" beating "P2".
 *
 * WHY THIS TEST SPAWNS A SWIFT COMPILER. The encoder is now sdk/swift/
 * ReplayExtras.swift and the decoder is TypeScript. Two tests - one asserting
 * the Swift side emits bytes it believes are right, one asserting the TS side
 * parses bytes it wrote itself - would both pass while the two disagreed about,
 * say, where the version byte goes or how a 49-byte name is trimmed. So this
 * compiles the REAL Swift encoder (its own source, not a copy: Base32.swift was
 * split out of MessageEnvelope.swift precisely so the codec has no CFoolish
 * dependency and can build standalone) and feeds its stdout to the REAL
 * decodeExtras the replay screen calls.
 *
 * The strongest assertion here is not "the names come back" but "the Swift blob
 * is byte-identical to the TypeScript encoder's blob for the same input". That
 * is what stops the two implementations drifting on the trimming rule, which is
 * the only part of this format with any judgement in it.
 *
 * Pure test - needs no Postgres. Skips when there is no Swift toolchain (Linux
 * CI); it is a Mac-side guard on a Mac-side file.
 * ========================================================================== */

import { test, after } from 'node:test';
import assert from 'node:assert/strict';
import { execFileSync, spawnSync } from 'node:child_process';
import { mkdtempSync, writeFileSync, rmSync } from 'node:fs';
import { join } from 'node:path';
import { tmpdir } from 'node:os';

import {
  encodeExtras, decodeExtras, splitReplayCode,
} from '../server/api/common/replay/extras.ts';
import { codeToGame } from '../server/api/common/replay/codec.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; }

const REPO = new URL('..', import.meta.url).pathname;

/** Is there a Swift compiler on this machine at all? */
const hasSwift = (() => {
  const r = spawnSync('swiftc', ['--version'], { stdio: 'ignore' });
  return r.status === 0;
})();

/* The one-time build: the two production sources plus a stdin->stdout driver.
 * `moves` is passed in rather than hard-coded so a test can hand it a real
 * code and check the moves half survived untouched. */
const DRIVER = `
import Foundation

struct Input: Decodable { let moves: String; let names: [String] }
let raw = FileHandle.standardInput.readDataToEndOfFile()
let input = try! JSONDecoder().decode(Input.self, from: raw)
print(ReplayExtras.code(moves: input.moves, names: input.names))
`;

let binary: string | null = null;
let workdir: string | null = null;

function encoder(): string {
  if (binary) return binary;
  workdir = mkdtempSync(join(tmpdir(), 'foolish_replay_names_'));
  const main = join(workdir, 'main.swift');
  writeFileSync(main, DRIVER);
  const out = join(workdir, 'encode_names');
  execFileSync('swiftc', [
    join(REPO, 'sdk/swift/Base32.swift'),
    join(REPO, 'sdk/swift/ReplayExtras.swift'),
    main, '-o', out,
  ], { stdio: ['ignore', 'pipe', 'pipe'] });
  binary = out;
  return out;
}

/** Run the real Swift encoder; returns the full URL code it would put in a link. */
function swiftCode(moves: string, names: string[]): string {
  return execFileSync(encoder(), [], {
    input: JSON.stringify({ moves, names }),
    encoding: 'utf8',
  }).trim();
}

after(() => { if (workdir) rmSync(workdir, { recursive: true, force: true }); });

/* A real v6 moves code is not needed to test the names channel - the two halves
 * are independent by construction, which is the point of the dash - but using a
 * plausible one keeps the "moves half untouched" assertion honest about shape. */
const MOVES = 'ABCDEFGHIJKLMNOPQRSTUVWXYZ234567ABCDEFG';

test('the Swift encoder writes names the web replay screen reads back', { skip: !hasSwift && 'no swiftc on this machine' }, () => {
  const names = ['Sveta', 'Misha'];
  const code = swiftCode(MOVES, names);

  const { moves, extras } = splitReplayCode(code);
  assert.equal(moves, MOVES, 'the moves half must come through byte for byte');
  assert.ok(extras, 'a named table produced no extras segment');

  // The decoder the replay page actually calls, with the player count it takes
  // from the decoded moves.
  const back = decodeExtras(extras!, names.length, /*moveCount*/ 0);
  assert.deepEqual(back.names, names, 'the nicknames did not survive the round trip');
  assert.equal(back.startTime, null, 'names-only blob claims no timing');
});

test('Swift and TypeScript encode the same roster to the same bytes', { skip: !hasSwift && 'no swiftc on this machine' }, () => {
  // Free-form user text: Latin, Cyrillic, CJK, an emoji with a modifier, and a
  // seat nobody ever named.
  const rosters = [
    ['Sveta', 'Misha'],
    ['Владимир', 'Ольга', 'Пётр', 'Анна'],
    ['さくら', 'Ünïcodé', ''],
    ['🤡', 'A👍🏽B', 'x'],
    ['solo', '', '', ''],
  ];
  for (const names of rosters) {
    const mine = splitReplayCode(swiftCode(MOVES, names)).extras;
    const theirs = encodeExtras(names, null);
    assert.equal(mine, theirs, `Swift and TS disagree on ${JSON.stringify(names)}`);
    assert.deepEqual(decodeExtras(mine!, names.length, 0).names, names);
  }
});

test('an over-budget nickname is trimmed the same way on both sides', { skip: !hasSwift && 'no swiftc on this machine' }, () => {
  // extras.ts caps a name at 48 UTF-8 bytes. FMSG allows 64 (MSG_MAX_NAME) and
  // the nickname field allows 16 characters, so this is reachable from the app:
  // 16 emoji is 64 bytes on the wire and must lose the tail here.
  const long = '🤡'.repeat(16);                  // 64 bytes
  const cyrillic = 'Владимир'.repeat(4);         // 64 bytes
  // …and the case that separates the two plausible trimming rules. A thumbs-up
  // with a skin tone is ONE Character and TWO Unicode scalars, and the 48-byte
  // line falls in the middle of one here (1 + 7x8 = 57 bytes). Trimming whole
  // Characters, which is the Swift-shaped instinct, drops 8 bytes at a time and
  // lands somewhere the TS encoder - which trims code points, `Array.from` -
  // never lands. Same bytes or nothing.
  const split = 'A' + '👍🏽'.repeat(7);        // 57 bytes, 15 scalars, 8 Characters
  for (const name of [long, cyrillic, split]) {
    const names = [name, 'Bob'];
    const mine = splitReplayCode(swiftCode(MOVES, names)).extras;
    assert.equal(mine, encodeExtras(names, null), `trim differs for ${name}`);

    const back = decodeExtras(mine!, 2, 0).names!;
    assert.ok(Buffer.byteLength(back[0], 'utf8') <= 48, 'trimmed name is still over budget');
    assert.ok(name.startsWith(back[0]), 'the trim kept a prefix of the real name');
    assert.ok(back[0].length > 0, 'the trim ate the whole name');
    assert.equal(back[1], 'Bob', 'the trim desynchronized the seat after it');
  }
});

test('a NUL inside a nickname cannot terminate a name early', { skip: !hasSwift && 'no swiftc on this machine' }, () => {
  // NUL is the field terminator. Both encoders strip it rather than escaping it;
  // if one of them let it through, every seat after it would shift by one.
  const names = ['a\u0000b', 'Bob', 'Cyd'];
  const extras = splitReplayCode(swiftCode(MOVES, names)).extras;
  assert.equal(extras, encodeExtras(names, null));
  assert.deepEqual(decodeExtras(extras!, 3, 0).names, ['ab', 'Bob', 'Cyd']);
});

test('a table where nobody is named emits the old bare code', { skip: !hasSwift && 'no swiftc on this machine' }, () => {
  // Nothing to say: an all-empty roster decodes to the "P1"/"P2" the reader
  // already shows, so the segment would be bytes for nothing - and a link that
  // is byte-identical to what previous builds emitted is one less thing to
  // explain. Same for a game with no seat count to pad to.
  for (const names of [[], ['', ''], ['', '', '', '']]) {
    const code = swiftCode(MOVES, names);
    assert.equal(code, MOVES, `an anonymous table wrote an extras segment: ${code}`);
    assert.equal(splitReplayCode(code).extras, null);
  }
});

test('the extras segment never disturbs the moves the link decodes to', { skip: !hasSwift && 'no swiftc on this machine' }, () => {
  // Constraint 4: existing codes keep working, and a new code's game is the
  // same game the bare code named. urlToGame/codeToGame cut at the dash.
  const bare = 'MZXW6YTBOI7654321ABCDEFG';
  const withNames = swiftCode(bare, ['Ann', 'Bob']);
  assert.notEqual(withNames, bare, 'this test would prove nothing without a segment');
  assert.equal(codeToGame(splitReplayCode(withNames).moves), codeToGame(bare),
    'the game behind the link changed when names were added');
});
