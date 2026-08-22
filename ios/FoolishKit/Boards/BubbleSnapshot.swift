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

    /// The bubble's wool: a CROP of the weave at the board's own magnification,
    /// bottom-anchored exactly like `TableBackground`.
    ///
    /// Round-6 #14, verbatim: "wool is too zoomed out in the bubble preview -
    /// basically, try to keep the threads the same size visually no matter the
    /// view". The cause was here and it was an `.aspectRatio(.fill)`: fitting
    /// the whole 1920x1080 weave into a 300x195 balloon drew it at
    /// max(300/1920, 195/1080) = 0.181 pt/texel where the live board draws it
    /// at 0.775, so the bubble showed the ENTIRE picture shrunk instead of a
    /// WINDOW onto it - 14.5pt plaid blocks against the board's 62pt, four
    /// times too small. Drawing `TableWeave` (which is pinned to
    /// `WoolTexture.pointsPerTexel`) and clipping it to the balloon makes the
    /// two literally the same pixels at the same size; there is no second scale
    /// left to disagree with.
    ///
    /// A ZStack LAYER with an explicit frame, not a `.background` — a
    /// background renders unreliably inside ImageRenderer. Nothing here is
    /// async either: `FTextures.wool` is a loaded image, so it is available on
    /// the synchronous pass ImageRenderer makes (the old procedural path had to
    /// call the generator inline because `TableBackground`'s `.task` never runs
    /// under ImageRenderer).
    /// The frame is what SIZES the weave now (round 16: TableWeave fills its
    /// container and clips itself). The balloon is far smaller than the bake in
    /// both axes, so the magnification it draws at is still exactly
    /// `pointsPerTexel` — the cover floor never engages here, and these are
    /// still literally the board's pixels.
    private static var wool: some View {
        TableWeave()
            .frame(width: size.width, height: size.height)
    }

    /// Render `publicView` (which MUST be a viewer:-1 / no-hand view) into a
    /// bubble image. Returns nil only if the renderer fails. MainActor because
    /// ImageRenderer walks a live SwiftUI view.
    @MainActor
    public static func render(publicView: GameView, names: [Int: String] = [:],
                              scheme: ColorScheme = .light) -> UIImage? {
        let content = ZStack {
            FColor.fallback
            Self.wool
            MessageBoardView(view: publicView, names: names)
        }
        .frame(width: size.width, height: size.height)
        // Round-7 #3: the bubble follows the SENDER's scheme ("bubble display
        // didn't pick up on the dark mode?"). It is baked at send time, so the
        // only scheme available is this device's - a dark-mode sender's bubble
        // is dark, and a recipient in the opposite scheme sees it as sent (like
        // any screenshot). `scheme` propagates to the wool, the cards and the
        // text treatments through the environment, so the whole image agrees.
        .environment(\.colorScheme, scheme)
        // Round-5 M4's clamp: ImageRenderer walks whatever accessibility text
        // size the HOST app is currently at, not a neutral default — an
        // AX-XXXL host would blow the board's card faces out of the 300×195
        // balloon the same way B3 blows them out of the live board. The bubble
        // is a fixed-size snapshot, so it must never inherit that setting.
        .dynamicTypeSize(.large)
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
    public static func renderLobby(joinedNames: [String], scheme: ColorScheme = .light) -> UIImage? {
        let content = ZStack {
            FColor.fallback
            Self.wool
            VStack(spacing: 6) {
                // Round-6 #17: `onTableText` (Tokens.swift) - thick ink, since
                // this whole card is drawn straight on `Self.wool` above.
                Text(FStrings.t("ios.lobby"))
                    .font(.headline).onTableText()
                VStack(spacing: 3) {
                    ForEach(Array(joinedNames.enumerated()), id: \.offset) { i, name in
                        Text("\(i + 1). \(name)")
                            .font(.subheadline).onTableText()
                            .lineLimit(1)
                    }
                }
                // Round-5 M10: was `.black.opacity(0.55)` directly on the
                // weave — the finding's own example of the fix that already
                // exists elsewhere ("Game over" at full-opacity + a real
                // shadow) not yet applied here. Full-opacity FColor.ink (dark
                // text) takes a LIGHT shadow, not a dark one — a dark shadow
                // under dark text on a light-ish weave adds nothing. Round-6
                // #17 added the weight `onTableText` now carries.
                Text(FStrings.t("ios.msg.joininvite"))
                    .font(.caption).onTableText()
                    .padding(.top, 2)
            }
            .padding()
        }
        .frame(width: size.width, height: size.height)
        .environment(\.colorScheme, scheme)   // follows the sender's scheme (round-7 #3)
        // Round-5 M4's clamp — see the twin comment in `render(publicView:)`
        // above; the lobby roster names must not blow out of the balloon
        // either.
        .dynamicTypeSize(.large)
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
    public static func render(env: MessageEnvelope, scheme: ColorScheme = .light) async -> UIImage? {
        if env.phase == 0 {
            return renderLobby(joinedNames: env.joins.sorted { $0.seat < $1.seat }.map(\.name),
                               scheme: scheme)
        }
        guard let publicView = await MessageKernel.shared.residentView(viewer: -1) else { return nil }
        let names = Dictionary(env.joins.map { ($0.seat, $0.name) }, uniquingKeysWith: { a, _ in a })
        return render(publicView: publicView, names: names, scheme: scheme)
    }
}
