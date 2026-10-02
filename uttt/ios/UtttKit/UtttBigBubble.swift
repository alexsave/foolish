#if UTTT_BIG_BOARD
import CUttt
import Messages

/// THE BIG GAME'S BUBBLE PICTURE (docs/BIG_BOARD.md): the 59,049 cells of the
/// board drawn by BubbleDataKit as the bubble's picture, and read back.
///
/// THREE PIXELS A CELL (`.robust243`, 729 x 732). One pixel a cell carries a
/// dense board through Messages' two JPEG passes but not a real game's sparse
/// one: the reading of an almost-empty board lost cells, measured in the
/// kit's tests (BubbleData.swift's comment on `robust243`).
///
/// The kit's two sources are compiled into UtttKit, and only when the feature
/// is (project.yml); this is the one place they are called from, and the
/// extension stages and reads through these two calls.
public enum UtttBigBubble {
    /// Put `cells` (UtttBig.cells) on `layout` as its picture.
    public static func put(cells: [UInt8], on layout: MSMessageTemplateLayout) throws {
        try BubbleData.put(symbols: cells, geometry: .robust243, on: layout)
    }

    /// The cells a big bubble's picture reads back to, or nil when it has no
    /// picture or the kit refuses it. A reading's margins are logged, so a
    /// real send's distance from a misread shows in the device log, and it
    /// goes into the diagnostics' history (UtttBigDiag).
    public static func cells(from message: MSMessage) -> [UInt8]? {
        let r = inspect(message, pixels: false)
        UtttBigDiag.recordRead(message, r)
        guard r.result == UTI_BIG_DIAG_R_OK, let symbols = r.symbols else {
            UtttLog.fault("big-read", "refused: \(r.error.map { "\($0)" } ?? "no picture")")
            return nil
        }
        UtttLog.note("big-read", "\(symbols.count) cells, risky \(r.risky), min margin \(r.minMargin), \(r.micros / 1000) ms")
        return symbols
    }

    /// What reading a bubble's picture was like: the kit's verdict and
    /// reading, how long it took, and (with `pixels`) the picture drawn at
    /// its own size for the diagnostics' greys.
    struct Inspection {
        var result: Int32 = UTI_BIG_DIAG_R_NO_PICTURE
        var error: BubbleDataError?
        var symbols: [UInt8]?
        var cells = 0, risky = 0, minMargin = 0
        var micros = 0
        var width = 0, height = 0
        var rgba: [UInt8] = []
    }

    /// Read `message`'s picture with the kit at the size it arrived.
    static func inspect(_ message: MSMessage, pixels: Bool) -> Inspection {
        var r = Inspection()
        let start = DispatchTime.now().uptimeNanoseconds
        guard let cg = BubbleData.picture(of: message) else { return r }
        do {
            let (symbols, reading) = try BubbleData.symbols(from: cg, cells: UtttBig.side)
            r.result = UTI_BIG_DIAG_R_OK
            r.symbols = symbols
            r.cells = reading.cells; r.risky = reading.risky; r.minMargin = reading.minMargin
        } catch let e as BubbleDataError {
            r.error = e
            r.result = code(e)
        } catch {
            r.result = UTI_BIG_DIAG_R_IMAGE
        }
        r.micros = Int((DispatchTime.now().uptimeNanoseconds - start) / 1000)
        r.width = cg.width
        r.height = cg.height
        if pixels { r.rgba = (try? BubbleData.pixels(of: cg)) ?? [] }
        return r
    }

    private static func code(_ e: BubbleDataError) -> Int32 {
        switch e {
        case .geometry: return UTI_BIG_DIAG_R_GEOMETRY
        case .capacity: return UTI_BIG_DIAG_R_CAP
        case .magic:    return UTI_BIG_DIAG_R_MAGIC
        case .version:  return UTI_BIG_DIAG_R_VERSION
        case .kind:     return UTI_BIG_DIAG_R_KIND
        case .length:   return UTI_BIG_DIAG_R_LENGTH
        case .checksum: return UTI_BIG_DIAG_R_CHECK
        case .symbol:   return UTI_BIG_DIAG_R_SYMBOL
        case .image:    return UTI_BIG_DIAG_R_IMAGE
        }
    }
}
#endif
