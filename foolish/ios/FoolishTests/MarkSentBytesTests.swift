// WHICH BYTES WENT OUT - round 43, and the equivalence that justified collapsing
// two rounds of fixes into one line.
//
// `markSent` decides which chain the human actually sent, from two sources that
// can each fail: the host's payload (which travels from `didStartSending`
// through a root-view rebuild and a SwiftUI `onChange` into a Task, and can
// arrive NIL or STALE) and `lastSealed`, the chain this controller sealed
// itself.
//
// It used to decide in two layers, written in two different rounds:
//
//     var sent = payload ?? (pending.isEmpty ? nil : lastSealed)   // 1.0(26), the nil
//     if let host = payload, let mine = lastSealed,
//        host != mine, !pending.isEmpty { sent = mine }            // 1.0(36), the stale
//
// The second silently reversed the first's stated preference without deleting
// the sentence that stated it, so the code said the host was preferred and did
// the opposite. One rule underneath: WITH MOVES STAGED, OUR OWN SEALED CHAIN IS
// THE BUBBLE; WITH NOTHING STAGED, ONLY THE HOST CAN SAY.
//
// WHY THIS FILE EXISTS. That collapse is a claim of equivalence on the SEND
// PATH, which is where 1.0(22), 1.0(24), 1.0(26), 1.0(27) and 1.0(36) all went
// wrong in turn - a mistake here strands the withheld settlement that only
// `markSent` releases, or leaves `pending` full so the next arrival red-retracts
// a move the thread already has. So the equivalence is EXERCISED over the whole
// input space rather than reasoned about: three booleans, eight cases, checked
// against the superseded expression written out longhand below.
//
// MUTATION-CHECKED: `staged ? (host ?? sealed) : host` (the preference the old
// prose claimed) fails; `sealed ?? host` unconditionally fails; `staged ? sealed
// : host` fails. Each was run.

import XCTest
@testable import FoolishKit

final class MarkSentBytesTests: XCTestCase {

    private let hostBytes = Data([1, 2, 3])
    private let sealedBytes = Data([9, 9])

    /// The two-layer logic this replaced, longhand. Deliberately a transcription
    /// of the deleted code and NOT a tidied version of it - a paraphrase would
    /// prove the new rule matches my reading of the old one, which is the thing
    /// in question.
    private func legacy(staged: Bool, host: Data?, sealed: Data?) -> Data? {
        var sent = host ?? (staged ? sealed : nil)
        if let h = host, let mine = sealed, h != mine, staged { sent = mine }
        return sent
    }

    /// All eight combinations, with the host and the sealed chain DIFFERENT -
    /// the case the two layers disagreed about most.
    func testTheOneRuleMatchesTheTwoLayersItReplaced() {
        for staged in [true, false] {
            for host in [hostBytes, nil] as [Data?] {
                for sealed in [sealedBytes, nil] as [Data?] {
                    XCTAssertEqual(
                        MessageTurnController.sentBytes(staged: staged, host: host, sealed: sealed),
                        legacy(staged: staged, host: host, sealed: sealed),
                        "staged=\(staged) host=\(host == nil ? "nil" : "set") "
                        + "sealed=\(sealed == nil ? "nil" : "set")")
                }
            }
        }
    }

    /// …and again where the two sources AGREE, which is the ordinary send and
    /// the case a naive equivalence check would pass on by accident.
    func testTheyAlsoAgreeWhenTheBytesAreTheSame() {
        for staged in [true, false] {
            XCTAssertEqual(
                MessageTurnController.sentBytes(staged: staged, host: hostBytes, sealed: hostBytes),
                legacy(staged: staged, host: hostBytes, sealed: hostBytes))
        }
    }

    // MARK: the rule itself, stated positively

    /// With moves staged there is exactly one bubble in the input field and this
    /// controller sealed it, so a send signal can only be that bubble going out
    /// - whatever the host handed over, absent or stale.
    func testStagedPrefersOurOwnSealedChain() {
        XCTAssertEqual(MessageTurnController.sentBytes(staged: true, host: hostBytes, sealed: sealedBytes),
                       sealedBytes, "a STALE host payload does not win over the chain we sealed")
        XCTAssertEqual(MessageTurnController.sentBytes(staged: true, host: nil, sealed: sealedBytes),
                       sealedBytes, "…nor does a missing one")
    }

    /// With nothing staged this controller made no bubble, so it has no claim -
    /// which is also the shape a genuinely foreign signal arrives in (a reload
    /// handed a chain this controller never made). The refusal downstream is
    /// what handles that, and it needs the host's bytes to recognise it.
    func testUnstagedTakesOnlyTheHost() {
        XCTAssertEqual(MessageTurnController.sentBytes(staged: false, host: hostBytes, sealed: sealedBytes),
                       hostBytes, "with nothing staged our own sealed chain proves nothing")
        XCTAssertNil(MessageTurnController.sentBytes(staged: false, host: nil, sealed: sealedBytes),
                     "a bytesless signal with nothing staged must stay nil and take the refusal")
    }

    /// The one case with no answer at all: nothing staged and nothing sealed.
    func testNothingStagedAndNothingSealedIsNil() {
        XCTAssertNil(MessageTurnController.sentBytes(staged: false, host: nil, sealed: nil))
        XCTAssertNil(MessageTurnController.sentBytes(staged: true, host: nil, sealed: nil))
    }
}
