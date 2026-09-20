// The SWIFT half of the gate.
//
// Every rule is asserted in C (`make -C c tests`, 2282 assertions, plus the
// bridge's 74). What is left for these is the part C cannot see: that the screen
// asks the kernel instead of remembering, and that what the model hands SwiftUI
// is what the masked view said and nothing more.
//
// So there is no test here that a villager cannot read the wolf line - that test
// lives in C, where it can diff two whole masked blobs. What is here is that the
// MODEL does not go around it.
import XCTest
@testable import WerewolfKit

@MainActor
final class NightModelTests: XCTestCase {

    /// A fixed seed, so a test that names a seat names the same seat forever.
    private func deal(_ players: Int, seed v: UInt8 = 7) throws {
        var s = Data(count: 32)
        for i in 0..<32 { s[i] = UInt8(truncatingIfNeeded: Int(v) * 31 + i * 7 + 1) }
        try Kernel.shared.newGame(seed: s, players: players)
        for seat in 0..<players {
            try Kernel.shared.setRoster(seat: seat, name: "P\(seat + 1)")
        }
    }

    private func seat(with role: Role, players: Int) -> Int {
        for s in 0..<players where Kernel.shared.role(s, of: s) == role { return s }
        return -1
    }

    // ------------------------------------------------------------ the floor ---

    func testTheFloorHoldsSendForTenSeconds() throws {
        try deal(7)
        var now: UInt16 = 1000
        let m = NightModel(mySeat: 0, clock: { now })
        XCTAssertEqual(m.floorRemaining, 10)
        m.pick(1)
        XCTAssertNil(m.send(gameId: 1, parent: nil), "send is refused while the floor stands")
        XCTAssertEqual(m.refusal, "Take a moment.")
        now = 1009
        XCTAssertNil(m.send(gameId: 1, parent: nil), "and one second early is still early")
        now = 1010
        XCTAssertNotNil(m.send(gameId: 1, parent: nil), "and allowed on the tenth second")
    }

    func testTheFloorIsTheKernelsAnswerAndNotTheCountdown() throws {
        try deal(7)
        var now: UInt16 = 2000
        let m = NightModel(mySeat: 0, clock: { now })
        m.pick(1)
        // The published countdown is for the label. `send` asks the kernel again,
        // so a stale countdown cannot buy an early send - which is what a timer
        // that fired once and was never corrected would do.
        XCTAssertEqual(m.floorRemaining, 10)
        now = 2010
        XCTAssertEqual(m.floorRemaining, 10, "the label has not ticked yet")
        XCTAssertNotNil(m.send(gameId: 1, parent: nil), "but the kernel knows the floor is spent")
    }

    // ------------------------------------------------------------ the tiles ---

    func testEverySeatsPromptIsTheSameShape() throws {
        try deal(8)
        // Three different sentences, one for each role - and each is only ever
        // rendered on its own device. What matters is that they are the same
        // LENGTH of interaction: a question about one other seat.
        let wolf = seat(with: .wolf, players: 8)
        let seer = seat(with: .seer, players: 8)
        let vill = seat(with: .villager, players: 8)
        XCTAssertGreaterThanOrEqual(wolf, 0)
        for s in [wolf, seer, vill] {
            let m = NightModel(mySeat: s, clock: { 0 })
            XCTAssertEqual(m.choosable.count, 7, "everybody picks from the same seven")
            XCTAssertFalse(m.prompt.isEmpty)
        }
    }

    func testOnlyAWolfIsOfferedTheChannel() throws {
        try deal(8)
        let wolf = seat(with: .wolf, players: 8)
        let vill = seat(with: .villager, players: 8)
        XCTAssertTrue(NightModel(mySeat: wolf, clock: { 0 }).amWolf)
        XCTAssertFalse(NightModel(mySeat: vill, clock: { 0 }).amWolf)
        // And the decider is wolf knowledge, so a villager's model has none.
        XCTAssertNil(NightModel(mySeat: vill, clock: { 0 }).decider)
        XCTAssertNotNil(NightModel(mySeat: wolf, clock: { 0 }).decider)
    }

    func testAVillagersModelCarriesNoChannelRows() throws {
        try deal(8)
        let wolf = seat(with: .wolf, players: 8)
        let vill = seat(with: .villager, players: 8)
        try Kernel.shared.nightAct(seat: wolf, target: vill, line: "him")
        XCTAssertEqual(NightModel(mySeat: wolf, clock: { 0 }).channel.count, 1)
        XCTAssertEqual(NightModel(mySeat: vill, clock: { 0 }).channel.count, 0)
        XCTAssertEqual(NightModel(mySeat: -1, clock: { 0 }).channel.count, 0, "nor a spectator's")
    }

    func testASpectatorIsNotOfferedASend() throws {
        try deal(7)
        var now: UInt16 = 1000
        let m = NightModel(mySeat: -1, clock: { now })
        now = 1010
        m.pick(1)
        XCTAssertNil(m.picked, "an unseated device cannot pick")
        XCTAssertNil(m.send(gameId: 1, parent: nil), "nor send")
    }

    // ------------------------------------------------------- one record only ---

    func testOneTapIsOneRecordAndOneBubble() throws {
        try deal(7)
        let wolf = seat(with: .wolf, players: 7)
        // The clock has to MOVE, or the floor never spends and the send this test
        // is about is refused for the right reason at the wrong moment.
        var now: UInt16 = 1000
        let m = NightModel(mySeat: wolf, clock: { now })
        now = 1010
        m.line = "take the loud one"
        m.pick(wolf == 0 ? 1 : 0)
        let before = Kernel.shared.turn(wolf)
        let payload = m.send(gameId: 0xABC, parent: nil)
        XCTAssertNotNil(payload, "the tap sealed a bubble")
        XCTAssertEqual(Kernel.shared.turn(wolf), before + 1, "and exactly one record")
        XCTAssertTrue(m.staged)
        // A second tap must not produce a second bubble. One bubble per player per
        // night is the whole design, and the model is the last place that could
        // break it by accident.
        XCTAssertNil(m.send(gameId: 0xABC, parent: nil), "a second tap sends nothing")
        XCTAssertEqual(Kernel.shared.turn(wolf), before + 1, "and adds no record")
    }

    func testTheLineRidesInsideTheRecordRatherThanBesideIt() throws {
        try deal(7)
        let wolf = seat(with: .wolf, players: 7)
        let other = wolf == 0 ? 1 : 0
        var now: UInt16 = 1000
        let m = NightModel(mySeat: wolf, clock: { now })
        now = 1010
        m.line = "the quiet one"
        m.pick(other)
        let payload = m.send(gameId: 1, parent: nil)
        XCTAssertNotNil(payload)
        // One bubble, and the wolves can read the line out of the game it built.
        XCTAssertEqual(Kernel.shared.chatCount(wolf), 1)
        XCTAssertEqual(Kernel.shared.chatLine(wolf, 0), "the quiet one")
        XCTAssertEqual(Kernel.shared.ownTarget(wolf), other)
    }

    func testAVillagerCannotSmuggleALine() throws {
        try deal(8)
        let vill = seat(with: .villager, players: 8)
        var now: UInt16 = 1000
        let m = NightModel(mySeat: vill, clock: { now })
        now = 1010
        // The screen never shows a villager the field, so this is defence in
        // depth - but a refusal rather than a silent drop is what stops a client
        // believing it sent something and then sending a second bubble.
        m.line = "let me in"
        m.pick(vill == 0 ? 1 : 0)
        XCTAssertNil(m.send(gameId: 1, parent: nil), "refused")
        XCTAssertNotNil(m.refusal)
        XCTAssertEqual(Kernel.shared.turn(vill), 0, "and nothing was recorded")
    }

    // ------------------------------------------------------------- the skip ---

    func testTheCarryIsOfferedOnlyAfterAMinuteAndOnlyWithSomebodyLate() throws {
        try deal(7)
        var now: UInt16 = 5000
        let m = NightModel(mySeat: 3, clock: { now })
        XCTAssertFalse(m.carryOffered(lastSealAt: 5000), "not at once")
        now = 5059
        XCTAssertFalse(m.carryOffered(lastSealAt: 5000), "not a second early")
        now = 5060
        XCTAssertTrue(m.carryOffered(lastSealAt: 5000), "on the minute, with six seats late")
    }

    func testACarriedSendProducesTheLongerChain() throws {
        try deal(7)
        // Seats 0 and 1 send. Seat 3 then carries, which takes seat 2's pass with
        // it - so seat 3's chain is two records longer than seat 2's own would be.
        try Kernel.shared.nightAct(seat: 0, target: 1)
        try Kernel.shared.nightAct(seat: 1, target: 0)
        let before = Kernel.shared.turn(3)
        var now: UInt16 = 9000
        let m = NightModel(mySeat: 3, clock: { now })
        now = 9010
        m.pick(0)
        XCTAssertNotNil(m.send(gameId: 1, parent: nil, carry: true))
        XCTAssertEqual(Kernel.shared.turn(3), before + 2, "one carried pass plus my own move")
        XCTAssertEqual(Kernel.shared.sent(3, 2), .carried, "and seat three was passed for")
    }

    // ------------------------------------------------------------ the board ---

    func testWaitingOnIsEverybodyWhichIsWhyItIsSafeToShow() throws {
        try deal(6)
        let m = NightModel(mySeat: 0, clock: { 1000 })
        XCTAssertEqual(m.waitingOn.count, 6)
        try Kernel.shared.nightAct(seat: 1, target: 0)
        XCTAssertEqual(m.waitingOn.count, 5)
        XCTAssertFalse(m.waitingOn.contains(1))
        // The row for a seat that sent says only that it sent. Same value for a
        // wolf and a villager, which is asserted properly in C.
        XCTAssertEqual(m.sent(1), .yes)
    }

    func testTheDeadAreDimmedAndTheirRoleIsPublic() throws {
        try deal(7)
        let wolf = seat(with: .wolf, players: 7)
        var mark = -1
        for s in 0..<7 where s != wolf && Kernel.shared.role(s, of: s) == .villager { mark = s; break }
        XCTAssertGreaterThanOrEqual(mark, 0)
        for s in 0..<7 {
            let t = (s == mark) ? wolf : mark
            try Kernel.shared.nightAct(seat: s, target: t)
        }
        XCTAssertEqual(Kernel.shared.phase(0), .day)
        let m = NightModel(mySeat: 0, clock: { 0 })
        XCTAssertFalse(m.isAlive(mark))
        XCTAssertEqual(m.role(of: mark), .villager, "a dead seat's role is public")
    }
}

// The LOBBY's Swift half. The rules are asserted in C; what is left here is that
// the screen asks for them and that the create path cannot deal.
@MainActor
final class LobbyTests: XCTestCase {

    private func seed(_ v: UInt8) -> Data {
        var s = Data(count: 32)
        for i in 0..<32 { s[i] = UInt8(truncatingIfNeeded: Int(v) * 31 + i * 7 + 1) }
        return s
    }

    func testCreatingDealsNobody() throws {
        let k = Kernel.shared
        try k.createLobby(seed: seed(3), chatIsDM: false, myName: "Alex")
        XCTAssertEqual(k.phase(-1), .lobby)
        XCTAssertEqual(k.lobbyJoined, 1, "only the creator is seated")
        XCTAssertEqual(k.playerCount(-1), 0, "and there is no table")
        // The whole point: there is nothing to look at, so there is nothing to
        // re-create for.
        for s in 0..<k.maxPlayers {
            XCTAssertEqual(k.role(s, of: s), .unknown, "seat \(s) has no role")
        }
        XCTAssertEqual(k.myRole(0), .unknown, "not even the creator's own")
    }

    func testTheLobbyAsksTheKernelForItsOneControl() throws {
        let k = Kernel.shared
        try k.createLobby(seed: seed(4), chatIsDM: false, myName: "Alex")
        XCTAssertEqual(k.lobbyOffered(mySeat: 0, iSentTheNewest: true), .waiting,
                       "the newest sender is not asked to post the same thing twice")
        XCTAssertEqual(k.lobbyOffered(mySeat: 0, iSentTheNewest: false), .invite)
        XCTAssertEqual(k.lobbyOffered(mySeat: -1, iSentTheNewest: false), .join)
        XCTAssertEqual(k.lobbyNeeds, k.minPlayers - 1)
        XCTAssertFalse(k.lobbyCanExit(mySeat: 0), "the creator alone has nothing to leave")

        for n in ["Sveta", "Kim", "Lee", "Ana"] { try k.joinLobby(myName: n) }
        XCTAssertEqual(k.lobbyJoined, 5)
        XCTAssertEqual(k.lobbyNeeds, 0)
        XCTAssertEqual(k.lobbyOffered(mySeat: 0, iSentTheNewest: false), .start)
        XCTAssertEqual(k.lobbyOffered(mySeat: 0, iSentTheNewest: true), .waiting,
                       "and still stands aside while there is room")
        XCTAssertEqual(k.myRole(0), .unknown, "and STILL nobody has a role")
    }

    func testAOneToOneChatSaysItCannotBePlayed() throws {
        let k = Kernel.shared
        try k.createLobby(seed: seed(5), chatIsDM: true, myName: "Alex")
        XCTAssertEqual(k.lobbyCapacity(chatIsDM: true), 2)
        try k.joinLobby(myName: "Sveta")
        // Said once, rather than counted toward a Start that can never arrive.
        XCTAssertEqual(k.lobbyOffered(mySeat: 0, iSentTheNewest: false), .tooFew)
        XCTAssertThrowsError(try k.joinLobby(myName: "Kim"), "and nobody else fits")
    }

    func testStartDealsAndOnlyThen() throws {
        let k = Kernel.shared
        try k.createLobby(seed: seed(6), chatIsDM: false, myName: "Alex")
        for n in ["Sveta", "Kim", "Lee", "Ana", "Bo"] { try k.joinLobby(myName: n) }
        let lobby = try k.seal(gameId: 0x5EED, sentAt: 0, parent: nil)
        XCTAssertEqual(k.myRole(0), .unknown, "still nothing")

        // The resident kernel is polluted between the lobby and Start, which is its
        // ordinary state in the extension - every chat decodes through it.
        try k.newGame(seed: seed(7), players: 9)
        XCTAssertEqual(k.playerCount(0), 9)

        try k.startFromLobby(lobby)
        XCTAssertEqual(k.phase(0), .night)
        XCTAssertEqual(k.playerCount(0), 6, "six, from the joins and not the resident nine")
        var wolves = 0
        for s in 0..<6 where k.role(s, of: s) == .wolf { wolves += 1 }
        XCTAssertEqual(wolves, 1, "a six-table gets one wolf")
        var seers = 0
        for s in 0..<6 where k.role(s, of: s) == .seer { seers += 1 }
        XCTAssertEqual(seers, 1, "and exactly one seer")
    }

    func testABelowMinimumLobbyCannotBeStarted() throws {
        let k = Kernel.shared
        try k.createLobby(seed: seed(8), chatIsDM: false, myName: "Alex")
        try k.joinLobby(myName: "Sveta")
        let lobby = try k.seal(gameId: 1, sentAt: 0, parent: nil)
        XCTAssertThrowsError(try k.startFromLobby(lobby),
                            "two cannot be dealt, whatever the UI offered")
    }
}
