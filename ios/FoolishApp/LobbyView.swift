// LobbyView.swift — the pre-game lobby for an online game (§16.D5). Shown while
// the online game's `view` is still WAITING: the creator adds bot opponents and
// starts. Mirrors the web Lobby (src/components/Lobby.tsx) but pared to the
// essential flow — seated players, Add Bot, Start — in the wool/wood redesign
// language. Everything reacts to `game.view`, which the player_views feed keeps
// live (a bot joining, or the deal, arrives as a new row).
//
// Bot removal / drag-reorder / named bot picker (the web has all three) are
// follow-ups: the masked `view` is seat-based and carries no player_ids, and the
// `bots` roster isn't client-readable, so those need extra server surface.

import SwiftUI
import FoolishKit

struct LobbyView: View {
    @ObservedObject var game: OnlineGame
    let onLeave: () -> Void

    private let maxPlayers = 8

    private var players: [PlayerView] { game.view?.players.sorted { $0.seat < $1.seat } ?? [] }
    private var canAddBot: Bool { players.count < maxPlayers && !game.lobbyBusy }
    private var canStart: Bool { players.count >= 2 && !game.lobbyBusy }

    var body: some View {
        VStack(spacing: FSpace.l) {
            header
            playerList
            Spacer(minLength: FSpace.l)
            actions
        }
        .padding(FSpace.xl)
        .padding(.top, FSpace.xxl)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }

    private var header: some View {
        VStack(spacing: FSpace.xs) {
            Text(FStrings.t("lobby").uppercased())
                .font(FType.numeral(34))
                .foregroundColor(FColor.textPrimary)
            // The game id doubles as the invite code (foolish.cards/<id>).
            Text("\(FStrings.t("game_code")): \(game.gameId.uppercased())")
                .font(FType.body(13))
                .foregroundColor(FColor.textDim)
                .textSelection(.enabled)
        }
        .accessibilityElement(children: .combine)
    }

    private var playerList: some View {
        VStack(spacing: FSpace.s) {
            ForEach(players) { p in
                playerPlaque(p)
            }
        }
    }

    private func playerPlaque(_ p: PlayerView) -> some View {
        let isMe = p.seat == (game.view?.viewer ?? -1)
        let name = p.name.isEmpty ? "P\(p.seat)" : p.name
        return HStack(spacing: FSpace.m) {
            Image(systemName: p.strategyKey == 0 ? "person.fill" : "cpu")
                .font(.system(size: 15))
                .foregroundColor(FColor.textDim)
            Text(isMe ? "\(name) (\(FStrings.t("ios.you")))" : name)
                .font(FType.title(18))
                .foregroundColor(FColor.textPrimary)
                .lineLimit(1)
            Spacer()
            // idle (0) shows a hollow dot, ready shows a filled check.
            Image(systemName: p.seatStatus == .idle ? "circle" : "checkmark.circle.fill")
                .font(.system(size: 16))
                .foregroundColor(p.seatStatus == .idle ? FColor.textDim : FColor.win)
        }
        .padding(.horizontal, FSpace.l)
        .frame(maxWidth: .infinity, minHeight: 54)
        .background(WoodSurface(seed: Double(p.seat) / 8.0, cornerRadius: FRadius.button))
        .clipShape(RoundedRectangle(cornerRadius: FRadius.button, style: .continuous))
        .overlay(
            RoundedRectangle(cornerRadius: FRadius.button, style: .continuous)
                .strokeBorder(isMe ? FColor.accent.opacity(0.7) : .clear, lineWidth: 1.5)
        )
        .fixedSize(horizontal: false, vertical: true)
    }

    private var actions: some View {
        VStack(spacing: FSpace.m) {
            FButton(FStrings.t("add_bot"), kind: .secondary, enabled: canAddBot) { game.addBot() }
            FButton(FStrings.t("start_game"), kind: .primary, enabled: canStart) { game.startGame() }
            if players.count < 2 {
                Text(FStrings.t("add_bot_hint"))
                    .font(FType.body(12))
                    .foregroundColor(FColor.textDim)
            }
        }
    }
}
