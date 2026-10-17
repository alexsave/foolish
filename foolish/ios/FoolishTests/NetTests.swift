// NetTests.swift - the online app's crossing into the kernel's client
// (sdk/swift/NetClient.swift): the generated writer hands C the intent, and the
// generated readers hand back what C wrote. What the client decides is tested
// in C (c_base/test/unit_netclient.c, unit_v1.c, unit_stream.c); this pins the
// Swift side of the bridge.

import XCTest
@testable import FoolishKit
import FoolishNet

@MainActor
final class NetTests: XCTestCase {
    override func setUp() async throws { NetClient.reset() }

    func testASignInIntentBecomesTheKernelsRequest() throws {
        XCTAssertNotEqual(NetClient.do(NC_DO_SIGN_IN, name: "ANA", password: "pw"), 0)
        let r = try XCTUnwrap(NetClient.nextRequest())
        XCTAssertNotEqual(r.id, 0)
        XCTAssertEqual(r.method, NC_POST)
        XCTAssertEqual(r.path, "/v1/auth/signin")
        XCTAssertEqual(r.auth, "")
        // netwire.h NW_SIGNIN: [NW_V1][kind][str8 name][str8 password]
        XCTAssertEqual(r.body, [NW_V1, NW_SIGNIN, 3, 0x41, 0x4e, 0x41, 2, 0x70, 0x77])
        XCTAssertNil(NetClient.nextRequest())
    }

    func testNothingNeedingAUserIsAcceptedSignedOut() {
        XCTAssertEqual(NetClient.do(NC_DO_CREATE), 0)
        XCTAssertEqual(NetClient.error?.why, ASK_WHY_AUTH)
        XCTAssertEqual(NetClient.session?.signedIn, 0)
        XCTAssertNil(NetClient.sessionBlob())
        XCTAssertEqual(NetClient.dueMs(), -1)
    }

    func testARefusedSignInFinishesItsTicketWithTheRefusal() throws {
        let t = NetClient.do(NC_DO_SIGN_IN, name: "ANA", password: "pw")
        let r = try XCTUnwrap(NetClient.nextRequest())
        // ask.h AskRefusal: an account refusal, code NW_ACCOUNT_E_BAD_LOGIN.
        let refusal = Data([1, UInt8(ASK_WHY_ACCOUNT), UInt8(NW_ACCOUNT_E_BAD_LOGIN), 0, 0, 0, 0, 0, 0, 0])
        let d = try XCTUnwrap(NetClient.onResponse(r.id, 400, refusal))
        XCTAssertEqual(d.ticket, t)
        XCTAssertEqual(d.why, ASK_WHY_ACCOUNT)
        XCTAssertEqual(d.code, NW_ACCOUNT_E_BAD_LOGIN)
        let e = NcErrorSnap(why: d.why, code: d.code, detail: d.detail, status: d.status)
        XCTAssertEqual(NetError.refused(e).errorDescription, "Invalid login credentials")
        XCTAssertTrue(NetClient.events().isEmpty)
    }

    func testTheStreamIsShutUntilOpened() {
        XCTAssertNil(NetClient.streamNext())
        XCTAssertEqual(NetClient.streamPath, "/v1/stream?proto=nw1")
    }
}
