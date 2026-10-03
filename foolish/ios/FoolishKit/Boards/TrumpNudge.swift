import CoreGraphics

/// THE BARE TRUMP GLYPH SITS A LITTLE UP AND RIGHT ON THE LIVE BOARD.
///
/// Owner: "Trump indicator should be slightly up and right". "Trump indicator"
/// is the owner's name for the bare suit glyph that replaces the stock and the
/// flipped card once both are gone (1.1(55): "if there still is a flipped card
/// on board, don't show Trump indicator yet!"; round 16: "Top left corner Trump
/// indicator is a bit low"), so only the glyph moves. The stock, its count chip
/// and the flipped card keep the origins FDeckWell pins (round 4 note 6).
///
/// Measured on the drawer before the change (iPhone Air, compact and expanded,
/// 2 and 4 seats): the glyph's ink began 15.3pt from the felt's left edge and
/// 40.3pt from its top, while the discard pile across the board begins 34.0pt
/// from the top. Six up puts the glyph's top on the discard pile's top, and
/// four right takes it off the felt's rounded corner.
///
/// The live board only. On the public bubble Messages paints the app's roundel
/// over this corner (PublicBoardLayout.balloonIconCentre), so up there would put
/// more of the glyph under it; that board keeps zero.
///
/// `trump.nudge=0` in `dev.flags` puts the glyph back on its inset.
public enum TrumpNudge {
    public static let byDefault = true

    /// The nudge, in scale-1 points: x right, y down.
    public static let shift = CGSize(width: 4, height: -6)

    /// What the glyph is moved by with the flag `on` or off.
    public static func offset(on: Bool) -> CGSize { on ? shift : .zero }

    /// What the live board passes to FDeckWell.
    public static var live: CGSize { offset(on: enabled) }

    public static var enabled: Bool {
        #if DEBUG || SOLO_TESTING
        return MessageDevBoard.flag("trump.nudge", shipping: byDefault)
        #else
        return byDefault
        #endif
    }
}
