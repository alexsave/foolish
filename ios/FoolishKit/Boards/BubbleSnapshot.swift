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
        // The REAL wool texture as a ZStack BASE layer (an explicit-frame Image,
        // not a .background — that renders unreliably inside ImageRenderer).
        // WoolTexture.image is synchronous (WoolBackground's async .task never runs
        // in ImageRenderer), so the bubble gets the same wool as the board.
        let content = ZStack {
            FColor.fallback
            Image(uiImage: WoolTexture.image(w: WoolTexture.webCanvas.w, h: WoolTexture.webCanvas.h))
                .resizable()
                .aspectRatio(contentMode: .fill)
                .frame(width: size.width, height: size.height)
                .clipped()
            MessageBoardView(view: publicView, names: names)
        }
        .frame(width: size.width, height: size.height)
        .environment(\.colorScheme, .light)   // the balloon image is theme-independent
        let renderer = ImageRenderer(content: content)
        renderer.scale = UIScreen.main.scale
        renderer.isOpaque = true
        return renderer.uiImage
    }

    /// Render a WAITING lobby's bubble image (§5.2) — the joined players, NOT a
    /// dealt board. A lobby seal leaves a fully-dealt game resident (the kernel
    /// deals every hand at `newGame`, before anyone has picked a count), so
    /// feeding that resident view to `render(publicView:)` drew a full table of
    /// cards onto a bubble that is only an invite — the extension showed the
    /// lobby while its own staged preview showed a played game. This draws what
    /// the human is actually looking at: the lobby roster, on the same wool.
    @MainActor
    public static func renderLobby(joinedNames: [String]) -> UIImage? {
        let content = ZStack {
            FColor.fallback
            Image(uiImage: WoolTexture.image(w: WoolTexture.webCanvas.w, h: WoolTexture.webCanvas.h))
                .resizable()
                .aspectRatio(contentMode: .fill)
                .frame(width: size.width, height: size.height)
                .clipped()
            VStack(spacing: 6) {
                Text(FStrings.t("ios.lobby"))
                    .font(.headline).fontWeight(.bold).foregroundStyle(FColor.ink)
                VStack(spacing: 3) {
                    ForEach(Array(joinedNames.enumerated()), id: \.offset) { i, name in
                        Text("\(i + 1). \(name)")
                            .font(.subheadline).foregroundStyle(FColor.ink)
                            .lineLimit(1)
                    }
                }
                Text(FStrings.t("ios.msg.joininvite"))
                    .font(.caption).foregroundStyle(.black.opacity(0.55))
                    .padding(.top, 2)
            }
            .padding()
        }
        .frame(width: size.width, height: size.height)
        .environment(\.colorScheme, .light)   // theme-independent, like the board bubble
        let renderer = ImageRenderer(content: content)
        renderer.scale = UIScreen.main.scale
        renderer.isOpaque = true
        return renderer.uiImage
    }

    /// THE bubble image for a sealed chain, picking lobby-vs-board itself: a
    /// WAITING envelope previews as its roster, anything else as the public
    /// table. Callers (the extension's `stage`, the harness's transcript) get
    /// the same picture for the same bytes — which is the point of having one
    /// entry: a preview that disagrees with what the extension shows is the
    /// round-3 "the bubble previews a dealt board but the extension shows the
    /// lobby" report, and two call sites branching on `phase` separately is how
    /// that comes back.
    ///
    /// Reads the RESIDENT game for the board case, so the caller must have just
    /// sealed or decoded `env`'s payload (both do). Nil if there is nothing to
    /// render.
    @MainActor
    public static func render(env: MessageEnvelope) async -> UIImage? {
        if env.phase == 0 {
            return renderLobby(joinedNames: env.joins.sorted { $0.seat < $1.seat }.map(\.name))
        }
        guard let publicView = await MessageKernel.shared.residentView(viewer: -1) else { return nil }
        let names = Dictionary(env.joins.map { ($0.seat, $0.name) }, uniquingKeysWith: { a, _ in a })
        return render(publicView: publicView, names: names)
    }
}
