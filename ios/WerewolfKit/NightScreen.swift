// The night screen. ONE screen, and every role gets the same one.
//
// That is the product, not a shortcut. The prompt differs by a few words, the
// wolves get a channel below the board, and everything else - the grid, the taps,
// the countdown, the button - is identical, because anybody watching the thread
// can see WHO sent and WHEN, and the only defence is that those two facts mean
// nothing.
//
// What this file may not do, restated where it is most tempting to break: it
// answers no rule. Who is a wolf, what this seat may see, whose call tonight is,
// whether Send is legal - the kernel, every time, through NightModel.
import SwiftUI

public struct NightScreen: View {
    @ObservedObject private var model: NightModel

    /// The game's identity and the bubble this one is composed against. Held by
    /// the extension, because they are Messages' business and not the kernel's.
    private let gameId: UInt64
    private let parent: Data?
    private let lastSealAt: UInt16

    /// Handed the sealed payload to stage. The human always presses send - this
    /// closure never sends anything itself.
    private let stage: (Data) -> Void

    public init(model: NightModel,
                gameId: UInt64,
                parent: Data?,
                lastSealAt: UInt16,
                stage: @escaping (Data) -> Void) {
        self.model = model
        self.gameId = gameId
        self.parent = parent
        self.lastSealAt = lastSealAt
        self.stage = stage
    }

    public var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 18) {
                header
                board
                if model.amWolf { packChannel }
                if let r = model.refusal { refusalLine(r) }
                sendButton
                waiting
            }
            .padding(.horizontal, Night.gutter)
            .padding(.vertical, 20)
        }
        .background(Night.ground.ignoresSafeArea())
        .foregroundStyle(Night.ink)
    }

    // ------------------------------------------------------------- header ---

    private var header: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text("Night \(model.night + 1)")
                .font(.system(size: 13, weight: .semibold))
                .tracking(1.6)
                .foregroundStyle(Night.quiet)
            Text(model.prompt)
                .font(.system(size: 24, weight: .semibold))
                .fixedSize(horizontal: false, vertical: true)
            // The role, said plainly to the one person entitled to know it. It is
            // on this screen and nowhere else - there is no shared surface that
            // could ever render somebody else's.
            Text(roleLine)
                .font(.system(size: 14))
                .foregroundStyle(Night.quiet)
        }
        .accessibilityElement(children: .combine)
    }

    private var roleLine: String {
        switch model.myRole {
        case .wolf:
            guard let d = model.decider else { return "You are a wolf." }
            return d == model.mySeat
                ? "You are a wolf, and tonight the call is yours."
                : "You are a wolf. Tonight \(model.name(d)) makes the call."
        case .seer:     return "You are the seer."
        case .villager: return "You are a villager."
        case .unknown:  return "You are watching."
        }
    }

    // -------------------------------------------------------------- board ---

    private var board: some View {
        LazyVGrid(columns: [GridItem(.adaptive(minimum: 148), spacing: 10)], spacing: 10) {
            ForEach(0..<model.seatCount, id: \.self) { seat in
                SeatTile(model: model, seat: seat, picked: model.picked == seat)
                    .onTapGesture { model.pick(seat) }
                    .allowsHitTesting(seat != model.mySeat && model.isAlive(seat) && !model.iHaveSent)
            }
        }
    }

    // ------------------------------------------------------------ channel ---

    private var packChannel: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text("The pack")
                .font(.system(size: 12, weight: .semibold))
                .tracking(1.4)
                .foregroundStyle(Night.pack.opacity(0.9))
            ForEach(Array(model.channel.enumerated()), id: \.offset) { _, row in
                HStack(alignment: .top, spacing: 8) {
                    Text(model.name(row.seat))
                        .font(.system(size: 13, weight: .semibold))
                        .foregroundStyle(Night.pack.opacity(0.95))
                    Text(row.line)
                        .font(.system(size: 13))
                        .fixedSize(horizontal: false, vertical: true)
                }
            }
            if !model.iHaveSent {
                // ONE FIELD, AND IT IS NOT A MESSAGE. What is typed here rides
                // inside the same record as the pick - the line and the choice are
                // one bubble, always, because a second bubble is countable and
                // three extra bubbles on a five-player night is three wolves.
                TextField("One line to the pack", text: $model.line, axis: .vertical)
                    .textFieldStyle(.plain)
                    .font(.system(size: 14))
                    .padding(10)
                    .background(RoundedRectangle(cornerRadius: 10).fill(Night.raised))
                    .overlay(RoundedRectangle(cornerRadius: 10).stroke(Night.pack.opacity(0.45), lineWidth: 1))
                    .onChange(of: model.line) { _, new in
                        // Clipped here as well as in the kernel, so the field cannot
                        // show a line longer than the one that will be sent.
                        let bytes = Array(new.utf8)
                        if bytes.count > Kernel.shared.chatMax {
                            model.line = String(decoding: bytes.prefix(Kernel.shared.chatMax), as: UTF8.self)
                        }
                    }
                Text("Rides inside your choice. It is not a second message.")
                    .font(.system(size: 11))
                    .foregroundStyle(Night.quiet)
            }
        }
        .padding(14)
        .background(RoundedRectangle(cornerRadius: Night.corner).fill(Night.pack.opacity(0.10)))
        .overlay(RoundedRectangle(cornerRadius: Night.corner).stroke(Night.pack.opacity(0.35), lineWidth: 1))
    }

    private func refusalLine(_ text: String) -> some View {
        Text(text)
            .font(.system(size: 13, weight: .medium))
            .foregroundStyle(Night.chosen)
    }

    // --------------------------------------------------------------- send ---

    private var sendButton: some View {
        let floor = model.floorRemaining
        let ready = floor == 0 && model.picked != nil && !model.iHaveSent
        return VStack(spacing: 8) {
            Button {
                if let payload = model.send(gameId: gameId, parent: parent) { stage(payload) }
            } label: {
                Text(buttonTitle(floor: floor))
                    .font(.system(size: 17, weight: .semibold))
                    .frame(maxWidth: .infinity, minHeight: 50)
            }
            .buttonStyle(.plain)
            .background(RoundedRectangle(cornerRadius: Night.corner)
                .fill(ready ? Night.chosen : Night.raised))
            .foregroundStyle(ready ? Night.ground : Night.quiet)
            .disabled(!ready)
            // THE TEN-SECOND FLOOR, and this is the whole of what the user sees of
            // it. It is here because an instant answer means you had no decision to
            // make, and the only players with no decision to make are the villagers
            // - so ten seconds of everyone waiting costs the village nothing and
            // costs the wolves their last tell.
            if floor > 0 && !model.iHaveSent {
                Text("Everyone takes a moment. \(floor)s")
                    .font(.system(size: 12))
                    .foregroundStyle(Night.quiet)
            }
            if model.carryOffered(lastSealAt: lastSealAt) && !model.iHaveSent {
                Button {
                    if let payload = model.send(gameId: gameId, parent: parent, carry: true) { stage(payload) }
                } label: {
                    Text("Send, and pass for whoever is asleep")
                        .font(.system(size: 13, weight: .medium))
                }
                .buttonStyle(.plain)
                .foregroundStyle(Night.quiet)
                .disabled(floor > 0)
            }
        }
    }

    private func buttonTitle(floor: Int) -> String {
        if model.iHaveSent { return "Sent" }
        if model.picked == nil { return "Choose somebody" }
        if floor > 0 { return "Send" }
        return "Send"
    }

    // ------------------------------------------------------------ waiting ---

    private var waiting: some View {
        let left = model.waitingOn
        return Group {
            if left.isEmpty {
                Text("Everyone has sent. The morning is next.")
                    .font(.system(size: 13))
                    .foregroundStyle(Night.quiet)
            } else {
                Text("Still to send: \(left.map { model.name($0) }.joined(separator: ", "))")
                    .font(.system(size: 13))
                    .foregroundStyle(Night.quiet)
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
    }
}

// One seat. The tile for a wolf and the tile for a villager are the same tile -
// there is nothing on it that a role could change, and that is checked in C
// (ww_view.c contract 1), not here.
struct SeatTile: View {
    @ObservedObject var model: NightModel
    let seat: Int
    let picked: Bool

    var body: some View {
        let alive = model.isAlive(seat)
        let known = model.role(of: seat)
        return VStack(alignment: .leading, spacing: 6) {
            HStack(spacing: 6) {
                Text(model.name(seat))
                    .font(.system(size: 15, weight: .semibold))
                    .lineLimit(1)
                if seat == model.mySeat {
                    Text("you")
                        .font(.system(size: 11, weight: .medium))
                        .foregroundStyle(Night.quiet)
                }
            }
            Text(subtitle(alive: alive, known: known))
                .font(.system(size: 12))
                .foregroundStyle(Night.quiet)
                .lineLimit(1)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(12)
        .background(RoundedRectangle(cornerRadius: Night.corner)
            .fill(picked ? Night.chosen.opacity(0.18) : Night.raised))
        .overlay(RoundedRectangle(cornerRadius: Night.corner)
            .stroke(picked ? Night.chosen : Night.edge, lineWidth: picked ? 2 : 1))
        .opacity(alive ? 1 : 0.45)
        .accessibilityElement(children: .combine)
        .accessibilityAddTraits(picked ? [.isButton, .isSelected] : .isButton)
    }

    private func subtitle(alive: Bool, known: Role) -> String {
        if !alive {
            switch known {
            case .wolf:     return "out - was a wolf"
            case .seer:     return "out - was the seer"
            case .villager: return "out - was a villager"
            case .unknown:  return "out"
            }
        }
        // A LIVING SEAT'S LINE SAYS ONLY WHETHER THEY SENT. Same words for a wolf
        // and a villager, which is the contract this screen exists to honour.
        // The pack line is the one exception and it is only ever rendered on a
        // wolf's own device.
        if known == .wolf && seat != model.mySeat { return packSubtitle }
        switch model.sent(seat) {
        case .no:      return "has not sent"
        case .yes:     return "sent"
        case .carried: return "passed for"
        }
    }

    private var packSubtitle: String {
        switch model.sent(seat) {
        case .no:      return "pack - has not sent"
        case .yes:     return "pack - sent"
        case .carried: return "pack - passed for"
        }
    }
}
