// COPIED from pickemup/ios/PickemupKit/Screens/LobbyScreen.swift at 03eb3362 - a later lift into shared/ replaces it
//
// pickemup's lobby shape: the title, the roster in seat order, and one
// control, which is the kernel's verdict for this phone (`TableModel.offered`):
// Join (with the name field), Start, or nothing but a foot line. Wood
// buttons, 52pt, square corners. The rules gate, Leave and the dealer line are
// not in the proof of concept.

import SwiftUI

public struct LobbyScreen: View {
    @ObservedObject var host: ChuiniuHost
    @State private var name = ""
    @FocusState private var nameFocused: Bool

    public init(host: ChuiniuHost) { self.host = host }

    private var table: TableModel { host.table }

    public var body: some View {
        VStack(spacing: 12) {
            Text(host.word(.lobbyTitle))
                .font(.system(size: 17, weight: .heavy))
                .onFeltText()
            VStack(alignment: .leading, spacing: 8) {
                ForEach(table.seats) { seat in
                    HStack(spacing: 10) {
                        Cup(width: 20)
                        Text(seat.lobbyRow.isEmpty ? seat.name : seat.lobbyRow)
                            .font(.system(size: 15, weight: .heavy))
                            .onFeltText()
                            .lineLimit(1)
                    }
                }
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(.horizontal, 14)
            if table.offered == .join {
                NameField(prompt: host.word(.namePrompt), name: $name, focused: $nameFocused)
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
        .onAppear { if name.isEmpty { name = host.kernel.currentNickname } }
    }

    @ViewBuilder private var controls: some View {
        switch table.offered {
        case .join:
            WoodButton(title: host.word(.join), height: 52, enabled: host.kernel.nameAccepted(name), fontSize: 16) {
                nameFocused = false
                host.join(name: name)
            }
        case .start:
            WoodButton(title: host.word(.start), height: 52, fontSize: 16) { host.start() }
        case .waiting, .full, .alone:
            EmptyView()
        }
    }

    private var footLine: String? {
        switch table.offered {
        case .waiting: return host.word(.lobbyWaiting)
        case .alone:   return host.word(.lobbyWaiting)
        case .full:    return host.word(.lobbyFull)
        case .join, .start: return nil
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
    }
}
