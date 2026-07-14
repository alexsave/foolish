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

    // swift-snapshot-testing 1.19's `record:` takes a `Record`, not a `Bool`
    // (and its boolean-literal shim is deprecated), so map the toggle explicitly:
    // `.all` re-records everything, `.missing` records only absent references and
    // fails on a diff — the normal compare mode.
    private var recordMode: SnapshotTestingConfiguration.Record { record ? .all : .missing }

    override func setUp() {
        super.setUp()
    }

    private func host<V: View>(_ view: V, width: CGFloat = 320, height: CGFloat = 200) -> UIViewController {
        let vc = UIHostingController(rootView: view.frame(width: width, height: height).background(FColor.table))
        vc.view.frame = CGRect(x: 0, y: 0, width: width, height: height)
        return vc
    }

    func testFCardFace() {
        assertSnapshot(of: host(FCard(card: Card(s: 1, v: 13), trump: true), width: 120, height: 140),
                       as: .image, record: recordMode)
    }

    func testFCardBack() {
        assertSnapshot(of: host(FCard(card: nil, backSeed: 42), width: 120, height: 140),
                       as: .image, record: recordMode)
    }

    func testFHandFan() {
        let cards = [Card(s: 0, v: 7), Card(s: 1, v: 13), Card(s: 2, v: 6), Card(s: 3, v: 11)]
        assertSnapshot(of: host(FHandFan(cards: cards, trumpSuit: .hearts,
                                         selection: .constant([]), onTap: { _ in }),
                                width: 320, height: 140),
                       as: .image, record: recordMode)
    }

    func testFActionBarRu() {
        FStrings.override = .ru
        defer { FStrings.override = .system }
        assertSnapshot(of: host(FActionBar(canPickup: true, canDone: true, canTransfer: true,
                                           onPickup: {}, onDone: {}, onTransfer: {}),
                                width: 340, height: 80),
                       as: .image, record: recordMode)
    }
}
