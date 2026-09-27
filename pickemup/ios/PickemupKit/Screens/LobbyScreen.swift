// COPIED from foolish/ios/FoolishKit/Messages/LobbyScreens.swift at c3d99192 - replaced by lift step S9
//
// foolish's lobby with the Passing checkbox deleted, and with it the rules
// gate (D27): the title, the numbered roster (lowest free seat first), who
// deals, and one control, which is the kernel's verdict for this phone
// (pk_lobby_offered): Join, Start (plus Leave while I may), the invitation
// again when I am alone, or nothing but a line. Wood buttons, 52pt, square
// corners. Creating stages the invitation by itself: there is no Send
// invite button (UI.html "Lobby").
//
// The name gate is foolish's NameGateView folded in: the field shows when
// this phone has no nickname yet, and Join lights once the kernel's verdict
// on the name is OK (16 characters, D23).

import CPickemup
import SwiftUI

public struct LobbyScreen: View {
    @ObservedObject var model: TableModel
    @State private var name = PickemupSeats.nickname
    @FocusState private var nameFocused: Bool
    /// THE ROWS' MOTION IS THE KERNEL'S (grid "Join" and "Leave", A13): the
    /// plan a Join, a Leave or an adopted lobby bubble laid out, sampled as
    /// the table samples its own (BeatPlayer).
    @ObservedObject private var player: BeatPlayer
    @State private var anchors: [String: CGRect] = [:]
    private static let rowGap: CGFloat = 6

    public init(model: TableModel) {
        self.model = model
        self.player = model.player
    }

    private var offered: Int { model.table?.offered ?? 0 }
    private var needsName: Bool { PickemupSeats.nickname.isEmpty && offered == PK_LOBBY_JOIN }
    private var nameOK: Bool { Pk.nameVerdict(name) == PK_NAME_OK }

    public var body: some View {
        TimelineView(.animation(paused: !player.animating)) { _ in
            let fx = player.effects(player.ms(), anchors: anchors)
            content(fx)
                .environment(\.pkFX, fx)
                .coordinateSpace(name: boardSpace)
                .onPreferenceChange(PkAnchorKey.self) { anchors = $0 }
        }
    }

    /// One row's pitch: its own height and the gap under it.
    private var pitch: CGFloat { (anchors["roster.0"]?.height ?? 0) + Self.rowGap }

    private func row(_ text: String) -> some View {
        Text(text)
            .font(.system(size: 15, weight: .heavy))
            .onFeltText()
            .lineLimit(1)
    }

    private func content(_ fx: [String: PkFX]) -> some View {
        VStack(spacing: 12) {
            Text(Pk.string("LOBBY_TITLE"))
                .font(.system(size: 17, weight: .heavy))
                .onFeltText()
            VStack(alignment: .leading, spacing: Self.rowGap) {
                ForEach(0..<model.seatCount, id: \.self) { seat in
                    row(Pk.words(PK_API_W_LOBBY_ROW, seat))
                        .pkAnchor("roster.\(seat)")
                        .offset(y: (fx["roster.\(seat)"]?.close ?? 0) * pitch)
                }
            }
            .frame(maxWidth: .infinity, alignment: .topLeading)
            .overlay(alignment: .topLeading) {
                // the row that left, where it stood, as it read (A13)
                if let g = fx["roster.gone"], let k = g.gone {
                    row(Pk.words(PK_API_W_LOBBY_GONE, k))
                        .opacity(g.opacity)
                        .offset(y: CGFloat(k) * pitch)
                        .allowsHitTesting(false)
                }
            }
            .padding(.horizontal, 14)
            if model.seatCount > 0 {
                Text(Pk.words(PK_API_W_LOBBY_DEALER))
                    .font(.system(size: 12, weight: .semibold))
                    .onFeltText(FColor.textDim)
            }
            if needsName {
                NameField(name: $name, focused: $nameFocused)
            }
            controls
            if let foot = footLine {
                Text(foot)
                    .font(.system(size: 12, weight: .semibold))
                    .onFeltText(FColor.textDim)
            }
            Spacer(minLength: 0)
        }
        .padding(.horizontal, 22)
        .padding(.top, 22)
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .top)
        .background(FeltBackground())
    }

    @ViewBuilder private var controls: some View {
        HStack(spacing: 12) {
            switch offered {
            case PK_LOBBY_JOIN:
                WoodButton(title: Pk.string("BTN_JOIN"), height: 52, enabled: !needsName || nameOK, fontSize: 16) {
                    if needsName { PickemupSeats.nickname = name }
                    model.join()
                }
            case PK_LOBBY_START:
                WoodButton(title: Pk.string("BTN_START"), height: 52, fontSize: 16) { model.start() }
            default:
                // INVITE (alone), WAITING and FULL carry no control of their
                // own: the foot line says which (UI.html Lobby 01, 03, 05).
                EmptyView()
            }
            if (model.table?.canExit ?? 0) != 0 {
                WoodButton(title: Pk.string("BTN_LEAVE"), height: 52, fontSize: 16) { model.leave() }
            }
        }
    }

    private var footLine: String? {
        switch offered {
        case PK_LOBBY_WAITING: return Pk.string("LOBBY_WAITING")
        case PK_LOBBY_INVITE:  return Pk.string("LOBBY_ALONE")
        case PK_LOBBY_FULL:    return Pk.string("LOBBY_FULL")
        default:               return nil
        }
    }
}

/// The nickname field: foolish's white field on the felt, 40pt.
struct NameField: View {
    @Binding var name: String
    var focused: FocusState<Bool>.Binding

    var body: some View {
        TextField(Pk.string("LOBBY_NAME_PROMPT"), text: $name)
            .textInputAutocapitalization(.words)
            .autocorrectionDisabled()
            .submitLabel(.done)
            .focused(focused)
            .font(.system(size: 15))
            .foregroundColor(.black)
            .padding(.horizontal, 12)
            .frame(height: 40)
            .background(RoundedRectangle(cornerRadius: 8).fill(Color.white.opacity(0.92)))
            .pkAnchor("name")
    }
}

/// Before the first game on this phone: the name, then the new lobby.
public struct NameGateScreen: View {
    @State private var name = ""
    @FocusState private var focused: Bool
    let onDone: () -> Void

    public init(onDone: @escaping () -> Void) { self.onDone = onDone }

    public var body: some View {
        VStack(spacing: 12) {
            Text(Pk.string("LOBBY_TITLE"))
                .font(.system(size: 17, weight: .heavy))
                .onFeltText()
            NameField(name: $name, focused: $focused)
            WoodButton(title: Pk.string("LOBBY_TITLE"), height: 52, enabled: Pk.nameVerdict(name) == PK_NAME_OK,
                       fontSize: 16) {
                PickemupSeats.nickname = name
                onDone()
            }
            Spacer(minLength: 0)
        }
        .padding(.horizontal, 22)
        .padding(.top, 22)
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .top)
        .background(FeltBackground())
    }
}

/// A link the kernel refused: why, in its words (6.5).
public struct UnreadableScreen: View {
    let code: Int
    public init(code: Int) { self.code = code }

    public var body: some View {
        VStack(spacing: 8) {
            Text(Pk.string("UNREADABLE"))
                .font(.system(size: 17, weight: .heavy))
                .onFeltText()
            Text(Pk.words(PK_API_W_ERROR, code))
                .font(.system(size: 13, weight: .semibold))
                .onFeltText(FColor.textDim)
                .multilineTextAlignment(.center)
            Spacer(minLength: 0)
        }
        .padding(22)
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .top)
        .background(FeltBackground())
    }
}

/// The rules page (6.6): the kernel's title and lines.
public struct RulesSheet: View {
    public init() {}
    public var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 12) {
                Text(Pk.words(PK_API_W_RULES_TITLE))
                    .font(.system(size: 20, weight: .heavy))
                // as many lines as the kernel has (PK_RULES_N)
                ForEach(Array(Pk.rules.enumerated()), id: \.offset) { _, line in
                    Text(line).font(.system(size: 15))
                }
            }
            .padding(20)
            .frame(maxWidth: .infinity, alignment: .leading)
        }
    }
}
