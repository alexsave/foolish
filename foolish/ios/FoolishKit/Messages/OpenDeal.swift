import Foundation

/// A STARTED CHAT DEALS ITS CARDS.
///
/// Owner: "Did not see card deal". A started bubble is a game dealt with no
/// move made yet, and the board animates whatever the chain's replay holds. The
/// kernel now encodes such a game (a zero-atom v6 code, c/src/replay.c), so its
/// replay is one step - the deal, round-robin at the kernel's pace - and the
/// bridge hands it over as the opening stream (fio_replay_last_events_packed,
/// "a chain that IS only the deal"). Opening the started bubble, and Start
/// arriving on a lobby or tapped here, all play it.
///
/// `open.deal=0` in `dev.flags` puts back the quiet open: the started board
/// appears already dealt, as it did before.
public enum OpenDeal {
    public static let showsByDefault = true

    public static var shows: Bool {
        #if DEBUG || SOLO_TESTING
        return MessageDevBoard.flag("open.deal", shipping: showsByDefault)
        #else
        return showsByDefault
        #endif
    }
}
