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
        Button(action: { Haptics.fire(.drop); action() }) {
            Text(title)
                .font(FType.title(17))
                .frame(maxWidth: .infinity, minHeight: 52)     // 44pt+ hit target (a11y floor)
                .foregroundColor(foreground)
                .background(background)
                .overlay(
                    RoundedRectangle(cornerRadius: FRadius.card)
                        .strokeBorder(border, lineWidth: kind == .secondary ? 1.5 : 0)
                )
                .clipShape(RoundedRectangle(cornerRadius: FRadius.card))
        }
        .disabled(!enabled)
        .opacity(enabled ? 1 : 0.4)
    }

    private var foreground: Color {
        switch kind {
        case .primary, .destructive: return FColor.textPrimary
        case .secondary: return FColor.textPrimary
        }
    }
    private var background: Color {
        switch kind {
        case .primary, .destructive: return FColor.accent
        case .secondary: return .clear
        }
    }
    private var border: Color {
        kind == .secondary ? FColor.textDim.opacity(0.6) : .clear
    }
}
