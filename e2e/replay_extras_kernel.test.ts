/* =============================================================================
 * The replay extras blob is the kernel's, and it is still the shipped format
 * =============================================================================
 * #113. The nicknames and per-move timing behind the dash in a share link used
 * to be encoded in three places and two languages - TypeScript on the server
 * and in the browser, Swift in the iMessage extension - with a parity test
 * holding the two implementations together. A copy kept in step by a test can
 * only ever say "the copy agrees"; it can never say "the answer is right".
 * There is one implementation now, c/src/replay_extras.c, and every host calls
 * it (the web and the server through wasm_replay_extras_*, the phone through
 * fio_replay_extras_link).
 *
 * WHICH MAKES THIS FILE'S JOB A DIFFERENT ONE. Nothing here proves two
 * implementations agree. What it proves is that the kernel speaks the format
 * that is already on the wire:
 *
 *   1. every blob a shipped build ever wrote still decodes, and to the same
 *      answer - replay links are shared and stored, so this is load-bearing;
 *   2. every blob the kernel writes is byte-identical to what the shipped
 *      format wrote for the same input, so a link made tomorrow reads the same
 *      as one made yesterday;
 *   3. the kernel round-trips its own output, including the parts with
 *      judgement in them (the 48-byte name budget trimmed by whole code points,
 *      the NUL that is a terminator and not a character, the seat count that
 *      lives in the moves and not in the blob);
 *   4. a malformed blob throws and never takes the replay down with it.
 *
 * (1) and (2) are measured against e2e/helpers/extras_format_v2.ts - the
 * TypeScript that WAS the format until this change, frozen there and imported
 * by nothing else. That is an oracle, not a second implementation: it is the
 * shipped bytes written down, and if it and the kernel ever disagree, the
 * kernel is what changed.
 *
 * NO FROZEN CODES. Every blob in here is generated, from a seeded PRNG and from
 * real games - pinning a base32 string as a fixture is how this repo has fooled
 * itself before.
 *
 * MUTATIONS RUN (each reverted):
 *   - c/src/replay_extras.c name_bytes: trim at a flat 48 bytes instead of
 *     backing off to a code-point boundary -> "the kernel writes the shipped
 *     format's bytes" fails on the emoji rosters.
 *   - c/src/replay_extras.c name_bytes: keep the NUL instead of stripping it
 *     -> "a NUL in a nickname cannot terminate a name early" fails.
 *   - c/src/replay_extras.c encode: write version 3 -> both compatibility
 *     tests fail (the frozen decoder refuses it, the byte diff differs).
 *   - c/src/replay_extras.c quantize_gap: drop the sqrt from the first bucket
 *     edge -> "the kernel writes the shipped format's bytes" fails on timing.
 *   - c/src/replay_extras.c decode: skip the n_gaps < move_count check ->
 *     "a corrupt gap count is refused" fails.
 *
 * Pure test - needs no Postgres.
 * ========================================================================== */

import { test } from 'node:test';
import { bytesToBigint } from '../server/api/common/replay/codec.ts';
import assert from 'node:assert/strict';

import {
  frozenEncodeExtras, frozenDecodeExtras,
} from './helpers/extras_format_v2.ts';
import {
  encodeExtras, encodeExtrasBytes, encodeExtrasFromGaps, decodeExtras,
  splitReplayCode, joinReplayCode,
} from '../server/api/common/replay/extras.ts';
import { codeToGame } from '../server/api/common/replay/codec.ts';
import { kernelReplayExtrasEncode, kernelReplayExtrasDecode, kernelReplayLink, ensureBotsAsync, kernelB32Encode, kernelB32Decode } from '../sdk/ts/wasm/bots.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; }

/* ------------------------------- the corpus ------------------------------- */

/** xorshift64. Seeded, so the corpus is the same corpus on every machine. */
function prng(seed: bigint) {
  const M = (1n << 64n) - 1n;
  let s = seed;
  return () => {
    s ^= (s << 13n) & M; s ^= s >> 7n; s ^= (s << 17n) & M; s &= M;
    return Number(s >> 11n) / 9007199254740992;
  };
}

/* Free-form user text: Latin, Cyrillic, CJK, Hangul, an emoji with a skin-tone
 * modifier, a seat nobody named, and three names that sit over the 48-byte
 * budget in three different ways. The last of those is the case that separates
 * the two plausible trimming rules: a thumbs-up with a skin tone is ONE
 * grapheme and TWO code points, and the 48-byte line falls in the middle of one
 * here (1 + 7x8 = 57 bytes). Trimming graphemes drops 8 bytes at a time and
 * lands somewhere trimming code points never does. */
const NAME_POOL = [
  '', 'Sveta', 'Misha', 'Владимир', 'さくら', 'Ünïcodé', '한국이름', 'ВАСЯ \u{1F0CF}',
  '\u{1F921}', 'A\u{1F44D}\u{1F3FD}B', 'a\u0000b',
  '\u{1F921}'.repeat(16),                    // 64 bytes
  'A' + '\u{1F44D}\u{1F3FD}'.repeat(7),      // 57 bytes, 15 code points, 8 graphemes
  'Владимир'.repeat(4),                      // 64 bytes
  'X'.repeat(60),
];

interface Case { names: string[] | null; startTime: number | null; gaps: number[] | null }

/** `n` rosters and gap lists, spanning nanoseconds to hours. */
function corpus(n: number, seed = 88172645463325252n): Case[] {
  const rnd = prng(seed);
  const rndi = (k: number) => Math.floor(rnd() * k);
  const out: Case[] = [];
  for (let i = 0; i < n; i++) {
    const nNames = rndi(9);
    const names = nNames ? Array.from({ length: nNames }, () => NAME_POOL[rndi(NAME_POOL.length)]) : null;
    let startTime: number | null = null;
    let gaps: number[] | null = null;
    if (rndi(2)) {
      startTime = 1750000000 + rndi(100000);
      // 1e-9 s (a simulation step) to 1e3 s (a coffee break), same one byte.
      const scale = Math.pow(10, rndi(13) - 9);
      gaps = Array.from({ length: rndi(40) }, () => (rndi(10) === 0 ? 0 : rnd() * 100 * scale));
    }
    out.push({ names, startTime, gaps });
  }
  return out;
}

const hex = (b: Uint8Array) => Array.from(b).map(x => x.toString(16).padStart(2, '0')).join('');

/** The gap curve is 7% coarse by design; agreement to a few ULP is exactness. */
function assertGapsAgree(got: number[], want: number[], where: string) {
  assert.equal(got.length, want.length, `${where}: gap count`);
  for (let i = 0; i < want.length; i++) {
    const tol = Math.max(Math.abs(want[i]) * 1e-12, Number.MIN_VALUE);
    assert.ok(Math.abs(got[i] - want[i]) <= tol,
      `${where}: gap ${i} got ${got[i]}, want ${want[i]}`);
  }
}

/* ------------------------------- the tests -------------------------------- */

test('every blob the shipped format ever wrote still decodes, to the same answer', async () => {
  await ensureBotsAsync();
  const cases = corpus(600);
  let withNames = 0, withTimes = 0;
  for (const c of cases) {
    const blob = frozenEncodeExtras(c.names, c.startTime, c.gaps);
    const moveCount = c.gaps ? c.gaps.length : 0;
    const playerCount = c.names ? c.names.length : 0;
    const theirs = frozenDecodeExtras(blob, playerCount, moveCount);
    const ours = kernelReplayExtrasDecode(blob, playerCount, moveCount);
    assert.deepEqual(ours.names, theirs.names, `names for ${JSON.stringify(c.names)}`);
    assert.equal(ours.startTime, theirs.startTime, 'start time');
    if (theirs.moveGaps === null) assert.equal(ours.moveGaps, null, 'no timing claimed');
    else assertGapsAgree(ours.moveGaps!, theirs.moveGaps, 'old blob');
    if (theirs.names) withNames++;
    if (theirs.moveGaps) withTimes++;
  }
  // The corpus has to actually contain both sections, or this proves half of it.
  assert.ok(withNames > 100 && withTimes > 100, `corpus is lopsided: ${withNames}/${withTimes}`);
});

test('the kernel writes the shipped format\'s bytes, for every roster and every tempo', async () => {
  await ensureBotsAsync();
  for (const c of corpus(600, 1234567891011n)) {
    const theirs = frozenEncodeExtras(c.names, c.startTime, c.gaps);
    const ours = kernelReplayExtrasEncode(c.names, c.startTime, c.gaps);
    assert.equal(hex(ours), hex(theirs),
      `bytes differ for ${JSON.stringify(c.names)} / ${c.gaps?.length ?? 0} gaps`);
  }
});

test('the kernel round-trips its own blob through the URL container', async () => {
  await ensureBotsAsync();
  const names = ['Sveta', 'Misha', '', 'Владимир'];
  const moveTimes = [1750000000, 1750000002.5, 1750000060, 1750000061];
  const extras = encodeExtras(names, moveTimes);
  const MOVES = 'MZXW6YTBOI7654321ABCDEFG';
  const full = joinReplayCode(MOVES, extras);

  const split = splitReplayCode(full);
  assert.equal(split.moves, MOVES, 'the moves half must come through byte for byte');
  assert.equal(split.extras, extras, 'the extras half must come through byte for byte');
  // A link with names names the same game as the link without them: the reader
  // cuts at the dash, so old codes and new codes decode identically.
  assert.equal(bytesToBigint(kernelB32Decode(split.moves)), bytesToBigint(kernelB32Decode(MOVES)), 'the game behind the link changed');

  const back = decodeExtras(split.extras!, names.length, moveTimes.length - 1);
  assert.deepEqual(back.names, names, 'the nicknames did not survive the round trip');
  assert.equal(back.startTime, moveTimes[0], 'start time');
  back.moveGaps!.forEach((g, i) => {
    const want = moveTimes[i + 1] - moveTimes[i];
    assert.ok(Math.abs(g - want) <= want * 0.08, `gap ${i}: got ${g}, want ${want}`);
  });

  // The raw-bytes door the server stores (game_snapshots.extras) is the same
  // blob, not a second encoding of it.
  assert.equal(hex(encodeExtrasBytes(names, moveTimes)), hex(kernelB32Decode(extras)),
    'the stored bytes and the URL segment disagree');
});

test('an over-budget nickname is cut to 48 bytes on a code-point boundary', async () => {
  await ensureBotsAsync();
  const long = '\u{1F921}'.repeat(16);                    // 64 bytes, 16 code points
  const cyrillic = 'Владимир'.repeat(4);                  // 64 bytes
  const split = 'A' + '\u{1F44D}\u{1F3FD}'.repeat(7);     // 57 bytes, mid-grapheme at 48
  for (const name of [long, cyrillic, split]) {
    const names = [name, 'Bob'];
    const back = decodeExtras(encodeExtras(names, null), 2, 0).names!;
    assert.ok(Buffer.byteLength(back[0], 'utf8') <= 48, `trimmed name is over budget: ${back[0]}`);
    assert.ok(name.startsWith(back[0]), 'the trim kept a prefix of the real name');
    assert.ok(back[0].length > 0, 'the trim ate the whole name');
    assert.ok(!back[0].includes('�'), 'the trim severed a UTF-8 sequence');
    assert.equal(back[1], 'Bob', 'the trim desynchronized the seat after it');
    // …and it is the SAME cut the format has always made.
    assert.equal(hex(kernelReplayExtrasEncode(names, null, null)),
                 hex(frozenEncodeExtras(names, null, null)), `trim differs for ${name}`);
  }
});

test('a NUL inside a nickname cannot terminate a name early', async () => {
  await ensureBotsAsync();
  // NUL is the field terminator. It is stripped rather than escaped; if it were
  // let through, every seat after it would shift by one.
  const names = ['a b', 'Bob', 'Cyd'];
  const blob = kernelReplayExtrasEncode(names, null, null);
  assert.equal(hex(blob), hex(frozenEncodeExtras(names, null, null)));
  assert.deepEqual(kernelReplayExtrasDecode(blob, 3, 0).names, ['ab', 'Bob', 'Cyd']);
});

test('the seat count comes from the moves, not from the blob', async () => {
  await ensureBotsAsync();
  // A roster is dense and seat-ordered and carries no count of its own, so the
  // reader has to be told how wide the table is. Ask for fewer seats than were
  // written and the parse stops early; ask for more and it runs out of names.
  const names = ['Ann', 'Bob', 'Cyd', 'Dee'];
  const blob = kernelReplayExtrasEncode(names, null, null);
  assert.deepEqual(kernelReplayExtrasDecode(blob, 4, 0).names, names);
  assert.deepEqual(kernelReplayExtrasDecode(blob, 2, 0).names, ['Ann', 'Bob']);
  assert.throws(() => kernelReplayExtrasDecode(blob, 5, 0), /unterminated name/,
    'a table wider than the roster must be refused, not padded with garbage');
});

test('a malformed blob is refused, never half-read', async () => {
  await ensureBotsAsync();
  assert.throws(() => kernelReplayExtrasDecode(new Uint8Array([2]), 2, 0), /truncated header/);
  assert.throws(() => kernelReplayExtrasDecode(new Uint8Array([]), 2, 0), /truncated header/);
  assert.throws(() => kernelReplayExtrasDecode(new Uint8Array([7, 1, 0, 0]), 2, 0),
    /unsupported version/, 'a future version must be refused, not guessed at');
  assert.throws(() => kernelReplayExtrasDecode(new Uint8Array([2, 2, 64, 0]), 0, 3),
    /truncated time header/);
  // A corrupt gap count: three moves' worth of gaps asked to cover five hundred.
  const short = kernelReplayExtrasEncode(['A', 'B'], 1000, [1000, 1000, 1000]);
  assert.throws(() => kernelReplayExtrasDecode(short, 2, 500), /gaps for .* moves/i);

  // And the property the replay screen depends on: whatever arrives, the reader
  // either answers or throws - it never hangs and never returns a partial board.
  const rnd = prng(424242n);
  for (let i = 0; i < 2000; i++) {
    const n = Math.floor(rnd() * 40);
    const junk = new Uint8Array(n);
    for (let k = 0; k < n; k++) junk[k] = Math.floor(rnd() * 256);
    try {
      const got = kernelReplayExtrasDecode(junk, Math.floor(rnd() * 9), Math.floor(rnd() * 40));
      if (got.names) for (const nm of got.names) assert.equal(typeof nm, 'string');
      if (got.moveGaps) for (const g of got.moveGaps) assert.ok(Number.isFinite(g), `gap ${g}`);
    } catch (e) {
      assert.match((e as Error).message, /^extras: /, 'an extras failure must say so');
    }
  }
});

test('a names-only blob claims no timing, and a timing-only blob claims no names', async () => {
  await ensureBotsAsync();
  const namesOnly = decodeExtras(encodeExtras(['Ann', 'Bob'], null), 2, 0);
  assert.deepEqual(namesOnly.names, ['Ann', 'Bob']);
  assert.equal(namesOnly.startTime, null, 'names-only blob claims timing');
  assert.equal(namesOnly.moveGaps, null, 'names-only blob claims gaps');

  const timesOnly = decodeExtras(encodeExtrasFromGaps(null, 1750000000, [1, 2, 3]), 4, 3);
  assert.equal(timesOnly.names, null, 'timing-only blob claims names');
  assert.equal(timesOnly.startTime, 1750000000);
  assert.equal(timesOnly.moveGaps!.length, 3);
});

test('the time curve holds from 1ns to a week, one byte per move either way', async () => {
  await ensureBotsAsync();
  for (const scale of [1e-9, 1e-6, 1e-3, 1, 3600, 86400 * 7]) {
    const raw = [1, 2.5, 7, 0.3, 40, 12, 0.9, 100];
    const gaps = raw.map(r => r * scale);
    const blob = kernelReplayExtrasEncode(null, 1750000000, gaps);
    // 2-byte header + 1 scale byte + 5 start bytes + one byte per move, whatever
    // the tempo. That size invariance is the whole point of the stored unit.
    assert.equal(blob.length, 2 + 6 + raw.length, `scale ${scale}: wrong blob size`);
    const back = kernelReplayExtrasDecode(blob, 0, raw.length);
    back.moveGaps!.forEach((g, i) => {
      const want = raw[i] * scale;
      assert.ok(Math.abs(g - want) <= want * 0.08, `scale ${scale}: gap ${i} got ${g}, want ${want}`);
    });
    // and byte for byte what the format has always written for these gaps
    assert.equal(hex(blob), hex(frozenEncodeExtras(null, 1750000000, gaps)));
  }
});

test('the kernel writes the whole link, and an anonymous table gets the bare one', async () => {
  await ensureBotsAsync();
  const MOVES = 'MZXW6YTBOI7654321ABCDEFG';
  const bare = `https://foolish.cards/${MOVES}`;
  // A link is the prefix, the code, and - only when somebody is named - a dash
  // and the extras segment. The bare form is what every build before names
  // emitted, so an anonymous table must still produce it character for
  // character; anything else silently changes what people already have.
  assert.equal(kernelReplayLink(MOVES, []), bare);
  assert.equal(kernelReplayLink(MOVES, ['', '', '']), bare, 'an anonymous table wrote a segment');
  assert.equal(kernelReplayLink(MOVES, ['\u0000', '']), bare, 'a name of NULs is a name of nothing');

  // A long game's v6 code runs to tens of KB of base32; the link builder must
  // carry it whole rather than through some fixed buffer of its own.
  const long = 'ABCDEFGHIJKLMNOPQRSTUVWXYZ234567'.repeat(600);   // 19,200 chars
  assert.equal(kernelReplayLink(long, []), `https://foolish.cards/${long}`);
  assert.ok(kernelReplayLink(long, ['Ann', 'Bo']).startsWith(`https://foolish.cards/${long}-`));

  const named = kernelReplayLink(MOVES, ['Sveta', '', 'Владимир']);
  assert.ok(named.startsWith(bare + '-'), `the roster disturbed the link: ${named}`);
  const segment = named.slice(bare.length + 1);
  // …and the segment is exactly the codec's blob for that roster, base32'd -
  // the link builder adds nothing of its own.
  assert.equal(segment, kernelB32Encode(kernelReplayExtrasEncode(['Sveta', '', 'Владимир'], null, null)));
  // The reader cuts at the dash, so the game behind a named link is the game
  // behind the bare one.
  const { moves } = splitReplayCode(named.slice('https://foolish.cards/'.length));
  assert.equal(bytesToBigint(kernelB32Decode(moves)), bytesToBigint(kernelB32Decode(MOVES)));
});

test('the base32 the URL carries survives the trip in both directions', async () => {
  await ensureBotsAsync();
  for (const c of corpus(120, 987654321n)) {
    const bytes = kernelReplayExtrasEncode(c.names, c.startTime, c.gaps);
    assert.equal(hex(kernelB32Decode(kernelB32Encode(bytes))), hex(bytes),
      'base32 is not the identity on an extras blob');
  }
});
