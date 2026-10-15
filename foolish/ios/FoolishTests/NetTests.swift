// NetTests.swift - the online app's crossing into the kernel's client
// (sdk/swift/NetClient.swift): the generated readers hand back what C wrote.
// What the client decides is tested in C (c_base/test/unit_netclient.c,
// unit_v1.c, unit_stream.c); this pins the Swift side of the bridge.

import XCTest
@testable import FoolishKit
import FoolishNet

@MainActor
final class NetTests: XCTestCase {
    override func setUp() async throws { NetClient.reset() }

    func testSignInRequestIsTheKernels() throws {
        let r = try XCTUnwrap(NetClient.signIn("ANA", "pw"))
        XCTAssertEqual(r.method, NC_POST)
        XCTAssertEqual(r.path, "/v1/auth/signin")
        XCTAssertEqual(r.auth, "")
        XCTAssertEqual(r.isRefresh, 0)
        // netwire.h NW_SIGNIN: [NW_V1][kind][str8 name][str8 password]
        XCTAssertEqual(r.body, [NW_V1, NW_SIGNIN, 3, 0x41, 0x4e, 0x41, 2, 0x70, 0x77])
    }

    func testNothingNeedingAUserIsBuiltSignedOut() {
        XCTAssertNil(NetClient.create())
        XCTAssertNil(NetClient.game("ab12cd"))
        XCTAssertEqual(NetClient.session?.signedIn, 0)
        XCTAssertNil(NetClient.sessionBlob())
    }

    func testAnActionAnswerAndARefusal() throws {
        // table.h: [fmt][status][reject code][u32 version]
        let a = try XCTUnwrap(NetClient.action(200, Data([1, 1, 100, 42, 0, 0, 0])))
        XCTAssertEqual(a.status, TABLE_STATUS_REJECTED)
        XCTAssertEqual(a.rejectCode, 100)
        XCTAssertEqual(a.version, 42)
        // ask.h AskRefusal: an account refusal, code NW_ACCOUNT_E_BAD_LOGIN.
        let refusal = Data([1, UInt8(ASK_WHY_ACCOUNT), UInt8(NW_ACCOUNT_E_BAD_LOGIN), 0, 0, 0, 0, 0, 0, 0])
        XCTAssertEqual(NetClient.readSession(400, refusal), ASK_WHY_ACCOUNT)
        XCTAssertEqual(NetClient.error?.code, NW_ACCOUNT_E_BAD_LOGIN)
        XCTAssertEqual(NetError.refused(NetClient.error).errorDescription, "Invalid login credentials")
    }

    func testTheStreamIsShutUntilOpened() {
        XCTAssertNil(NetClient.streamNext())
        XCTAssertEqual(NetClient.streamDueMs(), -1)
        XCTAssertEqual(NetClient.stream()?.path, "/v1/stream?proto=nw1")
    }
}
