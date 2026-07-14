// FButton.swift — the ENTIRE button family (§5.4): primary (red) and secondary
// (outline). No third style. One primary action per screen (§5.5).

import SwiftUI

public struct FButton: View {
    public enum Kind { case primary, secondary, destructive }

    private let title: String
    private let kind: Kind
    private let action: () -> Void
    private var enabled: Bool

    public init(_ title: String, kind: Kind = .primary, enabled: Bool = true, action: @escaping () -> Void) {
        self.title = title
        self.kind = kind
        self.enabled = enabled
        self.action = action
    }

    public var body: some View {
        // A per-title wood seed so adjacent buttons show different grain.
        let seed = Double(abs(title.hashValue % 100)) / 100.0
        return Button(action: { Haptics.fire(.drop); action() }) {
            Text(title)
                .font(FType.title(18))
                .tracking(0.5)
                .frame(maxWidth: .infinity, minHeight: 52)     // 44pt+ hit target (a11y floor)
                .foregroundColor(FColor.textPrimary)
                .shadow(color: .black.opacity(0.75), radius: 1, x: 0, y: 1)   // legible on wood
                .background(WoodSurface(seed: seed, cornerRadius: FRadius.button))
                .overlay( // primary gets a warm brand-red edge to stay the one CTA
                    RoundedRectangle(cornerRadius: FRadius.button, style: .continuous)
                        .strokeBorder(FColor.accent, lineWidth: kind == .primary ? 2 : 0)
                )
        }
        .buttonStyle(FPressStyle())
        .disabled(!enabled)
        .opacity(enabled ? 1 : 0.4)
    }
}

/// A tiny press feedback: the plank pushes down 1pt and dims, like the web's
/// translateY on :active.
struct FPressStyle: ButtonStyle {
    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .offset(y: configuration.isPressed ? 1 : 0)
            .brightness(configuration.isPressed ? -0.06 : 0)
            .animation(.easeOut(duration: 0.1), value: configuration.isPressed)
    }
}
