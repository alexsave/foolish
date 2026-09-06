// THE KEYBOARD OUR OWN FIELDS RAISE, and the hand-off that has to outlive them.
//
// Owner, from a player's screenshot: an expanded board with the iMessage
// keyboard still up over the bottom half of it, and no way to get rid of it -
// Messages hides its compose bar while a sheet is expanded, so there is no
// field to tap and nothing to swipe.
//
// The extension cannot lower a keyboard it did not raise (there is no
// UIApplication in an extension-API-only target, and MSMessagesAppViewController
// offers no control over the host's keyboard), so the ONE case it can fix is the
// one it causes: its own three name fields - NewGameSetup, LobbyView's join and
// NameGateView - each hand off to a closure that replaces the whole view, and a
// field removed while it is still first responder can leave the keyboard behind.
// Every one of them now resigns first and hands off a runloop turn later
// (`NewGameSetup.handOff` carries the full reasoning).
//
// WHY A SOURCE SCAN. There is nothing else available. `@FocusState` has no
// readable value from a test, SwiftUI publishes no first-responder state, and
// the failure is a race inside UIKit's responder chain on a real device inside
// another process's sheet - a snapshot cannot see it and a unit test cannot
// stage it. What CAN be pinned is the invariant the fix rests on: no name field
// without a focus binding, and no hand-off that skips `handOff`. That is the
// same guard the `.contentShape` fix took, and for the same reason.
import XCTest
@testable import FoolishKit

final class NameFieldKeyboardTests: XCTestCase {

    /// MessagesRootView.swift, the one file holding all three name fields.
    private func source() throws -> [String] {
        // #filePath is this file; the surface sits one directory over.
        let here = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
        let url = here.deletingLastPathComponent()
            .appendingPathComponent("FoolishKit/Messages/MessagesRootView.swift")
        return try String(contentsOf: url, encoding: .utf8).components(separatedBy: "\n")
    }

    private func code(_ lines: [String]) -> [String] {
        lines.filter { !$0.trimmingCharacters(in: .whitespaces).hasPrefix("//") }
    }

    /// Every `TextField` in the extension's surface binds `$nameFocused` - on
    /// its own line or the next one, which is how all three are written.
    func testEveryNameFieldIsFocusBound() throws {
        let src = code(try source())
        let fields = src.indices.filter { src[$0].contains("TextField(") }
        XCTAssertEqual(fields.count, 3,
                       "a name field was added or removed - it needs a focus binding too")
        for i in fields {
            let pair = src[i] + (i + 1 < src.count ? src[i + 1] : "")
            XCTAssertTrue(pair.contains(".focused($nameFocused)"),
                          "unbound TextField at line \(i + 1): \(src[i])")
        }
    }

    /// All three fields declare the pair, and all three own a `handOff`.
    func testEveryNameScreenOwnsTheHandOff() throws {
        let src = code(try source())
        XCTAssertEqual(src.filter { $0.contains("@FocusState private var nameFocused") }.count, 3)
        XCTAssertEqual(src.filter { $0.contains("private func handOff(") }.count, 3)
        // The order inside it is the whole fix: resign, then hop, then act.
        for i in src.indices where src[i].contains("private func handOff(") {
            let body = src[i...min(i + 5, src.count - 1)].joined(separator: "\n")
            XCTAssertTrue(body.contains("nameFocused = false"), "handOff at line \(i + 1) never resigns")
            XCTAssertTrue(body.contains("DispatchQueue.main.async"),
                          "handOff at line \(i + 1) acts in the same runloop turn as the resign")
        }
    }

    /// And nothing walks off one of these screens without going through it.
    func testNoNameScreenHandsOffDirectly() throws {
        let src = code(try source())
        for call in ["onStart(name)", "onJoin(name)", "onContinue(trimmed)"] {
            let sites = src.filter { $0.contains(call) }
            XCTAssertFalse(sites.isEmpty, "\(call) is gone - this test needs rewriting")
            for line in sites {
                XCTAssertTrue(line.contains("handOff {"),
                              "\(call) called without handOff, so the keyboard can outlive the field: \(line)")
            }
        }
    }
}
