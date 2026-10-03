import Foundation

/// THE FLIPPED TRUMP IS DEALT LIKE ANY OTHER CARD.
///
/// Owner: "the flipped card should also have a deal animation to whoever gets
/// it". The draw that empties the stock takes the trump from under it as its
/// last card. The kernel's plan names that card on the step (AnimPlan.Step
/// .trumpOut) for every viewer, and the board flies it face up from its own
/// slot (`MessageTableView.drawFlights`), letting go of the slot in the same
/// breath the flight leaves (`runEventStream`).
///
/// `trump.flight=0` in `dev.flags` puts back the draw as it was: every card
/// from the deck, backs to another seat, and the slot emptied as the stock's
/// cards leave.
public enum TrumpFlight {
    public static let fliesByDefault = true

    public static var flies: Bool {
        #if DEBUG || SOLO_TESTING
        return MessageDevBoard.flag("trump.flight", shipping: fliesByDefault)
        #else
        return fliesByDefault
        #endif
    }
}
