// TableWireTests.swift - the one table layout, pinned to the KERNEL, and the
// vanishing card that four Swift copies of it let through.
//
// Swift wrote the 2-bytes-per-battle table in four places, and the four agreed
// about every board the kernel can produce and disagreed about the one it
// cannot: a card on the table the viewer is not allowed to see. PlayWire
// spelled it as the no-card byte, which read back as an OPEN battle - Good
// withheld over a covered table, a drop target offered on a closed one - and
// nothing tested it because nothing could reach it from a kernel board. That
// is exactly how a real over-read survived here before, so the bytes are the
// kernel's now (fio_table_encode) and this file asks the kernel, through the
// same doors production uses, what a masked card on the table means.
//
// Two kinds of test. The GOLDEN ones read ios/Fixtures/table_goldens.bin,
// which c/ios/ios_table_goldens.c writes through the kernel and CI keeps fresh,
// and pin the one thing left to Swift: the mapping from a BattleView onto the
// door's suit/value pairs. The BEHAVIOURAL ones are the vanishing card itself,
// asked of PlayWire, ConflictFacts and PreBoutTable - and they are the ones a
// flipped sentinel turns red, which the goldens alone would not (a regenerated
// fixture agrees with whatever the kernel says).

import XCTest
@testable import FoolishKit

final class TableWireTests: XCTestCase {

    /// The conflict rule refuses to answer until a process says which client
    /// it is (ConflictWire.swift, AnimTransport). A test process has no entry
    /// point to say it in, so it says it here, as ConflictModelTests does.
    override func setUp() {
        super.setUp()
        AnimTransport.declare(.chain)
    }

    private func c(_ s: Int, _ v: Int) -> Card { Card(s: s, v: v) }

    // MARK: the fixture
    //
    // Its layout is stated once, in c/ios/ios_table_goldens.c. This is the
    // reader; a test bundle may read a format the kernel defines, it just may
    // not write one.

    private struct Vector {
        let name: String
        let battles: [BattleView]
        let wire: [UInt8]
    }

    private struct Reader {
        let b: [UInt8]
        var at = 0
        mutating func u8() -> Int { defer { at += 1 }; return Int(b[at]) }
        mutating func i8() -> Int { defer { at += 1 }; return Int(Int8(bitPattern: b[at])) }
        mutating func bytes(_ n: Int) -> [UInt8] { defer { at += n }; return Array(b[at..<(at + n)]) }
    }

    private func loadVectors() throws -> [Vector] {
        let url = try XCTUnwrap(
            Bundle(for: Self.self).url(forResource: "table_goldens", withExtension: "bin"),
            "table_goldens.bin missing from the test bundle - run `make ios-goldens`"
        )
        var r = Reader(b: [UInt8](try Data(contentsOf: url)))
        XCTAssertEqual(r.u8(), 1, "table_goldens.bin is not format 1")
        let n = r.u8()
        XCTAssertGreaterThan(n, 0, "the fixture carries no vectors")
        var out: [Vector] = []
        let none = Int(FIO_CARD_NONE)
        for _ in 0..<n {
            let nameLen = r.u8()
            let name = String(decoding: r.bytes(nameLen), as: UTF8.self)
            let nBattles = r.u8()
            var battles: [BattleView] = []
            for _ in 0..<nBattles {
                let attack = Card(s: r.i8(), v: r.i8())
                let cs = r.i8(), cv = r.i8()
                // The kernel's bare pair is what a BattleView with no cover
                // becomes on the way down; the reader turns it back.
                let cover: Card? = (cs == none && cv == none) ? nil : Card(s: cs, v: cv)
                battles.append(BattleView(attack: attack, defense: cover))
            }
            let wireLen = r.u8()
            out.append(Vector(name: name, battles: battles, wire: r.bytes(wireLen)))
        }
        XCTAssertEqual(r.at, r.b.count, "the fixture did not read to its end")
        return out
    }

    // MARK: the goldens

    func testEveryBoardEncodesAsTheKernelWroteIt() throws {
        for v in try loadVectors() {
            XCTAssertEqual(TableWire.encode(v.battles), v.wire, v.name)
        }
    }

    /// The fixture is only worth having if it carries the case the four
    /// writers disagreed about, and the masked cell must be a byte of its own:
    /// neither a card's nor the bare cover's. Asserted by comparison, not by
    /// spelling the byte here - the value is the kernel's.
    func testTheFixtureCarriesTheMaskedCasesAsTheirOwnByte() throws {
        let byName = Dictionary(uniqueKeysWithValues: try loadVectors().map { ($0.name, $0) })
        for name in ["masked-attack", "masked-cover", "masked-both", "masked-among-named"] {
            XCTAssertNotNil(byName[name], "the fixture no longer carries \(name)")
        }
        let bare = try XCTUnwrap(byName["bare"]).wire          // [six, no-card]
        let covered = try XCTUnwrap(byName["covered"]).wire    // [six, nine]
        let masked = try XCTUnwrap(byName["masked-cover"]).wire
        XCTAssertEqual(masked[0], bare[0], "the named attack is the same card either way")
        XCTAssertNotEqual(masked[1], bare[1], "a cover the viewer cannot see is not 'no cover'")
        XCTAssertNotEqual(masked[1], covered[1], "…and not some card the arithmetic invented")
        XCTAssertGreaterThanOrEqual(masked[1], 52, "…and not any card at all")
    }

    // MARK: the vanishing card

    /// THE BUG THIS FILE EXISTS FOR. A nine covered by a card the viewer may
    /// not see is a covered nine: the attacker's bout is fully answered, so
    /// Good is live and the human menu keeps it. With the masked card spelled
    /// as "no card" the battle reads as open, Good is withheld, and the cover
    /// has vanished from a board that still shows it.
    func testACoverTheViewerCannotSeeStillClosesItsBattle() {
        let nine = c(0, 9)
        let table = [BattleView(attack: nine, defense: Card.hidden)]
        let menu = MoveWire.encode([Move(type: .good)])
        let probe = PlayWire.probe(menu: menu, battles: table, powerSuit: 3, isDefender: false,
                                   selection: [c(1, 6)], target: .table)
        XCTAssertTrue(probe.canSayGood, "a cover the viewer cannot see was read as no cover")
        XCTAssertEqual(PlayWire.humanMoves(menu: menu, battles: table).map(\.type), [.good],
                       "…and the human menu dropped Good over a covered table")
    }

    /// The other slot. An attack the viewer cannot see is a card that is there
    /// and equals no card the menu names, so nothing covers it - it is not a
    /// drop target, the Cover button aims elsewhere, and a drop on it is no
    /// move. The menu offers a cover of a NAMED nine to make the point: the
    /// masked cell must not be taken for it.
    func testAnAttackTheViewerCannotSeeIsNotADropTarget() {
        let nine = c(0, 9), jack = c(0, 11)
        let table = [BattleView(attack: Card.hidden, defense: nil)]
        let menu = MoveWire.encode([Move(type: .cover, cards: [jack], attackCards: [nine])])
        let probe = PlayWire.probe(menu: menu, battles: table, powerSuit: 3, isDefender: true,
                                   selection: [jack], target: .battle(0))
        XCTAssertNil(probe.move)
        XCTAssertTrue(probe.coverable.isEmpty)
        XCTAssertNil(probe.bestCover)
        // …and the battle is still open: a bare cell is bare whatever it sits under.
        XCTAssertFalse(PlayWire.probe(menu: MoveWire.encode([Move(type: .good)]), battles: table,
                                      powerSuit: 3, isDefender: false, selection: [jack],
                                      target: .table).canSayGood)
    }

    /// The conflict rule TAKES the masked cell rather than refusing the board:
    /// the six of diamonds under a cover nobody can name is still the six on
    /// the table, so the motion that put it there keeps. The revert beside it
    /// proves a plan was answered - a refused board answers nothing, and
    /// "nothing" reads as keep for every card, which would pass the first
    /// assertion for the wrong reason.
    func testTheConflictRuleStillStandsANamedAttackUnderAMaskedCover() {
        let six = c(3, 6)
        let facts = ConflictFacts(moved: [],
                                  openTable: [BattleView(attack: six, defense: Card.hidden)],
                                  myHand: [])
        XCTAssertEqual(facts.verdict(six, dest: .table), .keep, "the named attack no longer stands")
        XCTAssertEqual(facts.verdict(c(2, 5), dest: .table), .revert,
                       "no plan was answered - the kernel refused the board")
    }

    /// A table to be LAID OUT cannot hold a card nobody can name. A discard
    /// sweeps the last board with cards on it, unchecked - so a masked cover on
    /// that board would be handed to the grid as a card, and instead the board
    /// is not a board: the sweep has no table and the grid paints what it has.
    /// The same stream with the cover named lays out exactly that board, so it
    /// is the masked cell and nothing else that empties the answer.
    func testABoardWithACardNobodyCanNameIsNotLaidOut() {
        let six = c(3, 6), kingH = c(1, 13)
        func stream(cover: Card) -> [GameEvent] {
            [ev(.cover, cards: [kingH], state: view(battles: [BattleView(attack: six, defense: cover)])),
             ev(.discard, cards: [], state: view(battles: []))]
        }
        let masked = PreBoutTable(stream(cover: Card.hidden))
        XCTAssertTrue(masked.battles.isEmpty, "a board nobody can lay out was laid out: \(masked.battles)")
        XCTAssertFalse(masked.paired)

        let named = PreBoutTable(stream(cover: kingH))
        XCTAssertEqual(named.battles, [BattleView(attack: six, defense: kingH)])
        XCTAssertTrue(named.paired)
    }

    /// The rule the one writer that had it right was protecting, kept: a table
    /// holding a card nobody can name is never accounted for, so a sweep can
    /// add a cover to it but never be certified over it.
    func testASweepOverACardNobodyCanNameIsNotCertified() {
        let six = c(3, 6), nine = c(1, 9)
        let outer = [BattleView(attack: six, defense: nine)]
        XCTAssertTrue(PreBoutTable.covers(outer, [BattleView(attack: six, defense: nil)]))
        XCTAssertFalse(PreBoutTable.covers(outer, [BattleView(attack: six, defense: Card.hidden)]),
                       "a cover the viewer cannot see was read as no cover, and the swap certified")
    }

    // MARK: fixtures

    private func ev(_ kind: EventType, cards: [Card], state: GameView?) -> GameEvent {
        GameEvent(type: kind.rawValue, seat: 1, msg: 0, from: 1, to: 2,
                  cards: cards.map { Optional($0) }, target: nil, battle: nil, state: state)
    }

    private func view(battles: [BattleView]) -> GameView {
        GameView(status: 1, numPlayers: 2, powerSuit: 3, deckCount: 10, discardCount: 0,
                 hasFlipped: true, firstAttacker: 0, defender: 1, viewer: 0,
                 goodMask: 0, gameOver: -1, flipped: c(3, 12), battles: battles,
                 eliminationOrder: [],
                 players: [PlayerView(seat: 0, name: "", status: 2, handCount: 6,
                                      awaitingAttack: false, strategyKey: 0, hand: nil),
                           PlayerView(seat: 1, name: "", status: 2, handCount: 6,
                                      awaitingAttack: false, strategyKey: 0, hand: nil)])
    }
}
