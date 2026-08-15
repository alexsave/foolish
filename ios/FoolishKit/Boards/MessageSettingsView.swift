// MessageSettingsView.swift — the board's Settings sheet (1.0(4)), opened from
// the left Settings (gear) square. For now it holds one thing the owner asked
// for: a language override (System / English / Русский / 한국어), persisted by
// FStrings.override. Room to grow as more settings land.

import SwiftUI

public struct MessageSettingsView: View {
    private let onClose: () -> Void
    @State private var language: AppLanguage = FStrings.override

    public init(onClose: @escaping () -> Void = {}) { self.onClose = onClose }

    public var body: some View {
        ZStack {
            WoolBackground().ignoresSafeArea()
            VStack(alignment: .leading, spacing: FSpace.xl) {
                HStack {
                    Text(FStrings.t("ios.settings.title")).font(FType.title(22)).onWoolText()
                    Spacer()
                    Button(action: onClose) {
                        Text(FStrings.t("ios.done")).font(FType.title(16)).onWoolText()
                    }
                }

                VStack(alignment: .leading, spacing: FSpace.s) {
                    Text(FStrings.t("ios.settings.language")).font(FType.title(17)).onWoolText()
                    Picker("", selection: $language) {
                        ForEach(AppLanguage.allCases, id: \.self) { lang in
                            Text(lang.display).tag(lang)
                        }
                    }
                    .pickerStyle(.segmented)
                    .onChange(of: language) { FStrings.override = $0 }
                }

                Spacer(minLength: 0)
            }
            .padding(FSpace.xl)
        }
    }
}
