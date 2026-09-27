// COPIED from pickemup/ios/PickemupKit/Screens/LobbyScreen.swift at 8e216923 (itself COPIED from foolish/ios/FoolishKit/Messages/LobbyScreens.swift at c3d99192) - replaced by the lobby lift (T2)
//
// pickemup's lobby, on the TallyKernel seam: the title, the numbered roster,
// who starts, and one control, which is the kernel's verdict for this phone
// (LobbyModel.offer): Join, Start (plus Leave while I may), or nothing but a
// line. Wood buttons, 52pt, square corners. Creating stages the invitation by
// itself: there is no Send invite button.
//
// The name gate is folded in as pickemup's is: the field shows when this
// phone has no nickname yet, and Join lights once the kernel's verdict on the
// name is OK.

import SwiftUI

public struct LobbyScreen: View {
    @ObservedObject var model: TallyTable
    @State private var name = TallybonesSeats.nickname
    @FocusState private var nameFocused: Bool

    public init(model: TallyTable) { self.model = model }

    private var lobby: LobbyModel { model.view.lobby }
    private var needsName: Bool { TallybonesSeats.nickname.isEmpty && lobby.offer == .join }
    private var nameOK: Bool { model.kernel.nameIsOK(name) }
    private func s(_ k: TallyString) -> String { model.kernel.string(k) }

    public var body: some View {
        VStack(spacing: 12) {
            Text(s(.lobbyTitle))
                .font(.system(size: 17, weight: .heavy))
                .onFeltText()
            VStack(alignment: .leading, spacing: 6) {
                ForEach(Array(lobby.rows.enumerated()), id: \.offset) { i, row in
                    Text(row)
                        .font(.system(size: 15, weight: .heavy))
                        .onFeltText()
                        .lineLimit(1)
                        .tbAnchor("roster.\(i)")
                }
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(.horizontal, 14)
            if let foot = lobby.footnote {
                Text(foot)
                    .font(.system(size: 12, weight: .semibold))
                    .onFeltText(FColor.textDim)
            }
            if needsName {
                NameField(prompt: s(.namePrompt), name: $name, focused: $nameFocused)
            }
            controls
            if let line = footLine {
                Text(line)
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
            switch lobby.offer {
            case .join:
                WoodButton(title: s(.join), height: 52, enabled: !needsName || nameOK, fontSize: 16) {
                    if needsName { TallybonesSeats.set(nickname: name, kernel: model.kernel) }
                    model.join()
                }
            case .start:
                WoodButton(title: s(.start), height: 52, fontSize: 16) { model.start() }
            case .none, .invite, .waiting, .full:
                // no control of its own: the foot line says which
                EmptyView()
            }
            if lobby.canLeave {
                WoodButton(title: s(.leave), height: 52, fontSize: 16) { model.leave() }
            }
        }
    }

    private var footLine: String? {
        switch lobby.offer {
        case .waiting: return s(.lobbyWaiting)
        case .invite:  return s(.lobbyAlone)
        case .full:    return s(.lobbyFull)
        default:       return nil
        }
    }
}

/// The nickname field: foolish's white field on the felt, 40pt.
struct NameField: View {
    let prompt: String
    @Binding var name: String
    var focused: FocusState<Bool>.Binding

    var body: some View {
        TextField(prompt, text: $name)
            .textInputAutocapitalization(.words)
            .autocorrectionDisabled()
            .submitLabel(.done)
            .focused(focused)
            .font(.system(size: 15))
            .foregroundColor(.black)
            .padding(.horizontal, 12)
            .frame(height: 40)
            .background(RoundedRectangle(cornerRadius: 8).fill(Color.white.opacity(0.92)))
            .tbAnchor("name")
    }
}

/// Before the first game on this phone: the name, then the new lobby.
public struct NameGateScreen: View {
    let kernel: TallyKernel
    @State private var name = ""
    @FocusState private var focused: Bool
    let onDone: () -> Void

    public init(kernel: TallyKernel, onDone: @escaping () -> Void) {
        self.kernel = kernel
        self.onDone = onDone
    }

    public var body: some View {
        VStack(spacing: 12) {
            Text(kernel.string(.lobbyTitle))
                .font(.system(size: 17, weight: .heavy))
                .onFeltText()
            NameField(prompt: kernel.string(.namePrompt), name: $name, focused: $focused)
            WoodButton(title: kernel.string(.lobbyTitle), height: 52, enabled: kernel.nameIsOK(name),
                       fontSize: 16) {
                TallybonesSeats.set(nickname: name, kernel: kernel)
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

/// A link the kernel refused: why, in its words.
public struct UnreadableScreen: View {
    let kernel: TallyKernel
    let code: Int
    public init(kernel: TallyKernel, code: Int) {
        self.kernel = kernel
        self.code = code
    }

    public var body: some View {
        VStack(spacing: 8) {
            Text(kernel.string(.unreadable))
                .font(.system(size: 17, weight: .heavy))
                .onFeltText()
            Text(kernel.errorLine(code))
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
