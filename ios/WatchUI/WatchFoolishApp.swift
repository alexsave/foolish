// WatchFoolish - watchOS companion for Foolish (docs/WATCHOS_APP_PLAN.md).
//
// This is the W1 *scaffold* (§16 build plan): a buildable watchOS target that
// reserves the registered bundle id `cards.foolish.app.watchkitapp` so the app
// record and signing have a home on the wrist. It is deliberately minimal.
//
// NOT wired yet (the rest of W1, tracked in WATCHOS_APP_PLAN.md):
//   - the shared engine: FoolishKit has no watchOS destination and the C
//     xcframework has no watchOS slice, so this target does NOT depend on
//     FoolishKit. Wiring that (multi-platform FoolishKit + libfoolish watchOS
//     slices) is the next watch step.
//   - the real WatchUI (§4 one-glance table, two-tap turn) replaces this view.
//   - bundling: the host Foolish app does not embed this target yet, so iOS /
//     iMessage builds are untouched.
import SwiftUI

@main
struct WatchFoolishApp: App {
    var body: some Scene {
        WindowGroup {
            ContentView()
        }
    }
}

struct ContentView: View {
    var body: some View {
        Text("Foolish")
            .font(.headline)
    }
}
