// Overlays.swift — the ChooserOverlay (docs/WATCHOS_G_SPEC.md §5 G2b). Presented for an
// ambiguous cover (≥2 targets) or a cover-or-pass fork. Race-safe: only the ✕ dismisses;
// outside taps do nothing.

import SwiftUI

struct ChooserOverlay: View {
    let spec: ChooserSpec
    let onCover: (Move) -> Void
    let onPass: (Move) -> Void
    let onClose: () -> Void

    var body: some View {
        ZStack {
            WColor.bg.opacity(0.88).ignoresSafeArea()

            VStack(spacing: 10) {
                Text(spec.title)
                    .font(WFont.label(12.5))
                    .foregroundStyle(WColor.info)
                    .multilineTextAlignment(.center)
                    .lineLimit(2).minimumScaleFactor(0.8)

                HStack(spacing: 8) {  // cover targets
                    ForEach(spec.coverTargets) { t in
                        Button { onCover(t.move) } label: {
                            Glyph(card: t.attack, size: 32)
                                .frame(width: 49, height: 54)
                                .background(RoundedRectangle(cornerRadius: 8).fill(Color(white: 0.1)))
                        }
                        .buttonStyle(.plain)
                    }
                }

                if let p = spec.pass {
                    Button { onPass(p.move) } label: {
                        Text("PASS ▸ \(p.receiver)")
                            .font(WFont.heavy(13)).foregroundStyle(WColor.blue)
                            .padding(.horizontal, 12).frame(minHeight: 28)
                            .frame(maxWidth: .infinity)
                            .background(RoundedRectangle(cornerRadius: 14).fill(Color(hex: 0x001B38)))
                    }
                    .buttonStyle(.plain)
                    .padding(.horizontal, 8)
                }
            }
            .padding(.horizontal, 8)
            .padding(.top, 44)          // clear the ✕ row so the title never sits under it
            .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .top)

            // ✕ in the system-chevron position (one habit) — the ONLY dismissal.
            Button(action: onClose) {
                Image(systemName: "xmark")
                    .font(.system(size: 14, weight: .bold))
                    .foregroundStyle(WColor.info)
                    .frame(width: 33, height: 33)
                    .background(Circle().fill(Color(white: 0.14)))
            }
            .buttonStyle(.plain)
            .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
        }
    }
}
