// DesignTests - the study's materials and type as the bundle ships them
// (DECISIONS I27, I28): the baked planks open at the study's tile size, a
// plank's middle runs down the drawer's centre at every width, the two Fell
// faces register from ChuiniuKit's own bundle, a name is bright only on its
// turn, each verb wears the plate the study gives it, and nothing on the
// table says what a table does not say.

import SwiftUI
import XCTest
@testable import ChuiniuKit

final class DesignTests: XCTestCase {
    func testTheBakedImagesOpenAtTheStudysSizes() throws {
        let planks = try XCTUnwrap(CnTextures.planks, "cn_planks.jpg is in ChuiniuKit's bundle (make -C chuiniu/c tex-ios)")
        XCTAssertEqual(planks.size, PlankTile.size, "the planks are the study's 516 by 830 tile")
        XCTAssertEqual(planks.scale, 2, "two texels a point")
        let verd = try XCTUnwrap(CnTextures.verd, "cn_verd.png is in the bundle")
        XCTAssertEqual(verd.size, CGSize(width: 128, height: 128), "the verdigris at 128 points, as the study's --tex-verd")
        let crust = try XCTUnwrap(CnTextures.crust, "cn_crust.png is in the bundle")
        XCTAssertEqual(crust.size.width, 192, accuracy: 0.01, "the crust at 192 points, as .plate .crust")
        XCTAssertEqual(crust.size.height, 48, accuracy: 0.01)
        XCTAssertNotNil(CnTextures.bone, "cn_bone.png is in the bundle")
        // the drowned palette, not foolish's walnut: the planks' mean leans cold and is dark
        let mean = try XCTUnwrap(Self.mean(planks))
        XCTAssertLessThan(mean.r, mean.g, "the planks are not warm (r \(mean.r), g \(mean.g))")
        XCTAssertLessThan(mean.r, mean.b, "the planks are not warm (r \(mean.r), b \(mean.b))")
        XCTAssertLessThan(mean.r + mean.g + mean.b, 3 * 32, "the planks are near black")
    }

    func testAPlanksMiddleRunsDownTheCentreAtEveryWidth() {
        for w in [300.0, 375, 390, 402, 430, 440] as [CGFloat] {
            let left = PlankTile.left(centredOn: w / 2)
            XCTAssertLessThanOrEqual(left, 0, "\(w): the tile starts at or left of the edge")
            XCTAssertGreaterThan(left, -PlankTile.plank, "\(w): and less than a plank left of it")
            // the plank that holds the centre line has its middle on it, to the rounding of one point
            let k = ((w / 2 - left) / PlankTile.plank).rounded(.down)
            let middle = left + (k + 0.5) * PlankTile.plank
            XCTAssertEqual(middle, w / 2, accuracy: 0.5, "\(w): a plank's middle is on the centre line")
            // the turned stage: the same plank, its middle at the drawer's centre inside the overdraw
            let size = CGSize(width: w, height: 718)
            let o = StageUIView.tileOrigin(size), over = StageUIView.overdraw(size)
            let kk = ((0.95 * w - o.x) / PlankTile.plank).rounded(.down)
            XCTAssertEqual(over.minX + o.x + (kk + 0.5) * PlankTile.plank, w / 2, accuracy: 0.5,
                           "\(w): on the stage too, a plank's middle under my cup")
            XCTAssertEqual(o.y, PlankTile.top, "\(w): the tile's top 58 up, as the study's")
        }
    }

    func testTheTwoFellFacesRegisterFromTheFrameworksBundle() {
        XCTAssertTrue(FType.registered, "IM Fell English roman and small caps resolve by name")
        XCTAssertEqual(FType.uiSerif(14).fontName, FType.serifName, "the roman is Fell, not the system face")
        XCTAssertEqual(FType.uiSC(14).fontName, FType.scName, "the small caps are Fell SC")
        XCTAssertEqual(NameDecal.nameFont(short: false).pointSize, 14, "a name is 14")
        XCTAssertEqual(NameDecal.nameFont(short: true).pointSize, 12, "and 12 on a short board")
        XCTAssertEqual(FType.nameTracking(14), 14 * 0.14, accuracy: 1e-9, "tracked .14em")
    }

    func testANameIsBrightOnlyOnItsTurn() {
        func seat(_ turn: Bool, alive: Bool = true) -> StageName {
            StageName(seat: SeatModel(id: 1, name: "Bo", dice: 5, alive: alive, isTurn: turn, isMe: false))
        }
        XCTAssertTrue(Self.same(NameDecal.nameInk(seat(true)), Ink.ink), "the turn's name is the bright ink")
        XCTAssertTrue(Self.same(NameDecal.nameInk(seat(false)), Ink.inkdim), "every other name is the dim ink")
        var won = seat(false); won.won = true
        XCTAssertTrue(Self.same(NameDecal.nameInk(won), Ink.glow), "the winner's name is the glow")
    }

    func testEachVerbWearsTheStudysPlate() {
        XCTAssertEqual(BidPicker.callKind(enabled: true), .call, "Liar is the blood plate")
        XCTAssertEqual(BidPicker.callKind(enabled: false), .sunk, "and sunk when the kernel does not offer the call")
        XCTAssertEqual(BidPicker.raiseKind(enabled: true), .glow, "Raise is the lit bronze plate")
        XCTAssertEqual(BidPicker.raiseKind(enabled: false), .sunk, "and sunk below the least raise")
        XCTAssertEqual(PlankKind.call.ink, Color(hex: 0xF3DDCF), "the blood plate's word is the study's pale")
    }

    func testThePlatesWordsNeverFallUnderTheRomansFloor() {
        for narrow in [false, true] {
            let sizes = BidPlate.sizes(narrow: narrow)
            XCTAssertEqual(sizes.first, narrow ? 20 : 26, "the study's bid size first")
            XCTAssertEqual(sizes.min(), 15.5, "nothing under 15.5 in the roman (the Type tab)")
            XCTAssertEqual(sizes, sizes.sorted(by: >), "stepping down only")
        }
    }

    /// Two colours alike to a 255th in each sRGB channel.
    private static func same(_ a: UIColor, _ b: Color) -> Bool {
        var (r1, g1, b1, a1, r2, g2, b2, a2): (CGFloat, CGFloat, CGFloat, CGFloat, CGFloat, CGFloat, CGFloat, CGFloat) = (0, 0, 0, 0, 0, 0, 0, 0)
        guard a.getRed(&r1, green: &g1, blue: &b1, alpha: &a1), UIColor(b).getRed(&r2, green: &g2, blue: &b2, alpha: &a2) else { return false }
        return zip([r1, g1, b1, a1], [r2, g2, b2, a2]).allSatisfy { abs($0 - $1) < 1.0 / 255 }
    }

    /// The mean of an image's channels, drawn into 32 by 32.
    private static func mean(_ img: UIImage) -> (r: Double, g: Double, b: Double)? {
        guard let cg = img.cgImage else { return nil }
        var px = [UInt8](repeating: 0, count: 32 * 32 * 4)
        guard let ctx = CGContext(data: &px, width: 32, height: 32, bitsPerComponent: 8, bytesPerRow: 128,
                                  space: CGColorSpace(name: CGColorSpace.sRGB)!,
                                  bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue) else { return nil }
        ctx.interpolationQuality = .medium
        ctx.draw(cg, in: CGRect(x: 0, y: 0, width: 32, height: 32))
        var s = (0.0, 0.0, 0.0)
        for i in stride(from: 0, to: px.count, by: 4) { s.0 += Double(px[i]); s.1 += Double(px[i + 1]); s.2 += Double(px[i + 2]) }
        let n = Double(32 * 32)
        return (s.0 / n, s.1 / n, s.2 / n)
    }
}
