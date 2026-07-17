// ComponentSnapshotTests.swift — snapshot coverage for DesignSystem components
// in light/dark, Dynamic Type, and ru/ko strings (§13, §16.A6). Uses
// pointfreeco/swift-snapshot-testing.
//
// FIRST RUN on a Mac records reference images (set `isRecording = true` once,
// commit the __Snapshots__ folder, then set it back). These cannot be recorded
// in a headless Linux/CI environment without a simulator — the CI Mac job owns
// the reference images (§13 "snapshot tests").

import XCTest
import SwiftUI
import SnapshotTesting
@testable import FoolishKit

final class ComponentSnapshotTests: XCTestCase {

    // Flip to true on a Mac to (re)record references, then flip back and commit.
    private let record = false

    override func setUp() {
        super.setUp()
        // isRecording is global to the library.
        // SnapshotTesting.isRecording = record
    }

    private func host<V: View>(_ view: V, width: CGFloat = 320, height: CGFloat = 200) -> UIViewController {
        let vc = UIHostingController(rootView: view.frame(width: width, height: height).background(FColor.table))
        vc.view.frame = CGRect(x: 0, y: 0, width: width, height: height)
        return vc
    }

    func testFCardFace() {
        assertSnapshot(of: host(FCard(card: Card(s: 1, v: 13), trump: true), width: 120, height: 140),
                       as: .image, record: record)
    }

    func testFCardBack() {
        assertSnapshot(of: host(FCard(card: nil, backSeed: 42), width: 120, height: 140),
                       as: .image, record: record)
    }

    func testFHandFan() {
        let cards = [Card(s: 0, v: 7), Card(s: 1, v: 13), Card(s: 2, v: 6), Card(s: 3, v: 11)]
        assertSnapshot(of: host(FHandFan(cards: cards, trumpSuit: .hearts,
                                         selection: .constant([]), onTap: { _ in }),
                                width: 320, height: 140),
                       as: .image, record: record)
    }

    func testFActionBarRu() {
        FStrings.override = .ru
        defer { FStrings.override = .system }
        assertSnapshot(of: host(FActionBar(canPickup: true, canDone: true, canTransfer: true,
                                           onPickup: {}, onDone: {}, onTransfer: {}),
                                width: 340, height: 80),
                       as: .image, record: record)
    }

    // The iMessage expanded-bubble board (read-only public view). A mid-game 2p
    // state: one uncovered ♠7 attack, ♦ trump, deck 20, discard 6.
    func testMessageBoardMidGame() {
        let players = [
            PlayerView(seat: 0, name: "", status: 2, handCount: 6,
                       awaitingAttack: false, strategyKey: 0, hand: nil),
            PlayerView(seat: 1, name: "", status: 2, handCount: 5,
                       awaitingAttack: false, strategyKey: 0, hand: nil),
        ]
        let view = GameView(
            status: 1, numPlayers: 2, powerSuit: 1, deckCount: 20, discardCount: 6,
            hasFlipped: true, firstAttacker: 1, defender: 0, viewer: -1,
            goodMask: 0, gameOver: -1, flipped: Card(s: 1, v: 11),
            battles: [BattleView(attack: Card(s: 0, v: 7), defense: nil)],
            eliminationOrder: [], players: players)
        assertSnapshot(of: host(MessageBoardView(view: view, names: [0: "Sveta", 1: "Alex"]),
                                width: 360, height: 260),
                       as: .image, record: record)
    }

    /// The bubble image (§10/§11.3) actually renders headless — ImageRenderer over
    /// MessageBoardView — at Apple's 300×195pt template size, and it is a public
    /// board (viewer -1 fixture ⇒ no hand can appear). Not a reference snapshot;
    /// it proves the compose path produces a usable UIImage.
    @MainActor
    func testBubbleSnapshotRendersPublicBoard() {
        let players = [
            PlayerView(seat: 0, name: "", status: 2, handCount: 6,
                       awaitingAttack: false, strategyKey: 0, hand: nil),
            PlayerView(seat: 1, name: "", status: 2, handCount: 5,
                       awaitingAttack: false, strategyKey: 0, hand: nil),
        ]
        let view = GameView(
            status: 1, numPlayers: 2, powerSuit: 1, deckCount: 20, discardCount: 6,
            hasFlipped: true, firstAttacker: 1, defender: 0, viewer: -1,
            goodMask: 0, gameOver: -1, flipped: Card(s: 1, v: 11),
            battles: [BattleView(attack: Card(s: 0, v: 7), defense: nil)],
            eliminationOrder: [], players: players)
        let img = BubbleSnapshot.render(publicView: view, names: [0: "Sveta", 1: "Alex"])
        XCTAssertNotNil(img, "the bubble image renders")
        XCTAssertEqual(img?.size, BubbleSnapshot.size, "at the §11.3 template size")
        XCTAssertTrue(view.players.allSatisfy { $0.hand == nil }, "the source is a no-hand public view")
    }
}
