// TutorialView.swift — §6 screen 6 / §16.B6. The tutorial ported as an offline
// scripted game against the engine: a real 2-player LocalGame vs a gentle bot,
// fronted by a coach that teaches by doing. The lesson CONTENT is what's ported
// from the web tutorial (attack → defend → take/done → win), not the exact web
// deal (§16.B6 allows a fresh seed when the deal can't be reproduced).
//
// The board and rules are the real thing (same TableView / kernel) — the coach
// is a passive overlay, so the player learns the actual interaction.

import SwiftUI
import FoolishKit

struct TutorialView: View {
    @Environment(\.dismiss) private var dismiss

    // Fixed seed so the tutorial deal is stable. Opponent = handwritten (roster
    // id 2) — a calm, predictable teacher. Human is seat 0.
    @StateObject private var game = LocalGame(
        seed: Data((0..<32).map { UInt8(($0 * 29 + 11) & 0xFF) }),
        players: 2, humanSeat: 0, strategies: [1: 2]
    )

    private let tips = ["ios.tut_1", "ios.tut_2", "ios.tut_3", "ios.tut_4", "ios.tut_5"]
    @State private var step = 0
    @State private var coachDismissed = false

    var body: some View {
        ZStack {
            TableView(game: game, onLeave: { dismiss() })

            // Coach overlay — advances on tap; the last tip starts free play.
            if !coachDismissed {
                coach
            }

            if let fool = game.foolSeat {
                WinView(
                    game: game,
                    foolSeat: fool,
                    humanSeat: game.humanSeat,
                    onRematch: { dismiss() },     // one lesson is enough; back home
                    onHome: { dismiss() }
                )
            }
        }
        .animation(FMotion.chrome, value: coachDismissed)
        .animation(FMotion.chrome, value: step)
    }

    private var coach: some View {
        VStack {
            Spacer()
            VStack(spacing: FSpace.l) {
                Text(FStrings.t(tips[step]))
                    .font(FType.body(16))
                    .foregroundColor(FColor.textPrimary)
                    .multilineTextAlignment(.center)
                    .fixedSize(horizontal: false, vertical: true)

                FButton(step == tips.count - 1 ? FStrings.t("ios.tut_done") : FStrings.t("ios.tut_next")) {
                    if step < tips.count - 1 { step += 1 }
                    else { coachDismissed = true }
                }
            }
            .padding(FSpace.xl)
            .background(FColor.surface.opacity(0.98))
            .clipShape(RoundedRectangle(cornerRadius: FRadius.sheet))
            .padding(FSpace.l)
            .padding(.bottom, FSpace.xxl)
        }
        .background(Color.black.opacity(0.35).ignoresSafeArea())
        .transition(.opacity)
    }
}
