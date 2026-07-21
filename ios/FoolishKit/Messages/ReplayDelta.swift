// ReplayDelta.swift — the "since I last looked" replay window (notes 4/9/38,
// HARNESS_NOTES_TRIAGE, extended by notes 6/12/13 in HARNESS_NOTES_R2). Free
// functions, not nested in either type, so both `MessageTurnController`
// (which resolves the window once, in `begin()`, BEFORE the board's first
// paint — notes 6/12 needed this known synchronously, not after a 120ms
// sleep inside an animator Task) and `MessageTableView` (which steps through
// the resolved window to animate it) can see it without one owning the other.

import Foundation

/// game.h LOG_* (packed replay step types).
enum ReplayLogType {
    static let attack = 1, cover = 2, pass = 3, pickup = 4, discard = 6, draw = 9
}

/// The slice of the replay stream to animate on open. Prefers `from` (a
/// controller's diff against the previously-cached chain, notes 4/9) —
/// note 13: `from == replay.logs.count` is a MEANINGFUL value here, not the
/// same as `from == nil`. It means "the cached chain and the chain I just
/// adopted have exactly the same number of log entries — I've already shown
/// everything up to here", a real, empty delta. `from == nil` means "no
/// cached chain to diff against at all" (a genuine cache miss). The two used
/// to collapse onto the same `nil` (the caller excluded `from == logs.count`
/// before ever getting here), which sent a REOPEN of an already-fully-seen
/// chain into the heuristic fallback below — a heuristic with no memory of
/// what it already showed, so a pickup/draw sequence would sometimes replay
/// a second time and sometimes not, depending on whether the table happened
/// to read empty. See `MessageTurnController.openReplayFromLog`'s doc for
/// the other half of this (the `<=` that lets `from` reach `logs.count` at
/// all) and `GameSurface.adopt`'s doc for the caller-side half (no longer
/// excluding `bytes == winner` from being passed in as `prevPayload`).
///
/// Falls back — a fresh cache, or a genesis-adjacent open with nothing to
/// diff against — to a heuristic keyed on whether the table is currently
/// empty:
///   - Empty (a bout just ended): from the last LOG_DISCARD/LOG_PICKUP
///     onward — the SAME "since the last clearing event" window
///     `MessageTableView.lastBoutDraws` already uses for the interactive
///     sequence, but INCLUDING that clearing event itself so the
///     pickup/discard flight replays too (note 4).
///   - Non-empty (mid-bout): the trailing run of CONSECUTIVE same-seat
///     ATTACK/COVER/PASS entries. A triple cover is 3 separate LOG_COVER
///     entries, one per card (game.c handle_cover logs one GameLog PER cover
///     card), so a plain `logs.last(where:)` only ever replayed the
///     rightmost one (note 38); a pass is its own LOG_PASS type, so hunting
///     for the last ATTACK found the PREVIOUS attacker's cards from the
///     wrong seat (note 9).
func openReplayDelta(_ replay: DecodedReplay, from: Int?, battlesEmpty: Bool) -> [ReplayLog] {
    if let from, from >= 0, from <= replay.logs.count {
        return Array(replay.logs[from...])
    }
    if battlesEmpty {
        for i in stride(from: replay.logs.count - 1, through: 0, by: -1)
        where replay.logs[i].type == ReplayLogType.discard || replay.logs[i].type == ReplayLogType.pickup {
            return Array(replay.logs[i...])
        }
        return []
    }
    let placing: Set<Int> = [ReplayLogType.attack, ReplayLogType.cover, ReplayLogType.pass]
    guard let lastIdx = replay.logs.indices.last(where: { placing.contains(replay.logs[$0].type) })
    else { return [] }
    let seat = replay.logs[lastIdx].seat
    var start = lastIdx
    while start > 0, placing.contains(replay.logs[start - 1].type), replay.logs[start - 1].seat == seat {
        start -= 1
    }
    return Array(replay.logs[start...lastIdx])
}
