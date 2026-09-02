// MessageSettingsView.swift — the board's Settings sheet (1.0(4)), opened from
// the left Settings (gear) square. Two settings today:
//   - the language override (English / Русский / 한국어), persisted by
//     FStrings.override
//   - the TABLE MATERIAL (round 12): the wool weave, or a green casino baize
//     for a player who finds the weave busy behind the cards. Round 30 made
//     these two SWATCHES on one row, each drawn in its own material - see
//     `tableSwatch` for why this setting gets a different control from the
//     language list right under it.
//
// TABLE FIRST, then language (owner). It is the setting a player actually comes
// here to change once the game is running; language is a set-once.
//
// The LANGUAGE list is vertical WOOD blocks (owner: not the glass segmented
// picker), one per choice, the chosen one check-marked. The table is two
// swatches side by side (round 30) - the one place the two sections deliberately
// do not share `choiceRow`, because a material is a look and not a word. The table surface is a
// `.background`, not a ZStack sibling, so the content stays inside the safe area
// (a sibling ignoresSafeArea grows the stack and clips the title under the notch
// - the same trap MessagesRootView documents for the board).
//
// Changing either one repaints this sheet AND the board behind it immediately,
// because both come off `FPrefs` — see FPrefs.swift for why a static accessor
// alone could not do that.

import SwiftUI

public struct MessageSettingsView: View {
    private let onClose: () -> Void
    /// The live settings (see FPrefs): this sheet both READS them - so its own
    /// title and rows re-render into the language just picked, and the wool
    /// behind it becomes felt the moment felt is chosen - and writes them. The
    /// old `@State` copy did the first half only, for this one screen.
    @ObservedObject private var prefs = FPrefs.shared
    /// The swatches pick their own fallback colour off this, the same way every
    /// other surface does - see `swatchVariant`.
    @Environment(\.colorScheme) private var scheme

    public init(onClose: @escaping () -> Void = {}) { self.onClose = onClose }

    public var body: some View {
        VStack(alignment: .leading, spacing: FSpace.xl) {
            // No Done button: swiping the sheet down is the obvious dismissal
            // (owner). Title centered.
            Text(FStrings.t("ios.settings.title")).font(FType.title(22)).onTableText()
                .frame(maxWidth: .infinity, alignment: .center)

            section("ios.settings.table") {
                // ONE ROW, and the swatches ARE the choice (owner, round 30):
                // "make the two table styles a single row side by side ... make
                // their backgrounds the respective style rather than the wood
                // background", and "instead of text captions for the styles,
                // just have a check on whichever one is selected".
                //
                // Which is the right control for this setting and the wrong one
                // for the other. A language is a word - you read it and pick it.
                // A table is a LOOK, and a wooden pill reading "Green felt" asks
                // you to imagine the thing it is standing in front of. Two
                // swatches showing the actual weave and the actual baize answer
                // the question by being the answer, which is also why they lose
                // their captions: the label was describing a picture that is now
                // there to be looked at.
                HStack(spacing: FSpace.s) {
                    ForEach(TableSurface.allCases, id: \.self) { choice in
                        tableSwatch(choice)
                    }
                }
            }

            section("ios.settings.language") {
                ForEach(AppLanguage.allCases, id: \.self) { choice in
                    choiceRow(choice.display, chosen: choice == prefs.language) {
                        prefs.setLanguage(choice)
                    }
                }
            }

            Spacer(minLength: 0)
        }
        .padding(FSpace.xl)
        .padding(.top, FSpace.s)          // clear the sheet's rounded top corner
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
        .background(TableBackground().ignoresSafeArea())
    }

    private func section<C: View>(_ titleKey: String, @ViewBuilder rows: () -> C) -> some View {
        VStack(alignment: .leading, spacing: FSpace.s) {
            Text(FStrings.t(titleKey)).font(FType.title(17)).onTableText()
            VStack(spacing: FSpace.s) { rows() }
        }
    }

    /// One table swatch: the material itself, with a checkmark on the chosen one.
    ///
    /// A little taller than a `choiceRow` (64 against 52) because it is a picture
    /// rather than a line of text, and a fixed height rather than a minimum -
    /// `Color` expands forever given the chance, and the first cut of this let
    /// the two swatches grow into the sheet's whole spare height. The border is
    /// the row's, so the two sections still look like one screen.
    ///
    /// The checkmark wears `onWoodText` - white with a black shadow - and not
    /// the table treatment: `onTableText` adapts to the material the PLAYER
    /// chose, and would therefore pick its contrast against the wrong swatch
    /// half the time. White-with-a-shadow is the same glyph treatment the role
    /// marks use on the board for the same reason: it survives any surface.
    ///
    /// The label the caption used to carry moves to `accessibilityLabel`, so the
    /// two `ios.settings.table.*` strings are still doing a job and VoiceOver
    /// still hears "Wool" and "Green felt" rather than two unnamed buttons.
    private func tableSwatch(_ choice: TableSurface) -> some View {
        let chosen = choice == prefs.table
        return Button { prefs.setTable(choice) } label: {
            ZStack {
                Color(hex: FTextures.tableFallbackHex(FTextures.Variant(scheme, material: choice)))
                TableWeave(material: choice)
                if chosen {
                    Image(systemName: "checkmark")
                        .font(.system(size: 22, weight: .heavy))
                        .onWoodText()
                }
            }
            // A FIXED height, not a minimum: `Color` is infinitely expandable,
            // so `minHeight` let the swatches eat the sheet's whole spare
            // height and the row read as two posters.
            .frame(maxWidth: .infinity, minHeight: 64, maxHeight: 64)
            .overlay(Rectangle().strokeBorder(Color.black.opacity(chosen ? 0.75 : 0.35),
                                              lineWidth: chosen ? 2 : 1))
            .clipShape(Rectangle())
        }
        .accessibilityLabel(Text(FStrings.t(choice.labelKey)))
        .accessibilityAddTraits(chosen ? [.isButton, .isSelected] : .isButton)
    }

    /// One wood block: a label, and a checkmark when it is the current choice.
    /// Shared by both sections so a second setting cannot arrive wearing a
    /// different control.
    private func choiceRow(_ label: String, chosen: Bool, tap: @escaping () -> Void) -> some View {
        Button(action: tap) {
            HStack {
                Text(label).font(FType.title(17))
                Spacer()
                if chosen { Image(systemName: "checkmark").font(.system(size: 15, weight: .heavy)) }
            }
            .onWoodText()
            .padding(.horizontal, FSpace.l)
            .frame(maxWidth: .infinity, minHeight: 52, alignment: .leading)
            .background(WoodFill())
            .overlay(Rectangle().strokeBorder(Color.black.opacity(0.35), lineWidth: 1))
            .clipShape(Rectangle())
        }
    }
}
