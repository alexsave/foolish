// FActionBar.swift — the action buttons, matching the WEB ActionButtons: a
// VERTICAL stack of small wooden pills on the RIGHT (Attack / Cover / Pass / Take
// / Good), plus an optional Undo used only by the iMessage board once a move is
// staged. EVERY enable state is driven by the kernel's legal menu (via PlayWire),
// never hand-computed (§3). A card is played by selecting it and tapping a button,
// by tapping a target battle, or by dragging.

import SwiftUI

public struct FActionBar: View {
    /// Re-render this view when a setting changes (see FPrefs). Only the
    /// OBSERVATION matters - the strings still come from FStrings.t and the
    /// table surface still comes from FTextures.
    @ObservedObject private var prefs = FPrefs.shared
    /// Shared fixed width for every wooden action button (equal-width column).
    private static let w: CGFloat = pillWidth
    /// Every action pill's width, Undo's included - see `ActionPillSlot`.
    public static let pillWidth: CGFloat = 96
    /// The column's inner trailing inset: its `.padding(.horizontal, FSpace.m)`.
    public static let innerInset: CGFloat = FSpace.m
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
        .padding(.horizontal, Self.innerInset)
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


/// WHERE AN ACTION PILL SITS, whatever word is on it.
///
/// Owner, round 47: "the position of the action button when it is Pickup is
/// slightly off from the position of the action button when it is Undo...
/// Button should be fixed position and fixed width, only changing text." And:
/// "distance from left edge of screen and left edge of setting gear button
/// should be equal to distance from right edge of action button and right edge
/// of screen."
///
/// Measured before touching anything, on the simulator (440pt wide, 8 seats,
/// expanded, the same board before and after a Pickup): Pickup sat at
/// 320-416pt, 24.0pt from the right edge; Undo at 316-412pt, 28.0pt; the gear
/// 24.0pt from the left. The owner's build-70 recording agrees on a real phone
/// (Attack and Good 311-405, Undo 307-401). Same width, same row, Undo 4pt left.
///
/// The cause is round 10g. It hoisted Undo out of FActionBar into its own
/// always-present slot so it would stop flying in from 280pt above, and placed
/// that slot with `.padding(.trailing, 20)` - while the pills it replaced sit at
/// 4 outer + FSpace.m (12) inner = 16, a number the comment directly above that
/// line spells out. The settings squares mirror the pills exactly (4 outer +
/// FSpace.m inner on the leading side), so aligning Undo fixes both of the
/// owner's rules at once: one inset for every pill, and the same inset as the
/// gear. Both placements now read it from here instead of typing a number.
public enum ActionPillSlot {
    /// NO PLAY BUTTON BETWEEN THE TAP AND THE STAGE. Owner: "we hit it as
    /// attack, it disapares without changing to 'good'... then eventually come
    /// back as 'undo'". `play` clears the selection at once and applies in a
    /// Task, and for that one paint an attacker with no selection and nothing
    /// staged was offered Good. Ships on; `actions.holdwhileplaying=0` in
    /// `dev.flags` puts the flash back.
    public static let holdsWhilePlayingByDefault = true

    public static var holdsWhilePlaying: Bool {
        #if DEBUG || SOLO_TESTING
        return MessageDevBoard.flag("actions.holdwhileplaying", shipping: holdsWhilePlayingByDefault)
        #else
        return holdsWhilePlayingByDefault
        #endif
    }

    /// Undo lands where every other pill does. ON; `pill.aligned=0` in the DEBUG
    /// `dev.flags` file restores round 10g's inset for comparison.
    public static let alignedByDefault = true

    /// The column's outer inset inside the board, on the trailing side for the
    /// pills and on the leading side for the settings squares.
    public static let outerInset: CGFloat = 4

    /// Where every pill's trailing edge sits, from the board's content edge.
    public static var pillTrailing: CGFloat { outerInset + FActionBar.innerInset }

    /// Round 10g's inset for the Undo slot - 4pt wider than the column it was
    /// hoisted out of. Kept only for the flag-off path.
    static let legacyUndoTrailing: CGFloat = 20

    /// The trailing padding the Undo slot applies to its 96pt pill.
    public static func undoTrailing(aligned: Bool) -> CGFloat {
        aligned ? pillTrailing : legacyUndoTrailing
    }

    /// The live value: the shipping default in Release; in DEBUG, whatever
    /// `dev.flags` says, and the shipping default when it says nothing.
    public static var aligned: Bool {
        #if DEBUG || SOLO_TESTING
        return MessageDevBoard.flag("pill.aligned", shipping: alignedByDefault)
        #else
        return alignedByDefault
        #endif
    }
}
