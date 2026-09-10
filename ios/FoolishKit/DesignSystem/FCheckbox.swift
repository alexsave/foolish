// FCheckbox.swift - a wooden tick box.
//
// The owner, on the podkidnoy variant: "we'll need a wooden style checkbox in
// the lobby to set this." There was no checkbox in this app at all until now,
// and a system Toggle would have been the wrong thing twice over: it is a glass
// iOS switch on a table made of wool and wood, and it is a control whose whole
// meaning here is "a rule of this game is on" rather than "a setting of your
// phone is on".
//
// So it is built from the pieces the board is already made of: a WoodFill plank
// with the same rim FButton's wood kind wears, and the SAME hand-drawn FCheck
// the seat badges use to say a player has spoken. The tick a player sees under
// a name and the tick in this box are one glyph, deliberately - a check means
// "yes, this" everywhere in this app.
//
// The whole row is the target, label included: a 24pt box is under the
// 44pt-touch minimum on its own, and reaching for the words is what everyone
// does anyway.
//
// ONE WORD, and the box centred on it (owner, 1.0(17)). The label carried a
// parenthetical and, when clear, a second line explaining the other rule; both
// are gone. A checkbox in a lobby names the thing it turns on and nothing else -
// what the two games ARE is the rulebook's job, and the lobby has a rulebook
// button two inches away.

import SwiftUI

// Not `public`: nothing outside FoolishKit names it, and a public View in a
// dynamic framework exports its whole SwiftUI generic tree as symbol names
// (see RulesView.swift for the measurement).
struct FCheckbox: View {
    private let title: String
    private let isOn: Bool
    private let enabled: Bool
    private let action: (Bool) -> Void
    private let turn: Turn?

    /// SOMEBODY ELSE MOVED THIS BOX, and the plan says to show that happening
    /// (1.1(56); owner: "lets do the 'rotate in' or out thing for the
    /// checkbox"). `token` is bumped once per rules BEAT and `seconds` is the
    /// beat's own duration - both the kernel's (SurfacePlan), so the box turns
    /// at the same pace a card flies.
    ///
    /// DRIVEN BY THE BEAT, NOT BY `isOn`. A local tap must still feel instant -
    /// the tick moves under the finger the moment it lands (`passingWish`) -
    /// and a turn keyed on the value would take that away from the one person
    /// who already knows what they did.
    struct Turn: Equatable {
        let token: Int
        let seconds: TimeInterval
    }

    public init(_ title: String, isOn: Bool,
                enabled: Bool = true, turn: Turn? = nil,
                action: @escaping (Bool) -> Void) {
        self.title = title
        self.isOn = isOn
        self.enabled = enabled
        self.turn = turn
        self.action = action
    }

    private let box: CGFloat = 26

    /// HOW FAR THE BOX IS TURNED - 1 upright, edge-on at the halfway point.
    ///
    /// The SAME motion the seat badge makes when a fact about it stops being
    /// true (`FSeatBadge.badgeTurn`): an x-axis scale about the centre, 1 down
    /// to 0.001, which reads as a flat object turning away from you. Not a
    /// `rotation3DEffect`, deliberately - a real perspective rotation would be
    /// a second, differently-shaped turn standing next to the badge's, and the
    /// owner's note about the badge ("rotate only the badge, dim the name") is
    /// about there being ONE idiom here, not two that look nearly alike. 0.001
    /// rather than 0 for the badge's own reason: a view scaled to exactly zero
    /// can stop being laid out at all, and the row's height would twitch.
    static func boxTurn(edgeOn: Bool) -> CGFloat { edgeOn ? 0.001 : 1 }

    /// Half-way through the beat the box is edge-on; the tick it comes back
    /// with is the new one. One @State rather than two so the two halves cannot
    /// get out of step.
    @State private var edgeOn = false

    public var body: some View {
        Button(action: { Haptics.fire(.drop); action(!isOn) }) {
            // CENTRES, not baselines: the label is one word, so there is no
            // block of text for a baseline to belong to - the box and the word
            // are two objects of a size, and the eye lines up their middles.
            HStack(alignment: .center, spacing: FSpace.s) {
                plank
                Text(title)
                    .font(FType.body(15))
                    .onTableText(dimmed: !enabled)
                    .lineLimit(1)
                Spacer(minLength: 0)
            }
            // The row, not the box, is what a finger has to find.
            .contentShape(Rectangle())
            .frame(minHeight: 44)
        }
        .buttonStyle(.plain)
        .disabled(!enabled)
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(Text(title))
        .accessibilityValue(Text(isOn ? FStrings.t("ios.a11y.on") : FStrings.t("ios.a11y.off")))
        .accessibilityAddTraits(isOn ? [.isButton, .isSelected] : .isButton)
        // The beat landed. Turn edge-on over the first half of it and back over
        // the second, so the whole motion measures exactly what the plan said.
        //
        // WATCHING THE WHOLE VALUE, not `turn?.token`. An `onChange` closure
        // captures the properties of the body that BUILT it - the previous one -
        // so a closure keyed on the token read `turn` back as the value it had
        // BEFORE the change, which for the first turn of a session is nil: the
        // token moved, the guard below saw no duration and returned, and the
        // box swapped its tick with no motion at all. Filmed at 20fps and
        // logged (`secs=-1.0`), which is the only way that reads as anything
        // other than "the animation is too fast to see".
        .onChange(of: turn) { landed in
            AnimLog.say("checkbox turn secs=\(landed?.seconds ?? -1)")
            guard let landed, landed.seconds > 0 else { return }
            let half = landed.seconds / 2
            // A RUNLOOP TURN LATER, BOTH TIMES. This fires from inside the same
            // update that adopts the chain - the tick, the roster and the
            // controls all change in it - and a `withAnimation` raised inside
            // an ambient transaction that carries no animation is applied with
            // that transaction, i.e. instantly. Filmed at 20fps the box swapped
            // its tick in ONE frame and never turned, while the log said the
            // beat had landed with a 500ms duration: the animation was not too
            // fast to see, it was not an animation.
            DispatchQueue.main.async {
                withAnimation(.easeIn(duration: half)) { edgeOn = true }
                DispatchQueue.main.asyncAfter(deadline: .now() + half) {
                    withAnimation(.easeOut(duration: half)) { edgeOn = false }
                }
            }
        }
    }

    /// The box itself: a small wooden plank, sharp-cornered like every other
    /// wooden control here, dimmed the same way FButton dims (a black tint
    /// composited INTO the fill, never opacity on the whole view - see round-6
    /// #19: a translucent control lets the weave show through and reads as a
    /// ghost rather than as something switched off).
    private var plank: some View {
        ZStack {
            WoodFill()
                .overlay(Color.black.opacity(enabled ? 0 : 0.45))
            Rectangle()
                .strokeBorder(.black.opacity(enabled ? 0.35 : 0.2), lineWidth: 1)
            if isOn { FCheck(size: box - 6) }
        }
        .frame(width: box, height: box)
        .scaleEffect(x: Self.boxTurn(edgeOn: edgeOn), anchor: .center)
    }
}
