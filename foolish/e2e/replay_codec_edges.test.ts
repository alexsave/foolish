// The TypeScript a page that READS a share link wraps around the kernel, on the
// module the browser loads (web.wasm through sdk/ts/wasm/bots.ts): the
// path-segment classifier and the bigint form (server/api/common/replay_codec.ts),
// the split at the dash and the extras decode (server/api/common/replay_extras.ts),
// and the link builder's bridge with its refusals turned into the kernel's own
// sentences.
//
// What the kernel answers is c/tests/tests.c's business: the link forms a person
// pastes and the input refused as no code (test_link_parse), both link styles
// (test_extras_link_is_the_code_and_what_the_roster_says), a long code carried
// whole (test_extras_link_carries_a_long_code_whole), the extras codec over a
// seeded corpus of rosters and tempos (test_extras_corpus_round_trips), the seat
// count (test_extras_seat_count_comes_from_the_moves) and every refusal
// (test_extras_refuses_what_it_cannot_read, test_extras_decode_answers_or_refuses_any_bytes).

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { ensureBotsAsync, kernelB32Encode, kernelB32Decode, kernelReplayLink, kernelReplayExtrasDecode } from '../sdk/ts/wasm/bots.ts';
import { REPLAY_LINK_STYLE_URL, REPLAY_LINK_STYLE_QR } from '../sdk/ts/gen/msg_layout.bots.ts';

import {
  classifyPathSegment, bytesToBigint, bigintToBytes,
} from '../server/api/common/replay_codec.ts';
import { decodeExtras, splitReplayCode, joinReplayCode } from '../server/api/common/replay_extras.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; }

const PREFIX = 'https://foolish.cards/';
const MOVES = 'MZXW6YTBOI7654321ABCDEFG';

test('classifyPathSegment splits legacy shortcodes from long replay codes', () => {
  assert.equal(classifyPathSegment('abc123'), 'shortcode', 'short (<=7) is a legacy shortcode');
  assert.equal(classifyPathSegment('1234567'), 'shortcode', 'exactly 7 is still a shortcode');
  assert.equal(classifyPathSegment('12345678'), 'replay', '8+ chars is a replay code');
  assert.equal(classifyPathSegment('0123456789ab'), 'shortcode', 'a 12-hex game id (the width since L3) is a game');
  assert.equal(classifyPathSegment('0123456789AB'), 'shortcode', 'in any case (the lobby QR is upper-cased)');
  assert.equal(classifyPathSegment('0123456789ag'), 'replay', '12 characters that are not all hex are a replay code');
  assert.equal(classifyPathSegment('0123456789abc'), 'replay', '13 hex is not a game id');
});

// The link is BUILT by the kernel (replay_extras_link_styled, through
// kernelReplayLink) in two forms. Both carry the same code: they are one link,
// and the uppercase scheme-less one exists only so a QR stays in alphanumeric mode.
test('both link styles carry the code of a moves bigint', async () => {
  await ensureBotsAsync();
  for (const x of [0n, 1n, 123456789n, 2n ** 40n + 7n, 2n ** 61n + 12345n]) {
    const code = kernelB32Encode(bigintToBytes(x));
    assert.equal(bytesToBigint(kernelB32Decode(code)), x, 'the bare code decodes to the same bigint');
    assert.equal(kernelReplayLink(code, [], REPLAY_LINK_STYLE_URL), PREFIX + code, 'the https form');
    const qr = kernelReplayLink(code, [], REPLAY_LINK_STYLE_QR);
    assert.equal(qr, 'WWW.FOOLISH.CARDS/' + code, 'the QR form');
    assert.ok(/^[0-9A-Z $%*+\-./:]+$/.test(qr), `the QR form must stay in QR alphanumeric mode: ${qr}`);
  }
});

test('an anonymous table gets the bare link, and a named one rides behind the dash', async () => {
  await ensureBotsAsync();
  const bare = PREFIX + MOVES;
  assert.equal(kernelReplayLink(MOVES, []), bare);
  assert.equal(kernelReplayLink(MOVES, ['', '', '']), bare, 'an anonymous table wrote a segment');
  assert.equal(kernelReplayLink(MOVES, ['\u0000', '']), bare, 'a name of NULs is a name of nothing');

  // A long game's code runs to tens of KB of base32, and the bridge hands it to
  // the kernel whole.
  const long = 'ABCDEFGHIJKLMNOPQRSTUVWXYZ234567'.repeat(600);   // 19,200 chars
  assert.equal(kernelReplayLink(long, []), PREFIX + long);

  // The container a reading page takes apart: the moves half byte for byte, the
  // game behind it unchanged by the names, the names out of the other half.
  const names = ['Sveta', '', 'Владимир', 'a\u0000b'];
  const named = kernelReplayLink(MOVES, names);
  assert.ok(named.startsWith(bare + '-'), `the roster disturbed the link: ${named}`);
  const split = splitReplayCode(named.slice(PREFIX.length));
  assert.equal(split.moves, MOVES, 'the moves half must come through byte for byte');
  assert.equal(joinReplayCode(split.moves, split.extras), named.slice(PREFIX.length), 'split and join are inverse');
  assert.equal(bytesToBigint(kernelB32Decode(split.moves)), bytesToBigint(kernelB32Decode(MOVES)),
    'the game behind a named link is the game behind the bare one');
  const back = decodeExtras(split.extras!, names.length, 0);
  assert.deepEqual(back.names, ['Sveta', '', 'Владимир', 'ab'], 'every seat reads back its name, NUL stripped');
  assert.equal(back.startTime, null, 'a names-only blob claims no timing');
  assert.equal(back.moveGaps, null, 'and no gaps');
});

test('splitReplayCode / joinReplayCode are inverse', () => {
  assert.deepEqual(splitReplayCode(joinReplayCode('MOVES', 'EXTRAS')), { moves: 'MOVES', extras: 'EXTRAS' });
  // A moves-only code has no extras half.
  const joined = joinReplayCode('MOVESONLY', null);
  assert.equal(splitReplayCode(joined).extras, null, 'no extras half when omitted');
});

// The bridge turns each refusal into the kernel's own sentence (c/errtext,
// generated into sdk/ts/gen/errtext), which is what a reader that catches by
// message matches on.
test('a malformed extras blob throws the kernel\'s sentence for it', async () => {
  await ensureBotsAsync();
  const refuses = (blob: number[], players: number, moves: number, message: RegExp) =>
    assert.throws(() => kernelReplayExtrasDecode(new Uint8Array(blob), players, moves), message);
  refuses([], 2, 0, /extras: truncated header/);
  refuses([2], 2, 0, /extras: truncated header/);
  refuses([7, 1, 0, 0], 2, 0, /extras: unsupported version/);
  refuses([2, 1, 0x41, 0x42], 2, 0, /extras: unterminated name/);
  refuses([2, 2, 64, 0], 0, 3, /extras: truncated time header/);
  // A times section of three gaps asked to cover five hundred moves.
  refuses([2, 2, 64, 0, 0, 0, 0, 0, 1, 1, 1], 0, 500, /gaps for .* moves/i);
});
