// NameFieldAutofocus - the keyboard a name field raises for itself, and the two
// environment values it needs to know when.
//
// Used by all three name screens (NewGameSetup, LobbyView, NameGateView, in
// LobbyScreens.swift) and fed by MessagesRootView, which is the only view that
// can measure the drawer. It was inline in MessagesRootView.swift; on its own
// it is one job with one test (NameFieldKeyboardTests).

import SwiftUI

// MARK: - The keyboard a name field raises for itself
//
// Raise the keyboard on a name field, once, and only once the drawer is open
// and the host has finished opening it.
//
// Focus cannot just be set in `onAppear`. These screens are REACHED in compact,
// where a field cannot become first responder at all - that is the 2.1 dead end
// `expandForNameEntry` exists to cure - and the host's compact -> expanded
// transition is asynchronous, so a focus request made during it is dropped.
// Sleeping for a guessed duration would be a bet on how long Messages takes to
// open. Two live facts are used instead, and round 46b is the record of what
// each one alone gets wrong:
//
//  * the SURFACE's height, `collapseFraction` 1 in the compact band and 0 past
//    440pt - the same live measure the board's send hint trusts, and for the
//    same reason: the `style` prop goes stale across a grabber drag. It has to
//    come from the root (`surfaceHeight`); a field measuring itself reports
//    34pt forever;
//  * and the HOST's own answer for where its sheet is. Height alone is not
//    enough: for the first ~0.16s of a session our view is laid out at full
//    screen height before Messages installs it in the compact drawer, so the
//    height says "expanded" while the sheet is shut - the dead end exactly.
//
// Both were filmed on an iPhone 17 through the real Messages app; the timings
// quoted below are from that flight log.

/// The extension surface's LIVE height in points, published by the root's own
/// GeometryReader (`MessagesRootView.body`).
///
/// ROUND 46b: this exists because a view cannot measure a box it is not. The
/// first cut of `NameFieldAutofocus` put a GeometryReader in the name field's
/// `.background`, which reports the FIELD - filmed at 34pt, `collapseFraction`
/// 1, on every device and in every presentation style. Worse, 34pt never
/// changes, so the `onChange` watching it fired exactly once, at 0.24s, and
/// never again: the keyboard could not come up even in principle. Only the root
/// is given the drawer's height.
private struct SurfaceHeightKey: EnvironmentKey {
    static let defaultValue: CGFloat = 0
}

/// Does the HOST say its sheet is expanded, right now?
///
/// A closure, because it is a live read of the controller's own
/// `presentationStyle` at the moment of asking - NOT the `style` prop, which
/// is only as fresh as the last `present()` and so goes stale across a grabber
/// drag or an auto-transition (the note in `expandedContent` has the full
/// story). The default answers "no", which makes a host that never wired it up
/// - the harness, an older call site - simply not autofocus, rather than
/// autofocus at the wrong moment.
private struct HostIsExpandedKey: EnvironmentKey {
    static let defaultValue: () -> Bool = { false }
}

extension EnvironmentValues {
    var surfaceHeight: CGFloat {
        get { self[SurfaceHeightKey.self] }
        set { self[SurfaceHeightKey.self] = newValue }
    }
    var hostIsExpanded: () -> Bool {
        get { self[HostIsExpandedKey.self] }
        set { self[HostIsExpandedKey.self] = newValue }
    }
}

struct NameFieldAutofocus: ViewModifier {
    let active: Bool
    let focused: FocusState<Bool>.Binding
    /// The drawer's height and the host's own answer for where the sheet is.
    /// BOTH are needed, and the second one is not redundant: for the first
    /// ~0.16s of a session the extension's view is laid out at FULL SCREEN
    /// height (874pt on a 17, filmed) before Messages installs it in the
    /// compact drawer, so the height alone says "expanded" while the sheet is
    /// still shut - which is precisely the state in which a field cannot
    /// become first responder.
    @Environment(\.surfaceHeight) private var surfaceHeight
    @Environment(\.hostIsExpanded) private var hostIsExpanded
    /// Once only. Re-raising the keyboard every time the drawer is dragged back
    /// up would fight a human who deliberately dismissed it.
    @State private var fired = false

    func body(content: Content) -> some View {
        content
            // Both entry paths. `onAppear` covers a field that arrives on an
            // ALREADY expanded drawer (the JOIN row on a device whose human has
            // the sheet open), where no further resize is coming; `onChange`
            // covers the common one, where the field is on screen before
            // `expandForNameEntry`'s expand has finished.
            .onAppear { raise(surfaceHeight) }
            // The NEW height from the closure, never `self.surfaceHeight`: the
            // action captures the view value from the render BEFORE the change,
            // so reading the property here reports the previous height. Filmed:
            // the drawer resized 332 -> 748 and this read 332, which was the
            // last resize of the session, so the keyboard never came up.
            .onChange(of: surfaceHeight) { raise($0) }
    }

    private func raise(_ height: CGFloat) {
        guard active, !fired, hostIsExpanded(),
              MessageTableView.collapseFraction(height: height) == 0 else { return }
        fired = true
        focused.wrappedValue = true
    }
}
