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

public struct FCheckbox: View {
    private let title: String
    private let isOn: Bool
    private let enabled: Bool
    private let action: (Bool) -> Void

    public init(_ title: String, isOn: Bool,
                enabled: Bool = true, action: @escaping (Bool) -> Void) {
        self.title = title
        self.isOn = isOn
        self.enabled = enabled
        self.action = action
    }

    private let box: CGFloat = 26

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
    }
}
