// MessageSettingsView.swift — the board's Settings sheet (1.0(4)), opened from
// the left Settings (gear) square. For now it holds one thing the owner asked
// for: a language override (System / English / Русский / 한국어), persisted by
// FStrings.override. Room to grow as more settings land.
//
// The language choices are a vertical list of WOOD blocks (owner: not the glass
// segmented picker), one per language, the chosen one check-marked. The wool is
// a `.background`, not a ZStack sibling, so the content stays inside the safe
// area (a sibling ignoresSafeArea grows the stack and clips the title under the
// notch - the same trap MessagesRootView documents for the board).

import SwiftUI

public struct MessageSettingsView: View {
    private let onClose: () -> Void
    @State private var language: AppLanguage = FStrings.override

    public init(onClose: @escaping () -> Void = {}) { self.onClose = onClose }

    public var body: some View {
        VStack(alignment: .leading, spacing: FSpace.xl) {
            // No Done button: swiping the sheet down is the obvious dismissal
            // (owner). Title centered.
            Text(FStrings.t("ios.settings.title")).font(FType.title(22)).onWoolText()
                .frame(maxWidth: .infinity, alignment: .center)

            VStack(alignment: .leading, spacing: FSpace.s) {
                Text(FStrings.t("ios.settings.language")).font(FType.title(17)).onWoolText()
                VStack(spacing: FSpace.s) {
                    ForEach(AppLanguage.allCases, id: \.self) { lang in
                        languageRow(lang)
                    }
                }
            }

            Spacer(minLength: 0)
        }
        .padding(FSpace.xl)
        .padding(.top, FSpace.s)          // clear the sheet's rounded top corner
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
        .background(WoolBackground().ignoresSafeArea())
    }

    private func languageRow(_ lang: AppLanguage) -> some View {
        Button {
            language = lang
            FStrings.override = lang
        } label: {
            HStack {
                Text(lang.display).font(FType.title(17))
                Spacer()
                if lang == language { Image(systemName: "checkmark").font(.system(size: 15, weight: .heavy)) }
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
