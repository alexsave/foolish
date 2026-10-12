/* =============================================================================
 * One game_snapshots row, read into its line of the match history
 * =============================================================================
 * The history screen's loader decoded each row inline, inside an effect, where
 * only a full page render could reach it. The decode is a function now
 * (src/components/matchHistoryRow.ts decodeHistoryRow), and the full render in
 * e2e/ui_dom_snapshots.test.ts still holds the screen to its golden. These
 * cases hold what that golden cannot show: every seat's placement, the rows a
 * player is not in, and the rows and extras that do not decode.
 *
 * The games are the frozen seeded ones (e2e/helpers/seeded_codes.ts), and what
 * a line must say about one is asked of the kernel's own summary of it.
 * ========================================================================== */

import { test, before } from 'node:test';
import assert from 'node:assert/strict';
import { decodeHistoryRow, type HistorySnapshotRow } from '../src/components/matchHistoryRow.ts';
import { ensureBotsAsync, kernelB32Decode, replaySummary } from '../sdk/ts/wasm/bots.ts';
import { seededCode } from './helpers/seeded_codes.ts';
import { encodeExtrasBytes } from './helpers/extras_encode.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; console.error = () => {}; }
// A zone that is not UTC, so a stamp read as local time cannot pass for one read as UTC.
process.env.TZ = 'America/New_York';

before(async () => { await ensureBotsAsync(); });

const hex = (b: Uint8Array) => Array.from(b, (x) => x.toString(16).padStart(2, '0')).join('');
const ids = (np: number) => Array.from({ length: np }, (_, s) => `u-${s}`);

/** The row the server keeps for the frozen `np`:`seed` game, with `names` and a move every `gap` seconds. */
function row(np: number, seed: number, names: string[] | null, gap = 7): { row: HistorySnapshotRow; code: Uint8Array } {
    const code = seededCode(np, seed);
    const moves = replaySummary(code)!.moves;
    const times = Array.from({ length: moves + 1 }, (_, k) => 1_780_000_000 + k * gap);
    return {
        code,
        row: {
            id: `snap-${np}-${seed}`, player_ids: ids(np), moves: `\\x${hex(code)}`,
            extras: names ? `\\x${hex(encodeExtrasBytes(names, times))}` : null, created_at: '2026-09-10T12:00:00',
        },
    };
}

test('every seat of a game reads its own placement: out in order from 1, the fool last', () => {
    for (const [np, seed] of [[2, 7], [3, 41], [4, 42], [8, 43]]) {
        const { row: r, code } = row(np, seed, null);
        const summary = replaySummary(code)!;
        const ranks: number[] = [];
        for (let s = 0; s < np; s++) {
            const entry = decodeHistoryRow(r, `u-${s}`);
            assert.ok(entry, `${np}p: seat ${s} is in the game`);
            assert.equal(entry.mySeat, s);
            assert.equal(entry.playerCount, np);
            assert.equal(entry.foolSeat, summary.fool, `${np}p: the fool`);
            const out = summary.elimination.indexOf(s);
            assert.equal(entry.myRank, s === summary.fool ? np : out + 1, `${np}p: seat ${s}'s place`);
            ranks.push(entry.myRank);
        }
        assert.deepEqual(ranks.sort((a, b) => a - b), Array.from({ length: np }, (_, k) => k + 1), `${np}p: one seat per place`);
    }
});

test('a row with extras: the stored names, the game\'s length, and a code the replay screen reads back', () => {
    const names = ['Ada', 'Me', '%Cordite'];
    const { row: r, code } = row(3, 41, names, 9);
    const entry = decodeHistoryRow(r, 'u-1')!;
    assert.equal(entry.id, 'snap-3-41');
    assert.deepEqual(entry.names, names, 'the names as stored, a bot still %-prefixed');
    // The extras keep each gap to about a percent, not to the second.
    const played = replaySummary(code)!.moves * 9;
    assert.ok(entry.durationSec !== null && Math.abs(entry.durationSec - played) < played * 0.02, `the gaps between moves, summed (${entry.durationSec} of ${played} s)`);
    assert.equal(entry.createdAt.toISOString(), '2026-09-10T12:00:00.000Z', 'a stamp with no zone is UTC');
    const [moves, extras] = entry.code.split('-');
    assert.deepEqual(kernelB32Decode(moves), code, 'the moves half is the game');
    assert.deepEqual(kernelB32Decode(extras), encodeExtrasBytes(names, Array.from({ length: replaySummary(code)!.moves + 1 }, (_, k) => 1_780_000_000 + k * 9)), 'the extras half is the stored blob');
    assert.deepEqual(decodeHistoryRow({ ...r, moves: hex(code) }, 'u-1'), entry, 'hex reads the same without the bytea prefix');
    assert.equal(decodeHistoryRow({ ...r, created_at: '2026-09-10T12:00:00+02:00' }, 'u-1')!.createdAt.toISOString(), '2026-09-10T10:00:00.000Z', 'a stamp that names its zone keeps it');
});

test('a row without extras: seats are numbered, there is no length, and the code is the moves alone', () => {
    const { row: r, code } = row(4, 42, null);
    for (const extras of [null, '', '\\x']) {
        const entry = decodeHistoryRow({ ...r, extras }, 'u-0')!;
        assert.deepEqual(entry.names, ['#1', '#2', '#3', '#4']);
        assert.equal(entry.durationSec, null);
        assert.ok(!entry.code.includes('-'), `no extras half (${entry.code.length} chars)`);
        assert.deepEqual(kernelB32Decode(entry.code), code);
    }
});

test('extras that do not decode cost the names and the length, and nothing else', () => {
    const { row: r, code } = row(2, 7, ['Boris', 'Me']);
    const whole = decodeHistoryRow(r, 'u-1')!;
    const entry = decodeHistoryRow({ ...r, extras: '\\xffffffffffff' }, 'u-1')!;
    assert.deepEqual(entry.names, ['#1', '#2']);
    assert.equal(entry.durationSec, null);
    assert.deepEqual([entry.mySeat, entry.myRank, entry.foolSeat, entry.playerCount], [whole.mySeat, whole.myRank, whole.foolSeat, whole.playerCount]);
    assert.deepEqual(kernelB32Decode(entry.code.split('-')[0]), code, 'the link still opens the game');
});

test('a row the player holds no seat in is no line, and is not decoded to find that out', () => {
    const { row: r } = row(2, 7, null);
    assert.equal(decodeHistoryRow(r, 'u-nobody'), null);
    assert.equal(decodeHistoryRow({ ...r, moves: '\\xffff' }, 'u-nobody'), null, 'the moves are never read');
    assert.equal(decodeHistoryRow({ ...r, player_ids: null }, 'u-0'), null, 'a row with no seats has nobody in it');
});

test('a snapshot that does not decode throws, for the screen to skip', () => {
    const { row: r } = row(2, 7, null);
    assert.throws(() => decodeHistoryRow({ ...r, moves: '\\xffff' }, 'u-0'), /the snapshot does not decode/);
});
