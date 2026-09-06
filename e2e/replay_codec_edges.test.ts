// The replay round-trip test covers the happy path of the rANS codec; these
// exercise the pure codec/extras utilities around it that the round-trip never
// reaches: hex (pg-bytea) conversion, the path-segment classifier, the URL
// wrapper, the names-only extras blob, and the corrupt-extras guard.
//
// Pure test — needs no Postgres.

import { test } from 'node:test';
import assert from 'node:assert/strict';

import {
  bytesToHex, hexToBytes, classifyPathSegment, gameToUrl, urlToGame,
  URL_PREFIX, codeToGame,
} from '../server/api/common/replay/codec.ts';
import {
  encodeExtras, decodeExtras, splitReplayCode, joinReplayCode,
} from '../server/api/common/replay/extras.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; }

test('bytesToHex / hexToBytes round-trip, with and without the pg \\x prefix', () => {
  const bytes = new Uint8Array([0x00, 0x0a, 0xff, 0x42, 0x7f]);
  const hex = bytesToHex(bytes);              // bytesToHex always emits the "\x" prefix
  assert.ok(hex.startsWith('\\x'), 'bytesToHex emits a pg-bytea \\x prefix');
  assert.deepEqual(hexToBytes(hex), bytes, 'a \\x-prefixed hex round-trips');
  assert.deepEqual(hexToBytes(hex.slice(2)), bytes, 'a bare hex string decodes the same');
});

test('classifyPathSegment splits legacy shortcodes from long replay codes', () => {
  assert.equal(classifyPathSegment('abc123'), 'shortcode', 'short (<=7) is a legacy shortcode');
  assert.equal(classifyPathSegment('1234567'), 'shortcode', 'exactly 7 is still a shortcode');
  assert.equal(classifyPathSegment('12345678'), 'replay', '8+ chars is a replay code');
});

test('gameToUrl / urlToGame wrap and unwrap a moves bigint', () => {
  for (const x of [0n, 1n, 123456789n, 2n ** 40n + 7n]) {
    const url = gameToUrl(x);
    assert.ok(url.includes(URL_PREFIX), 'url carries the public prefix');
    assert.equal(urlToGame(url), x, `round-trips ${x}`);
    // The bare code (prefix stripped) also decodes via codeToGame.
    const bare = url.slice(url.indexOf(URL_PREFIX) + URL_PREFIX.length);
    assert.equal(codeToGame(bare), x, 'bare code decodes to the same bigint');
  }
});

// The link a person pastes is the one a browser hands them - https://, a real
// host, maybe a tracking query - not the printed WWW.FOOLISH.CARDS/ form. Every
// letter of `https` is in the base32 alphabet, so a prefix that is not
// recognised does not fail: it silently names a DIFFERENT game, which then dies
// deep in the kernel as "unsupported replay format version N" and sends the
// reader hunting for a codec bug that is not there.
test('urlToGame reads the link forms a person actually pastes', () => {
  const x = 2n ** 61n + 12345n;
  const code = gameToUrl(x).slice(URL_PREFIX.length);
  const forms = [
    code,
    `WWW.FOOLISH.CARDS/${code}`,
    `https://foolish.cards/${code}`,
    `https://www.foolish.cards/${code}`,
    `http://foolish.cards/${code}`,
    `https://foolish.cards/${code}/`,
    `foolish.cards/${code}`,
    `https://foolish.cards/${code}?utm_source=imessage`,
    `https://foolish.cards/${code}#top`,
    `https://foolish.cards/${code}-SOMEEXTRAS`,
    `  https://foolish.cards/${code}\n`,
  ];
  for (const pasted of forms) {
    assert.equal(urlToGame(pasted), x, `pasted link named a different game: ${JSON.stringify(pasted)}`);
  }
});

test('urlToGame refuses input that is not a replay code, and says so', () => {
  // base32Decode ignores stray characters, so without a guard each of these
  // decodes to SOME game and fails later under a name that is about the codec.
  for (const junk of [
    '',
    'WWW.FOOLISH.CARDS/',
    'https://foolish.cards/',
    'https://foolish.cards/MZXW6YTB0189',   // 0/1/8/9 are not in the alphabet
    'MZXW6YTB!!',
    'https://foolish.cards/hello world!',
  ]) {
    assert.throws(
      () => urlToGame(junk),
      /not a replay code/,
      `non-code input decoded instead of refusing: ${JSON.stringify(junk)}`,
    );
  }
});

test('encodeExtras with names but no move-times yields a decodable names-only blob', () => {
  const names = ['Alice', 'Bob', 'Céline'];
  const extras = encodeExtras(names, null);
  const back = decodeExtras(extras, names.length, /*moveCount*/ 0);
  assert.deepEqual(back.names, names, 'names survive with no timing data');
  assert.ok(back.moveGaps === null || back.moveGaps.length === 0, 'no move gaps decoded');
});

test('splitReplayCode / joinReplayCode are inverse', () => {
  assert.deepEqual(splitReplayCode(joinReplayCode('MOVES', 'EXTRAS')), { moves: 'MOVES', extras: 'EXTRAS' });
  // A moves-only code has no extras half.
  const joined = joinReplayCode('MOVESONLY', null);
  assert.equal(splitReplayCode(joined).extras, null, 'no extras half when omitted');
});

test('decodeExtras rejects a blob whose gap count cannot cover the move count', () => {
  const extras = encodeExtras(['A', 'B'], [1000, 2000, 3000]);  // 3 moves -> 2 gaps
  assert.throws(() => decodeExtras(extras, 2, /*moveCount*/ 500), /gaps for .* moves/i,
    'a wildly-too-large move count is rejected as corrupt');
});
