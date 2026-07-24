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
    /// An explicit fixed width - the wood background fills exactly this, so a
    /// column of compact buttons is a clean equal-width stack (not ragged to each
    /// word). `nil` keeps the label-sized compact / full-width primary behaviour.
    private var fixedWidth: CGFloat?

    public init(_ title: String, kind: Kind = .primary, enabled: Bool = true,
                compact: Bool = false, fixedWidth: CGFloat? = nil, action: @escaping () -> Void) {
        self.title = title
        self.kind = kind
        self.enabled = enabled
        self.compact = compact
        self.fixedWidth = fixedWidth
        self.action = action
    }

    public var body: some View {
        Button(action: { Haptics.fire(.drop); action() }) {
            titleText
                .padding(.horizontal, compact ? 16 : 0)
                .frame(minWidth: fixedWidth,
                       maxWidth: fixedWidth ?? (compact ? nil : .infinity),
                       minHeight: compact ? 40 : 52)
                .background(
                    backgroundView
                        // Round-6 #19: a disabled button used to be the WHOLE
                        // view at 40% opacity (`.opacity(enabled ? 1 : 0.4)`
                        // below it), which made the button translucent and let
                        // the wool weave behind it show straight through — the
                        // owner's complaint verbatim ("should not be
                        // transparent at all, just dimmed"). A black tint
                        // composited INTO the button's own opaque fill reads as
                        // a dimmed solid control instead of a ghost: nothing
                        // behind the BUTTON's outer edge shows through, it is
                        // just darker inside it.
                        .overlay(Color.black.opacity(enabled ? 0 : 0.45))
                )
                .overlay(
                    RoundedRectangle(cornerRadius: cornerRadius)
                        .strokeBorder(border.opacity(enabled ? 1 : 0.6),
                                      lineWidth: kind == .secondary ? 1.5 : (kind == .wood ? 1 : 0))
                )
                .clipShape(RoundedRectangle(cornerRadius: cornerRadius))
        }
        .disabled(!enabled)
    }

    /// The label itself. Wood buttons get round-6 #17's thick-white-on-wood
    /// treatment (via `onWoodText`, Tokens.swift) instead of the flat
    /// `foreground` colour every other kind uses — wood is a textured surface,
    /// not a flat fill, so it needs the weight+shadow pairing to stay legible.
    @ViewBuilder private var titleText: some View {
        let base = Text(title).font(compact ? FType.title(15) : FType.title(17))
        if kind == .wood {
            base.onWoodText(dimmed: !enabled)
        } else {
            base.foregroundColor(enabled ? foreground : foreground.opacity(0.55))
        }
    }

    // Wooden buttons are RECTANGLES in the web (sharp corners); the red/outline
    // buttons stay rounded.
    private var cornerRadius: CGFloat { kind == .wood ? 0 : FRadius.card }

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
