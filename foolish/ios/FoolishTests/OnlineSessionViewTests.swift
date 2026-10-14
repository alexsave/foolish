import SwiftUI
import XCTest
@testable import Foolish
@testable import FoolishKit
@testable import FoolishNet

/// Ready takes an online game from its lobby to its table.
///
/// Found playing online against a local backend: after Ready the lobby stayed
/// up with its "dealing" spinner for ever. The game had adopted the dealt board
/// (lldb showed it), and LobbyView, which observes the game, drew the spinner -
/// but the lobby/table switch lived in RootView, which observed only the
/// coordinator, so it never ran again.
///
/// This hosts the real session view in a real window, hands its game the two
/// rows the feed would (the lobby, then the dealt game), and compares what is
/// drawn with two references drawn the same way: the view of a game that is
/// still a lobby, and the view of a game that was dealt before it was shown.
/// SwiftUI exposes no accessibility tree to a unit test, so the screen is read
/// as pixels; comparing against references rather than thresholds keeps it
/// independent of the theme and the layout.
@MainActor
final class OnlineSessionViewTests: XCTestCase {

    private static let size = CGSize(width: 402, height: 874)

    private func seed(_ salt: Int) -> Data {
        Data((0..<32).map { UInt8((($0 &* 31) &+ salt &* 7 &+ 1) & 0xFF) })
    }

    /// One row for seat 0 of a dealt two-seat game, with the given status.
    private func row(_ engine: EngineC, blob: Data, status: GameStatus, version: Int) async throws -> AdoptedEnvelope {
        let t = PackedEnvelope.trailer(id: "sess01", name: "n", status: status.rawValue,
                                       names: ["ME", "%Vienna 1"], ids: ["me", "bot-1"],
                                       isAI: [false, true])
        let buf = PackedEnvelope.envelope(seat: 0, version: version, island: nil, viewBlob: blob, trailer: t)
        let decoded = await PackedGame.decode(buf, engine: engine)
        return try XCTUnwrap(decoded, "the \(status) row did not decode")
    }

    private func show(_ game: OnlineGame) throws -> UIWindow {
        let scene = try XCTUnwrap(UIApplication.shared.connectedScenes
            .compactMap { $0 as? UIWindowScene }.first,
            "no window scene - this test needs the app test host")
        let window = UIWindow(windowScene: scene)
        window.frame = CGRect(origin: .zero, size: Self.size)
        window.rootViewController = UIHostingController(rootView: OnlineSessionView(game: game, onHome: {}))
        window.makeKeyAndVisible()
        settle()
        return window
    }

    private func settle() { RunLoop.current.run(until: Date().addingTimeInterval(1.0)) }

    /// The window as a coarse grey grid (one cell per 6x6pt), 0...255.
    private static func grid(_ window: UIWindow) -> [Double] {
        let (w, h) = (Int(size.width) / 6, Int(size.height) / 6)
        let format = UIGraphicsImageRendererFormat()
        format.scale = 1
        let image = UIGraphicsImageRenderer(size: CGSize(width: w, height: h), format: format).image { _ in
            window.drawHierarchy(in: CGRect(x: 0, y: 0, width: w, height: h), afterScreenUpdates: true)
        }
        guard let cg = image.cgImage else { return [] }
        var px = [UInt8](repeating: 0, count: w * h * 4)
        guard let ctx = CGContext(data: &px, width: w, height: h, bitsPerComponent: 8,
                                  bytesPerRow: w * 4, space: CGColorSpaceCreateDeviceRGB(),
                                  bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)
        else { return [] }
        ctx.draw(cg, in: CGRect(x: 0, y: 0, width: w, height: h))
        return (0..<(w * h)).map { i in (Double(px[4 * i]) + Double(px[4 * i + 1]) + Double(px[4 * i + 2])) / 3 }
    }

    private static func distance(_ a: [Double], _ b: [Double]) -> Double {
        guard a.count == b.count, !a.isEmpty else { return .infinity }
        return zip(a, b).map { abs($0 - $1) }.reduce(0, +) / Double(a.count)
    }

    /// MUTANT: OnlineSessionView's `@ObservedObject var game` made a plain `let`
    /// (what RootView did before the fix).
    func testReadyTakesTheLobbyToTheTable() async throws {
        let engine = EngineC()
        try await engine.newGame(seed: seed(11), players: 2)
        let blob = try await PackedEnvelope.viewBlob(engine, seat: 0)
        let lobbyRow = try await row(engine, blob: blob, status: .waiting, version: 3)
        let dealtRow = try await row(engine, blob: blob, status: .playing, version: 4)

        // The references: a lobby, and a game that was dealt before it was shown.
        let lobbyRef = try show(OnlineGame(userId: UUID(), gameId: "sess01", initial: lobbyRow))
        let lobby = Self.grid(lobbyRef)
        lobbyRef.isHidden = true
        let tableRef = try show(OnlineGame(userId: UUID(), gameId: "sess01", initial: dealtRow))
        let table = Self.grid(tableRef)
        tableRef.isHidden = true
        XCTAssertGreaterThan(Self.distance(lobby, table), 5,
                             "premise: a lobby and a table draw alike, so this test cannot tell them apart")

        // The game under test: shown as a lobby, then dealt while on screen.
        let game = OnlineGame(userId: UUID(), gameId: "sess01", initial: lobbyRow)
        XCTAssertTrue(game.isWaiting, "premise: the first row is a lobby")
        let window = try show(game)
        defer { window.isHidden = true }
        XCTAssertLessThan(Self.distance(Self.grid(window), lobby), 1,
                          "premise: the lobby did not draw as the lobby reference")

        game.apply(dealtRow)                    // Ready -> the server deals
        XCTAssertFalse(game.isWaiting, "premise: the second row is a dealt game")
        settle()

        let now = Self.grid(window)
        let (toTable, toLobby) = (Self.distance(now, table), Self.distance(now, lobby))
        XCTAssertLessThan(toTable, toLobby,
                          "the game was dealt and the screen still shows the lobby "
                          + "(distance to the table \(toTable), to the lobby \(toLobby))")
        XCTAssertLessThan(toTable, 1, "the dealt game does not draw as the table reference")
    }
}
