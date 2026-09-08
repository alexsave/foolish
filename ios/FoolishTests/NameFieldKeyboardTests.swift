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

    // MARK: - Round 46: the keyboard comes up by itself, but only when owed

    /// The prefill rule, which is the ONE thing the drawer, the keyboard and
    /// all three fields now agree on. Real behaviour, not a source scan.
    func testPrefillIsEmptyOnlyWhenNoNameHasBeenChosen() {
        let store = MessageGameStore(defaults: UserDefaults(suiteName: "test.nick.\(UUID().uuidString)")!)

        // Untouched device: the neutral default is a placeholder, not a name.
        XCTAssertEqual(store.nicknamePrefill, "")
        XCTAssertTrue(store.needsNameEntry)

        // A chosen name prefills and asks for nothing.
        store.nickname = "Alex"
        XCTAssertEqual(store.nicknamePrefill, "Alex")
        XCTAssertFalse(store.needsNameEntry)

        // Whitespace is not a name.
        store.nickname = "   "
        XCTAssertEqual(store.nicknamePrefill, "")
        XCTAssertTrue(store.needsNameEntry)

        // The case the old per-view `== "Me"` tests got wrong: a device whose
        // STORED name is the placeholder used to report hasSetNickname == true
        // while every field still blanked it, so the drawer stayed compact over
        // a field that cannot be focused there - the 2.1 dead end, restored.
        store.nickname = "Me"
        XCTAssertEqual(store.nicknamePrefill, "")
        XCTAssertTrue(store.needsNameEntry,
                      "a stored placeholder must still count as owing a name")
        XCTAssertTrue(store.hasSetNickname,
                      "hasSetNickname is the weaker test - this is why needsNameEntry exists")
    }

    /// Every name field asks for the autofocus, and asks for it CONDITIONALLY.
    /// An unconditional one would re-raise the keyboard over a name the human
    /// already chose.
    func testEveryNameFieldAutofocusesOnlyWhenEmpty() throws {
        let src = code(try source())
        let mods = src.indices.filter { src[$0].contains(".modifier(NameFieldAutofocus(") }
        XCTAssertEqual(mods.count, 3,
                       "all three name fields raise their own keyboard, or none should")
        for i in mods {
            let pair = src[i] + (i + 1 < src.count ? src[i + 1] : "")
            XCTAssertTrue(pair.contains("active: name.isEmpty") || pair.contains("active: nickname.isEmpty"),
                          "autofocus at line \(i + 1) is unconditional: \(pair)")
        }
    }

    // MARK: - Round 46b: what the autofocus is allowed to MEASURE

    /// The autofocus must read the height published by the ROOT, and must not
    /// measure a box of its own.
    ///
    /// Filmed on a 17: the first cut wrapped the name field in
    /// `content.background(GeometryReader { ... })`, which reports the FIELD -
    /// 34pt, a collapse fraction of 1, in every presentation style. Worse, 34pt
    /// never changes, so the `onChange` watching it fired once at 0.24s and
    /// never again; the keyboard could not come up even in principle, and the
    /// round-46 tests all passed anyway because none of them asked WHICH height
    /// was being measured. This one does.
    func testAutofocusMeasuresTheSurfaceNotItself() throws {
        let src = code(try source())
        guard let i = src.firstIndex(where: { $0.contains("private struct NameFieldAutofocus") }),
              let end = src[i...].firstIndex(where: { $0.hasPrefix("}") && $0 != src[i] })
        else { return XCTFail("NameFieldAutofocus is gone - this test needs rewriting") }
        let body = src[i...end].joined(separator: "\n")
        XCTAssertTrue(body.contains("@Environment(\\.surfaceHeight)"),
                      "the autofocus no longer reads the root's published height")
        XCTAssertFalse(body.contains("GeometryReader"),
                       "a GeometryReader here measures the FIELD, not the drawer")
        // …and the root has to publish it, or the environment default (0) makes
        // the gate unreachable in the other direction.
        XCTAssertTrue(src.contains(where: { $0.contains(".environment(\\.surfaceHeight, geo.size.height)") }),
                      "nothing publishes surfaceHeight from the root GeometryReader")
    }

    /// A tall surface is NOT on its own proof that a field can take the
    /// keyboard. For the first ~0.16s of a session the extension's view is laid
    /// out at full screen height (874pt, filmed) before Messages installs it in
    /// the compact drawer, so the height alone reads "expanded" while the sheet
    /// is still shut - and a focus request made there is dropped, which is the
    /// exact dead end this whole feature exists to cure. The host's own live
    /// answer is the second half of the gate.
    func testAutofocusAlsoWaitsForTheHostToSayExpanded() throws {
        let src = code(try source())
        guard let i = src.firstIndex(where: { $0.contains("private func raise(_ height: CGFloat)") })
        else { return XCTFail("NameFieldAutofocus.raise is gone - this test needs rewriting") }
        let body = src[i...min(i + 4, src.count - 1)].joined(separator: "\n")
        XCTAssertTrue(body.contains("hostIsExpanded()"),
                      "the height alone can be a full-screen pre-layout reading")
        XCTAssertTrue(body.contains("collapseFraction"),
                      "the host's style alone does not say how tall the drawer is")
        XCTAssertTrue(body.contains("!fired"), "the autofocus is no longer once-only")
    }

    /// And the drawer is expanded on the same condition. If this guard is
    /// dropped, opening any conversation with a known name takes the screen
    /// over uninvited.
    func testExpandForNameEntryIsGatedOnOwingAName() throws {
        let src = code(try source())
        guard let i = src.firstIndex(where: { $0.contains("private func expandForNameEntry()") }) else {
            return XCTFail("expandForNameEntry is gone - this test needs rewriting")
        }
        let body = src[i...min(i + 4, src.count - 1)].joined(separator: "\n")
        XCTAssertTrue(body.contains("needsNameEntry"),
                      "expandForNameEntry no longer checks whether a name is owed")
        XCTAssertTrue(body.contains("guard"), "the check is not a guard, so it may not return early")
    }
}
