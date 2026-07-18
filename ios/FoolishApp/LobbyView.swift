// LobbyView.swift — the online pre-game lobby (§6 screen 2 / §16.D5), mirroring
// the web Lobby (src/components/Lobby.tsx): the game code + QR to share, the
// seated players with their ready state, an Add-bot button, and a Ready button
// that starts the game once everyone is ready (Ready = `meta start`; the server
// deals when all seats are ready). Renders while the online game is WAITING;
// RootView swaps to the table once it is PLAYING.

import SwiftUI
import FoolishKit
import FoolishNet

struct LobbyView: View {
    @ObservedObject var game: OnlineGame
    let onLeave: () -> Void

    private let joinBase = "https://foolish.cards/"

    var body: some View {
        ZStack {
            WoolBackground()
            if let view = game.view {
                VStack(spacing: FSpace.l) {
                    header
                    shareRow
                    playersList(view)
                    Spacer(minLength: 0)
                    if game.isWaiting {
                        if view.players.count < 8 {
                            FButton(FStrings.t("ios.add_bot"), kind: .wood) { game.addBot() }
                        }
                        if game.isSeated {
                            FButton(FStrings.t("ios.ready"), kind: .primary) { game.ready() }
                        } else {
                            FButton(FStrings.t("join_game"), kind: .primary) { game.joinSelf() }
                        }
                    } else {
                        ProgressView().tint(FColor.textPrimary)   // dealing…
                    }
                }
                .padding(FSpace.xl)
            } else {
                ProgressView().tint(FColor.textPrimary)
            }
        }
        .navigationBarBackButtonHidden(true)
    }

    private var header: some View {
        VStack(spacing: FSpace.xs) {
            Text(FStrings.t("ios.lobby"))
                .font(FType.title(26)).foregroundColor(FColor.textPrimary)
            Text("\(FStrings.t("ios.game_code")): \(game.gameId.uppercased())")
                .font(FType.body(14)).foregroundColor(FColor.textDim)
        }
        .padding(.top, FSpace.xl)
    }

    /// The join code as a QR + a share sheet — how a second player joins (web
    /// Lobby shows the same QR of `/​<game_id>`).
    private var shareRow: some View {
        let url = joinBase + game.gameId
        return VStack(spacing: FSpace.s) {
            if let qr = QRCode.image(for: url.uppercased()) {
                Image(uiImage: qr)
                    .interpolation(.none).resizable()
                    .frame(width: 120, height: 120)
                    .clipShape(RoundedRectangle(cornerRadius: FRadius.card))
            }
            ShareLink(item: URL(string: url)!) {
                Text(FStrings.t("ios.share_invite"))
                    .font(FType.body(14)).foregroundColor(FColor.accent)
            }
        }
    }

    private func playersList(_ view: GameView) -> some View {
        VStack(spacing: FSpace.s) {
            ForEach(view.players) { p in
                HStack(spacing: FSpace.m) {
                    if p.name.hasPrefix("%") {
                        Image(systemName: "gearshape.fill").font(.system(size: 13)).foregroundColor(FColor.textDim)
                    }
                    Text(displayName(p))
                        .font(FType.body(16)).foregroundColor(FColor.textPrimary)
                        .lineLimit(1)
                    Spacer()
                    // Ready = any non-idle seat status (web: status !== IDLE).
                    Image(systemName: p.seatStatus == .idle ? "circle" : "checkmark.circle.fill")
                        .font(.system(size: 18))
                        .foregroundColor(p.seatStatus == .idle ? FColor.textDim : FColor.win)
                }
                .padding(FSpace.m)
                .background(FColor.surface.opacity(0.85))
                .clipShape(RoundedRectangle(cornerRadius: FRadius.card))
            }
        }
    }

    private func displayName(_ p: PlayerView) -> String {
        if p.name.isEmpty { return "\(FStrings.t("players")) \(p.seat + 1)" }
        return BotNames.displayNickname(p.name)
    }
}
