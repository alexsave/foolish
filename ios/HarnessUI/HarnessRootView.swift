// HarnessRootView — the FoolishHarness chrome around the real MessagesRootView.
// NOT SHIPPING CODE (see FoolishHarnessApp.swift). The bottom half is the exact
// extension UI; the top strip is fake-host controls: a 2-8 player switcher, the
// transcript of "sent" bubbles, and who you currently are.

import SwiftUI
import FoolishKit

struct HarnessRootView: View {
    @StateObject private var model = HarnessModel()

    var body: some View {
        // The thing under test — driven exactly as the extension drives it. The
        // fake-host controls hang off it as a TOP SAFE-AREA INSET rather than a
        // VStack sibling: MessagesRootView's WoolBackground is `.ignoresSafeArea()`,
        // so as a sibling its hit region bled up over the control strip and the
        // player/2p buttons went dead to taps. An inset reserves the bar's space
        // AND keeps it topmost for hit-testing, so the board can ignore safe area
        // underneath without stealing the controls' touches.
        MessagesRootView(
            payloadURL: model.payloadURL,
            style: model.presentation,
            senderIsLocal: model.senderIsLocal,
            startNewGame: model.startNewGame,
            chatIsDM: model.chatIsDM,
            chatPlayers: model.playerCount,
            requestExpand: { model.expand() },
            onNewGame: { model.newGame() },
            onSend: { payload, seat in await MainActor.run { model.stage(payload, seat: seat) } },
            onUnstage: { model.unstage() }
        )
        .id(model.viewKey)   // reset @State when player/transcript/intent changes
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .safeAreaInset(edge: .top, spacing: 0) {
            VStack(spacing: 0) {
                controls
                Divider().overlay(Color.orange.opacity(0.4))
            }
            .background(Color(white: 0.09))   // opaque: the board's wool can't show through
        }
        .background(Color.black.ignoresSafeArea())
        .task {
            // DEV: `HARNESS_SEED=1` deals a board immediately for screenshotting
            // (interactive tapping needs Accessibility perms the CI shell lacks).
            if ProcessInfo.processInfo.environment["HARNESS_SEED"] != nil {
                await model.seedDemoGame()
            }
        }
    }

    private var controls: some View {
        VStack(spacing: 6) {
            HStack {
                Text("FoolishHarness — NOT the shipping extension")
                    .font(.system(size: 10, weight: .bold)).foregroundStyle(.orange)
                Spacer()
                // The compact "New game" menu is gone (one game per chat), so the
                // harness offers its own reset button.
                Button { model.newGame() } label: {
                    Text("New").font(.system(size: 11, weight: .semibold))
                        .padding(.horizontal, 8).padding(.vertical, 4)
                        .background(Color.white.opacity(0.14)).foregroundStyle(.white).clipShape(Capsule())
                }
                Picker("", selection: Binding(get: { model.playerCount },
                                              set: { model.setCount($0) })) {
                    ForEach(2...8, id: \.self) { Text("\($0)p").tag($0) }
                }.pickerStyle(.menu).tint(.orange)
            }

            // Participant switcher — tap a name to "become" that player.
            ScrollView(.horizontal, showsIndicators: false) {
                HStack(spacing: 6) {
                    ForEach(Array(model.participants.enumerated()), id: \.element.id) { idx, p in
                        Button { model.become(idx) } label: {
                            Text(p.name)
                                .font(.system(size: 11, weight: idx == model.localIndex ? .bold : .regular))
                                .padding(.horizontal, 10).padding(.vertical, 5)
                                .background(idx == model.localIndex ? Color.orange : Color.white.opacity(0.14))
                                .foregroundStyle(idx == model.localIndex ? .black : .white)
                                .clipShape(Capsule())
                        }
                    }
                }
            }

            // The transcript of sent bubbles (who sent what, in order).
            if !model.transcript.isEmpty {
                ScrollView(.horizontal, showsIndicators: false) {
                    HStack(spacing: 4) {
                        ForEach(Array(model.transcript.enumerated()), id: \.element.id) { i, m in
                            Text("\(i + 1)·\(m.senderName)")
                                .font(.system(size: 9)).foregroundStyle(.white.opacity(0.7))
                                .padding(.horizontal, 6).padding(.vertical, 2)
                                .background(Color.white.opacity(0.1)).clipShape(Capsule())
                        }
                    }
                }
            }

            HStack {
                Text("you are \(model.localName) · \(model.senderIsLocal ? "you sent the latest" : "incoming from someone else")")
                    .font(.system(size: 9)).foregroundStyle(.white.opacity(0.5))
                Spacer()
                // Simulated Messages presentation — tap to toggle (mirrors the
                // extension's collapse-on-stage / "Open the game" expand).
                let collapsed = model.presentation == .compact
                Button { model.togglePresentation() } label: {
                    Text(collapsed ? "▾ collapsed" : "▸ expanded")
                        .font(.system(size: 10, weight: .semibold))
                        .padding(.horizontal, 9).padding(.vertical, 5)
                        .background(collapsed ? Color.orange.opacity(0.85) : Color.white.opacity(0.14))
                        .foregroundStyle(collapsed ? .black : .white).clipShape(Capsule())
                }
                // The blue send arrow: only lit when the board has staged a move.
                Button { model.deliver() } label: {
                    Text("Send ➤").font(.system(size: 12, weight: .bold))
                        .padding(.horizontal, 14).padding(.vertical, 6)
                        .background(model.staged != nil ? Color.blue : Color.white.opacity(0.12))
                        .foregroundStyle(.white).clipShape(Capsule())
                }
                .disabled(model.staged == nil)
            }
            if !model.debugInfo.isEmpty {
                Text(model.debugInfo).font(.system(size: 8, design: .monospaced))
                    .foregroundStyle(.green).frame(maxWidth: .infinity, alignment: .leading)
            }
        }
        .padding(8)
        .background(Color(white: 0.09))
    }
}
