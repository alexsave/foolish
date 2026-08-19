// FSquareButton.swift — a square wood button (1.0(4)), the icon sibling of
// FButton's wood pills. It backs the Settings/Help pair the message board floats
// on the LEFT, mirroring the right-hand action column: same WoodFill surface,
// same sharp corners, hairline border and onWoodText ink, so the two sides read
// as one family. Square by construction (width == height).

import SwiftUI

public struct FSquareButton: View {
    private let systemImage: String
    private let side: CGFloat
    private let accessibility: String
    private let action: () -> Void

    /// `side` is BOTH the width and the height — the board sizes it to the action
    /// buttons' height so the two sides match (FActionBar's compact pills are 40pt
    /// tall).
    public init(systemImage: String, side: CGFloat = 40, accessibility: String = "",
                action: @escaping () -> Void) {
        self.systemImage = systemImage
        self.side = side
        self.accessibility = accessibility
        self.action = action
    }

    public var body: some View {
        Button(action: { Haptics.fire(.drop); action() }) {
            Image(systemName: systemImage)
                .font(.system(size: side * 0.44, weight: .heavy))
                .onWoodText()                                  // white ink + drop shadow, as the wood pills
                .frame(width: side, height: side)
                .background(WoodFill())
                .overlay(Rectangle().strokeBorder(Color.black.opacity(0.35), lineWidth: 1))
                .clipShape(Rectangle())                        // sharp corners — wood is a plank, not a pill
        }
        .accessibilityLabel(accessibility)
    }
}

/// The Settings (gear) + Rulebook (book) pair — ONE layout for every surface
/// that floats it bottom-left: the board, the New-game setup and the lobby
/// (durak-rules-redesign put it on the latter two). Two 40pt squares with a
/// 16pt gap, so the pair spans one action-button width (40 + 16 + 40 = 96 =
/// FActionBar width) and mirrors the right-hand action column wherever both
/// appear; the inner horizontal padding is FActionBar's own inset, so the pair
/// lands on the same edge line on every screen. Round-9 (owner): the help "?"
/// became a little book - it opens the rulebook, so it looks like one.
public struct SettingsHelpSquares: View {
    private let onSettings: () -> Void
    private let onHelp: () -> Void

    public init(onSettings: @escaping () -> Void, onHelp: @escaping () -> Void) {
        self.onSettings = onSettings
        self.onHelp = onHelp
    }

    public var body: some View {
        HStack(spacing: FSpace.l) {   // 40 + 16 + 40 = 96 = FActionBar width
            FSquareButton(systemImage: "gearshape.fill", side: 40,
                          accessibility: FStrings.t("ios.settings.title"), action: onSettings)
            FSquareButton(systemImage: "book.fill", side: 40,
                          accessibility: FStrings.t("ios.help"), action: onHelp)
            Spacer(minLength: 0)
        }
        .padding(.horizontal, FSpace.m)   // same inner inset as FActionBar
    }
}
