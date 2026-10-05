// StageHUD.swift - what lies FLAT over the turned table (DECISIONS I25): the
// bid plate and the shelf, each at the frame the stage's HUD gives, never
// turned. The places are the kernel's; the words are the model's.

import SwiftUI

extension CnStageHudSnap {
    /// A HUD rectangle (x y w h, points) as a CGRect; .null for a short array.
    static func rect(_ a: [Double]) -> CGRect {
        a.count == 4 ? CGRect(x: a[0], y: a[1], width: a[2], height: a[3]) : .null
    }

    var plateRect: CGRect? { hasPlate == 1 ? Self.rect(plate) : nil }
    var shelfRect: CGRect? { hasShelf == 1 ? Self.rect(shelf) : nil }
}

extension View {
    /// Put this view at a HUD rectangle (the drawer's points, top-left origin).
    func at(_ r: CGRect) -> some View {
        frame(width: r.width, height: r.height).position(x: r.midX, y: r.midY)
    }
}

/// The bid plate: the kernel's words for the bid, big, beside its face, on a
/// dark plate with a brass hairline (the existing tokens; the study's
/// verdigris plate is not restated here).
struct BidPlate: View {
    let text: String
    let face: Int?
    /// No bid yet: a quieter line in the same place (the kernel's headline).
    var quiet = false

    var body: some View {
        GeometryReader { geo in
            let narrow = geo.size.width < 140
            HStack(spacing: 8) {
                Text(text)
                    .font(.system(size: quiet ? 14 : (narrow ? 20 : 26), weight: .heavy))
                    .onFeltText(quiet ? FColor.textDim : FColor.textPrimary)
                    .lineLimit(quiet ? 2 : 1)
                    .multilineTextAlignment(.center)
                    .minimumScaleFactor(0.5)
                if let face { Die(face: face, size: narrow ? 24 : 30) }
            }
            .padding(.horizontal, 10)
            .frame(width: geo.size.width, height: geo.size.height)
            .background(
                RoundedRectangle(cornerRadius: 8, style: .continuous)
                    .fill(Color.black.opacity(0.55))
            )
            .overlay(
                RoundedRectangle(cornerRadius: 8, style: .continuous)
                    .strokeBorder(FColor.win.opacity(0.55), lineWidth: 1)
            )
        }
        .accessibilityElement(children: .combine)
    }
}
