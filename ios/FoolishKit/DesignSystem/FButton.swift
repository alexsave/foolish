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
                // A button label is ALWAYS one line (owner, on device: the
                // Russian "Отменить" wrapped inside the fixed-width wooden
                // pill). Tighter compact padding buys the longer locales room
                // first; a label that still cannot fit scales down instead of
                // wrapping.
                .lineLimit(1)
                .minimumScaleFactor(0.75)
                .padding(.horizontal, compact ? 10 : 0)
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
        .buttonStyle(FPressStyle())
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

/// THE ONE PRESS TREATMENT for every button this app draws, and - more to the
/// point - the thing that stops the SYSTEM from drawing one.
///
/// Owner, Eva's test pass: "for some reason I see a highlight around her wooden
/// buttons". A `Button` with a custom label and no explicit style gets
/// `.automatic`, and what `.automatic` resolves to is not ours to decide: it
/// varies by iOS version, by the container the button lands in, and by the
/// reader's accessibility settings (Button Shapes and Full Keyboard Access both
/// add decoration of their own, and Increase Contrast changes how the press
/// highlight is drawn). Every one of those paints OUTSIDE our wood rectangle,
/// which is exactly where the highlight was seen. It reproduces on a device
/// with those settings on and not on one without them, which is why it showed
/// up on a tester's phone and not the owner's.
///
/// A custom `ButtonStyle` replaces that whole layer: SwiftUI hands us the label
/// and the press state and draws nothing else, so there is no system chrome to
/// leak past our own border under any setting. The press feedback is then ours
/// to define, and it is a SCALE, not an opacity: round-6 #19 already settled
/// that a wood surface must never go translucent ("should not be transparent at
/// all, just dimmed"), and the same argument applies to a press - the wool
/// weave showing through a pressed plank is the same wrong picture.
///
/// `.plain` was the smaller change and was rejected: it removes the system
/// decoration but leaves NO press feedback at all, and a wooden button that
/// does not acknowledge a touch reads as broken on a laggy extension launch.
public struct FPressStyle: ButtonStyle {
    public init() {}
    public func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .scaleEffect(configuration.isPressed ? 0.96 : 1)
            // Explicit and value-scoped, so a press cannot be interpolated by
            // whatever ambient transaction happens to be in flight - the board
            // runs several, and the action column is nulled out of all of them
            // (MessageTableView's `.transaction { $0.animation = nil }`).
            .animation(.easeOut(duration: 0.08), value: configuration.isPressed)
    }
}
