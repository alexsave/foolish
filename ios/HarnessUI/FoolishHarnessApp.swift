// FoolishHarnessApp — a DEVELOPER TEST HOST for the iMessage game UI.
//
// ⚠️  THIS IS NOT HOW THE iMESSAGE APP EXTENSION SHIPS.  ⚠️
//
// The real product is `FoolishMessages`, an `MSMessagesAppViewController` that
// Apple's private Messages app instantiates and hands an `MSConversation`. That
// host is a signed Apple binary; you cannot add participants to it, and two
// simulators cannot iMessage each other — so there is no way to exercise a real
// 3-8 player group game in the simulator through the shipping path.
//
// This target rebuilds ONLY the host side, for testing. It renders the exact
// same `MessagesRootView` (now in FoolishKit) that the extension renders, but
// drives it from a fake in-memory transcript with a 2-8 "pretend participant"
// switcher. Because Foolish is fully turn-based — the entire game state travels
// as the message URL and seat identity is `SeatIdentity`'s pure logic over
// (payload, who-am-I) — this harness is faithful for the GROUP LOGIC and the UI.
//
// What it deliberately does NOT test (still needs the 2-user simulator + one
// real device pair): Messages presentation-style transitions, real insert-vs-send
// semantics, and receiving a message while the extension is closed. Those belong
// to Apple's host, which this cannot fake. See docs/IMESSAGE_MAC_RUNBOOK.md.

import SwiftUI
import FoolishKit

@main
struct FoolishHarnessApp: App {
    init() {
        // The rig POSES the iMessage extension, so it is the chain transport
        // (see MessagesViewController.viewDidLoad).
        AnimTransport.declare(.chain)
        // Each fake participant gets its OWN seat cache (see HarnessModel), so
        // seat identity resolves automatically — the DEBUG single-sim seat picker
        // would be wrong here.
        // The flag only exists in DEBUG builds; Release already resolves seats
        // automatically, which is the behavior the harness wants anyway.
        #if DEBUG
        MessageDebugFlags.pickSeatOnAdopt = false
        #endif
    }

    var body: some Scene {
        WindowGroup { HarnessRootView() }
    }
}
