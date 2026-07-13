// WinView.swift — §6 screen 4. Result + the fool-reveal moment, then a
// no-dead-end trio: Rematch · Share replay · Home (§5.5 "every terminal state
// offers rematch/share/home"). Share is stubbed until the native replay codec
// lands (M-C); the button is present so the screen is never a dead end.

import SwiftUI
import FoolishKit

struct WinView: View {
    let view: GameView?
    let foolSeat: Int
    let humanSeat: Int
    let onRematch: () -> Void
    let onHome: () -> Void

    @State private var revealed = false
    @State private var toast: String?

    private var humanWon: Bool { foolSeat != humanSeat }

    var body: some View {
        ZStack {
            Color.black.opacity(0.6).ignoresSafeArea()
            VStack(spacing: FSpace.xl) {
                Spacer()
                Text(humanWon ? FStrings.t("you_win") : FStrings.t("you_lose"))
                    .font(FType.numeral(40))
                    .foregroundColor(humanWon ? FColor.win : FColor.accent)
                    .scaleEffect(revealed ? 1 : 0.8)
                    .opacity(revealed ? 1 : 0)

                if let name = foolName {
                    Text(name)
                        .font(FType.title(18))
                        .foregroundColor(FColor.textPrimary)
                        .opacity(revealed ? 1 : 0)
                }
                Spacer()

                VStack(spacing: FSpace.m) {
                    FButton(FStrings.t("rematch"), kind: .primary, action: onRematch)
                    FButton(FStrings.t("share_replay"), kind: .secondary) {
                        // Native replay encode lands in M-C (fio_replay_encode_b32).
                        toast = FStrings.t("ios.online_soon")
                    }
                    FButton(FStrings.t("home"), kind: .secondary, action: onHome)
                }
            }
            .padding(FSpace.xl)
        }
        .fToast($toast)
        .onAppear {
            Haptics.fire(humanWon ? .win : .reject)
            withAnimation(FMotion.card.delay(0.15)) { revealed = true }
        }
    }

    private var foolName: String? {
        guard let p = view?.player(foolSeat) else { return nil }
        return p.seat == humanSeat ? FStrings.t("ios.you") : (p.name.isEmpty ? "P\(p.seat)" : p.name)
    }
}
