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

import SwiftUI

public struct FCheckbox: View {
    private let title: String
    /// The line under the label, shown only when the box is CLEAR. A checkbox
    /// that changes the rules should say what the other rule is, and it should
    /// say it where the change happened rather than in a help screen nobody
    /// opens mid-lobby. nil for a box that needs no such gloss.
    private let offCaption: String?
    private let isOn: Bool
    private let enabled: Bool
    private let action: (Bool) -> Void

    public init(_ title: String, isOn: Bool, offCaption: String? = nil,
                enabled: Bool = true, action: @escaping (Bool) -> Void) {
        self.title = title
        self.isOn = isOn
        self.offCaption = offCaption
        self.enabled = enabled
        self.action = action
    }

    private let box: CGFloat = 26

    public var body: some View {
        Button(action: { Haptics.fire(.drop); action(!isOn) }) {
            HStack(alignment: .firstTextBaseline, spacing: FSpace.s) {
                plank
                VStack(alignment: .leading, spacing: 2) {
                    Text(title)
                        .font(FType.body(15))
                        .onTableText(dimmed: !enabled)
                        .lineLimit(2)
                        .multilineTextAlignment(.leading)
                    if !isOn, let offCaption {
                        Text(offCaption)
                            .font(FType.body(12))
                            .onTableText(dimmed: true)
                            .lineLimit(2)
                            .multilineTextAlignment(.leading)
                    }
                }
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
        // Baseline-aligned rows put the box's TOP near the label's cap height;
        // nudge it down so the tick sits level with the words next to it.
        .alignmentGuide(.firstTextBaseline) { d in d[.bottom] - 5 }
    }
}
