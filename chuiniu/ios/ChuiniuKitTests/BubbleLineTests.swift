// BubbleLineTests - every bubble is one line (the owner's rule, docs_pkgY.md).
//
// Messages sets an MSMessageTemplateLayout's caption under the picture in the
// system font, the study's .bub-cap: 17 points across a 300-point bubble with
// 14 points of padding a side, so 272 points of text; a longer caption wraps
// or is cut. The kernel composes every caption and guarantees, without a
// font, that its width bound (cn_cap_width) stays within that budget. This
// file holds the bound and the captions against the phone's own font,
// regular and semibold (the wider is what Messages could draw), and holds the
// bubble picture's plate words to one line at the plate's own font.

import CoreText
import UIKit
import XCTest
@testable import ChuiniuKit

@MainActor
final class BubbleLineTests: XCTestCase {
    /// The caption's text width in the study's bubble: 300 less 14 a side.
    private static let room: CGFloat = 300 - 2 * 14
    private static let fonts = [UIFont.systemFont(ofSize: 17, weight: .regular),
                                UIFont.systemFont(ofSize: 17, weight: .semibold)]

    private static func width(_ s: String, _ font: UIFont) -> CGFloat {
        let line = CTLineCreateWithAttributedString(NSAttributedString(string: s, attributes: [.font: font]))
        return CGFloat(CTLineGetTypographicBounds(line, nil, nil, nil))
    }
    private static func widest(_ s: String) -> CGFloat { fonts.map { width(s, $0) }.max()! }

    /// The glyphs a name is made of: the widest of every class the kernel's
    /// bound knows, narrow ones, a space and a letter with its mark.
    private static let glyphs = ["W", "%", "M", "m", "w", "i", " ", "Œ", "Ѹ", "吹", "😀", "\u{FDFD}", "\u{1242B}",
                                 "Ǆ", "e\u{301}"]

    /// Names 1 to 16 code points (the seat-name cap, CN_NAME_MAX_CHARS), and
    /// no more than the wire's 48 bytes: one glyph throughout, or the glyphs
    /// cycling every `step`.
    private static var names: [String] {
        var out: [String] = []
        for len in 1...16 {
            for g in glyphs.indices {
                for step in 0...3 {
                    var s = "", scalars = 0
                    for i in 0... {
                        let u = glyphs[(g + (step == 0 ? 0 : i / step)) % glyphs.count]
                        if scalars + u.unicodeScalars.count > len || s.utf8.count + u.utf8.count > 48 { break }
                        s += u
                        scalars += u.unicodeScalars.count
                    }
                    out.append(s)
                }
            }
        }
        out.append(contentsOf: ["Alex", "Bo", "Player 6", "Maximiliana Wolf", "Владимир Петров"])
        return out
    }

    func testTheKernelsBoundIsAtLeastTheFontEverywhere() {
        XCTAssertEqual(BridgeKernel.captionBudget, Double(Self.room), "the kernel's budget is the bubble's room")
        var probes = (0x20...0x7E).map { String(UnicodeScalar(UInt8($0))) }
        probes += Self.glyphs + ["Ж", "Щ", "Ю", "あ", "한", "Æ", "ß", "🎲", "\u{102A}", "…"]
        for s in probes {
            let bound = CGFloat(BridgeKernel.captionBound(s)), real = Self.widest(s)
            XCTAssertLessThanOrEqual(real, bound + 0.01, "'\(s)' is \(real) points at 17, the kernel says \(bound)")
        }
    }

    func testEveryCaptionIsOneLine() throws {
        var lines = 0, widest: (CGFloat, String) = (0, "")
        func hold(_ line: String?, _ what: String) {
            guard let line else { XCTFail("\(what): no caption"); return }
            lines += 1
            let w = Self.widest(line)
            if w > widest.0 { widest = (w, line) }
            if w > Self.room { XCTFail("\(what): \(w) points, past \(Self.room): \(line)") }
        }
        for who in Self.names {
            hold(BridgeKernel.caption(.start, who: who), "the start")
            hold(BridgeKernel.caption(.invite, who: who), "the invitation")
            hold(BridgeKernel.caption(.joined, who: who), "a join")
            hold(BridgeKernel.caption(.left, who: who), "a leave")
            for q in 1...30 {
                for f in 2...6 {
                    hold(BridgeKernel.caption(.bid, who: who, quantity: q, face: f), "a bid")
                    hold(BridgeKernel.caption(.call, who: who, quantity: q, face: f), "a call")
                }
            }
        }
        print("BubbleLineTests: \(lines) captions, the widest \(widest.0) of \(Self.room) points: \(widest.1)")
        // the study's words where they fit, which is the usual case
        XCTAssertEqual(BridgeKernel.caption(.start, who: "Alex"), "Dice rolled. Alex bids first")
        XCTAssertEqual(BridgeKernel.caption(.call, who: "Bo", quantity: 3, face: 3), "Bo calls three 3s")
    }

    /// The bubble picture's plate (I18): the bid, staged or on the table,
    /// beside its die, or the reveal's tally, on one line at one of the
    /// plate's sizes, in the plate the bubble's HUD gives.
    func testThePlateWordsAreOneLine() throws {
        let plate = try XCTUnwrap(bubblePlate(), "the bubble's HUD gives a plate")
        let narrow = plate.width < BidPlate.narrowBelow
        var widest: (CGFloat, String) = (0, "")
        func hold(_ text: String?, face: Bool) {
            guard let text, !text.isEmpty else { XCTFail("no plate words"); return }
            let room = BidPlate.textRoom(width: plate.width, face: face)
            let fits = BidPlate.sizes(narrow: narrow).first { Self.width(text, FType.uiSerif($0)) <= room }
            let at = fits ?? BidPlate.sizes(narrow: narrow).last!
            let used = Self.width(text, FType.uiSerif(at)) / room
            if used > widest.0 { widest = (used, text) }
            XCTAssertNotNil(fits, "'\(text)' fits no size on one line in \(room) points")
        }
        for q in 1...30 { for f in 2...6 { hold(BridgeKernel.caption(.plateBid, who: "", quantity: q, face: f), face: true) } }
        for c in 0...30 { hold(BridgeKernel.caption(.tally, who: "", quantity: c), face: false) }
        print("BubbleLineTests: the plate \(plate.width) wide, the fullest \(widest.1) at \(widest.0) of its room")
    }

    // MARK: a real table for the bubble's HUD

    private var stores: [String: UserDefaults] = [:]

    private func phone(_ name: String) -> BridgeKernel {
        let suite = "chuiniu.linetests.\(name).\(ObjectIdentifier(self).hashValue)"
        let store = stores[name] ?? {
            let d = UserDefaults(suiteName: suite)!
            d.removePersistentDomain(forName: suite)
            stores[name] = d
            return d
        }()
        let k = BridgeKernel(store: store, devPerson: false)
        k.me(Data(repeating: UInt8(name.utf8.first!), count: 16))
        k.nickname(name)
        k.sender(nil, isDM: false, iSent: false)
        return k
    }

    /// The plate rectangle of a bubble begun on a two-seat game just
    /// started, as BubbleSnapshot is given it.
    private func bubblePlate() throws -> CGRect? {
        let alex = phone("Alex")
        XCTAssertTrue(alex.newGame(dm: true, seed: (0..<32).map { UInt8(($0 * 5 + 3) & 0xFF) }))
        let lobby = try XCTUnwrap(alex.stagedURL())
        let bo = phone("Bo")
        XCTAssertEqual(bo.adoptBubble(lobby), 0)
        XCTAssertTrue(bo.join(name: "Bo"))
        let start = try XCTUnwrap(bo.stagedURL())
        let me = phone("Alex")
        XCTAssertEqual(me.adoptBubble(start), 0)
        let stage = KernelSeam.stage()
        defer { stage.purge() }
        let hud = try XCTUnwrap(stage.begin(.bubble, drawer: BubbleSnapshot.size, scale: 1))
        return hud.plateRect
    }

    /// THE STAGED PATH: the caption a real bubble is staged with, two
    /// phones whose names are the widest the cap allows.
    func testTheStagedCaptionsOfAWideTableAreOneLine() throws {
        let w = String(repeating: "W", count: 16), m = String(repeating: "M", count: 16)
        var alex = phone(w)
        XCTAssertTrue(alex.newGame(dm: true, seed: (0..<32).map { UInt8(($0 * 3 + 1) & 0xFF) }))
        var caption = alex.table.bubbleCaption
        XCTAssertLessThanOrEqual(Self.widest(caption), Self.room, caption)
        let lobby = try XCTUnwrap(alex.stagedURL())
        let bo = phone(m)
        XCTAssertEqual(bo.adoptBubble(lobby), 0)
        XCTAssertTrue(bo.join(name: m))
        caption = bo.table.bubbleCaption
        XCTAssertLessThanOrEqual(Self.widest(caption), Self.room, caption)
        let start = try XCTUnwrap(bo.stagedURL())
        alex = phone(w)
        XCTAssertEqual(alex.adoptBubble(start), 0)
        XCTAssertTrue(alex.raise(quantity: 10, face: 6))
        caption = alex.table.bubbleCaption
        XCTAssertTrue(caption.hasSuffix("10 6s") || caption.hasSuffix("ten 6s"), caption)
        XCTAssertLessThanOrEqual(Self.widest(caption), Self.room, caption)
    }
}
