/* =============================================================================
 * One history row, read into its line of the match history
 * =============================================================================
 * The server cuts a game's summary when it ends and the list carries only that
 * (no replay, no player ids): the viewer's seat, place, the fool, the length and
 * the seats' names. historyEntry (src/components/matchHistoryRow.ts) turns a row
 * into the line. The full render is held to its golden by
 * e2e/ui_dom_snapshots.test.ts; these cases hold the mapping of the "none"
 * values and the names rule, which that golden cannot show.
 * ========================================================================== */

/* MUTATION CHECKS (each made, seen red at the named case, restored):
 *   row.rank === 0 -> === 1                       "rank 0 ... are no line"
 *   row.seat === NONE -> === 0                    "rank 0 ... are no line"
 *   fool NONE ? -1 : fool -> fool                 "the fool: 255 is -1"
 *   durationS === NO_TIME -> === 0                "the length"
 *   names.length === nPlayers -> >=               "names are used only when they name every seat"
 *   MatchHistory fetchReplay(id) -> fetchReplay(id + "x")   ui_dom_snapshots "a tap fetches the replay by the row id"
 *   MatchHistory router.push removed                        the same case
 */

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { historyEntry } from '../src/components/matchHistoryRow.ts';
import type { NetHistoryRow } from '../sdk/ts/net/net_client.ts';

const row = (over: Partial<NetHistoryRow> = {}): NetHistoryRow => ({
    id: 'r1', createdMs: Date.parse('2026-09-10T12:00:00Z'), nPlayers: 3, seat: 1, rank: 2, fool: 0, durationS: 95,
    names: ['Ada', 'Me', '%Cordite'], ...over,
});

test('a full row reads as it says', () => {
    const e = historyEntry(row())!;
    assert.equal(e.id, 'r1');
    assert.equal(e.createdAt.toISOString(), '2026-09-10T12:00:00.000Z');
    assert.deepEqual([e.playerCount, e.mySeat, e.myRank, e.foolSeat, e.durationSec], [3, 1, 2, 0, 95]);
    assert.deepEqual(e.names, ['Ada', 'Me', '%Cordite']);
});

test('rank 0 (no summary) and no seat are no line', () => {
    assert.equal(historyEntry(row({ rank: 0 })), null);
    assert.equal(historyEntry(row({ seat: 255 })), null);
    assert.notEqual(historyEntry(row({ seat: 254 })), null, 'only 255 is "none"');
});

test('the fool: 255 is -1, a seat is itself (seat 0 included)', () => {
    assert.equal(historyEntry(row({ fool: 255 }))!.foolSeat, -1);
    assert.equal(historyEntry(row({ fool: 0 }))!.foolSeat, 0);
    assert.equal(historyEntry(row({ fool: 2 }))!.foolSeat, 2);
});

test('the length: 0xffffffff is unknown, 0 is a length', () => {
    assert.equal(historyEntry(row({ durationS: 0xffffffff }))!.durationSec, null);
    assert.equal(historyEntry(row({ durationS: 0 }))!.durationSec, 0);
    assert.equal(historyEntry(row({ durationS: 0xfffffffe }))!.durationSec, 0xfffffffe);
});

test('names are used only when they name every seat; else the seats are numbered', () => {
    assert.deepEqual(historyEntry(row({ names: [] }))!.names, ['#1', '#2', '#3']);
    assert.deepEqual(historyEntry(row({ names: ['A', 'B'] }))!.names, ['#1', '#2', '#3'], 'too few');
    assert.deepEqual(historyEntry(row({ names: ['A', 'B', 'C', 'D'] }))!.names, ['#1', '#2', '#3'], 'too many');
    assert.deepEqual(historyEntry(row({ nPlayers: 2, names: ['A', 'B'] }))!.names, ['A', 'B']);
});
