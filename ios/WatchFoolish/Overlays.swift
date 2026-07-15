// Overlays.swift — the ChooserOverlay (docs/WATCHOS_G_SPEC.md §5 G2b). Presented for an
// ambiguous cover (≥2 targets) or a cover-or-pass fork. Race-safe: only the ✕ dismisses;
// outside taps do nothing.
//
// The cards float on the scrim with NO tile behind them (owner review): the glyph IS the
// button, exactly as in the lane, so the screen reads as the same objects enlarged rather
// than a different widget. Everything else is soft — an opaque-enough scrim, system type,
// and a gentle fade+scale on present.

import SwiftUI

struct ChooserOverlay: View {
    let spec: ChooserSpec
    let onCover: (Move) -> Void
    let onPass: (Move) -> Void
    let onClose: () -> Void

    @State private var shown = false

    /// A trump card can legally cover several same-rank attacks, so the row is not always
    /// three wide; shrink the icons rather than clip or scroll.
    private var itemCount: Int { spec.coverTargets.count + (spec.pass == nil ? 0 : 1) }
    private var iconSize: CGFloat {
        itemCount > HTuning.chooserTightAfter ? HTuning.chooserIconTight : HTuning.chooserIcon
    }

    /// One choice: the icon IS the button, with its verb under it in the lane's caption
    /// style — same size, same gray, so the overlay reads as the same language.
    private func choice<I: View>(verb: String,
                                 action: @escaping () -> Void,
                                 @ViewBuilder icon: () -> I) -> some View {
        Button(action: action) {
            VStack(spacing: HTuning.chooserCaptionGap) {
                icon()
                Text(verb).font(.system(size: HTuning.captionSize)).foregroundStyle(WColor.seat)
            }
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
    }

    var body: some View {
        ZStack {
            WColor.bg.opacity(HTuning.scrimOpacity).ignoresSafeArea()

            // One row of choices, each captioned with its own single word. No overall
            // prompt, no receiver name (owner review) — every item says what it does.
            HStack(spacing: HTuning.chooserGap) {
                ForEach(spec.coverTargets) { target in
                    choice(verb: "COVER") { onCover(target.move) } icon: {
                        Glyph(card: target.attack, size: iconSize)
                    }
                }
                if let p = spec.pass {
                    choice(verb: "PASS") { onPass(p.move) } icon: {
                        // ↑ the attack goes onward to the next seat; ↓ (the lane's
                        // terminal) is pickup, where it comes to you.
                        Image(systemName: "arrow.up")
                            .font(.system(size: iconSize * HTuning.chooserArrowScale, weight: .heavy))
                            .foregroundStyle(WColor.blue)
                            .frame(width: iconSize * HTuning.glyphFrameW, height: iconSize * HTuning.glyphFrameH)
                    }
                }
            }
            .padding(.horizontal, 8)
            .frame(maxWidth: .infinity, maxHeight: .infinity)

            // ✕ in the system-chevron position (one habit) — the ONLY dismissal.
            Button(action: onClose) {
                Image(systemName: "xmark")
                    .font(.system(size: 14, weight: .semibold))
                    .foregroundStyle(WColor.info)
                    .frame(width: 33, height: 33)
                    .background(Circle().fill(Color(white: 0.12)))
            }
            .buttonStyle(.plain)
            .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
        }
        .opacity(shown ? 1 : 0)
        .scaleEffect(shown ? 1 : HTuning.chooserScaleFrom)
        .animation(.easeOut(duration: HTuning.chooserFade), value: shown)
        .onAppear { shown = true }
    }
}
