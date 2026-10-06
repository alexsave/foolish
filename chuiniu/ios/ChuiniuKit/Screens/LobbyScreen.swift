// LobbyScreen.swift - the table before the first throw (the study's lobby,
// UI.html `screen(... 'lobby')`; DECISIONS I27): the title, a water hairline,
// the roster in seat order as seat bands, each with its name in the small
// caps and its cup seen from above, the foot line, then the controls: the
// name field with Join for a phone that is not seated, Start and Leave for
// one that is. Which is shown, and lit or sunk, is the kernel's verdict for
// this phone (`TableModel.offered`, `canExit`, `Kernel.nameAccepted`), read
// by `LobbyControls`; nothing here decides a rule. On the planks, like every
// screen.
//
// THE TITLE IS LATIN. The study's lockup is 吹牛 over "Chui Niu", but no CJK
// face is bundled and none with an OFL licence was at hand to subset, so the
// title is the kernel's GAME_NAME in the roman, as before.
//
// EVERY CONTROL STAYS IN THE DRAWER: the roster scrolls between the title
// and the controls, so a compact drawer (about 328 points) with six seats
// still shows Start and Leave, each at least 44 points tall.

import SwiftUI

/// What the lobby offers this phone, read from the kernel's verdicts: one
/// field per control, nil when it is not shown. A pure reading, so a test
/// asks it of every offer.
struct LobbyControls: Equatable {
    /// The name field with Join beside it: lit once the kernel accepts the
    /// name typed (`Kernel.nameAccepted`), sunk before.
    var join: PlankKind?
    /// Start: lit when the kernel offers this phone START, sunk while it is
    /// seated and may not.
    var start: PlankKind?
    /// Leave: quiet when the kernel says I may get up (`canExit`), sunk
    /// while I am seated and may not (alone at the table).
    var leave: PlankKind?
    /// The line under the roster.
    var foot: Word?

    /// nil once the game has started: there is no lobby.
    static func of(_ t: TableModel, nameAccepted: Bool) -> LobbyControls? {
        guard t.phase == .lobby else { return nil }
        let seated = t.me != nil
        let leave: PlankKind? = seated ? (t.canExit ? .quiet : .sunk) : nil
        switch t.offered {
        case .join:
            return LobbyControls(join: nameAccepted ? .glow : .sunk, start: nil, leave: nil, foot: nil)
        case .start:
            return LobbyControls(join: nil, start: .glow, leave: leave, foot: nil)
        case .full:
            return LobbyControls(join: nil, start: nil, leave: nil, foot: .lobbyFull)
        case .waiting, .alone:
            return LobbyControls(join: nil, start: seated ? .sunk : nil, leave: leave, foot: .lobbyWaiting)
        }
    }
}

/// Where each of the lobby's parts landed, in the screen's own points (the
/// tests' frames): "name", "join", "start", "leave", and "screen".
struct LobbyFrames: PreferenceKey {
    static let defaultValue: [String: CGRect] = [:]
    static func reduce(value: inout [String: CGRect], nextValue: () -> [String: CGRect]) {
        value.merge(nextValue()) { $1 }
    }
}

extension View {
    fileprivate func lobbyFrame(_ key: String) -> some View {
        background(GeometryReader { g in
            Color.clear.preference(key: LobbyFrames.self, value: [key: g.frame(in: .named(LobbyScreen.space))])
        })
    }
}

public struct LobbyScreen: View {
    @ObservedObject var host: ChuiniuHost
    @State private var name = ""
    @FocusState private var nameFocused: Bool
    /// The tests' window on the layout: every part's frame as laid out.
    private var onFrames: (([String: CGRect]) -> Void)?

    static let space = "lobby"
    /// The least a control may be tall: the system's tap target.
    static let controlHeight: CGFloat = 44

    public init(host: ChuiniuHost) { self.host = host }
    init(host: ChuiniuHost, onFrames: @escaping ([String: CGRect]) -> Void) {
        self.host = host
        self.onFrames = onFrames
    }

    private var table: TableModel { host.table }
    private var controls: LobbyControls? { LobbyControls.of(table, nameAccepted: host.kernel.nameAccepted(name)) }

    public var body: some View {
        GeometryReader { geo in
            let tall = geo.size.height > 400
            VStack(alignment: .leading, spacing: 0) {
                Text(host.word(.lobbyTitle))
                    .font(FType.serif(tall ? 34 : 26))
                    .bidInk()
                    .frame(maxWidth: .infinity)
                WaterRule().padding(.vertical, tall ? 16 : 8)
                ScrollView(.vertical, showsIndicators: false) {
                    VStack(alignment: .leading, spacing: 8) {
                        ForEach(table.seats) { seat in SeatRow(seat: seat, you: host.word(.lobbyYou)) }
                        if let foot = controls?.foot {
                            Text(host.word(foot))
                                .font(FType.serif(15.5))
                                .onPlanks(Ink.inkdim)
                                .padding(.top, 6)
                        }
                    }
                }
                .scrollBounceBehavior(.basedOnSize)
                .frame(maxHeight: .infinity, alignment: .top)
                if let c = controls { buttons(c).padding(.top, 10) }
            }
            .padding(.horizontal, 18)
            .padding(.top, tall ? 26 : 14)
            .padding(.bottom, 14)
            .frame(width: geo.size.width, height: geo.size.height, alignment: .top)
            .lobbyFrame("screen")
        }
        .coordinateSpace(name: Self.space)
        .background(PlanksBackground().ignoresSafeArea())
        .onAppear { if name.isEmpty { name = host.kernel.currentNickname } }
        .onPreferenceChange(LobbyFrames.self) { onFrames?($0) }
    }

    @ViewBuilder private func buttons(_ c: LobbyControls) -> some View {
        VStack(spacing: 10) {
            if let join = c.join {
                HStack(spacing: 10) {
                    NameField(prompt: host.word(.namePrompt), name: $name, focused: $nameFocused)
                        .lobbyFrame("name")
                    PlankButton(title: host.word(.join), kind: join, width: 120, height: Self.controlHeight, seed: 11) {
                        nameFocused = false
                        host.join(name: name)
                    }
                    .fixedSize()
                    .lobbyFrame("join")
                }
            }
            if c.start != nil || c.leave != nil {
                HStack(spacing: 10) {
                    if let start = c.start {
                        PlankButton(title: host.word(.start), kind: start, height: Self.controlHeight, seed: 12) { host.start() }
                            .lobbyFrame("start")
                    }
                    if let leave = c.leave {
                        PlankButton(title: host.word(.leave), kind: leave, width: 120, height: Self.controlHeight, seed: 13) { host.leave() }
                            .fixedSize()
                            .lobbyFrame("leave")
                    }
                }
            }
        }
    }
}

/// One seat on the roster (`.seatband`, 40 tall): the name in the small
/// caps, "(you)" dim after mine, and the seat's cup from above with the dice
/// it sits down with.
struct SeatRow: View {
    let seat: SeatModel
    let you: String

    var body: some View {
        HStack(spacing: 10) {
            HStack(spacing: 6) {
                Text(seat.name)
                    .font(FType.sc(14))
                    .tracking(FType.nameTracking(14))
                    .onPlanks(Ink.ink)
                if seat.isMe {
                    Text(you)
                        .font(FType.sc(14))
                        .tracking(14 * 0.06)
                        .onPlanks(Color(hex: 0x6F8A81))
                }
            }
            .lineLimit(1)
            Spacer(minLength: 0)
            Cup(width: 28, count: seat.dice > 0 ? seat.dice : nil, seed: seat.id * 7)
        }
        .frame(height: 40)
        .padding(.horizontal, 12)
        .background(SeatBand())
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
