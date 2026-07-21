// note 13 (HARNESS_NOTES_R2): `openReplayDelta`'s `from == replay.logs.count`
// case ("the cached chain and the one I just adopted have the same log
// count, I've already shown everything") must resolve to an EMPTY delta —
// not the structural heuristic fallback, which has no memory of what it
// already showed and would sometimes replay the trailing pickup/discard run
// a second time. This is the pure Swift half of the fix (ReplayDelta.swift);
// MessageTurnControllerTests exercises the other half end to end (a real
// reopen of an already-cached chain via `prevPayload`).
import XCTest
@testable import FoolishKit

final class ReplayDeltaTests: XCTestCase {

    private func log(_ type: Int, seat: Int, card: Card = Card(s: 0, v: 1)) -> ReplayLog {
        ReplayLog(type: type, seat: seat, defenderIndex: -1, pairs: [ReplayPair(primary: card, target: nil)])
    }

    private func replay(_ logs: [ReplayLog]) -> DecodedReplay {
        DecodedReplay(version: 1, nPlayers: 2, trump: Card(s: 0, v: 1), firstAttacker: 0,
                      fool: -1, discardCount: 0, eliminationOrder: [], logs: logs)
    }

    /// The core note-13 assertion. Deliberately crafted so the HEURISTIC
    /// fallback (from: nil) finds something NON-empty (a trailing pickup) —
    /// if the fix regressed back to routing `from == logs.count` through the
    /// heuristic, this test would see that same non-empty pickup, not an
    /// empty array, and fail.
    func testFromEqualToLogCountResolvesToEmptyNotTheHeuristic() {
        let r = replay([log(ReplayLogType.attack, seat: 0), log(ReplayLogType.pickup, seat: 1)])

        // Sanity: the heuristic ALONE really does find the trailing pickup
        // (proves this fixture would fail loudly if the real path fell
        // through to it instead of resolving from `from` directly).
        let heuristic = openReplayDelta(r, from: nil, battlesEmpty: true)
        XCTAssertEqual(heuristic.map(\.type), [ReplayLogType.pickup],
                       "sanity: the heuristic finds the trailing pickup run")

        // The fix: "nothing new" (from == logs.count) is a real empty delta.
        let delta = openReplayDelta(r, from: r.logs.count, battlesEmpty: true)
        XCTAssertTrue(delta.isEmpty, "an already-fully-seen chain must replay nothing")
    }

    /// The ordinary notes 4/9/38 path (a real mid-stream index) is untouched
    /// by the `<=` change.
    func testFromMidStreamStillReturnsTheRealDelta() {
        let r = replay([log(ReplayLogType.attack, seat: 0),
                        log(ReplayLogType.cover, seat: 1),
                        log(ReplayLogType.pickup, seat: 1)])
        let delta = openReplayDelta(r, from: 1, battlesEmpty: false)
        XCTAssertEqual(delta.map(\.type), [ReplayLogType.cover, ReplayLogType.pickup])
    }

    /// `from` PAST the log count is a genuine anomaly (not "nothing new")
    /// and must still fall back to the heuristic, not silently resolve empty.
    func testFromPastLogCountFallsBackToHeuristic() {
        let r = replay([log(ReplayLogType.attack, seat: 0), log(ReplayLogType.pickup, seat: 1)])
        let delta = openReplayDelta(r, from: r.logs.count + 1, battlesEmpty: true)
        XCTAssertEqual(delta.map(\.type), [ReplayLogType.pickup])
    }

    /// No `from` at all (a genuine cache miss) keeps using the heuristic —
    /// unaffected by note 13's fix, which only concerns the `== ` boundary.
    func testNoFromInfoUsesTheHeuristic() {
        let r = replay([log(ReplayLogType.attack, seat: 0), log(ReplayLogType.discard, seat: -1)])
        let delta = openReplayDelta(r, from: nil, battlesEmpty: true)
        XCTAssertEqual(delta.map(\.type), [ReplayLogType.discard])
    }
}
