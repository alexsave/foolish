// FButton.swift — the ENTIRE button family (§5.4): primary (red) and secondary
// (outline). No third style. One primary action per screen (§5.5).

import SwiftUI

public struct FButton: View {
    public enum Kind { case primary, secondary, destructive, wood }

    private let title: String
    private let kind: Kind
    private let action: () -> Void
    private var enabled: Bool
    /// Compact: a small pill sized to its label (the in-game action buttons, web
    /// parity), not the full-width 52pt primary button.
    private var compact: Bool

    public init(_ title: String, kind: Kind = .primary, enabled: Bool = true,
                compact: Bool = false, action: @escaping () -> Void) {
        self.title = title
        self.kind = kind
        self.enabled = enabled
        self.compact = compact
        self.action = action
    }

    public var body: some View {
        Button(action: { Haptics.fire(.drop); action() }) {
            Text(title)
                .font(compact ? FType.title(15) : FType.title(17))
                .padding(.horizontal, compact ? 16 : 0)
                .frame(maxWidth: compact ? nil : .infinity, minHeight: compact ? 40 : 52)
                .foregroundColor(foreground)
                .background(backgroundView)
                .overlay(
                    RoundedRectangle(cornerRadius: FRadius.card)
                        .strokeBorder(border, lineWidth: kind == .secondary ? 1.5 : (kind == .wood ? 1 : 0))
                )
                .clipShape(RoundedRectangle(cornerRadius: FRadius.card))
        }
        .disabled(!enabled)
        .opacity(enabled ? 1 : 0.4)
    }

    @ViewBuilder private var backgroundView: some View {
        switch kind {
        case .wood: WoodFill()
        case .primary, .destructive: FColor.accent
        case .secondary: Color.clear
        }
    }

    private var foreground: Color {
        switch kind {
        // Bone text on the accent red, the wood grain, or the felt (secondary).
        case .primary, .destructive, .secondary, .wood: return FColor.textPrimary
        }
    }
    private var border: Color {
        switch kind {
        case .secondary: return FColor.textDim.opacity(0.6)
        case .wood: return .black.opacity(0.35)
        default: return .clear
        }
    }
}
