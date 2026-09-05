// StoreFormatTests - the on-disk containers, now that they are bytes.
//
// `MessageGameStore` (App Group UserDefaults) and `ReplayStore` (a file in
// Application Support) both wrote JSON. They write PackedWriter bytes behind a
// format byte now, and the keys/filename were bumped rather than migrated, so
// what has to be pinned is the three things a container change can get wrong:
//
//   * a row survives the round trip, including the payload it WRAPS;
//   * a container it cannot read is NO ROWS - never half a list, never a crash;
//   * the old JSON a device already holds is one of those unreadable things,
//     which is the clean cut stated rather than assumed.
//
// The hand arrangement has its own all-52-cards crossing test next door, in
// MessageTurnControllerTests.testHandOrderStoreRoundTripClearAndCap: it is
// stored as dense ids, and a dense-id crossing is invisible until the last card.

import XCTest
@testable import FoolishKit

final class StoreFormatTests: XCTestCase {

    private func freshStore() -> MessageGameStore {
        MessageGameStore(defaults: UserDefaults(suiteName: "test.fmt.\(UUID().uuidString)")!)
    }

    // MARK: the high-water chain

    /// THE PAYLOAD CROSSES UNCHANGED. It used to be base64 inside JSON because
    /// JSON cannot hold bytes; the container can, so what goes in is what comes
    /// out - byte for byte, including a NUL and a byte no UTF-8 decoder would
    /// accept. Rule P compares whole chains, so a store that altered one would
    /// be answering about a different chain.
    ///
    /// MUTATION (MessageGameStore.persistLatest): write the chain through
    /// `String(decoding:as: UTF8.self)` and back - the high bytes come home as
    /// U+FFFD and this fails on the first assertion.
    func testAStoredChainIsTheChainThatWasStored() {
        let store = freshStore()
        let payload = Data([0x00, 0xFF, 0xFE, 0x7B, 0x22, 0x80, 0x0A, 0x5C] + (0...255).map(UInt8.init))
        store.setLatestChain(gameId: "7", chatKey: "a|b", payload: payload)
        XCTAssertEqual(store.latestChain(gameId: "7", chatKey: "a|b"), payload,
                       "every byte, including the ones no text encoding survives")
        XCTAssertNil(store.latestChain(gameId: "7", chatKey: "other"),
                     "a foreign chat is still a miss")

        // Two rows, so the walk across records is exercised rather than a
        // single-row happy path that any layout would pass.
        let second = Data([1, 2, 3])
        store.setLatestChain(gameId: "8", chatKey: "a|b", payload: second)
        XCTAssertEqual(store.latestChain(gameId: "7", chatKey: "a|b"), payload)
        XCTAssertEqual(store.latestChain(gameId: "8", chatKey: "a|b"), second)

        store.forgetLatestChain(gameId: "7")
        XCTAssertNil(store.latestChain(gameId: "7", chatKey: "a|b"))
        XCTAssertEqual(store.latestChain(gameId: "8", chatKey: "a|b"), second,
                       "…and the neighbour is untouched")
    }

    // MARK: the cut

    /// WHAT A DEVICE HOLDING YESTERDAY'S JSON SEES: nothing, cleanly. The keys
    /// were bumped, so the old blobs are not even looked at; this writes JSON
    /// under the NEW key, which is the harsher version of the same question -
    /// an unreadable container is no rows, and the store keeps working.
    func testAContainerThisBuildCannotReadIsNoRowsAndNotACrash() {
        let suite = "test.fmt.\(UUID().uuidString)"
        let defaults = UserDefaults(suiteName: suite)!
        let json = Data(#"{"7":{"chatKey":"a|b","seat":1}}"#.utf8)
        defaults.set(json, forKey: "fmsg.seats.v2")
        defaults.set(json, forKey: "fmsg.latest.v3")
        defaults.set(json, forKey: "fmsg.handorder.v2")

        let store = MessageGameStore(defaults: defaults)
        XCTAssertNil(store.seat(gameId: "7", chatKey: "a|b"), "unreadable is empty")
        XCTAssertNil(store.latestChain(gameId: "7", chatKey: "a|b"))
        XCTAssertTrue(store.handOrder(gameId: "7").isEmpty)

        // …and a write over the top establishes a container this build owns.
        store.setSeat(gameId: "7", chatKey: "a|b", seat: 3)
        XCTAssertEqual(store.seat(gameId: "7", chatKey: "a|b"), 3)
    }

    /// A TRUNCATED container is also no rows. Half a list read as a whole one
    /// would silently drop the tail on the next write, which is how a store
    /// loses data quietly.
    ///
    /// MUTATION (MessageGameStore.allSeats): `continue` instead of returning an
    /// empty map when a record does not decode, and the truncated blob comes
    /// back as one row.
    func testHalfAContainerIsNoContainer() {
        let suite = "test.fmt.\(UUID().uuidString)"
        let defaults = UserDefaults(suiteName: suite)!
        let store = MessageGameStore(defaults: defaults)
        store.setSeat(gameId: "7", chatKey: "a|b", seat: 1)
        store.setSeat(gameId: "8", chatKey: "a|b", seat: 2)
        let whole = try? XCTUnwrap(defaults.data(forKey: "fmsg.seats.v2"))
        let full = try? XCTUnwrap(whole)
        XCTAssertNotNil(full)
        guard let full else { return }

        for cut in 1..<full.count {
            defaults.set(full.prefix(cut), forKey: "fmsg.seats.v2")
            XCTAssertNil(MessageGameStore(defaults: defaults).seat(gameId: "8", chatKey: "a|b"),
                         "a container cut at \(cut) of \(full.count) bytes is not a container")
        }
        defaults.set(full, forKey: "fmsg.seats.v2")
        XCTAssertEqual(MessageGameStore(defaults: defaults).seat(gameId: "8", chatKey: "a|b"), 2,
                       "…and the whole one still reads")
    }

    // MARK: the saved-replay index

    /// The bookmark list round-trips through bytes, including the fields JSON
    /// was carrying for free: a nil result, a -1 fool, and a code with the
    /// characters a text container would have to escape.
    ///
    /// MUTATION (ReplayStore.encode): drop the `fool` byte and every later
    /// field shifts, so the decode fails and this comes back empty.
    func testTheSavedReplayIndexRoundTripsAsBytes() {
        let rows = [
            ReplayRecord(code: "1ABC-DEF", savedAt: Date(timeIntervalSince1970: 1_700_000_000.5),
                         players: 2, fool: 0, myResult: "win"),
            ReplayRecord(code: "", savedAt: Date(timeIntervalSince1970: 0),
                         players: 8, fool: -1, myResult: nil),
            ReplayRecord(code: "a\"b\\c,\u{1F0A1}", savedAt: Date(timeIntervalSince1970: 1.25),
                         players: 3, fool: 7, myResult: "lose"),
        ]
        XCTAssertEqual(ReplayStore.decode(ReplayStore.encode(rows)), rows)

        // An unreadable file is no rows - the same answer a missing one gives.
        XCTAssertEqual(ReplayStore.decode(nil), [])
        XCTAssertEqual(ReplayStore.decode(Data()), [])
        XCTAssertEqual(ReplayStore.decode(Data(#"[{"code":"1ABC"}]"#.utf8)), [],
                       "the old JSON index is one of the things this cannot read")
        let whole = ReplayStore.encode(rows)
        for cut in 0..<whole.count {
            XCTAssertEqual(ReplayStore.decode(whole.prefix(cut)), [],
                           "a file cut at \(cut) bytes is not a file")
        }
    }

    /// The file itself, through the public API: save de-dupes on the code and
    /// moves the row to the top, delete removes it, and both survive a reopen.
    func testTheReplayFileSurvivesAReopen() throws {
        let name = "replaytest-\(UUID().uuidString).bin"
        let store = ReplayStore(filename: name)
        defer {
            let dir = FileManager.default.urls(for: .applicationSupportDirectory,
                                               in: .userDomainMask)[0]
            try? FileManager.default.removeItem(at: dir.appendingPathComponent(name))
        }
        store.save(ReplayRecord(code: "A", savedAt: Date(timeIntervalSince1970: 10),
                                players: 2, fool: 0, myResult: nil))
        store.save(ReplayRecord(code: "B", savedAt: Date(timeIntervalSince1970: 20),
                                players: 3, fool: 1, myResult: "win"))
        store.save(ReplayRecord(code: "A", savedAt: Date(timeIntervalSince1970: 30),
                                players: 2, fool: 2, myResult: "lose"))

        let reopened = ReplayStore(filename: name)
        XCTAssertEqual(reopened.all().map(\.code), ["A", "B"], "newest first, de-duped on the code")
        XCTAssertEqual(reopened.all().first?.fool, 2, "the later save wins")

        reopened.delete(code: "A")
        XCTAssertEqual(ReplayStore(filename: name).all().map(\.code), ["B"])
    }
}
