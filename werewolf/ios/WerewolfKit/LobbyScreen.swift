// The lobby. An open one: no player-count picker, and the count decided at Start
// by who actually joined.
//
// WHAT THIS SCREEN MUST NEVER SHOW, and the reason is the whole product: anything
// about the deal. Creating locked a seed and nothing else - no roles exist yet, on
// any device, and the kernel has none to hand over (`ww_msg_replay` gives a lobby
// zero seats on purpose). That is what makes re-creating pointless: there is
// nothing to look at, so there is nothing to re-roll for.
//
// The control is whatever `Kernel.lobbyOffered` returned. This file does not
// decide it, does not soften it and does not add a second button beside it - a
// lobby control re-decided in Swift is one two clients can disagree about, and in
// this game one of those disagreements hands somebody a role.
import SwiftUI

public struct LobbyScreen: View {
    /// This device's seat on THIS bubble's roster, or -1. A lobby seat is resolved
    /// against the bubble in front of you rather than the cache, because an older
    /// invite predates a join that has since happened - see `ww_seat.h`.
    private let mySeat: Int
    /// Whether the bubble on screen is this device's own. The newest sender stands
    /// aside while there is still room.
    private let iSentTheNewest: Bool
    private let onJoin: () -> Void
    private let onStart: () -> Void
    private let onInvite: () -> Void
    private let onExit: () -> Void

    public init(mySeat: Int,
                iSentTheNewest: Bool,
                onJoin: @escaping () -> Void,
                onStart: @escaping () -> Void,
                onInvite: @escaping () -> Void,
                onExit: @escaping () -> Void) {
        self.mySeat = mySeat
        self.iSentTheNewest = iSentTheNewest
        self.onJoin = onJoin
        self.onStart = onStart
        self.onInvite = onInvite
        self.onExit = onExit
    }

    public var body: some View {
        let k = Kernel.shared
        let joined = k.lobbyJoined
        let control = k.lobbyOffered(mySeat: mySeat, iSentTheNewest: iSentTheNewest)
        return ScrollView {
            VStack(alignment: .leading, spacing: 16) {
                Text("Werewolf")
                    .font(.system(size: 26, weight: .semibold))
                Text(headline(control: control, joined: joined, needs: k.lobbyNeeds))
                    .font(.system(size: 15))
                    .foregroundStyle(Night.quiet)
                    .fixedSize(horizontal: false, vertical: true)

                VStack(spacing: 8) {
                    ForEach(0..<joined, id: \.self) { i in
                        HStack {
                            Text(k.rosterName(i) ?? "Seat \(k.rosterSeat(i) + 1)")
                                .font(.system(size: 15, weight: .medium))
                            if k.rosterSeat(i) == mySeat {
                                Text("you")
                                    .font(.system(size: 11))
                                    .foregroundStyle(Night.quiet)
                            }
                            Spacer()
                        }
                        .padding(.vertical, 10)
                        .padding(.horizontal, 12)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .background(RoundedRectangle(cornerRadius: 10).fill(Night.raised))
                    }
                }

                button(for: control)

                if k.lobbyCanExit(mySeat: mySeat) {
                    Button("Leave", action: onExit)
                        .buttonStyle(.plain)
                        .font(.system(size: 13))
                        .foregroundStyle(Night.quiet)
                }

                // NO ROLES, NO SEED, NO DEAL - and it is worth saying out loud on
                // the screen, because the reason a player should trust this game is
                // exactly that nobody could have looked.
                Text("Roles are dealt when the game starts, from a seed locked when this invite was made. Nobody - including whoever made it - knows anything yet.")
                    .font(.system(size: 12))
                    .foregroundStyle(Night.quiet)
                    .fixedSize(horizontal: false, vertical: true)
            }
            .padding(Night.gutter)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
        .background(Night.ground.ignoresSafeArea())
        .foregroundStyle(Night.ink)
        .accessibilityIdentifier("lobby")
    }

    private func headline(control: LobbyControl, joined: Int, needs: Int) -> String {
        switch control {
        case .tooFew:
            // The one case a count would be a lie: this chat holds two, and two
            // cannot be a werewolf game however long anybody waits.
            return "Werewolf needs at least \(Kernel.shared.minPlayers) players, so it cannot be played in a one-to-one chat. Start a group message instead."
        case .full:
            return "All \(joined) seats are taken."
        default:
            if needs > 0 {
                return joined == 1
                    ? "You are in. \(needs) more \(needs == 1 ? "player" : "players") and this can start."
                    : "\(joined) in. \(needs) more \(needs == 1 ? "player" : "players") and this can start."
            }
            return "\(joined) in. Ready when somebody starts it."
        }
    }

    @ViewBuilder
    private func button(for control: LobbyControl) -> some View {
        switch control {
        case .join:    primary("Join", action: onJoin, id: "lobby.join")
        case .start:   primary("Start the game", action: onStart, id: "lobby.start")
        case .invite:  primary("Send the invite", action: onInvite, id: "lobby.invite")
        case .waiting, .full, .tooFew:
            // Nothing. A disabled button here would be a lie in three different
            // ways - there is genuinely nothing for this viewer to do, and an
            // enabled-looking control that refuses is how a player learns to tap
            // twice.
            EmptyView()
        }
    }

    private func primary(_ title: String, action: @escaping () -> Void, id: String) -> some View {
        Button(action: action) {
            Text(title)
                .font(.system(size: 17, weight: .semibold))
                .frame(maxWidth: .infinity, minHeight: 50)
        }
        .buttonStyle(.plain)
        .background(RoundedRectangle(cornerRadius: Night.corner).fill(Night.chosen))
        .foregroundStyle(Night.ground)
        .accessibilityIdentifier(id)
    }
}
