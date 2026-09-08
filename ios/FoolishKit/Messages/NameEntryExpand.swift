// WHEN MESSAGES ACTUALLY HONOURS AN `.expanded` REQUEST - round 47.
//
// Round 46 gave the name screens a drawer that opens itself
// (`MessagesRootView.expandForNameEntry`) and a keyboard that comes up with it
// (`NameFieldAutofocus`). Both were INERT on a real iPhone, and every test
// passed anyway, because the code was shaped correctly and the HOST was
// throwing the request away.
//
// WHAT WAS MEASURED. A probe build in the real Messages app on an iPhone 17
// (iOS 26.3), instrumented to issue exactly ONE `requestPresentationStyle
// (.expanded)` per cold open and to report whether the host ever answered.
// Eight cold opens, alternating between the two moments:
//
//   issued from SwiftUI `onAppear` (round 46's moment, ~0.17s in)   0/4 landed
//   issued in the host's compact-install `willTransition` (~0.47s)  4/4 landed
//
// The landing ones expanded 127ms after the request and the keyboard came up
// 9ms after that; the dropped ones produced no transition at all, so
// `NameFieldAutofocus` never passed its `hostIsExpanded()` gate either. One
// session's trace, times in seconds from the extension's launch:
//
//   0.029  willTransition -> compact,  didTransition -> compact   (see below)
//   0.080  our view is laid out at 874pt - FULL SCREEN, not the drawer
//   0.170  a name screen appears and asks to expand         <- DROPPED, silently
//   0.433  the surface snaps 874 -> 854 -> 298pt: the drawer
//   0.434  willTransition -> compact                        <- from here it sticks
//   0.560  willTransition/didTransition -> expanded
//   0.569  the field takes the keyboard
//   0.960  didTransition -> compact  (the install's own, arriving late)
//
// THE MOMENT, NOT A DELAY. The pair at 0.029 arrives synchronously with
// `willBecomeActive`, before Messages has installed our view in the drawer at
// all - it is the host stating the style it is about to present in, not a
// transition it has performed. The real installation is the one at 0.434, and
// a request made before it is discarded with no callback, no error and no
// second chance. So the fix is not "sleep long enough": it is "ask again the
// first time the host tells us it is moving the drawer". A delay would be a bet
// on a number that was never the mechanism - and the measurement says the
// mechanism is the event, because a request issued with ZERO delay from inside
// that callback lands every time.
//
// WHAT THIS TYPE IS. The decision, and only the decision - no UIKit, no
// Messages, no view. `MessagesViewController` feeds it the host events it
// already receives and issues a request whenever this says to. Pulled out
// because the alternative is untestable: the behaviour lives inside another
// process's sheet, and round 46's whole failure was a set of tests that could
// only see the shape of our own code. This one can at least be driven through
// the exact event sequence that was filmed - see NameEntryExpandTests, which
// says plainly what it can and cannot prove.

/// The drawer half of "a device that owes us a name lands able to type".
///
/// Feed it `wanted` when a name screen asks for the drawer and `transition`
/// for every presentation-style callback the host makes; it answers whether to
/// issue `requestPresentationStyle(.expanded)` right now.
public struct NameEntryExpand {

    /// Everything that can change the answer.
    public enum Event: Equatable {
        /// A name screen with an empty field appeared and wants the drawer.
        case wanted
        /// The host reported a presentation-style transition, beginning
        /// (`will`) or complete (`did`). Only the STYLE matters here; both
        /// phases are fed in because the filmed sequence proves the useful one
        /// (the compact install) announces itself with a `will` whose `did`
        /// does not arrive for another half second.
        case transition(toCompact: Bool)
    }

    /// A request has been made and the host has not yet answered with an
    /// expanded transition. Public so a trace can read it.
    public private(set) var pending = false

    /// Re-issues spent on the current request.
    public private(set) var retries = 0

    /// When the current request was made, on the caller's clock.
    private var wantedAt: Double = 0

    /// ONE spare. The measurement says the first compact transition after the
    /// request is the one that lands (4/4), so a second re-issue is headroom
    /// for a device that sequences its installation differently - not a poll.
    private let maxRetries = 2

    /// And a window, because a retry is only ever the tail of the SAME opening.
    /// Filmed, the landing retry arrived 264-370ms after the request; two
    /// seconds is more than five times the worst of those, and it is what stops
    /// a stale request from turning a collapse the human asked for, minutes
    /// later, into an expand nobody asked for. (The host's own
    /// `presentationStyle` is not a safe substitute: it was filmed answering
    /// "compact" for a third of a second AFTER the drawer had visibly expanded,
    /// which is why `pending` is cleared by the transition CALLBACK rather than
    /// by asking where the sheet is.)
    private let window = 2.0

    public init() {}

    /// Note an event; `true` means issue `.expanded` now. `now` is any
    /// monotonic seconds clock - the host passes `CACurrentMediaTime()`, and a
    /// test passes whatever it likes.
    public mutating func note(_ event: Event, now: Double) -> Bool {
        switch event {
        case .wanted:
            // Already asking. A second name screen appearing inside the same
            // opening (the lobby's join row after the setup card, say) does not
            // get its own budget.
            guard !pending else { return false }
            pending = true
            retries = 0
            wantedAt = now
            // Issue immediately, because when the drawer is ALREADY installed -
            // a name screen reached mid-session, which is every path except the
            // cold open - this is the request that works, and waiting for a
            // transition that is not coming would make the feature worse than
            // it is today.
            return true

        case .transition(toCompact: false):
            // The host is expanded, which is the whole ask. Stop, and in
            // particular stop before the late compact `didTransition` that
            // Messages emits AFTER the expand (0.960 in the trace above): with
            // this cleared, that one cannot turn into a re-request, and neither
            // can a human dragging the drawer shut a moment later.
            pending = false
            retries = 0
            return false

        case .transition(toCompact: true):
            guard pending else { return false }
            guard retries < maxRetries, now - wantedAt <= window else {
                // Spent, or stale. Leave the drawer alone rather than keep asking.
                pending = false
                return false
            }
            retries += 1
            return true
        }
    }
}
