// BubbleData.swift - the Swift face of CBubbleData (bubble_data.h): a board
// of three-state cells, or a run of bytes, as a grey picture for a Messages
// bubble, and back. Every rule (the frame, the checksum, the levels, the
// thresholds) is in the C; this file only moves pixels between CoreGraphics
// and the C's RGBA buffers and turns its return codes into errors.
import CBubbleData
import CoreGraphics
import Foundation
import ImageIO

/// The shape of a picture: `cells` a side (the board is cells x cells, under
/// a one-cell header row) and `pixelsPerCell` pixels to a cell.
public struct BubbleDataGeometry: Hashable, Sendable {
    public var cells: Int
    public var pixelsPerCell: Int

    public init(cells: Int, pixelsPerCell: Int) {
        self.cells = cells
        self.pixelsPerCell = pixelsPerCell
    }

    /// 243 x 243 cells at one pixel each: a 243 x 244 px picture, about 72 KB
    /// after a real send, and the one that has arrived every time. FOR DENSE
    /// PICTURES ONLY: a sparse board - a game's, a lone mark between empty
    /// cells, and the header row's own count and CRC cells - does NOT survive
    /// the two JPEGs at this size (a lone grey comes back near 200 and reads
    /// as empty; the checksum refuses the picture rather than misread it).
    /// Measured 2026-10-01: uttt/c/tests/uttt_big_chain.c refused 21,330 of
    /// 40,712 positions of one game; testSparseBoardNeedsThreePixelsACell
    /// holds it. A game board uses robust243.
    public static let board243 = BubbleDataGeometry(cells: 243, pixelsPerCell: 1)
    /// The same board at three pixels a cell (729 x 732 px, about 460 KB after
    /// a real send), which carries a sparse board with a margin of about 25
    /// and survives a far harsher encoder or a resize to about 2.5 px a cell.
    public static let robust243 = BubbleDataGeometry(cells: 243, pixelsPerCell: 3)

    /// A picture a send keeps whole: a side of at least 64 cells and no side
    /// over 1200 px.
    public var isValid: Bool {
        guard let n = Int32(exactly: cells), let p = Int32(exactly: pixelsPerCell) else { return false }
        return bd_geometry_ok(n, p) != 0
    }

    public var width: Int { cells * pixelsPerCell }
    public var height: Int { (cells + 1) * pixelsPerCell }

    /// Symbols a board of this side carries (cells x cells), or 0 for a side
    /// the format does not take.
    public var symbolCapacity: Int { BubbleData.symbolCapacity(cells: cells) }
    /// Bytes the byte layer carries (3 for every 16 cells), or 0.
    public var byteCapacity: Int { BubbleData.byteCapacity(cells: cells) }
}

/// One case per BD_E* code, and `image` for a picture CoreGraphics could not
/// make or read.
public enum BubbleDataError: Error, Equatable, Sendable {
    case geometry, capacity, magic, version, kind, length, checksum, symbol
    case image

    init(code: Int32) {
        switch code {
        case BD_EGEOMETRY: self = .geometry
        case BD_ECAP: self = .capacity
        case BD_EMAGIC: self = .magic
        case BD_EVERSION: self = .version
        case BD_EKIND: self = .kind
        case BD_ELENGTH: self = .length
        case BD_ECHECK: self = .checksum
        case BD_ESYMBOL: self = .symbol
        default: preconditionFailure("bubble_data returned \(code), which is not a BD_E* code")
        }
    }
}

/// What reading a picture was like (BdReading): cells read, cells that sat
/// within 16 of a decision threshold, and the smallest such distance (0...64).
/// A picture that read right with many risky cells is one more
/// recompression from reading wrong.
public struct BubbleDataReading: Hashable, Sendable {
    public var cells: Int
    public var risky: Int
    public var minMargin: Int
}

public enum BubbleData {
    public static func symbolCapacity(cells: Int) -> Int {
        guard let n = Int32(exactly: cells) else { return 0 }
        return max(0, Int(bd_symbol_capacity(n)))
    }

    public static func byteCapacity(cells: Int) -> Int {
        guard let n = Int32(exactly: cells) else { return 0 }
        return max(0, Int(bd_byte_capacity(n)))
    }

    // ------------------------------------------------------------- writing

    /// A board as a picture: `symbols` are the cells (each 0, 1 or 2), at
    /// most cells x cells of them, row by row.
    public static func image(symbols: [UInt8], geometry: BubbleDataGeometry) throws -> CGImage {
        let n = try side(geometry)
        var cells = [UInt8](repeating: 0, count: geometry.cells * (geometry.cells + 1))
        guard let count = Int32(exactly: symbols.count) else { throw BubbleDataError.capacity }
        try check(symbols.withUnsafeBufferPointer { s in
            cells.withUnsafeMutableBufferPointer { c in
                bd_frame(s.baseAddress, count, Int32(BD_KIND_SYMBOLS), 0, n, c.baseAddress)
            }
        })
        return try paint(cells, geometry)
    }

    /// Bytes as a picture, 3 bytes to 16 cells; at most `byteCapacity`.
    public static func image(bytes: Data, geometry: BubbleDataGeometry) throws -> CGImage {
        let n = try side(geometry)
        var cells = [UInt8](repeating: 0, count: geometry.cells * (geometry.cells + 1))
        guard let count = Int32(exactly: bytes.count) else { throw BubbleDataError.capacity }
        try check(bytes.withUnsafeBytes { b in
            cells.withUnsafeMutableBufferPointer { c in
                bd_encode_bytes(b.bindMemory(to: UInt8.self).baseAddress, count, n, c.baseAddress)
            }
        })
        return try paint(cells, geometry)
    }

    // ------------------------------------------------------------- reading

    /// The board in a picture of a `cells`-side grid, read at whatever size
    /// the picture came back at. Throws rather than misreads.
    public static func symbols(from image: CGImage, cells: Int) throws
        -> (symbols: [UInt8], reading: BubbleDataReading)
    {
        let (grid, reading, n) = try sample(image, cells: cells)
        var out = [UInt8](repeating: 0, count: cells * cells)
        let got = grid.withUnsafeBufferPointer { g in
            out.withUnsafeMutableBufferPointer { o in
                bd_unframe(g.baseAddress, n, o.baseAddress, Int32(o.count), nil, nil)
            }
        }
        try check(got)
        return (Array(out.prefix(Int(got))), reading)
    }

    /// The bytes in a picture made by `image(bytes:geometry:)`.
    public static func bytes(from image: CGImage, cells: Int) throws
        -> (bytes: Data, reading: BubbleDataReading)
    {
        let (grid, reading, n) = try sample(image, cells: cells)
        var out = [UInt8](repeating: 0, count: byteCapacity(cells: cells))
        let got = grid.withUnsafeBufferPointer { g in
            out.withUnsafeMutableBufferPointer { o in
                bd_decode_bytes(g.baseAddress, n, o.baseAddress, Int32(o.count))
            }
        }
        try check(got)
        return (Data(out.prefix(Int(got))), reading)
    }

    // ------------------------------------------------------------ the pixels

    /// sRGB in and out, so a grey of 128 is 128 in the file and 128 back.
    static let colorSpace = CGColorSpace(name: CGColorSpace.sRGB)!
    static let bitmapInfo = CGImageAlphaInfo.noneSkipLast.rawValue

    private static func side(_ geometry: BubbleDataGeometry) throws -> Int32 {
        guard geometry.isValid else { throw BubbleDataError.geometry }
        return Int32(geometry.cells)
    }

    private static func check(_ code: Int32) throws {
        if code < 0 { throw BubbleDataError(code: code) }
    }

    private static func paint(_ cells: [UInt8], _ geometry: BubbleDataGeometry) throws -> CGImage {
        let w = geometry.width, h = geometry.height
        var rgba = [UInt8](repeating: 0, count: w * h * 4)
        try check(cells.withUnsafeBufferPointer { c in
            rgba.withUnsafeMutableBufferPointer { px in
                bd_paint(c.baseAddress, Int32(geometry.cells), Int32(geometry.pixelsPerCell), px.baseAddress)
            }
        })
        guard let provider = CGDataProvider(data: Data(rgba) as CFData),
              let image = CGImage(width: w, height: h, bitsPerComponent: 8, bitsPerPixel: 32,
                                  bytesPerRow: w * 4, space: colorSpace,
                                  bitmapInfo: CGBitmapInfo(rawValue: bitmapInfo), provider: provider,
                                  decode: nil, shouldInterpolate: false, intent: .defaultIntent)
        else { throw BubbleDataError.image }
        return image
    }

    /// The picture drawn into an sRGB RGBA buffer at its own size, then
    /// sampled by the C into n x (n + 1) cells.
    private static func sample(_ image: CGImage, cells: Int) throws
        -> (grid: [UInt8], reading: BubbleDataReading, n: Int32)
    {
        guard let n = Int32(exactly: cells), bd_symbol_capacity(n) > 0 else { throw BubbleDataError.geometry }
        let w = image.width, h = image.height
        // The C refuses these sizes too; refusing here first means a hostile
        // picture never gets a buffer allocated for it.
        guard w >= cells, h >= cells + 1, w <= Int(BD_MAX_READ_SIDE), h <= Int(BD_MAX_READ_SIDE)
        else { throw BubbleDataError.geometry }
        var rgba = [UInt8](repeating: 0, count: w * h * 4)
        let drawn = rgba.withUnsafeMutableBytes { buf -> Bool in
            guard let ctx = CGContext(data: buf.baseAddress, width: w, height: h, bitsPerComponent: 8,
                                      bytesPerRow: w * 4, space: colorSpace, bitmapInfo: bitmapInfo)
            else { return false }
            ctx.interpolationQuality = .none
            ctx.draw(image, in: CGRect(x: 0, y: 0, width: w, height: h))
            return true
        }
        guard drawn else { throw BubbleDataError.image }
        var grid = [UInt8](repeating: 0, count: cells * (cells + 1))
        var r = BdReading()
        try check(rgba.withUnsafeBufferPointer { px in
            grid.withUnsafeMutableBufferPointer { g in
                bd_sample(px.baseAddress, Int32(w), Int32(h), n, g.baseAddress, &r)
            }
        })
        return (grid, BubbleDataReading(cells: Int(r.cells), risky: Int(r.risky), minMargin: Int(r.min_margin)), n)
    }

    /// A picture from encoded bytes (the JPEG a bubble carries), or nil.
    public static func cgImage(data: Data) -> CGImage? {
        guard let src = CGImageSourceCreateWithData(data as CFData, nil) else { return nil }
        return CGImageSourceCreateImageAtIndex(src, 0, nil)
    }
}

// ---------------------------------------------------------------- UIKit

#if canImport(UIKit)
import UIKit

extension BubbleData {
    /// The picture as a UIImage at scale 1, so a pixel is a point and a
    /// layout draws it at the size it was painted.
    public static func uiImage(symbols: [UInt8], geometry: BubbleDataGeometry) throws -> UIImage {
        UIImage(cgImage: try image(symbols: symbols, geometry: geometry), scale: 1, orientation: .up)
    }

    public static func uiImage(bytes: Data, geometry: BubbleDataGeometry) throws -> UIImage {
        UIImage(cgImage: try image(bytes: bytes, geometry: geometry), scale: 1, orientation: .up)
    }

    /// A UIImage's pixels; a UIImage with no CGImage behind it is drawn into one.
    static func cgImage(of image: UIImage) -> CGImage? {
        if let cg = image.cgImage { return cg }
        let format = UIGraphicsImageRendererFormat()
        format.scale = 1
        return UIGraphicsImageRenderer(size: image.size, format: format).image { _ in image.draw(at: .zero) }.cgImage
    }

    public static func symbols(from image: UIImage, cells: Int) throws
        -> (symbols: [UInt8], reading: BubbleDataReading)
    {
        guard let cg = cgImage(of: image) else { throw BubbleDataError.image }
        return try symbols(from: cg, cells: cells)
    }

    public static func bytes(from image: UIImage, cells: Int) throws
        -> (bytes: Data, reading: BubbleDataReading)
    {
        guard let cg = cgImage(of: image) else { throw BubbleDataError.image }
        return try bytes(from: cg, cells: cells)
    }
}
#endif

// -------------------------------------------------------------- Messages

#if canImport(Messages) && os(iOS)
import Messages

extension BubbleData {
    /// Put a board on a template layout as its picture. Messages turns the
    /// picture into a JPEG when the message goes; the grey levels survive that.
    public static func put(symbols: [UInt8], geometry: BubbleDataGeometry,
                           on layout: MSMessageTemplateLayout) throws
    {
        layout.image = try uiImage(symbols: symbols, geometry: geometry)
    }

    public static func put(bytes: Data, geometry: BubbleDataGeometry,
                           on layout: MSMessageTemplateLayout) throws
    {
        layout.image = try uiImage(bytes: bytes, geometry: geometry)
    }

    /// The picture a message carries: its template layout's `image`, or the
    /// file at `mediaFileURL` when there is no image; for a live layout, its
    /// alternate (template) layout. Nil when the message carries no picture.
    public static func picture(of message: MSMessage) -> CGImage? {
        let template: MSMessageTemplateLayout?
        switch message.layout {
        case let t as MSMessageTemplateLayout: template = t
        case let live as MSMessageLiveLayout: template = live.alternateLayout
        default: template = nil
        }
        guard let layout = template else { return nil }
        if let image = layout.image, let cg = cgImage(of: image) { return cg }
        if let url = layout.mediaFileURL, let data = try? Data(contentsOf: url) { return cgImage(data: data) }
        return nil
    }

    public static func symbols(from message: MSMessage, cells: Int) throws
        -> (symbols: [UInt8], reading: BubbleDataReading)
    {
        guard let cg = picture(of: message) else { throw BubbleDataError.image }
        return try symbols(from: cg, cells: cells)
    }

    public static func bytes(from message: MSMessage, cells: Int) throws
        -> (bytes: Data, reading: BubbleDataReading)
    {
        guard let cg = picture(of: message) else { throw BubbleDataError.image }
        return try bytes(from: cg, cells: cells)
    }
}
#endif
