// FActionBar.swift — the action buttons, matching the WEB ActionButtons: a
// VERTICAL stack of small wooden pills on the RIGHT (Attack / Cover / Pass / Take
// / Good), plus an optional Undo used only by the iMessage board once a move is
// staged. EVERY enable state is driven by the kernel's legal menu (via CardPlay),
// never hand-computed (§3). A card is played by selecting it and tapping a button,
// by tapping a target battle, or by dragging.

import SwiftUI

public struct FActionBar: View {
    /// Re-render this view when a setting changes (see FPrefs). Only the
    /// OBSERVATION matters - the strings still come from FStrings.t and the
    /// table surface still comes from FTextures.
    @ObservedObject private var prefs = FPrefs.shared
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
            // Every button carries `.transition(.identity)` so it appears and
            // disappears INSTANTLY - even when the flip happens inside a
            // `withAnimation` from the bout-end sequence (a pickup stages `Undo`
            // via `canSend` right as the sweep animation runs, so its insertion
            // would otherwise ride that ambient spring and the outgoing Pickup pill
            // would linger, fading, above the Undo that replaced it - the "ghostly
            // Pickup floating above Undo"). Identity transition = no insert/remove
            // animation under ANY transaction, so Pickup is simply gone and Undo is
            // simply there, exactly as Attack -> Undo already reads.
            VStack(alignment: .trailing, spacing: FSpace.s) {
                if canAttack { FButton(FStrings.t("attack"), kind: .wood, compact: true, fixedWidth: Self.w, action: onAttack).transition(.identity) }
                if canCover  { FButton(FStrings.t("cover"),  kind: .wood, compact: true, fixedWidth: Self.w, action: onCover).transition(.identity) }
                if canPass   { FButton(FStrings.t("pass"),   kind: .wood, compact: true, fixedWidth: Self.w, action: onPass).transition(.identity) }
                if canPickup { FButton(FStrings.t("pickup"), kind: .wood, compact: true, fixedWidth: Self.w, action: onPickup).transition(.identity) }
                // Good reads as the WORD "Good" (note 7), same as every other
                // wooden pill in this column. A previous batch put the FCheck
                // glyph here too, borrowing it from FSeatBadge's per-seat status
                // pip (the little sword/shield/check row) - but that pip marks
                // "this seat already said good", a different thing from the
                // button that SAYS it. FCheck stays a status-only glyph; this
                // button is text like its siblings.
                if canDone   { FButton(FStrings.t("good"), kind: .wood, compact: true, fixedWidth: Self.w, action: onDone).transition(.identity) }
                if canUndo   { FButton(FStrings.t("ios.msg.undo"), kind: .wood, compact: true, fixedWidth: Self.w, action: onUndo).transition(.identity) }
            }
        }
        .padding(.horizontal, FSpace.m)
        // Buttons SNAP in and out - no fade, no reflow (owner: "buttons should
        // not move / never float"). The chrome cross-fade this used to carry
        // (`.animation(FMotion.chrome, value: canPickup/canUndo/...)`) was the
        // actual "ghostly Pickup floating up above Undo": staging a pickup flips
        // canPickup true->false and canUndo false->true in one update, and while
        // the two DIFFERENT pills cross-faded they briefly stacked (Pickup on top
        // of Undo) as the VStack collapsed - so the outgoing Pickup rose as it
        // faded. Applied INSIDE this view, that animation sat below the board's
        // own `.transaction { animation = nil }`, so nothing upstream could stop
        // it. Dropping it entirely is the fix: a button that turns off just
        // disappears, and the one replacing it is simply there.
        .transaction { $0.animation = nil }
    }
}
