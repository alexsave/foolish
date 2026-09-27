// TableDefaultTests.swift - the table surface a player gets before they choose,
// and the order the two choices are offered in.
//
// The owner's ask was one sentence with two halves in it: "prefer felt for all
// the screenshots, in fact it should probably be default and the option on the
// left in settings." Both halves are one-token facts - a `?? .felt` and a case
// order - which is exactly the kind of thing that gets quietly reverted by a
// later merge, or by somebody "tidying" an enum back into alphabetical order,
// with no test anywhere going red. So both are pinned here.
//
// The THIRD assertion in this file is the one that actually needed thought. A
// default is only safe to change because it is a FALLBACK: `setTable` writes
// the chosen surface's `rawValue` as a STRING, so an install that has already
// picked wool re-reads the string "wool" and never consults the default at all.
// That is a claim about the read path, not about the enum, so it is tested
// against the read path - with the real UserDefaults key, holding the real
// stored form a shipped build would have left there.

import XCTest
@testable import FoolishKit

@MainActor
final class TableDefaultTests: XCTestCase {

    /// THE key `FPrefs` persists under. Spelled out rather than read off the
    /// type: `tableKey` is private, and a test that shared the constant could
    /// not notice the key being renamed out from under every existing install.
    private static let key = "ios.table.surface"

    /// Whatever the test host had, restored exactly - including "nothing",
    /// which is a different state from "some string" and is the one a fresh
    /// install is in.
    private var saved: String??
    /// The singleton's live value, restored separately: `setTable` moves an
    /// in-memory `@Published` as well as the key, and putting the key back does
    /// not walk that back.
    private var savedLive: TableSurface?

    override func setUp() {
        super.setUp()
        saved = UserDefaults.standard.string(forKey: Self.key)
        savedLive = FPrefs.shared.table
    }

    override func tearDown() {
        if let savedLive { FPrefs.shared.setTable(savedLive) }
        // …and the KEY last, because `setTable` above just wrote it. Restoring
        // "no stored preference" is the case that needs this order.
        if let saved, let value = saved {
            UserDefaults.standard.set(value, forKey: Self.key)
        } else {
            UserDefaults.standard.removeObject(forKey: Self.key)
        }
        saved = nil
        savedLive = nil
        super.tearDown()
    }

    // MARK: 1 - a fresh install sits at felt

    /// No stored preference is the fresh-install state, and it resolves to felt.
    ///
    /// Through `storedTable`, not through `defaultSurface`: the constant being
    /// right is worth nothing if the read that every surface in the app actually
    /// goes through has its own `??` somewhere. (It had exactly that until this
    /// change - two copies of the fallback, one per reader.)
    func testAFreshInstallGetsFelt() {
        UserDefaults.standard.removeObject(forKey: Self.key)
        XCTAssertEqual(FPrefs.storedTable, .felt,
                       "a player who has never opened settings should sit at felt")
        XCTAssertEqual(FPrefs.defaultSurface, .felt)
    }

    /// A stored string no case answers to - a preference written by a build with
    /// a surface this one does not have - lands on the default rather than
    /// crashing or picking the first case by accident.
    func testAnUnreadableStoredSurfaceFallsBackToTheDefault() {
        UserDefaults.standard.set("velvet", forKey: Self.key)
        XCTAssertEqual(FPrefs.storedTable, FPrefs.defaultSurface)
    }

    // MARK: 2 - felt is the option on the left

    /// `MessageSettingsView` lays its swatches out with
    /// `ForEach(TableSurface.allCases)`, so the case order IS the row order and
    /// this is the whole of "felt on the left".
    func testFeltIsTheFirstOptionInThePicker() {
        XCTAssertEqual(TableSurface.allCases, [.felt, .wool],
                       "the settings row renders allCases in order - felt goes first")
        XCTAssertEqual(TableSurface.allCases.first, .felt)
    }

    /// The default is also the option the picker offers first, which is the
    /// owner's ask as one sentence: a fresh install opens settings and finds its
    /// own surface already check-marked on the left, not in second place.
    func testTheDefaultAndTheLeftmostOptionAreTheSameSurface() {
        XCTAssertEqual(TableSurface.allCases.first, FPrefs.defaultSurface)
    }

    // MARK: 3 - and an install that already chose keeps its choice

    /// The change is a DEFAULT change, so every surface that was already stored
    /// has to survive it untouched. Both of them, because "wool survives" and
    /// "felt survives" fail for different reasons: the first if the default ever
    /// starts overriding a stored value, the second if the read starts ignoring
    /// the key once the stored value equals the default.
    func testAStoredChoiceSurvivesTheDefaultChange() {
        for chosen in TableSurface.allCases {
            UserDefaults.standard.set(chosen.rawValue, forKey: Self.key)
            XCTAssertEqual(FPrefs.storedTable, chosen,
                           "an install that chose \(chosen) must still be on \(chosen)")
        }
    }

    /// …and the stored form is the raw STRING, not an ordinal. This is the
    /// assertion that makes reordering the cases safe: had the preference been
    /// persisted as a case INDEX, moving felt to the front would have silently
    /// switched every wool player to felt and every felt player to wool.
    func testThePreferenceIsPersistedAsAStringNotAnOrdinal() {
        FPrefs.shared.setTable(.wool)
        XCTAssertEqual(UserDefaults.standard.string(forKey: Self.key), "wool")
        FPrefs.shared.setTable(.felt)
        XCTAssertEqual(UserDefaults.standard.string(forKey: Self.key), "felt")

        // The exact bytes a shipped build wrote before felt moved to the front,
        // read back through today's enum. "wool" is still wool.
        UserDefaults.standard.set("wool", forKey: Self.key)
        XCTAssertEqual(TableSurface(rawValue: "wool"), .wool)
        XCTAssertEqual(FPrefs.storedTable, .wool)
    }
}
