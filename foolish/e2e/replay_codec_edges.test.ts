// The replay round-trip test covers the happy path of the rANS codec; these
// exercise the pure codec/extras utilities around it that the round-trip never
// reaches: hex (pg-bytea) conversion, the path-segment classifier, the URL
// wrapper, the names-only extras blob, and the corrupt-extras guard.
//
// Pure test — needs no Postgres.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
  kernelB32Encode, kernelB32Decode, kernelReplayLink, REPLAY_LINK, kernelReplayLinkParse,
} from '../sdk/ts/wasm/bots.ts';

import {
  bytesToHex, hexToBytes, classifyPathSegment, bytesToBigint, bigintToBytes,
} from '../server/api/common/replay/codec.ts';
import {
  encodeExtras, decodeExtras, splitReplayCode, joinReplayCode,
} from '../server/api/common/replay/extras.ts';

// A pasted link as the moves bigint: the kernel strips and refuses
// (replay_link_parse), the kernel decodes (replay_b32_decode). Composed here
// rather than in the product, which never needed the bigint form.
const urlToGame = (url: string): bigint =>
    bytesToBigint(kernelB32Decode(kernelReplayLinkParse(url)));

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

// The link is BUILT by the kernel now (replay_extras_link_styled, through
// kernelReplayLink) in two forms, and read back here. Both must name the same
// game: they are one link, and the uppercase scheme-less one exists only so a
// QR stays in alphanumeric mode.
test('both kernel link styles wrap a moves bigint that urlToGame unwraps', () => {
  for (const x of [0n, 1n, 123456789n, 2n ** 40n + 7n]) {
    const code = kernelB32Encode(bigintToBytes(x));
    const url = kernelReplayLink(code, [], REPLAY_LINK.url);
    const qr = kernelReplayLink(code, [], REPLAY_LINK.qr);
    assert.ok(url.startsWith('https://foolish.cards/'), `https form: ${url}`);
    assert.ok(qr.startsWith('WWW.FOOLISH.CARDS/'), `qr form: ${qr}`);
    assert.ok(/^[0-9A-Z $%*+\-./:]+$/.test(qr), `the QR form must stay in QR alphanumeric mode: ${qr}`);
    assert.equal(urlToGame(url), x, `https form round-trips ${x}`);
    assert.equal(urlToGame(qr), x, `qr form round-trips ${x}`);
    assert.equal(bytesToBigint(kernelB32Decode(code)), x, 'bare code decodes to the same bigint');
  }
});

// The link a person pastes is the one a browser hands them - https://, a real
// host, maybe a tracking query - as well as the QR's WWW.FOOLISH.CARDS/ form.
// Every
// letter of `https` is in the base32 alphabet, so a prefix that is not
// recognised does not fail: it silently names a DIFFERENT game, which then dies
// deep in the kernel as "unsupported replay format version N" and sends the
// reader hunting for a codec bug that is not there.
test('urlToGame reads the link forms a person actually pastes', () => {
  const x = 2n ** 61n + 12345n;
  const code = kernelB32Encode(bigintToBytes(x));
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
  // The kernel's decoder ignores stray characters, so without a guard each of these
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
