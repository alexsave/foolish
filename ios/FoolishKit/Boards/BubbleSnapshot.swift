// BubbleSnapshot — the 300×195pt image on the message bubble (design §10, §11.3).
//
// This picture rides in the MSMessage and shows on lock screens and in
// notifications, so it MUST be the PUBLIC table: both hands as backs, no face-up
// card belonging to any seat. That safety is structural — it renders
// `MessageBoardView` from the spectator view (viewer -1), which carries no hand —
// not a promise this code has to keep by hand.

import SwiftUI
import UIKit

public enum BubbleSnapshot {
    /// Apple's template-image size for the balloon (§11.3). Points; the renderer
    /// scales to the device.
    public static let size = CGSize(width: 300, height: 195)

    /// Render `publicView` (which MUST be a viewer:-1 / no-hand view) into a
    /// bubble image. Returns nil only if the renderer fails. MainActor because
    /// ImageRenderer walks a live SwiftUI view.
    @MainActor
    public static func render(publicView: GameView, names: [Int: String] = [:]) -> UIImage? {
        let content = MessageBoardView(view: publicView, names: names)
            .frame(width: size.width, height: size.height)
            // The bubble rides the icon's rich Khokhloma red (not system gray), so
            // the balloon reads as Foolish at a glance in the thread. The felt/wool
            // table lives in the extension board; the bubble is the branded card.
            .background(Color(hex: 0x98200A))
            .environment(\.colorScheme, .light)   // the balloon image is theme-independent
        let renderer = ImageRenderer(content: content)
        renderer.scale = UIScreen.main.scale
        renderer.isOpaque = true
        return renderer.uiImage
    }
}
