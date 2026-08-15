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
