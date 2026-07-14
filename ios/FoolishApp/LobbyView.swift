// LobbyView.swift — the pre-game lobby for an online game (§16.D5). Shown while
// the online game's `view` is WAITING. Mirrors the web Lobby
// (src/components/Lobby.tsx): seated players (drag to reorder, ✕ to remove a
// bot), a bot picker that cycles the roster, Start, and a QR/invite code that
// deep-links into the game. Everything reacts to `game.roster` / `game.view`,
// which the player_views feed keeps live.

import SwiftUI
import FoolishKit

struct LobbyView: View {
    @ObservedObject var game: OnlineGame
    let onLeave: () -> Void

    private let maxPlayers = 8

    @State private var allBots: [OnlineService.BotOption] = []
    @State private var botIndex = 0
    /// Local seat order for optimistic drag-reorder; re-synced from the feed.
    @State private var order: [LobbyPlayer] = []

    private var seatedBotIds: Set<String> { Set(game.roster.filter(\.isAI).map(\.playerId)) }
    private var selectableBots: [OnlineService.BotOption] { allBots.filter { !seatedBotIds.contains($0.id) } }
    private var selectedBot: OnlineService.BotOption? {
        guard !selectableBots.isEmpty else { return nil }
        let i = ((botIndex % selectableBots.count) + selectableBots.count) % selectableBots.count
        return selectableBots[i]
    }
    private var canAddBot: Bool { game.roster.count < maxPlayers && !game.lobbyBusy }
    private var canStart: Bool { game.roster.count >= 2 && !game.lobbyBusy }
    /// The universal link a scan/tap resolves to the app (installed) or the site.
    private var gameURL: String { "https://foolish.cards/\(game.gameId)" }

    var body: some View {
        VStack(spacing: FSpace.m) {
            header
            playerList
            Spacer(minLength: FSpace.s)
            actions
        }
        .padding(FSpace.xl)
        .padding(.top, FSpace.m)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .task { allBots = await OnlineService.shared.fetchBots() }
        .onAppear { order = game.roster }
        .onChange(of: game.roster) { order = $0 }
    }

    // MARK: header + invite

    private var header: some View {
        VStack(spacing: FSpace.s) {
            Text(FStrings.t("lobby").uppercased())
                .font(FType.numeral(28))
                .foregroundColor(FColor.textPrimary)
            if let qr = QRCode.image(for: gameURL, points: 112) {
                Image(uiImage: qr)
                    .resizable().interpolation(.none)
                    .frame(width: 112, height: 112)
                    .padding(FSpace.xs)
                    .background(FColor.card)
                    .clipShape(RoundedRectangle(cornerRadius: 10, style: .continuous))
            }
            Text("\(FStrings.t("game_code")): \(game.gameId.uppercased())")
                .font(FType.body(13)).foregroundColor(FColor.textDim)
                .textSelection(.enabled)
        }
        .accessibilityElement(children: .combine)
    }

    // MARK: player list (reorder via drag, ✕ removes a bot)

    private var playerList: some View {
        List {
            ForEach(order) { p in
                plaque(p)
                    .listRowBackground(Color.clear)
                    .listRowSeparator(.hidden)
                    .listRowInsets(EdgeInsets(top: FSpace.xs, leading: 0, bottom: FSpace.xs, trailing: 0))
                    .deleteDisabled(!p.isAI)      // only bots can be removed
            }
            .onMove(perform: move)
            .onDelete(perform: remove)
        }
        .listStyle(.plain)
        .scrollContentBackground(.hidden)
        .environment(\.editMode, .constant(.active))   // always show reorder / remove affordances
        .frame(height: CGFloat(max(order.count, 1)) * 70)
    }

    private func move(from: IndexSet, to: Int) {
        order.move(fromOffsets: from, toOffset: to)
        game.rearrange(newOrder: order.map(\.playerId))
    }

    private func remove(at offsets: IndexSet) {
        for i in offsets where order.indices.contains(i) && order[i].isAI {
            game.removeBot(order[i].playerId)
        }
        order.remove(atOffsets: offsets)
    }

    private func plaque(_ p: LobbyPlayer) -> some View {
        let isMe = p.playerId == game.myPlayerId
        let display = p.name.hasPrefix("%") ? String(p.name.dropFirst()) : (p.name.isEmpty ? "P\(p.seat)" : p.name)
        return HStack(spacing: FSpace.m) {
            Image(systemName: p.isAI ? "cpu" : "person.fill")
                .font(.system(size: 15)).foregroundColor(FColor.textDim)
            Text(isMe ? "\(display) (\(FStrings.t("ios.you")))" : display)
                .font(FType.title(18)).foregroundColor(FColor.textPrimary).lineLimit(1)
            Spacer()
            Image(systemName: p.isAI ? "checkmark.circle.fill" : "circle")
                .font(.system(size: 16))
                .foregroundColor(p.isAI ? FColor.win : FColor.textDim)
        }
        .padding(.horizontal, FSpace.l)
        .frame(maxWidth: .infinity, minHeight: 54)
        .background(WoodSurface(seed: Double(abs(p.playerId.hashValue % 100)) / 100.0, cornerRadius: FRadius.button))
        .clipShape(RoundedRectangle(cornerRadius: FRadius.button, style: .continuous))
        .overlay(
            RoundedRectangle(cornerRadius: FRadius.button, style: .continuous)
                .strokeBorder(isMe ? FColor.accent.opacity(0.7) : .clear, lineWidth: 1.5)
        )
        .fixedSize(horizontal: false, vertical: true)
    }

    // MARK: actions

    private var actions: some View {
        VStack(spacing: FSpace.m) {
            botPickerRow
            FButton(FStrings.t("start_game"), kind: .primary, enabled: canStart) { game.startGame() }
            if game.roster.count < 2 {
                Text(FStrings.t("add_bot_hint"))
                    .font(FType.body(12)).foregroundColor(FColor.textDim)
            }
        }
    }

    private var botPickerRow: some View {
        let label = selectedBot.map { FStrings.t("add_bot_named", ["name": $0.displayName]) } ?? FStrings.t("add_bot")
        return HStack(spacing: FSpace.s) {
            if selectableBots.count > 1 { cycle("chevron.left", -1) }
            FButton(label, kind: .secondary, enabled: canAddBot) {
                game.addBot(botId: selectedBot?.id)   // nil → server random pick
            }
            if selectableBots.count > 1 { cycle("chevron.right", 1) }
        }
    }

    private func cycle(_ system: String, _ delta: Int) -> some View {
        Button(action: { Haptics.fire(.pickUp); botIndex += delta }) {
            Image(systemName: system)
                .font(.system(size: 18, weight: .semibold))
                .foregroundColor(FColor.textPrimary)
                .frame(width: 40, height: 52)
        }
    }
}
