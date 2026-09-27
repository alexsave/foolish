// COPIED from foolish/ios/FoolishKit/DesignSystem/FButton.swift, FSquareButton.swift and Haptics.swift at c3d99192 - replaced by lift step S2
//
// foolish's wood pill (96 x 40 in a row here, U9; 52pt tall in the lobby),
// its square button (40 x 40), the press style and the one haptic map. The
// verbs and their words are the kernel's (Pk.string); nothing here names one.

import SwiftUI
import UIKit

public enum PkHaptic { case pickUp, drop, reject }

public enum Haptics {
    public static var isEnabled = true
    public static func fire(_ h: PkHaptic) {
        guard isEnabled else { return }
        switch h {
        case .pickUp: UIImpactFeedbackGenerator(style: .light).impactOccurred()
        case .drop:   UIImpactFeedbackGenerator(style: .medium).impactOccurred()
        case .reject: UIImpactFeedbackGenerator(style: .rigid).impactOccurred()
        }
    }
}

/// foolish's press: .96 for 80ms, and no system decoration outside the plank.
public struct FPressStyle: ButtonStyle {
    public init() {}
    public func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .scaleEffect(configuration.isPressed ? 0.96 : 1)
            .animation(FMotion.press, value: configuration.isPressed)
    }
}

/// A wood plank with square corners and a hairline edge. `ink` overrides the
/// white lettering (the Last card! pill's amber, U11).
struct WoodButton: View {
    let title: String
    var width: CGFloat? = nil
    var height: CGFloat = 40
    var enabled = true
    var ink: Color? = nil
    var fontSize: CGFloat = 15
    let action: () -> Void

    var body: some View {
        Button(action: { Haptics.fire(.drop); action() }) {
            label
                .lineLimit(1)
                .minimumScaleFactor(0.75)
                .padding(.horizontal, width == nil ? 10 : 0)
                .frame(minWidth: width, maxWidth: width ?? .infinity, minHeight: height, maxHeight: height)
                .background(WoodFill().overlay(Color.black.opacity(enabled ? 0 : 0.45)))
                .overlay(Rectangle().strokeBorder(Color.black.opacity(enabled ? 0.35 : 0.21), lineWidth: 1))
                .clipShape(Rectangle())
        }
        .buttonStyle(FPressStyle())
        .disabled(!enabled)
    }

    @ViewBuilder private var label: some View {
        let t = Text(title).font(.system(size: fontSize, weight: .heavy))
        if let ink {
            t.foregroundStyle(ink).shadow(color: .black.opacity(0.5), radius: 1, y: 1)
        } else {
            t.onWoodText(dimmed: !enabled)
        }
    }
}

/// foolish's FSquareButton: 40pt, one SF Symbol in wood ink.
struct SquareButton: View {
    let systemImage: String
    var side: CGFloat = 40
    let accessibility: String
    let action: () -> Void

    var body: some View {
        Button(action: { Haptics.fire(.drop); action() }) {
            Image(systemName: systemImage)
                .font(.system(size: side * 0.44, weight: .heavy))
                .onWoodText()
                .frame(width: side, height: side)
                .background(WoodFill())
                .overlay(Rectangle().strokeBorder(Color.black.opacity(0.35), lineWidth: 1))
                .clipShape(Rectangle())
        }
        .buttonStyle(FPressStyle())
        .accessibilityLabel(accessibility)
    }
}
