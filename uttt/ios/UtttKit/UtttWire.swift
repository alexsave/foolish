import Foundation

/// What a bubble carries: an opaque string the kernel wrote.
///
/// AN EXTENSION CANNOT ENUMERATE THE TRANSCRIPT. It is handed exactly one
/// message - the one that was tapped - so every bubble carries the whole game
/// and its whole roster, about fifty bytes.
///
/// THIS FILE KNOWS NOTHING ABOUT THOSE BYTES. The layout, the text form, the
/// seat tags and every rule about who may do what are in
/// `uttt/c/src/uttt_msg.h`; this only moves the kernel's string in and out of
/// an `MSMessage.url`. If a method here ever parses, hashes or encodes
/// something, that is a rule on the wrong side of the line.
public struct UtttWire: Equatable {

    /// The kernel's text for one message: `?m=<base32>`.
    public let text: String

    /// The resident message, as the kernel writes it. Nil only if the
    /// resident position cannot be a message (a seeded debug game at ply 0).
    public static var resident: UtttWire? {
        Uttt.messageText.map(UtttWire.init(text:))
    }

    public init(text: String) { self.text = text }

    /// A bubble's URL, if it carries anything at all. Whether it is a message
    /// this build reads is the kernel's question: see `readable`.
    public init?(url: URL?) {
        guard let s = url?.absoluteString, !s.isEmpty else { return nil }
        text = s
    }

    /// NO SCHEME, BECAUSE MESSAGES DROPS ONE IT WILL NOT VOUCH FOR. This was
    /// `uttt://g?...` and the assignment `message.url = wire.url` came back
    /// NIL on the very next line - silently, with the bubble still sending.
    /// Measured at the moment of assignment:
    ///
    ///     uttt://g?v=1&s=2                 dropped
    ///     uttt:?v=1&s=2                    dropped
    ///     ?v=1&s=2                         kept
    ///     https://foolish.cards/u?v=1&s=2  kept
    ///
    /// So the kernel writes a bare query, which is what Apple's own sample
    /// carries and promises no destination.
    public var url: URL { URL(string: text) ?? URL(fileURLWithPath: "uttt") }

    public var readable: Bool { Uttt.readable(text) }

    /// Put this message into the kernel - roster and game - and leave it
    /// there. False means it is not one this build reads.
    @discardableResult
    public func load() -> Bool { Uttt.read(text) }

    /// Whether two bubbles are the same game rather than the same shape.
    public func isSameGame(as other: UtttWire) -> Bool { Uttt.sameGame(text, other.text) }
}
