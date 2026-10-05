// LobbyScreen.swift - the table before the first throw (the study's lobby,
// UI.html `screen(... 'lobby')`; DECISIONS I27): the title, a water hairline,
// the roster in seat order as seat bands each with its cup seen from above,
// and one control, which is the kernel's verdict for this phone
// (`TableModel.offered`): Join (with the name field), Start, or nothing but a
// foot line. On the planks, like every screen.

import SwiftUI

public struct LobbyScreen: View {
    @ObservedObject var host: ChuiniuHost
    @State private var name = ""
    @FocusState private var nameFocused: Bool

    public init(host: ChuiniuHost) { self.host = host }

    private var table: TableModel { host.table }

    public var body: some View {
        GeometryReader { geo in
            let tall = geo.size.height > 400
            VStack(alignment: .leading, spacing: 0) {
                Text(host.word(.lobbyTitle))
                    .font(FType.serif(tall ? 34 : 26))
                    .bidInk()
                    .frame(maxWidth: .infinity)
                WaterRule().padding(.vertical, tall ? 16 : 8)
                VStack(spacing: 8) {
                    ForEach(table.seats) { seat in
                        HStack(spacing: 10) {
                            Text(seat.lobbyRow.isEmpty ? seat.name : seat.lobbyRow)
                                .font(FType.sc(14))
                                .tracking(FType.nameTracking(14))
                                .onPlanks(Ink.ink)
                                .lineLimit(1)
                            Spacer(minLength: 0)
                            Cup(width: 28, count: seat.dice > 0 ? seat.dice : nil, seed: seat.id * 7)
                        }
                        .frame(height: 40)
                        .padding(.horizontal, 12)
                        .background(SeatBand())
                    }
                }
                if let foot = footLine {
                    Text(foot)
                        .font(FType.serif(15.5))
                        .onPlanks(Ink.inkdim)
                        .padding(.top, 6)
                }
                Spacer(minLength: 0)
                controls
            }
            .padding(.horizontal, 18)
            .padding(.top, tall ? 26 : 14)
            .padding(.bottom, 14)
            .frame(width: geo.size.width, height: geo.size.height, alignment: .top)
        }
        .background(PlanksBackground().ignoresSafeArea())
        .onAppear { if name.isEmpty { name = host.kernel.currentNickname } }
    }

    @ViewBuilder private var controls: some View {
        switch table.offered {
        case .join:
            HStack(spacing: 10) {
                NameField(prompt: host.word(.namePrompt), name: $name, focused: $nameFocused)
                PlankButton(title: host.word(.join), kind: host.kernel.nameAccepted(name) ? .glow : .sunk, width: 120, seed: 11) {
                    nameFocused = false
                    host.join(name: name)
                }
                .fixedSize()
            }
        case .start:
            PlankButton(title: host.word(.start), kind: .glow, seed: 12) { host.start() }
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

/// `.rule-water`: a cold hairline, bright in the middle, gone at the ends.
struct WaterRule: View {
    var body: some View {
        LinearGradient(colors: [.clear, Ink.glow.opacity(0.45), .clear], startPoint: .leading, endPoint: .trailing)
            .frame(height: 1)
            .shadow(color: Ink.glow.opacity(0.25), radius: 3)
    }
}

/// The nickname field (`.field`): a dark well on the planks with a cold
/// hairline, the name in the roman, the caret in the glow.
struct NameField: View {
    let prompt: String
    @Binding var name: String
    var focused: FocusState<Bool>.Binding

    var body: some View {
        TextField("", text: $name, prompt: Text(prompt).font(FType.serif(17)).foregroundStyle(Color(hex: 0x5F7A72)))
            .textInputAutocapitalization(.words)
            .autocorrectionDisabled()
            .submitLabel(.done)
            .focused(focused)
            .font(FType.serif(17))
            .foregroundStyle(Ink.ink)
            .tint(Ink.glow)
            .padding(.horizontal, 14)
            .frame(height: 44)
            .background(
                RoundedRectangle(cornerRadius: 4, style: .circular)
                    .fill(Color(.sRGB, red: 2 / 255, green: 10 / 255, blue: 10 / 255, opacity: 0.62))
                    .overlay(RoundedRectangle(cornerRadius: 4, style: .circular).strokeBorder(Ink.glow.opacity(0.12), lineWidth: 1))
            )
    }
}
