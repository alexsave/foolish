// GameSession.swift — the shared surface the board renders against (§16.D5).
// LocalGame (offline) and OnlineGame (online) both conform, so ONE TableView
// drives both. Everything the board reads is here; every state answer still
// comes from the kernel (§3) — the conformers differ only in WHERE the state
// comes from (a local kernel instance vs the server's masked-view feed).

import Foundation
import Combine   // ObservableObject

@MainActor
public protocol GameSession: ObservableObject {
    /// The local player's masked view of the table. nil until first load.
    var view: GameView? { get }
    /// The local seat's legal-move menu (kernel-computed). Drives enable-states.
    var humanLegal: [Move] { get }
    /// Seats with a pending action (bitmask) — per-seat "thinking" marks.
    var actorMask: Int { get }
    /// True while an opponent is deliberating / the server hasn't confirmed.
    var thinking: Bool { get }
    /// A transient reject to surface (rigid haptic + toast).
    var lastReject: EngineError? { get }
    /// Fool seat once the game ends, else nil.
    var foolSeat: Int? { get }
    /// The seat the local player controls.
    var humanSeat: Int { get }
    /// Cards the local player has played but the source of truth hasn't confirmed
    /// yet — the board dims + locks them (the Stage C1 in-flight affordance, §8.2).
    var inFlight: Set<String> { get }

    /// Seat → display name, for seats whose masked view carries none. Offline
    /// games fill this with localized bot names (docs/IOS_BOT_NAMING.md); online
    /// rows carry their own `%`-nickname, so this stays empty there. Default
    /// `[:]` so `OnlineGame` need not implement it.
    var seatNames: [Int: String] { get }

    /// Forward a move intent.
    func play(_ move: Move)
    /// Encode the game to a shareable replay URL (nil if unavailable).
    func makeShareURL() async -> URL?
}

public extension GameSession {
    var seatNames: [Int: String] { [:] }
}
