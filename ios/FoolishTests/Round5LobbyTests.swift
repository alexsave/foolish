// Round5LobbyTests — the round-5 lobby/nickname decisions that reduce to pure
// functions, pinned so they cannot quietly regress (docs/APP_REVIEW_NOTES.md
// B1, M9). Style-matched to Round4Tests.swift: these are all synchronous, pure
// checks, no kernel and no async — MessageLobbyTests.swift already covers the
// end-to-end lobby/kernel path.
import XCTest
@testable import FoolishKit

final class Round5LobbyTests: XCTestCase {

    // MARK: - NicknameGate (round-5 B1, the UI half)

    func testOrdinaryShortNameIsOK() {
        guard case .ok(let name) = NicknameGate.check("Vera") else {
            return XCTFail("expected .ok for an ordinary short name")
        }
        XCTAssertEqual(name, "Vera")
    }

    /// THE bug this round fixes. "Владимир" is 8 CHARACTERS but 16 UTF-8
    /// bytes — under the OLD 12-byte wire cap (docs/APP_REVIEW_NOTES.md B1)
    /// this failed to seal and landed the user on "this game link is
    /// damaged". The wire cap is being raised to 64 bytes this same round
    /// (MSG_MAX_NAME, c/src/msg_wire.h) specifically so ordinary Cyrillic
    /// names like this one fit — NicknameGate's own `maxBytes` must agree
    /// with that new budget, not the old one, or this test would still fail.
    func testCyrillicNameWithinTheNewByteCapIsOK() {
        let name = "Владимир"
        XCTAssertEqual(name.count, 8, "sanity: 8 characters")
        XCTAssertEqual(name.utf8.count, 16, "sanity: 16 UTF-8 bytes — 2 bytes per Cyrillic letter")
        guard case .ok(let checked) = NicknameGate.check(name) else {
            return XCTFail("expected .ok — this is the exact name B1 broke")
        }
        XCTAssertEqual(checked, name)
    }

    func testExactlySixteenCharactersIsOK() {
        let name = String(repeating: "a", count: NicknameGate.maxChars)
        guard case .ok(let checked) = NicknameGate.check(name) else {
            return XCTFail("expected .ok at the character-cap boundary")
        }
        XCTAssertEqual(checked, name)
    }

    func testSeventeenCharactersIsTooLong() {
        let name = String(repeating: "a", count: NicknameGate.maxChars + 1)
        XCTAssertEqual(NicknameGate.check(name), .tooLong)
    }

    /// A name over the BYTE cap while still under the CHARACTER cap — the
    /// owner's rule is explicitly "if it goes over EITHER", so a short string
    /// of multi-byte grapheme clusters must be caught even though `.count`
    /// alone would wave it through. Family emoji (person-ZWJ-person-ZWJ-...)
    /// are each ONE Swift `Character` (one extended grapheme cluster) but
    /// several multi-byte Unicode scalars wide.
    func testShortEmojiStringOverTheByteCapIsTooLong() {
        let family = "\u{1F468}\u{200D}\u{1F469}\u{200D}\u{1F467}\u{200D}\u{1F466}" // 👨‍👩‍👧‍👦 — one grapheme cluster
        let name = String(repeating: family, count: 4)
        XCTAssertLessThanOrEqual(name.count, NicknameGate.maxChars,
                                 "sanity: still within the character cap — this must exercise the BYTE path")
        XCTAssertGreaterThan(name.utf8.count, NicknameGate.maxBytes,
                             "sanity: but blows the byte cap")
        XCTAssertEqual(NicknameGate.check(name), .tooLong)
    }

    func testBlankIsEmpty() {
        XCTAssertEqual(NicknameGate.check(""), .empty)
    }

    func testWhitespaceOnlyIsEmpty() {
        XCTAssertEqual(NicknameGate.check("   "), .empty)
    }

    func testLeadingAndTrailingSpacesAreTrimmed() {
        guard case .ok(let checked) = NicknameGate.check("  Vera  ") else {
            return XCTFail("expected .ok")
        }
        XCTAssertEqual(checked, "Vera", "the STORED/sealed name is the trimmed one, not the raw field text")
    }

    // MARK: - LobbyControls.offered (round-5 M9)

    /// M9's headline case: I sent the newest bubble (I just joined, or just
    /// re-staged the invite) and the lobby still has room — Start is withheld
    /// and I get `.waiting` instead, at every joined count short of full.
    func testLastSenderWithRoomDoesNotOfferStart() {
        for joined in 2...7 {
            XCTAssertEqual(LobbyControls.offered(mySeat: 0, joined: joined, capacity: 8, iSentTheInvite: true),
                          .waiting, "joined=\(joined)/8: I sent the newest bubble and there is still room")
        }
    }

    /// The full-lobby exemption: once the lobby is full, nobody else COULD
    /// join instead, so the last joiner may still start immediately.
    func testLastSenderInAFullLobbyMayStart() {
        XCTAssertEqual(LobbyControls.offered(mySeat: 0, joined: 8, capacity: 8, iSentTheInvite: true),
                      .start, "a full lobby always offers Start to its last joiner")
    }

    /// Note 2's "join and start", the case the owner specifically did not
    /// want M9 to break: a DM (capacity 2) is full the instant its second
    /// player joins, and that joiner IS the newest bubble's sender. A literal
    /// reading of M9 would strand them one pointless round-trip short of
    /// playing the game they just joined; the full-lobby exemption is what
    /// keeps this flow intact.
    func testDMJoinerFillingTheLastSeatMayStartImmediately() {
        XCTAssertEqual(LobbyControls.offered(mySeat: 1, joined: 2, capacity: 2, iSentTheInvite: true),
                      .start, "the DM's second-and-last joiner may start right away")
    }

    /// Authorship only matters once 2+ have joined AND there is room — a
    /// player who is NOT the last sender may always start once 2+ have
    /// joined, exactly as before M9.
    func testNonLastSenderMayStartOnceTwoHaveJoined() {
        for joined in 2...8 {
            XCTAssertEqual(LobbyControls.offered(mySeat: 0, joined: joined, capacity: 8, iSentTheInvite: false),
                          .start, "joined=\(joined)/8: the newest bubble is someone else's")
        }
    }

    /// The join/invite/waiting/full cases M9 does not touch: unaffected.
    func testUnaffectedCasesStillHold() {
        XCTAssertEqual(LobbyControls.offered(mySeat: nil, joined: 1, capacity: 8), .join)
        XCTAssertEqual(LobbyControls.offered(mySeat: nil, joined: 8, capacity: 8), .full)
        XCTAssertEqual(LobbyControls.offered(mySeat: 0, joined: 1, capacity: 8), .invite,
                      "joined but alone, and the newest bubble is not mine")
        XCTAssertEqual(LobbyControls.offered(mySeat: 0, joined: 1, capacity: 8, iSentTheInvite: true), .waiting,
                      "joined but alone, and the newest bubble IS mine (round-4 note 1, unchanged by M9)")
    }

    /// Full enumeration of the round-5 gate over every (joined, capacity,
    /// authorship) a joined player can be in: the newest sender is NEVER
    /// offered `.start` while the lobby has room, and IS offered `.start` the
    /// instant the lobby is full — the two halves of M9's rule, holding at
    /// every capacity and every joined count, not just the specific cases
    /// spelled out above.
    func testExhaustiveGateHoldsTheRound5RuleAtEveryCapacityAndCount() {
        for capacity in 2...8 {
            for joined in 0...capacity {
                for mine in [false, true] {
                    let a = LobbyControls.offered(mySeat: 0, joined: joined, capacity: capacity,
                                                  iSentTheInvite: mine)
                    if joined >= 2 {
                        if mine && joined < capacity {
                            XCTAssertEqual(a, .waiting,
                                          "cap=\(capacity) joined=\(joined): last sender, room left")
                        } else {
                            XCTAssertEqual(a, .start,
                                          "cap=\(capacity) joined=\(joined) mine=\(mine): "
                                          + "either not the last sender, or the lobby is full")
                        }
                    } else {
                        XCTAssertEqual(a, mine ? .waiting : .invite,
                                      "cap=\(capacity) joined=\(joined) mine=\(mine): unchanged by M9")
                    }
                }
            }
        }
    }

    /// A non-joined player's answer (join/full) never depends on authorship —
    /// `iSentTheInvite` is meaningless when I have no seat, and must not leak
    /// into that branch.
    func testNonJoinedPlayerIsUnaffectedByAuthorship() {
        for capacity in 2...8 {
            for joined in 0...capacity {
                for mine in [false, true] {
                    let a = LobbyControls.offered(mySeat: nil, joined: joined, capacity: capacity,
                                                  iSentTheInvite: mine)
                    XCTAssertEqual(a, joined < capacity ? .join : .full,
                                  "cap=\(capacity) joined=\(joined) mine=\(mine)")
                }
            }
        }
    }
}
