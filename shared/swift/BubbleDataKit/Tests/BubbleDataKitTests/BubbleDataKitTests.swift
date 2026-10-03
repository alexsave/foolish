// The kit through the chain a real send runs, built here with ImageIO: the
// extension's JPEG at quality 0.50 (4:2:0), decoded, the transport's JPEG at
// 0.89, decoded, then read. tools/layout_probe/README.md ("A real send") is
// where those two encoders and the 1200 px cut were measured.
import CBubbleData
import CoreGraphics
import Foundation
import ImageIO
import XCTest

@testable import BubbleDataKit

/// splitmix64: a fixed stream, so a failure names the same board every run.
struct SplitMix {
    var state: UInt64
    mutating func next() -> UInt64 {
        state &+= 0x9E37_79B9_7F4A_7C15
        var z = state
        z = (z ^ (z >> 30)) &* 0xBF58_476D_1CE4_E5B9
        z = (z ^ (z >> 27)) &* 0x94D0_49BB_1331_11EB
        return z ^ (z >> 31)
    }
    mutating func symbols(_ count: Int) -> [UInt8] { (0..<count).map { _ in UInt8(next() % 3) } }
    mutating func bytes(_ count: Int) -> Data { Data((0..<count).map { _ in UInt8(truncatingIfNeeded: next()) }) }
}

/// JPEG bytes at ImageIO quality `q`.
func jpeg(_ image: CGImage, quality q: Double) throws -> Data {
    let data = NSMutableData()
    let dst = try XCTUnwrap(CGImageDestinationCreateWithData(data as CFMutableData, "public.jpeg" as CFString, 1, nil))
    CGImageDestinationAddImage(dst, image, [kCGImageDestinationLossyCompressionQuality: q] as CFDictionary)
    XCTAssertTrue(CGImageDestinationFinalize(dst))
    return data as Data
}

/// The first component's sampling factors from a JPEG's start-of-frame
/// segment: 0x22 is luma at twice the chroma both ways, which is 4:2:0.
func lumaSampling(_ jpeg: Data) -> (factors: UInt8, components: Int)? {
    let b = [UInt8](jpeg)
    var i = 2
    while i + 4 < b.count, b[i] == 0xFF {
        let marker = b[i + 1], len = Int(b[i + 2]) << 8 | Int(b[i + 3])
        if marker == 0xC0 || marker == 0xC1 || marker == 0xC2 {
            guard i + 11 < b.count else { return nil }
            return (b[i + 11], Int(b[i + 9]))
        }
        i += 2 + len
    }
    return nil
}

/// The picture rescaled to w x h, the way a resize does it.
func scaled(_ image: CGImage, width w: Int, height h: Int, quality: CGInterpolationQuality) throws -> CGImage {
    let ctx = try XCTUnwrap(CGContext(data: nil, width: w, height: h, bitsPerComponent: 8, bytesPerRow: 0,
                                      space: BubbleData.colorSpace, bitmapInfo: BubbleData.bitmapInfo))
    ctx.interpolationQuality = quality
    ctx.draw(image, in: CGRect(x: 0, y: 0, width: w, height: h))
    return try XCTUnwrap(ctx.makeImage())
}

final class BubbleDataKitTests: XCTestCase {
    /// What a real send does to a picture: q0.50 4:2:0, decode, q0.89, decode.
    /// Both JPEGs are checked to be 4:2:0, so the chain is the one measured.
    func send(_ image: CGImage, file: StaticString = #filePath, line: UInt = #line) throws -> CGImage {
        let first = try jpeg(image, quality: 0.50)
        XCTAssertEqual(lumaSampling(first)?.factors, 0x22, "the extension's JPEG is not 4:2:0", file: file, line: line)
        XCTAssertEqual(lumaSampling(first)?.components, 3, file: file, line: line)
        let decoded = try XCTUnwrap(BubbleData.cgImage(data: first))
        let second = try jpeg(decoded, quality: 0.89)
        XCTAssertEqual(lumaSampling(second)?.factors, 0x22, "the transport's JPEG is not 4:2:0", file: file, line: line)
        return try XCTUnwrap(BubbleData.cgImage(data: second))
    }

    func testGeometry() {
        XCTAssertTrue(BubbleDataGeometry.board243.isValid)
        XCTAssertTrue(BubbleDataGeometry.robust243.isValid)
        XCTAssertEqual(BubbleDataGeometry.board243.width, 243)
        XCTAssertEqual(BubbleDataGeometry.board243.height, 244)
        XCTAssertEqual(BubbleDataGeometry.robust243.width, 729)
        XCTAssertEqual(BubbleDataGeometry.robust243.height, 732)
        XCTAssertEqual(BubbleDataGeometry.board243.symbolCapacity, 59049)
        XCTAssertEqual(BubbleDataGeometry.board243.byteCapacity, 11070)
        XCTAssertFalse(BubbleDataGeometry(cells: 63, pixelsPerCell: 1).isValid)
        XCTAssertFalse(BubbleDataGeometry(cells: 243, pixelsPerCell: 5).isValid)
        XCTAssertFalse(BubbleDataGeometry(cells: Int.max, pixelsPerCell: 1).isValid)
        XCTAssertEqual(BubbleDataGeometry(cells: 10, pixelsPerCell: 1).byteCapacity, 0)
        XCTAssertThrowsError(try BubbleData.image(symbols: [0], geometry: .init(cells: 243, pixelsPerCell: 5))) {
            XCTAssertEqual($0 as? BubbleDataError, .geometry)
        }
    }

    func boardThroughASend(_ geometry: BubbleDataGeometry) throws {
        var rng = SplitMix(state: 243)
        let board = rng.symbols(geometry.symbolCapacity)
        let picture = try BubbleData.image(symbols: board, geometry: geometry)
        XCTAssertEqual(picture.width, geometry.width)
        XCTAssertEqual(picture.height, geometry.height)
        let received = try send(picture)
        let (back, reading) = try BubbleData.symbols(from: received, cells: geometry.cells)
        XCTAssertEqual(back.count, board.count)
        XCTAssertEqual(zip(back, board).filter { $0 != $1 }.count, 0, "cells differ")
        XCTAssertEqual(reading.cells, 243 * 244)
        print("board \(geometry.cells) cells at \(geometry.pixelsPerCell) px through q0.50 + q0.89: " +
              "min margin \(reading.minMargin), \(reading.risky) risky of \(reading.cells)")
        XCTAssertGreaterThan(reading.minMargin, 0)
    }

    func testBoard243At1pxThroughASend() throws { try boardThroughASend(.board243) }
    func testBoard243At3pxThroughASend() throws { try boardThroughASend(.robust243) }

    /// A SPARSE BOARD IS THE ONE A GAME SENDS, and dense random symbols hid
    /// what happens to it (uttt/c/tests/uttt_big_chain.c, 2026-10-01): a lone
    /// grey cell between white ones - in the board, or in the header row,
    /// whose count and CRC cells are sparse too - comes back from the two
    /// JPEGs near or over the 191 threshold and reads as empty. Over boards
    /// of 1, 40 and 400 scattered marks from 24 streams: at 1 px a cell some
    /// are REFUSED (never misread - the checksum catches it) and the rest read
    /// at a margin a further recompression would not leave alone; at 3 px
    /// every one reads whole with more than the risky margin to spare.
    func testSparseBoardNeedsThreePixelsACell() throws {
        var refusedAt1 = 0, worstAt1 = 64, worstAt3 = 64, boards = 0
        for seed in 1...24 {
            for marks in [1, 40, 400] {
                var rng = SplitMix(state: UInt64(seed * 1000 + marks))
                var board = [UInt8](repeating: 0, count: 243 * 243)
                for i in 0..<marks { board[Int(rng.next() % 59049)] = UInt8(1 + i % 2) }
                boards += 1
                let one = try send(try BubbleData.image(symbols: board, geometry: .board243))
                if let (back, r) = try? BubbleData.symbols(from: one, cells: 243) {
                    XCTAssertEqual(back, board, "1 px: read, so it must be the board (never misread)")
                    worstAt1 = min(worstAt1, r.minMargin)
                } else {
                    refusedAt1 += 1
                }
                let three = try send(try BubbleData.image(symbols: board, geometry: .robust243))
                let (back, r) = try BubbleData.symbols(from: three, cells: 243)
                XCTAssertEqual(back, board, "a sparse board at 3 px must read whole (seed \(seed), \(marks) marks)")
                worstAt3 = min(worstAt3, r.minMargin)
            }
        }
        print("sparse boards (\(boards)) at 1 px: \(refusedAt1) refused, worst margin of the rest \(worstAt1); " +
              "at 3 px: all read, worst margin \(worstAt3)")
        XCTAssertTrue(refusedAt1 > 0 || worstAt1 < Int(BD_RISKY_MARGIN), "1 px is not fit for a sparse board")
        XCTAssertGreaterThan(worstAt3, Int(BD_RISKY_MARGIN), "3 px must leave more than the risky margin")
    }

    func testBytesAtCapacityThroughASend() throws {
        for geometry in [BubbleDataGeometry.board243, .robust243] {
            var rng = SplitMix(state: 11070)
            let bytes = rng.bytes(geometry.byteCapacity)
            let received = try send(try BubbleData.image(bytes: bytes, geometry: geometry))
            let (back, reading) = try BubbleData.bytes(from: received, cells: geometry.cells)
            XCTAssertEqual(back, bytes)
            print("bytes \(bytes.count) at \(geometry.pixelsPerCell) px through q0.50 + q0.89: " +
                  "min margin \(reading.minMargin), \(reading.risky) risky")
        }
        // One byte over is refused at the start, not truncated.
        var rng = SplitMix(state: 1)
        XCTAssertThrowsError(try BubbleData.image(bytes: rng.bytes(11071), geometry: .board243)) {
            XCTAssertEqual($0 as? BubbleDataError, .capacity)
        }
    }

    /// A picture painted larger than a send keeps (243 cells at 6 px, 1458 x
    /// 1464) is cut to 1200 on its longer side, as the transport does; at
    /// 4.9 px a cell it still reads. Painted at 3 px and then upscaled by
    /// nearest neighbour, because the kit refuses to paint over 1200 itself.
    func testCutTo1200StillReads() throws {
        var rng = SplitMix(state: 1200)
        let board = rng.symbols(243 * 243)
        let painted = try BubbleData.image(symbols: board, geometry: .robust243)
        let big = try scaled(painted, width: 1458, height: 1464, quality: .none)
        let cut = try scaled(big, width: 1458 * 1200 / 1464, height: 1200, quality: .high)
        let second = try BubbleData.cgImage(data: jpeg(cut, quality: 0.89))
        let received = try XCTUnwrap(second)
        XCTAssertEqual(received.height, 1200)
        let (back, reading) = try BubbleData.symbols(from: received, cells: 243)
        XCTAssertEqual(back, board)
        print("243 cells cut to \(received.width)x\(received.height): min margin \(reading.minMargin), " +
              "\(reading.risky) risky")
    }

    /// A picture shrunk to 1.2 px a cell (README: thousands of cells wrong)
    /// is refused, never read as some other board.
    func testShrunkTooFarIsRefused() throws {
        var rng = SplitMix(state: 300)
        let board = rng.symbols(243 * 243)
        let painted = try BubbleData.image(symbols: board, geometry: .robust243)
        let small = try send(try scaled(painted, width: 300, height: 301, quality: .high))
        XCTAssertThrowsError(try BubbleData.symbols(from: small, cells: 243)) { error in
            XCTAssertNotNil(error as? BubbleDataError)
        }
        // Under a pixel a cell, the geometry itself is refused.
        let tiny = try scaled(painted, width: 200, height: 201, quality: .high)
        XCTAssertThrowsError(try BubbleData.symbols(from: tiny, cells: 243)) {
            XCTAssertEqual($0 as? BubbleDataError, .geometry)
        }
    }

    /// One cell of a framed board flipped before painting: the picture
    /// survives the send exactly, so the read gets the flipped cell, and the
    /// checksum refuses it.
    func testDamagedPictureIsRefused() throws {
        let n = 243
        var rng = SplitMix(state: 7)
        let board = rng.symbols(n * n)
        var cells = [UInt8](repeating: 0, count: n * (n + 1))
        XCTAssertEqual(bd_frame(board, Int32(board.count), Int32(BD_KIND_SYMBOLS), 0, Int32(n), &cells), BD_EOK)
        cells[n + 12345] = (cells[n + 12345] + 1) % 3
        var rgba = [UInt8](repeating: 0, count: n * (n + 1) * 4)
        XCTAssertEqual(bd_paint(cells, Int32(n), 1, &rgba), BD_EOK)
        let provider = try XCTUnwrap(CGDataProvider(data: Data(rgba) as CFData))
        let picture = try XCTUnwrap(CGImage(width: n, height: n + 1, bitsPerComponent: 8, bitsPerPixel: 32,
                                            bytesPerRow: n * 4, space: BubbleData.colorSpace,
                                            bitmapInfo: CGBitmapInfo(rawValue: BubbleData.bitmapInfo),
                                            provider: provider, decode: nil, shouldInterpolate: false,
                                            intent: .defaultIntent))
        XCTAssertThrowsError(try BubbleData.symbols(from: try send(picture), cells: n)) {
            XCTAssertEqual($0 as? BubbleDataError, .checksum)
        }
    }

    /// The other refusals, through the Swift face: a picture that is not
    /// ours, the wrong layer, and the wrong grid size.
    func testRefusals() throws {
        let white = try scaled(try BubbleData.image(symbols: [], geometry: .board243), width: 243, height: 244,
                               quality: .none)
        let ctx = try XCTUnwrap(CGContext(data: nil, width: 243, height: 244, bitsPerComponent: 8, bytesPerRow: 0,
                                          space: BubbleData.colorSpace, bitmapInfo: BubbleData.bitmapInfo))
        ctx.setFillColor(gray: 1, alpha: 1)
        ctx.fill(CGRect(x: 0, y: 0, width: 243, height: 244))
        let blank = try XCTUnwrap(ctx.makeImage())
        XCTAssertThrowsError(try BubbleData.symbols(from: blank, cells: 243)) {
            XCTAssertEqual($0 as? BubbleDataError, .magic)
        }
        // An empty board is ours, and reads as no symbols.
        XCTAssertEqual(try BubbleData.symbols(from: white, cells: 243).symbols, [])

        var rng = SplitMix(state: 5)
        let board = try BubbleData.image(symbols: rng.symbols(1000), geometry: .board243)
        XCTAssertThrowsError(try BubbleData.bytes(from: board, cells: 243)) {
            XCTAssertEqual($0 as? BubbleDataError, .kind)
        }
        // Read as a 240 grid, the header row is in the wrong place.
        XCTAssertThrowsError(try BubbleData.symbols(from: board, cells: 240))
        XCTAssertThrowsError(try BubbleData.image(symbols: [3], geometry: .board243)) {
            XCTAssertEqual($0 as? BubbleDataError, .symbol)
        }
    }
}
