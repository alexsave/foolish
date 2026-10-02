#if UTTT_BIG_BOARD
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
    /// real send's distance from a misread shows in the device log.
    public static func cells(from message: MSMessage) -> [UInt8]? {
        do {
            let (symbols, reading) = try BubbleData.symbols(from: message, cells: UtttBig.side)
            UtttLog.note("big-read", "\(symbols.count) cells, risky \(reading.risky), min margin \(reading.minMargin)")
            return symbols
        } catch {
            UtttLog.fault("big-read", "refused: \(error)")
            return nil
        }
    }
}
#endif
