// LocalGame.swift — the offline game session. Owns one EngineC (kernel
// instance), publishes the human seat's masked GameView, forwards the human's
// moves, and drives the bot roster between human turns with UX pacing and a
// thermal/battery guard (§7.2, §16.B2).
//
// This is the offline half of what becomes the `GameSession` protocol in
// Milestone D (§16.D5) — LocalGame and OnlineGame will both vend `view`,
// `play(_:)`, `legalMoves(for:)`, `actorMask`. Kept concrete here so SwiftUI's
// ObservableObject wiring stays simple until that refactor.

import Foundation
import SwiftUI
import FoolishKit

@MainActor
public final class LocalGame: ObservableObject, GameSession {

    /// Offline moves apply synchronously against the local kernel — there is no
    /// in-flight state (that's an online-only affordance, §8.2).
    public var inFlight: Set<String> { [] }

    /// The human seat's masked view of the table. nil until the first refresh.
    @Published public private(set) var view: GameView?
    /// True while a bot is deliberating (drives the "thinking" indicator).
    @Published public private(set) var thinking: Bool = false
    /// The last move a bot made — the animation layer diffs against it (B4).
    @Published public private(set) var lastBotMove: Move?
    /// The kernel's animation plan for the last thing that happened — a bot
    /// cycle or the human's own move. The board plays these
    /// (matchedGeometryEffect, §16.B4); it never derives them by diffing two
    /// `GameView`s, which is why `BoardDiff.swift` does not exist.
    @Published public private(set) var lastEvents: [GameEvent] = []
    /// A transient reject surfaced to the UI (rigid haptic + toast, §8.2 C1).
    @Published public private(set) var lastReject: EngineError?
    /// Fool seat once the game ends, else nil.
    @Published public private(set) var foolSeat: Int?
    /// The human seat's current legal-move menu (kernel-computed). The board
    /// derives every enable-state from this — never a hand-rolled rule (§3).
    @Published public private(set) var humanLegal: [Move] = []
    /// The same menu as the kernel wrote it. The board's play rules read the
    /// wire (PlayWire), not the decode - see GameSession.
    @Published public private(set) var humanLegalPacked: Data = MoveWire.emptyMenu
    /// Seats with a pending action right now (bitmask) — drives per-seat
    /// "thinking" marks. Kernel-computed (fio_actor_mask).
    @Published public private(set) var actorMask: Int = 0

    public let humanSeat: Int
    public let players: Int

    /// Seat → localized bot name, built by the caller (AppCoordinator) since the
    /// offline kernel view carries no names (docs/IOS_BOT_NAMING.md §3 wiring 2).
    public let seatNames: [Int: String]

    private let engine = EngineC()
    /// Seat → roster strategy id, as requested by the caller. The thermal guard
    /// may temporarily run a heavier seat as `espresso`; this is the seat's
    /// "true" assignment to restore once the device cools (§7.2).
    private let requestedStrategies: [Int: Int]
    private var driveTask: Task<Void, Never>?

    /// - Parameters:
    ///   - seed: 32+ bytes ⇒ reproducible wide deal.
    ///   - players: 2...8.
    ///   - humanSeat: which seat the local player controls.
    ///   - strategies: seat → roster strategy id for every OTHER seat.
    public init(seed: Data, players: Int, humanSeat: Int = 0, strategies: [Int: Int],
                seatNames: [Int: String] = [:]) {
        self.players = players
        self.humanSeat = humanSeat
        self.requestedStrategies = strategies
        self.seatNames = seatNames
        Task { await self.boot(seed: seed) }
    }

    private func boot(seed: Data) async {
        do {
            try await engine.newGame(seed: seed, players: players)
            for (seat, sid) in requestedStrategies where seat != humanSeat {
                try? await engine.setSeatStrategy(seat: seat, strategyId: sid)
            }
            await refresh()
            drive()
        } catch {
            lastReject = error as? EngineError ?? .unknown(-999)
        }
    }

    // MARK: - Human intents

    /// Legal moves for the human seat (kernel-computed; never hand-rolled).
    public func legalMoves() async -> [Move] {
        (try? await engine.legalMoves(seat: humanSeat)) ?? []
    }

    /// Apply a human move, then let the bots respond. Reject → published so the
    /// UI can fire the rigid haptic + toast and unlock the touched cards (C1).
    public func play(_ move: Move) {
        Task {
            do {
                try await engine.apply(seat: humanSeat, move: move)
                lastReject = nil
                // The human's own card flies by the same kernel plan a bot's does.
                lastEvents = (try? await engine.lastEvents(viewer: humanSeat)) ?? []
                await refresh()
                drive()
            } catch {
                lastReject = error as? EngineError ?? .unknown(-999)
            }
        }
    }

    // MARK: - Replay sharing (§7.3, §16.C2)

    /// Encode the finished (or in-progress) game to a shareable code, save it to
    /// the local replay list, and return the foolish.cards URL. nil if encoding
    /// fails (e.g. the log buffer overflowed — very long games).
    public func makeShareURL() async -> URL? {
        guard let code = try? await engine.replayEncodeCode(), !code.isEmpty else { return nil }
        let fool = foolSeat ?? -1
        let myResult = fool < 0 ? nil : (fool == humanSeat ? "lose" : "win")
        ReplayStore.shared.save(ReplayRecord(code: code, savedAt: Date(),
                                             players: players, fool: fool, myResult: myResult))
        // …WITH THE ROSTER ATTACHED, the same as the iMessage link
        // (MessageTurnController.replayURL). A code with no names decodes to
        // "P1"/"P2" on the website, which is a worse page than the one the
        // sharer was looking at.
        //
        // The names here are LOCALIZED BOT NAMES (AppCoordinator builds them),
        // so a Russian player's shared replay shows Russian bot names to
        // whoever opens it. Raised with the owner and waved through: "a russian
        // players will have different bot names but whatever." The alternative -
        // baking in English names nobody on this device ever saw - is worse,
        // because the whole point of the link is to show the game as it was
        // played. The HUMAN seat has no entry in `seatNames` and pads to an
        // empty name, which decodes to the reader's own default exactly as an
        // unnamed iMessage seat does.
        return MessageEnvelope.replayLink(
            code: code, names: ReplayExtras.seatNames(seatNames, count: players))
    }

    // MARK: - Bot drive loop

    private func drive() {
        driveTask?.cancel()
        driveTask = Task { await self.runBots() }
    }

    /// The bot loop is now one kernel call per cycle
    /// (docs/C_CORE_CONSOLIDATION.md F2/F3). Everything it used to decide in
    /// Swift — which of several eligible bots goes next, whether a silent action
    /// deserves a pause, how long to wait — is the kernel's answer, so the phone
    /// and the website run the same cycle. What stays here is what the doc calls
    /// the host's job: timers, thermal policy, and rendering.
    private func runBots() async {
        while !Task.isCancelled {
            await applyThermalPolicy()

            thinking = true
            guard let drive = try? await engine.botDrive(humanSeats: [humanSeat]) else {
                thinking = false
                break
            }
            thinking = false
            if Task.isCancelled { break }

            // Show what happened before waiting on it. Silent actions bundle
            // into this same cycle and carry no delay of their own.
            if let visible = drive.lastVisible { lastBotMove = visible.move }
            if !drive.events.isEmpty { lastEvents = drive.events }
            if !drive.actions.isEmpty { await refresh() }

            if drive.isOver { foolSeat = drive.ended; break }
            // No bot could act: the human owes a move. Wait for their input —
            // drive() is called again once they play.
            if drive.actions.isEmpty && drive.stop == .noEligible { break }

            if drive.delayMs > 0 {
                try? await Task.sleep(nanoseconds: UInt64(drive.delayMs) * 1_000_000)
                if Task.isCancelled { break }
            }
        }
        await refresh()
        if let over = try? await engine.gameOver(), over >= 0 { foolSeat = over }
    }

    private func refresh() async {
        if let v = try? await engine.state(viewer: humanSeat) { view = v }
        // Publish the human's legal menu too, so the board's enable-states are
        // always kernel-driven and synchronous to read in view bodies.
        // One kernel read, published as both forms so they cannot describe
        // different menus.
        let packed = (try? await engine.legalPackedData(seat: humanSeat)) ?? MoveWire.emptyMenu
        humanLegalPacked = packed
        humanLegal = MoveWire.decode(packed)
        actorMask = (try? await engine.actorMask()) ?? 0
    }

    // MARK: - Thermal guard (§7.2, §16.B2)
    //
    // Pacing used to live here as a 600-1200ms jitter whose comment claimed to
    // mirror the server — it never did (the server paces at 3000ms with a human
    // watching). It is now one kernel table, bot_pacing_ms, and arrives as
    // BotDrive.delayMs.

    /// When the device is hot, temporarily run heavy solvers as espresso so the
    /// game never freezes and the phone never cooks (§7.2, §15 risk 2). When it
    /// cools, restore each seat's requested strategy.
    private var espressoId: Int? // resolved lazily from the roster
    private var downgraded = false

    private func applyThermalPolicy() async {
        let hot = ProcessInfo.processInfo.thermalState.rawValue >= ProcessInfo.ThermalState.serious.rawValue
        if hot == downgraded { return }

        if espressoId == nil {
            let count = await engine.strategyCount()
            for i in 0..<count where (try? await engine.strategyName(i)) == "espresso" { espressoId = i }
        }
        for (seat, sid) in requestedStrategies where seat != humanSeat {
            let target = hot ? (espressoId ?? sid) : sid
            try? await engine.setSeatStrategy(seat: seat, strategyId: target)
        }
        downgraded = hot
    }

    deinit { driveTask?.cancel() }
}
