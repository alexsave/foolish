// TranscriptBubbleView — the game bubble as drawn LIVE in the Messages
// transcript (MSMessageLiveLayout), added in 1.0(7).
//
// Why this exists: a template layout bakes its picture AND its text on the
// SENDER's device, so a Korean player's "앨릭스가 K♠로 공격" travelled as literal
// Korean to everyone, and a dark-mode sender's board arrived dark for a
// light-mode reader. A live layout instead hands the RECEIVING device the game
// bytes and lets it draw the balloon itself — this view. Because every string
// routes through `FStrings.t` (the reader's locale) and no `colorScheme` is
// forced, the SAME payload renders in each viewer's own language and appearance.
//
// THE RULE holds: nothing here decides a move. It decodes the payload through
// the one kernel (`MessageKernel.transcriptBubble`, a single atomic hop so
// concurrent bubbles in a long thread can't clobber each other's resident game)
// and describes it through the one summariser (`MessageSummary.describe`, shared
// with the sender's baked `summaryText` so the two can never disagree).
//
// Read-only by construction: a transcript bubble has no gestures. A tap is
// Messages' own — it launches the full extension (compact), routed as usual.

import Foundation
import SwiftUI

public struct TranscriptBubbleView: View {
    /// The presented message's url (`conversation.selectedMessage?.url` in the
    /// transcript instance). nil / not-ours degrades to a plain wool balloon.
    private let payloadURL: URL?

    /// The 300×195 public board (from `BubbleSnapshot.size`) plus a caption strip
    /// below it, on one continuous wool. A fixed design the body scales to the
    /// balloon width the host hands us (`contentSizeThatFits` returns this aspect).
    public static let designSize = CGSize(width: BubbleSnapshot.size.width,
                                          height: BubbleSnapshot.size.height + 40)

    @State private var view: GameView?
    @State private var joins: [MessageJoin] = []
    @State private var names: [Int: String] = [:]
    @State private var captionText: String = ""
    @State private var isLobby = false

    public init(payloadURL: URL?) { self.payloadURL = payloadURL }

    public var body: some View {
        GeometryReader { geo in
            let scale = geo.size.width / Self.designSize.width
            design
                .frame(width: Self.designSize.width, height: Self.designSize.height)
                .scaleEffect(scale, anchor: .topLeading)
        }
        .aspectRatio(Self.designSize.width / Self.designSize.height, contentMode: .fit)
        // A fixed-size balloon must never inherit the reader's AX text size, or the
        // board's card faces blow out of it — the same clamp BubbleSnapshot bakes in.
        .dynamicTypeSize(.large)
        .task(id: payloadURL) { await load() }
    }

    private var design: some View {
        ZStack {
            FColor.fallback
            // ONE continuous wool behind board AND caption — a single bottom-anchored
            // crop, so there is no texture seam between the two (a two-region stack
            // would show one). Same weave, same magnification as the live board.
            WoolWeave()
                .frame(width: Self.designSize.width, height: Self.designSize.height, alignment: .bottom)
                .clipped()
            VStack(spacing: 0) {
                boardArea
                    .frame(width: BubbleSnapshot.size.width, height: BubbleSnapshot.size.height)
                caption
                    .frame(width: BubbleSnapshot.size.width,
                           height: Self.designSize.height - BubbleSnapshot.size.height)
            }
        }
    }

    /// A WAITING lobby previews its roster (matching the baked `renderLobby`);
    /// every other phase previews the public table. A payload we couldn't decode
    /// leaves the wool bare under the generic caption.
    @ViewBuilder private var boardArea: some View {
        if isLobby {
            VStack(spacing: 4) {
                Text(FStrings.t("ios.lobby")).font(.headline).onWoolText()
                ForEach(Array(joins.sorted { $0.seat < $1.seat }.enumerated()), id: \.offset) { i, j in
                    Text("\(i + 1). \(j.name)").font(.subheadline).onWoolText().lineLimit(1)
                }
            }
            .padding()
        } else if let view {
            MessageBoardView(view: view, names: names)
        }
    }

    private var caption: some View {
        Text(captionText.isEmpty ? FStrings.t("ios.msg.tap") : captionText)
            .font(.footnote.weight(.semibold))
            .multilineTextAlignment(.center)
            .lineLimit(2)
            .minimumScaleFactor(0.7)
            .onWoolText()
            .padding(.horizontal, 10)
    }

    @MainActor private func load() async {
        // Not our link (a bare replay code, a foreign url) or an unreadable base32:
        // degrade to the plain wool balloon + generic tap line, never an error card.
        guard let url = payloadURL,
              let bytes = try? MessageEnvelope.payloadBytes(url: url),
              let (env, v, events) = try? await MessageKernel.shared.transcriptBubble(payload: bytes)
        else {
            view = nil; isLobby = false; joins = []
            captionText = FStrings.t("ios.msg.tap")
            return
        }
        let nm = Dictionary(env.joins.map { ($0.seat, $0.name) }, uniquingKeysWith: { a, _ in a })
        names = nm
        joins = env.joins
        isLobby = env.phase == 0
        view = isLobby ? nil : v
        captionText = MessageSummary.describe(env: env, names: nm, view: v, events: events)
    }
}
