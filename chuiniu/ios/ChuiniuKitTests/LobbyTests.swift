// LobbyTests - the lobby shows every phone the controls the kernel offers it:
// the creator Start (sunk alone, lit with two) and Leave (sunk alone), a
// phone not seated the name field with Join, a creator with no name yet the
// name field whose Join makes the lobby, and a started game no lobby at all.
// The lobby's layout keeps every control inside a compact drawer, each at
// least a tap target tall.
//
// Every state is a real one, played through the bridge by phones as
// BridgeKernelTests plays them (each phone its own defaults and identity).

import SwiftUI
import UIKit
import XCTest
@testable import ChuiniuKit

@MainActor
final class LobbyTests: XCTestCase {
    private var stores: [String: UserDefaults] = [:]
    private static let seed: [UInt8] = (0..<32).map { UInt8(($0 * 5 + 3) & 0xFF) }

    private func store(_ name: String) -> UserDefaults {
        let suite = "chuiniu.lobbytests.\(name).\(ObjectIdentifier(self).hashValue)"
        return stores[name] ?? {
            let d = UserDefaults(suiteName: suite)!
            d.removePersistentDomain(forName: suite)
            stores[name] = d
            return d
        }()
    }

    /// A phone named `name`; `named: false` is a phone that has never typed one.
    private func phone(_ name: String, named: Bool = true) -> BridgeKernel {
        let k = BridgeKernel(store: store(name), devPerson: false)
        k.me(Data(repeating: UInt8(name.utf8.first!), count: 16))
        if named { k.nickname(name) }
        k.sender(nil, isDM: false, iSent: false)
        return k
    }

    private func controls(_ k: BridgeKernel, name: String = "Zed") -> LobbyControls? {
        LobbyControls.of(k.table, nameAccepted: k.nameAccepted(name))
    }

    func testEachOfferShowsItsControls() throws {
        // the creator alone: Start sunk, Leave sunk, waiting
        var alex = phone("Alex")
        XCTAssertTrue(alex.newGame(dm: false, seed: Self.seed))
        XCTAssertEqual(controls(alex), LobbyControls(join: nil, start: .sunk, leave: .sunk, foot: .lobbyWaiting),
                       "alone: Start and Leave are there, sunk")
        XCTAssertEqual(alex.table.seats.map(\.name), ["Alex"])
        XCTAssertEqual(alex.table.seats.map(\.isMe), [true])
        XCTAssertEqual(alex.table.seats.map(\.dice), [CN_START_DICE], "the cup carries the dice a seat sits down with")
        XCTAssertEqual(alex.word(.leave), "Leave")
        XCTAssertEqual(alex.word(.lobbyYou), "(you)")
        let lobby = try XCTUnwrap(alex.stagedURL())

        // a phone not seated: the name field with Join, lit once the name is accepted
        var bo = phone("Bo")
        XCTAssertEqual(bo.adoptBubble(lobby), 0)
        XCTAssertEqual(controls(bo, name: "Bo"), LobbyControls(join: .glow, start: nil, leave: nil, foot: nil))
        XCTAssertEqual(controls(bo, name: ""), LobbyControls(join: .sunk, start: nil, leave: nil, foot: nil),
                       "no name: Join is sunk")
        XCTAssertTrue(bo.join(name: "Bo"))
        XCTAssertEqual(controls(bo), LobbyControls(join: nil, start: .sunk, leave: .quiet, foot: .lobbyWaiting),
                       "the newest joiner with room left may not start, but may leave")
        let joined = try XCTUnwrap(bo.stagedURL())

        // the creator with two: Start lit, Leave quiet
        alex = phone("Alex")
        XCTAssertEqual(alex.adoptBubble(joined), 0)
        XCTAssertEqual(controls(alex), LobbyControls(join: nil, start: .glow, leave: .quiet, foot: nil))

        // Bo gets up: the bubble says so, and Bo is offered Join again
        bo = phone("Bo")
        XCTAssertEqual(bo.adoptBubble(joined), 0)
        XCTAssertEqual(bo.leave(), "Bo left")
        XCTAssertEqual(controls(bo, name: "Bo"), LobbyControls(join: .glow, start: nil, leave: nil, foot: nil))
        let left = try XCTUnwrap(bo.stagedURL())
        alex = phone("Alex")
        XCTAssertEqual(alex.adoptBubble(left), 0)
        XCTAssertEqual(alex.table.seats.count, 1)
        XCTAssertEqual(controls(alex), LobbyControls(join: nil, start: .sunk, leave: .sunk, foot: .lobbyWaiting),
                       "alone again: sunk")
        XCTAssertNil(alex.leave(), "nobody else is seated: the kernel refuses")

        // a started game has no lobby
        bo = phone("Bo")
        XCTAssertEqual(bo.adoptBubble(left), 0)
        XCTAssertTrue(bo.join(name: "Bo"))
        let again = try XCTUnwrap(bo.stagedURL())
        alex = phone("Alex")
        XCTAssertEqual(alex.adoptBubble(again), 0)
        XCTAssertTrue(alex.start())
        XCTAssertNil(controls(alex), "started: no lobby")
    }

    func testACreatorWithNoNameIsAskedForOneAndItsJoinMakesTheLobby() throws {
        let cy = phone("Cy", named: false)
        XCTAssertFalse(cy.newGame(dm: true, seed: Self.seed), "the kernel seats a creator under a name")
        XCTAssertEqual(cy.table.phase, .lobby)
        XCTAssertTrue(cy.table.seats.isEmpty)
        XCTAssertEqual(controls(cy, name: "Cy"), LobbyControls(join: .glow, start: nil, leave: nil, foot: nil),
                       "the name field with Join")
        XCTAssertTrue(cy.join(name: "Cy"))
        XCTAssertEqual(cy.table.me, 0)
        XCTAssertEqual(cy.table.seats.map(\.name), ["Cy"])
        XCTAssertEqual(cy.table.bubbleCaption, "Cy wants a game of Chui Niu")
        XCTAssertNotNil(cy.stagedURL(), "the invitation is staged")
        XCTAssertEqual(controls(cy), LobbyControls(join: nil, start: .sunk, leave: .sunk, foot: .lobbyWaiting))
    }

    // MARK: the layout

    /// A group lobby of `seats`, resident on `viewer`'s phone.
    private func lobby(seats: Int, viewer: String) throws -> BridgeKernel {
        let names = ["Alex", "Bo", "Cy", "Di", "Ed", "Fay"]
        let alex = phone("Alex")
        XCTAssertTrue(alex.newGame(dm: false, seed: Self.seed))
        var link = try XCTUnwrap(alex.stagedURL())
        for s in 1..<seats {
            let p = phone(names[s])
            XCTAssertEqual(p.adoptBubble(link), 0)
            XCTAssertTrue(p.join(name: names[s]))
            link = try XCTUnwrap(p.stagedURL())
        }
        let v = phone(viewer)
        XCTAssertEqual(v.adoptBubble(link), 0)
        return v
    }

    /// The lobby laid out in a window of `size`: every part's frame.
    private func frames(_ kernel: BridgeKernel, _ size: CGSize) -> [String: CGRect] {
        var got: [String: CGRect] = [:]
        let host = ChuiniuHost(kernel: kernel)
        let hc = UIHostingController(rootView: LobbyScreen(host: host) { got = $0 })
        let window = UIWindow(frame: CGRect(origin: .zero, size: size))
        window.rootViewController = hc
        window.isHidden = false
        hc.view.frame = window.bounds
        let until = Date().addingTimeInterval(3)
        while got["screen"] == nil && Date() < until {
            hc.view.setNeedsLayout()
            hc.view.layoutIfNeeded()
            RunLoop.main.run(until: Date().addingTimeInterval(0.02))
        }
        RunLoop.main.run(until: Date().addingTimeInterval(0.1))
        window.isHidden = true
        return got
    }

    private func assertInside(_ f: [String: CGRect], _ keys: [String], _ size: CGSize, _ what: String) {
        let screen = CGRect(origin: .zero, size: size)
        for k in keys {
            guard let r = f[k] else { XCTFail("\(what): no \(k) laid out"); continue }
            XCTAssertTrue(screen.contains(r), "\(what): \(k) at \(r) leaves the \(size) drawer")
            XCTAssertGreaterThanOrEqual(r.height, LobbyScreen.controlHeight, "\(what): \(k) is \(r.height) tall")
            XCTAssertGreaterThanOrEqual(r.width, LobbyScreen.controlHeight, "\(what): \(k) is \(r.width) wide")
        }
    }

    func testEveryControlStaysInsideACompactAndAnExpandedDrawer() throws {
        let drawers = [CGSize(width: 390, height: 328), CGSize(width: 320, height: 328), CGSize(width: 390, height: 718)]
        for size in drawers {
            // five seated and the creator may start: Start and Leave
            let alex = try lobby(seats: 5, viewer: "Alex")
            assertInside(frames(alex, size), ["start", "leave"], size, "Alex at five, \(size)")
            // a sixth phone, not seated: the name field and Join
            let fay = try lobby(seats: 5, viewer: "Fay")
            assertInside(frames(fay, size), ["name", "join"], size, "Fay at five, \(size)")
        }
    }
}
