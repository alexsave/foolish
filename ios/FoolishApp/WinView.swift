// WinView.swift — §6 screen 4. Result + the fool-reveal moment, then a
// no-dead-end trio: Rematch · Share replay · Home (§5.5 "every terminal state
// offers rematch/share/home"). Share now produces a real base32 replay code
// (native codec, §16.C2): a ShareLink to foolish.cards/<code> plus a QR.

import SwiftUI
import FoolishKit

struct WinView: View {
    @ObservedObject var game: LocalGame
    let foolSeat: Int
    let humanSeat: Int
    let onRematch: () -> Void
    let onHome: () -> Void

    @State private var revealed = false
    @State private var shareURL: URL?
    @State private var qr: UIImage?
    @State private var showQR = false

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

                if showQR, let qr {
                    Image(uiImage: qr)
                        .interpolation(.none)
                        .resizable()
                        .frame(width: 200, height: 200)
                        .clipShape(RoundedRectangle(cornerRadius: FRadius.card))
                        .transition(.opacity)
                }
                Spacer()

                VStack(spacing: FSpace.m) {
                    FButton(FStrings.t("rematch"), kind: .primary, action: onRematch)
                    shareControls
                    FButton(FStrings.t("home"), kind: .secondary, action: onHome)
                }
            }
            .padding(FSpace.xl)
        }
        .onAppear {
            Haptics.fire(humanWon ? .win : .reject)
            withAnimation(FMotion.card.delay(0.15)) { revealed = true }
            Task {
                shareURL = await game.makeShareURL()
                if let s = shareURL?.absoluteString { qr = QRCode.image(for: s) }
            }
        }
    }

    @ViewBuilder
    private var shareControls: some View {
        if let shareURL {
            HStack(spacing: FSpace.m) {
                ShareLink(item: shareURL) {
                    Text(FStrings.t("share_replay"))
                        .font(FType.title(17))
                        .frame(maxWidth: .infinity, minHeight: 52)
                        .foregroundColor(FColor.textPrimary)
                        .overlay(RoundedRectangle(cornerRadius: FRadius.card)
                            .strokeBorder(FColor.textDim.opacity(0.6), lineWidth: 1.5))
                }
                Button(action: { withAnimation(FMotion.chrome) { showQR.toggle() } }) {
                    Image(systemName: "qrcode")
                        .font(.system(size: 20))
                        .frame(width: 52, height: 52)
                        .foregroundColor(FColor.textPrimary)
                        .overlay(RoundedRectangle(cornerRadius: FRadius.card)
                            .strokeBorder(FColor.textDim.opacity(0.6), lineWidth: 1.5))
                }
                .accessibilityLabel("Show QR code")
            }
        } else {
            // Encoding not ready (or failed) — keep the screen non-dead-end.
            FButton(FStrings.t("share_replay"), kind: .secondary, enabled: false, action: {})
        }
    }

    private var foolName: String? {
        guard let p = game.view?.player(foolSeat) else { return nil }
        return p.seat == humanSeat ? FStrings.t("ios.you") : (p.name.isEmpty ? "P\(p.seat)" : p.name)
    }
}
