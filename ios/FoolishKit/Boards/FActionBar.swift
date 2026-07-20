// FActionBar.swift — the action buttons, matching the WEB ActionButtons: a
// VERTICAL stack of small wooden pills on the RIGHT (Attack / Cover / Pass / Take
// / Good), plus an optional Undo used only by the iMessage board once a move is
// staged. EVERY enable state is driven by the kernel's legal menu (via CardPlay),
// never hand-computed (§3). A card is played by selecting it and tapping a button,
// by tapping a target battle, or by dragging.

import SwiftUI

public struct FActionBar: View {
    /// Shared fixed width for every wooden action button (equal-width column).
    private static let w: CGFloat = 96
    public let canAttack: Bool     // attacker: the selection is a legal attack
    public let canCover: Bool      // defender: the selection can cover some uncovered attack
    public let canPass: Bool       // defender: the selection is a legal pass/transfer
    public let canPickup: Bool     // defender: MOVE_PICKUP available (take the table)
    public let canDone: Bool       // attacker: MOVE_GOOD available (бито), all covered
    public let canUndo: Bool       // iMessage: a move is staged — un-stage it
    public let onAttack: () -> Void
    public let onCover: () -> Void
    public let onPass: () -> Void
    public let onPickup: () -> Void
    public let onDone: () -> Void
    public let onUndo: () -> Void

    public init(canAttack: Bool = false, canCover: Bool = false, canPass: Bool = false,
                canPickup: Bool = false, canDone: Bool = false, canUndo: Bool = false,
                onAttack: @escaping () -> Void = {}, onCover: @escaping () -> Void = {},
                onPass: @escaping () -> Void = {}, onPickup: @escaping () -> Void = {},
                onDone: @escaping () -> Void = {}, onUndo: @escaping () -> Void = {}) {
        self.canAttack = canAttack; self.canCover = canCover; self.canPass = canPass
        self.canPickup = canPickup; self.canDone = canDone; self.canUndo = canUndo
        self.onAttack = onAttack; self.onCover = onCover; self.onPass = onPass
        self.onPickup = onPickup; self.onDone = onDone; self.onUndo = onUndo
    }

    public var body: some View {
        // Small wooden pills, stacked vertically, pinned right (web parity).
        HStack {
            Spacer(minLength: 0)
            // All buttons share ONE fixed width so the wooden column is a clean
            // rectangle stack (web parity), not ragged to each word's length.
            VStack(alignment: .trailing, spacing: FSpace.s) {
                if canAttack { FButton(FStrings.t("attack"), kind: .wood, compact: true, fixedWidth: Self.w, action: onAttack) }
                if canCover  { FButton(FStrings.t("cover"),  kind: .wood, compact: true, fixedWidth: Self.w, action: onCover) }
                if canPass   { FButton(FStrings.t("pass"),   kind: .wood, compact: true, fixedWidth: Self.w, action: onPass) }
                if canPickup { FButton(FStrings.t("pickup"), kind: .wood, compact: true, fixedWidth: Self.w, action: onPickup) }
                // Good reads as a green check, not the word "Good" (§13): same
                // wooden-pill footprint as the other compact buttons, icon centred.
                if canDone   { FGoodButton(width: Self.w, action: onDone) }
                if canUndo   { FButton(FStrings.t("ios.msg.undo"), kind: .wood, compact: true, fixedWidth: Self.w, action: onUndo) }
            }
        }
        .padding(.horizontal, FSpace.m)
        .animation(FMotion.chrome, value: canAttack)
        .animation(FMotion.chrome, value: canCover)
        .animation(FMotion.chrome, value: canPass)
        .animation(FMotion.chrome, value: canPickup)
        .animation(FMotion.chrome, value: canDone)
        .animation(FMotion.chrome, value: canUndo)
    }
}

/// The Good button, icon-only (§13: "Good" reads as a green check, not a word).
/// Mirrors FButton's `.wood`/`compact` look exactly (WoodFill background, sharp
/// corners, the same border) rather than growing FButton into a label-view type —
/// this is the only wood button that isn't a Text, so a small standalone view is
/// less risky than making FButton generic over its content.
private struct FGoodButton: View {
    let width: CGFloat
    let action: () -> Void

    var body: some View {
        Button(action: { Haptics.fire(.drop); action() }) {
            FCheck(size: 20)
                .frame(width: width, height: 40)
                .background(WoodFill())
                .overlay(
                    Rectangle().strokeBorder(.black.opacity(0.35), lineWidth: 1)
                )
        }
        .accessibilityLabel(Text("Good"))
    }
}
