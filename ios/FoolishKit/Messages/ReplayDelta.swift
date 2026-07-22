// ReplayDelta.swift — the LOG_* step-type codes (game.h), shared by the parts of
// MessageTableView that still read the raw replay log: the LIVE bout-end
// sequence (flyBoutEndToDiscard -> lastBoutDraws), which reads draw ORDER and
// COUNTS off residentReplay().
//
// The OPEN-replay's own "which cards flew where" derivation used to live here too
// (openReplayDelta, a GameView/log heuristic). It is deleted: the open-replay is
// now the KERNEL's viewer-aware evwire stream (MessageKernel.lastMoveEvents ->
// fio_replay_last_events_packed -> EvWire.decode), so nothing on the client
// re-derives animations from the log any more. What remains is only these type
// codes, still needed to read draw counts for the live path's badge animations.

import Foundation

/// game.h LOG_* (packed replay step types).
enum ReplayLogType {
    static let attack = 1, cover = 2, pass = 3, pickup = 4, discard = 6, draw = 9
}
