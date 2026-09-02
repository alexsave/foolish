// ChatListView — the FoolishHarness chat list, reached via the back chevron in
// HarnessRootView's ContactNavBar (Messages-host redesign, requirement 2).
// NOT SHIPPING CODE (see FoolishHarnessApp.swift).
//
// Real Messages routes "back" from an open conversation to the conversation
// LIST, not to a special harness control. So the old two-button "Chat A" /
// "Chat B" switcher (which used to live inside the top control strip) is
// gone; this screen is what replaces it. It shows one row per simulated
// conversation (`HarnessModel.chatSummaries`), labeled from whichever
// participant is currently "you" — e.g. "Vera chat A" / "Vera chat B" for a
// 2-player game, or "Group chat A" / "Group chat B" for 3+ — because in every
// case both rows are the SAME people, just two separate threads (not
// possible on a real phone, but exactly what the cross-chat isolation fix
// this harness reproduces needs: see HarnessModel's type doc for why "same
// participants, two chatKeys" is the precondition for that bug).
//
// Deliberately does NOT show the participant ("who am I") swap — that lives
// in HarnessRootView's DevBar, OUTSIDE the simulated phone entirely
// (requirement 3), a different axis from "which conversation is open" that
// this screen answers. Sized to exactly fill the shared SEScreen frame
// (requirement 4) — no `.ignoresSafeArea()` of its own; the parent
// `simulatedPhone` already clips everything to that fixed 375x667pt box.

import SwiftUI
import FoolishKit

struct ChatListScreen: View {
    @ObservedObject var model: HarnessModel
    let onSelect: (Int) -> Void

    var body: some View {
        VStack(spacing: 0) {
            header
            List {
                ForEach(model.chatSummaries) { chat in
                    Button { onSelect(chat.id) } label: { row(chat) }
                        .listRowBackground(Color(white: 0.07))
                }
            }
            .listStyle(.plain)
            .scrollContentBackground(.hidden)
            .background(Color.black)
        }
        .background(Color(white: 0.05))
    }

    /// Plain "Messages"-style title, plus a small dev-only subtitle so this
    /// still reads as harness chrome and not a claim about the real app's
    /// conversation list.
    private var header: some View {
        VStack(spacing: 2) {
            Text("Messages")
                .font(.system(size: 17, weight: .semibold)).foregroundStyle(.white)
            Text("DEV: pick a simulated conversation")
                .font(.system(size: 10, weight: .semibold)).foregroundStyle(.orange)
        }
        .frame(maxWidth: .infinity)
        .padding(.top, 12).padding(.bottom, 10)
        .background(Color(white: 0.07))
    }

    private func row(_ chat: HarnessModel.ChatSummary) -> some View {
        HStack(spacing: 12) {
            Circle()
                .fill(Color.blue.opacity(0.55))
                .frame(width: 44, height: 44)
                .overlay(
                    Text(model.contactInitials)
                        .font(.system(size: 15, weight: .semibold)).foregroundStyle(.white)
                )
            VStack(alignment: .leading, spacing: 2) {
                HStack(spacing: 6) {
                    Text(chat.label)
                        .font(.system(size: 15, weight: .semibold)).foregroundStyle(.white)
                    if chat.isCurrent {
                        Text("current")
                            .font(.system(size: 9, weight: .bold)).foregroundStyle(.black)
                            .padding(.horizontal, 6).padding(.vertical, 2)
                            .background(Color.orange).clipShape(Capsule())
                    }
                }
                Text(chat.preview)
                    .font(.system(size: 12)).foregroundStyle(.white.opacity(0.5)).lineLimit(1)
            }
            Spacer()
        }
        .padding(.vertical, 6)
        .contentShape(Rectangle())   // the whole row is tappable, not just the text/avatar
    }
}
