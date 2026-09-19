// What the extension shows, chosen from the resident game's phase.
//
// Three surfaces and one router. The router asks the kernel and nothing else -
// there is no "what was I showing last" state here, because a surface that
// remembers is a surface that can disagree with the game.
import SwiftUI

public struct RootView: View {
    /// The seat this device resolved to, or -1. A spectator is what an unresolved
    /// seat degrades to, never a guess: guessing hands somebody else's night to
    /// the wrong phone.
    private let mySeat: Int
    private let gameId: UInt64
    private let parent: Data?
    private let lastSealAt: UInt16
    private let stage: (Data) -> Void
    private let create: (Int) -> Void

    @StateObject private var night: NightModel

    public init(mySeat: Int,
                gameId: UInt64,
                parent: Data?,
                lastSealAt: UInt16,
                stage: @escaping (Data) -> Void,
                create: @escaping (Int) -> Void) {
        self.mySeat = mySeat
        self.gameId = gameId
        self.parent = parent
        self.lastSealAt = lastSealAt
        self.stage = stage
        self.create = create
        _night = StateObject(wrappedValue: NightModel(mySeat: mySeat))
    }

    public var body: some View {
        switch Kernel.shared.phase(mySeat) {
        case .lobby:
            SetupScreen(create: create)
        case .night:
            NightScreen(model: night, gameId: gameId, parent: parent,
                        lastSealAt: lastSealAt, stage: stage)
        case .day:
            MorningScreen(mySeat: mySeat)
        case .over:
            MorningScreen(mySeat: mySeat)
        }
    }
}

// The table size, and nothing else. Roles are dealt from a seed the OS mints, so
// there is no setting here that could change the game - which is why this screen
// is three taps and not a form.
struct SetupScreen: View {
    let create: (Int) -> Void
    @State private var players = 7

    var body: some View {
        VStack(spacing: 18) {
            Text("Werewolf")
                .font(.system(size: 28, weight: .semibold))
            Text("Five to ten players, in this thread.")
                .font(.system(size: 14))
                .foregroundStyle(Night.quiet)
            Stepper("\(players) players", value: $players,
                    in: Kernel.shared.minPlayers...Kernel.shared.maxPlayers)
                .font(.system(size: 16, weight: .medium))
                .padding(.horizontal, 24)
            Button {
                create(players)
            } label: {
                Text("Deal")
                    .font(.system(size: 17, weight: .semibold))
                    .frame(maxWidth: .infinity, minHeight: 50)
            }
            .buttonStyle(.plain)
            .background(RoundedRectangle(cornerRadius: Night.corner).fill(Night.chosen))
            .foregroundStyle(Night.ground)
            .padding(.horizontal, Night.gutter)
        }
        .padding(.vertical, 28)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .background(Night.ground.ignoresSafeArea())
        .foregroundStyle(Night.ink)
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
        .background(Night.ground.ignoresSafeArea())
        .foregroundStyle(Night.ink)
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
