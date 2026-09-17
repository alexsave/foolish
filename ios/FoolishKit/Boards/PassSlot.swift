import Foundation

/// THE PASS PREVIEW'S EMPTY SLOT STAYS PUT.
///
/// Owner, on the pass: "the worst". Filmed with the rig's squares, the two pairs
/// on the table moved three times for one pass - the slot closing as the finger
/// crossed a pair (a drop there is a cover the kernel refuses), and closing
/// again on release, a beat before the kernel published the pair that was going
/// to stand in it. The kernel decides (`anim_pass_slot_shown`); these are its
/// two rules' switches. Both ship on.
public enum PassSlot {
    /// Keep the slot from the release until the table has the new pair.
    /// `pass.holdslot=0` in `dev.flags` puts back the close-on-release.
    public static let holdByDefault = true
    /// Keep the slot while the finger crosses a pair it cannot drop on.
    /// `pass.stickyslot=0` in `dev.flags` puts back the close-on-crossing.
    public static let stickyByDefault = true

    public static var hold: Bool {
        #if DEBUG || SOLO_TESTING
        return MessageDevBoard.flag("pass.holdslot", shipping: holdByDefault)
        #else
        return holdByDefault
        #endif
    }

    public static var sticky: Bool {
        #if DEBUG || SOLO_TESTING
        return MessageDevBoard.flag("pass.stickyslot", shipping: stickyByDefault)
        #else
        return stickyByDefault
        #endif
    }
}
