// What the extension shows, chosen from the resident game's phase.
//
// The router asks the kernel and nothing else - there is no "what was I showing
// last" state here, because a surface that remembers is a surface that can
// disagree with the game.
//
// ONE DEBUG-ONLY DEVIATION, and it is the solo rig (SoloSeatPicker): in a debug
// build the operator may re-seat themselves, because a seven-player night cannot
// be looked at any other way on one simulator. A RELEASE BUILD OFFERS NO SEAT
// CHOICE AT ALL - see SoloSeatPicker's own note for why that is more serious here
// than in the game this was forked from, and ios/scripts/release_gate.sh for the
// gate that proves it.
import SwiftUI

public struct RootView: View {
    /// The seat this device resolved to, or -1.
    private let mySeat: Int
    private let gameId: UInt64
    private let parent: Data?
    private let lastSealAt: UInt16
    private let stage: (Data) -> Void
    /// The lobby actions. They are the extension's because each one seals a bubble
    /// against the chain it was composed on, which is Messages' business.
    private let create: () -> Void
    private let join: () -> Void
    private let start: () -> Void
    private let invite: () -> Void
    private let exit: () -> Void
    /// Whether the bubble on screen is this device's own - the input to the lobby's
    /// "the newest sender stands aside" rule.
    private let iSentTheNewest: Bool

    /// The seat currently being PLAYED. In Release this is always `mySeat` - there
    /// is no code that can move it, because the only writer is compiled out.
    @State private var seat: Int

    public init(mySeat: Int,
                gameId: UInt64,
                parent: Data?,
                lastSealAt: UInt16,
                iSentTheNewest: Bool,
                stage: @escaping (Data) -> Void,
                create: @escaping () -> Void,
                join: @escaping () -> Void,
                start: @escaping () -> Void,
                invite: @escaping () -> Void,
                exit: @escaping () -> Void) {
        self.mySeat = mySeat
        self.gameId = gameId
        self.parent = parent
        self.lastSealAt = lastSealAt
        self.iSentTheNewest = iSentTheNewest
        self.stage = stage
        self.create = create
        self.join = join
        self.start = start
        self.invite = invite
        self.exit = exit
        _seat = State(initialValue: mySeat)
    }

    public var body: some View {
        VStack(spacing: 0) {
            #if DEBUG || SOLO_TESTING
            if SoloSeatPicker.offered && Kernel.shared.phase(-1) != .lobby {
                SoloSeatPicker(seatCount: Kernel.shared.playerCount(-1), current: seat) { s in
                    seat = s
                }
                .padding(.horizontal, Night.gutter)
                .padding(.top, 10)
            }
            #endif
            surface
        }
        .background(Night.ground.ignoresSafeArea())
    }

    @ViewBuilder
    private var surface: some View {
        switch Kernel.shared.phase(seat) {
        case .lobby:
            // A lobby with nobody in it is not a lobby yet - it is a thread with no
            // game in it, and the only thing to offer is making one.
            if Kernel.shared.lobbyJoined == 0 {
                NewGameScreen(create: create)
            } else {
                LobbyScreen(mySeat: mySeat, iSentTheNewest: iSentTheNewest,
                            onJoin: join, onStart: start, onInvite: invite, onExit: exit)
            }
        case .night:
            if seat < 0 {
                // An unresolved seat gets the PUBLIC surface, never a choice. In
                // Release this is the only branch an unresolved seat can take.
                SpectatorScreen()
            } else {
                // `.id(seat)` so re-seating rebuilds the model. A NightModel
                // carries a send-floor clock started when the screen opened, and
                // reusing one across a seat change would hand the next seat a
                // floor the previous seat already spent - which is exactly the
                // tell the floor exists to remove.
                NightHost(seat: seat, gameId: gameId, parent: parent,
                          lastSealAt: lastSealAt, stage: stage)
                    .id(seat)
            }
        case .day, .over:
            MorningScreen(mySeat: seat)
        }
    }
}

/// Owns one seat's NightModel. A separate view purely so `.id(seat)` above has
/// something to re-identify.
struct NightHost: View {
    let seat: Int
    let gameId: UInt64
    let parent: Data?
    let lastSealAt: UInt16
    let stage: (Data) -> Void

    @StateObject private var model: NightModel

    init(seat: Int, gameId: UInt64, parent: Data?, lastSealAt: UInt16,
         stage: @escaping (Data) -> Void) {
        self.seat = seat
        self.gameId = gameId
        self.parent = parent
        self.lastSealAt = lastSealAt
        self.stage = stage
        _model = StateObject(wrappedValue: NightModel(mySeat: seat))
    }

    var body: some View {
        NightScreen(model: model, gameId: gameId, parent: parent,
                    lastSealAt: lastSealAt, stage: stage)
    }
}

// ONE BUTTON, AND IT IS NOT "DEAL".
//
// NO PLAYER-COUNT PICKER, deliberately, and this is the security decision rather
// than a simplification. A count chosen at creation is a count the creator can
// tune before anything commits, and a creation that can be tuned is a creation
// that can be repeated until it suits. The count is decided AT START by who
// actually joined, so there is nothing on this screen to tune and nothing to
// learn by tapping it again.
struct NewGameScreen: View {
    let create: () -> Void

    var body: some View {
        VStack(spacing: 16) {
            Text("Werewolf")
                .font(.system(size: 28, weight: .semibold))
            Text("Five to ten players, in this thread. Send the invite and let people join - the roles are dealt when somebody starts it.")
                .font(.system(size: 14))
                .multilineTextAlignment(.center)
                .foregroundStyle(Night.quiet)
                .padding(.horizontal, 24)
            Button(action: create) {
                Text("New game")
                    .font(.system(size: 17, weight: .semibold))
                    .frame(maxWidth: .infinity, minHeight: 50)
            }
            .buttonStyle(.plain)
            .background(RoundedRectangle(cornerRadius: Night.corner).fill(Night.chosen))
            .foregroundStyle(Night.ground)
            .padding(.horizontal, Night.gutter)
            .accessibilityIdentifier("setup.newgame")
        }
        .padding(.vertical, 28)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .foregroundStyle(Night.ink)
    }
}

// WHAT AN UNRESOLVED SEAT SEES, and Release's whole answer to an ambiguous
// identity. Built from the spectator view (`viewer == -1`), which is the same
// masked view every other surface is built from - so it is public-safe by
// construction rather than by somebody remembering to check it. It carries no
// role, no channel, no reading and no decider, and the C suite asserts that.
struct SpectatorScreen: View {
    var body: some View {
        let k = Kernel.shared
        let n = k.playerCount(-1)
        return VStack(alignment: .leading, spacing: 14) {
            Text("Night \(k.night(-1) + 1)")
                .font(.system(size: 13, weight: .semibold))
                .tracking(1.6)
                .foregroundStyle(Night.quiet)
            Text("You are watching this one.")
                .font(.system(size: 22, weight: .semibold))
            ForEach(0..<max(n, 0), id: \.self) { seat in
                HStack {
                    Text(Self.name(seat)).font(.system(size: 15))
                    Spacer()
                    Text(Self.sentWord(seat))
                        .font(.system(size: 13))
                        .foregroundStyle(Night.quiet)
                }
                .opacity(k.isAlive(-1, seat) ? 1 : 0.45)
            }
            Spacer()
        }
        .padding(Night.gutter)
        .frame(maxWidth: .infinity, alignment: .leading)
        .foregroundStyle(Night.ink)
        .accessibilityIdentifier("spectator")
    }

    private static func name(_ seat: Int) -> String {
        let k = Kernel.shared
        for i in 0..<k.rosterCount where k.rosterSeat(i) == seat {
            if let n = k.rosterName(i) { return n }
        }
        return "Seat \(seat + 1)"
    }

    private static func sentWord(_ seat: Int) -> String {
        guard Kernel.shared.isAlive(-1, seat) else { return "out" }
        switch Kernel.shared.sent(-1, seat) {
        case .no:      return "has not sent"
        case .yes:     return "sent"
        case .carried: return "passed for"
        }
    }
}

// The morning: who the night took. The role of the dead is public, so this screen
// is the same for everybody - which is what makes the argument that follows it
// worth having.
struct MorningScreen: View {
    let mySeat: Int

    var body: some View {
        let k = Kernel.shared
        let night = k.night(mySeat)
        let victim = k.victim(mySeat, night: night)
        return VStack(alignment: .leading, spacing: 14) {
            Text("Morning \(night + 1)")
                .font(.system(size: 13, weight: .semibold))
                .tracking(1.6)
                .foregroundStyle(Night.quiet)
            if victim == noSeat {
                Text("Nobody died.")
                    .font(.system(size: 24, weight: .semibold))
            } else {
                Text("\(name(victim)) is gone.")
                    .font(.system(size: 24, weight: .semibold))
                Text(roleWord(k.role(mySeat, of: victim)))
                    .font(.system(size: 15))
                    .foregroundStyle(Night.quiet)
            }
            if k.winner(mySeat) != .none {
                Text(k.winner(mySeat) == .wolves ? "The wolves win." : "The village wins.")
                    .font(.system(size: 18, weight: .semibold))
                    .foregroundStyle(Night.chosen)
            } else {
                Text("Talk it out, then vote.")
                    .font(.system(size: 15))
                    .foregroundStyle(Night.quiet)
            }
            Spacer()
        }
        .padding(Night.gutter)
        .frame(maxWidth: .infinity, alignment: .leading)
        .foregroundStyle(Night.ink)
        .accessibilityIdentifier("morning")
    }

    private func name(_ seat: Int) -> String {
        let k = Kernel.shared
        for i in 0..<k.rosterCount where k.rosterSeat(i) == seat {
            if let n = k.rosterName(i) { return n }
        }
        return "Seat \(seat + 1)"
    }

    private func roleWord(_ r: Role) -> String {
        switch r {
        case .wolf:     return "They were a wolf."
        case .seer:     return "They were the seer."
        case .villager: return "They were a villager."
        case .unknown:  return ""
        }
    }
}
