// MessageSettingsView.swift — the board's Settings sheet (1.0(4)), opened from
// the left Settings (gear) square. Two settings today:
//   - the language override (English / Русский / 한국어), persisted by
//     FStrings.override
//   - the TABLE MATERIAL (round 12): the wool weave, or a green casino baize
//     for a player who finds the weave busy behind the cards
//
// TABLE FIRST, then language (owner). It is the setting a player actually comes
// here to change once the game is running; language is a set-once.
//
// Both are vertical lists of WOOD blocks (owner: not the glass segmented
// picker), one per choice, the chosen one check-marked — so the two sections
// share `choiceRow` rather than growing a second look. The table surface is a
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

    public init(onClose: @escaping () -> Void = {}) { self.onClose = onClose }

    public var body: some View {
        VStack(alignment: .leading, spacing: FSpace.xl) {
            // No Done button: swiping the sheet down is the obvious dismissal
            // (owner). Title centered.
            Text(FStrings.t("ios.settings.title")).font(FType.title(22)).onTableText()
                .frame(maxWidth: .infinity, alignment: .center)

            section("ios.settings.table") {
                ForEach(TableSurface.allCases, id: \.self) { choice in
                    choiceRow(FStrings.t(choice.labelKey), chosen: choice == prefs.table) {
                        prefs.setTable(choice)
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
