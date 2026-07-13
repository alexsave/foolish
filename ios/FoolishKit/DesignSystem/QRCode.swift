// QRCode.swift — render a replay share URL to a QR image (§16.C2). CoreImage
// CIQRCodeGenerator, error-correction level L (smallest QR — the base32 code is
// all QR-alphanumeric so it stays compact), rendered crisp at the requested
// size on the bone card color. The web does this with qrcode.react; this is the
// native equivalent.

import SwiftUI
import CoreImage.CIFilterBuiltins

public enum QRCode {
    private static let context = CIContext()

    /// A QR image for `string`, sized `points` square. Returns nil on failure.
    public static func image(for string: String, points: CGFloat = 220) -> UIImage? {
        let filter = CIFilter.qrCodeGenerator()
        filter.message = Data(string.utf8)
        filter.correctionLevel = "L"
        guard let output = filter.outputImage else { return nil }

        // Scale the tiny generator output up to the target, keeping hard edges.
        let scale = (points * 3) / output.extent.width   // 3x for retina
        let scaled = output.transformed(by: CGAffineTransform(scaleX: scale, y: scale))

        // Tint: ink modules on a bone background, matching the card identity.
        let colored = scaled
            .applyingFilter("CIFalseColor", parameters: [
                "inputColor0": CIColor(red: 0x17/255.0, green: 0x14/255.0, blue: 0x0F/255.0), // ink
                "inputColor1": CIColor(red: 0xF4/255.0, green: 0xEF/255.0, blue: 0xE6/255.0), // card
            ])

        guard let cg = context.createCGImage(colored, from: colored.extent) else { return nil }
        return UIImage(cgImage: cg)
    }
}
