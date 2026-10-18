// A /m/ link is the SENDER's bytes, and the extension decodes it the moment a
// bubble is opened. c_base's unfurl fuzzer found a 155-byte one whose replay
// body claims an atom count it never runs out of: before REPLAY_MAX_ATOMS was
// enforced in the kernel (replay.c), the decode never returned and the shipped
// extension hung on the tap. The kernel test pins the guard in C
// (test_a_replay_code_that_never_ends_is_refused); this one pins that the
// extension's own path - URL, payloadBytes, the surface router - reaches it,
// so the guard is in the xcframework this app links and the bubble opens on
// the damaged screen at once.
import XCTest
@testable import FoolishKit

@MainActor
final class HostileLinkTests: XCTestCase {

    /// foolish/server/fuzz/corpus/unfurl/repro-endless-replay, byte for byte: the '1'
    /// text-format version and then the base32 envelope.
    private let segment =
        "164BQAAXPZWVYSZ2FE0MAQYAAAAIAAEAAAAAAAAAAAAAPJTBXDSOQYNGC3ZeLGHGEMHRQH2T7CEZZAD43FF27HZCO3M7GFKNASAABAACGQQHILXUF22CYACCK2N7B2WIHQT6BY6DAADULZ36CMVMNO"

    func testTheEndlessReplayLinkOpensDamagedAtOnce() async throws {
        let url = try XCTUnwrap(URL(string: "https://foolish.cards/m/" + segment))
        let payload = try MessageEnvelope.payloadBytes(url: url)
        XCTAssertFalse(payload.isEmpty, "the link's base32 decodes")

        // A hung kernel call cannot be cancelled, so the bound is an expectation
        // the test stops waiting for, not a cancelled task.
        let done = expectation(description: "the router answers")
        var screen: SurfaceScreen?
        Task { @MainActor in
            screen = await MessageSurfaceRouter.resolve(payload: payload, startNewGame: false,
                                                        chatKey: "hostile-link-test")
            done.fulfill()
        }
        await fulfillment(of: [done], timeout: 10)
        XCTAssertEqual(screen, .damaged, "an atom count past REPLAY_MAX_ATOMS is refused, so the bubble is damaged")
    }
}
