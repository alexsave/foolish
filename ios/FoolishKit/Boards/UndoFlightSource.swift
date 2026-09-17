import CoreGraphics

/// WHERE AN UNDONE CARD FLIES HOME FROM.
///
/// Owner, on build 72: "the throw in works fine. animates, then collapses. Then
/// if you hit UNDO, the card that you just threw in jumps from its position on
/// the table to the center of the table, then animates back to your hand."
///
/// Filmed with the rig's squares: the card's slot at x=276.8 in the drawer, its
/// flight starting at x=172.8. The flight log had the rest. The undo publishes
/// the new board and its table re-lays out - two pairs, no thrown card - and
/// that reached `lastBattleCardFrames` before `flyUndoReturn` looked, 16ms and a
/// Task later. The card's own frame was gone, and the fallback was
/// `lastBattleFrames.values.first`: whichever slot a Dictionary happens to list
/// first, which differs from launch to launch (slot 1, 35pt off, on one run;
/// slot 0, 104pt off, on another). Older than the throw-in slide - it was there
/// with `table.slide=0` too.
///
/// So the board snapshots the frames when the undo STARTS, and this answers
/// from the card's own frame, then its own slot, then not at all: a card that
/// snaps home beats one that flies from somebody else's place.
public enum UndoFlightSource {
    /// `battles` is the table BEFORE the undo - the one the card was on.
    public static func rect(for card: Card, in battles: [BattleView],
                            cardFrames: [String: CGRect], slotFrames: [Int: CGRect],
                            ownSlotOnly: Bool) -> CGRect? {
        if let own = cardFrames[card.identity] { return own }
        guard ownSlotOnly else { return slotFrames.values.first }
        guard let i = battles.firstIndex(where: { $0.attack == card || $0.defense == card }),
              let slot = slotFrames[i] else { return nil }
        // The card as FBattleGrid lays it in its slot: 50x70, bottom-centred.
        return CGRect(x: slot.midX - 25, y: slot.maxY - 70, width: 50, height: 70)
    }

    /// Ships on. `undo.ownslot=0` in `dev.flags` puts back the any-slot lookup.
    public static let ownSlotByDefault = true

    /// The shipping default in Release; in DEBUG, whatever `dev.flags` says.
    public static var ownSlot: Bool {
        #if DEBUG || SOLO_TESTING
        return MessageDevBoard.flag("undo.ownslot", shipping: ownSlotByDefault)
        #else
        return ownSlotByDefault
        #endif
    }
}
