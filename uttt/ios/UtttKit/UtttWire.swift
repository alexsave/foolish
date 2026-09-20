import CryptoKit
import Foundation

/// What a bubble carries, and the only thing it carries.
///
/// AN EXTENSION CANNOT ENUMERATE THE TRANSCRIPT. It is handed exactly one
/// message - the one that was tapped - and there is no way to walk back to the
/// one before it. So every bubble carries the whole game, which at about
/// twenty-two bytes is four percent of the body budget and leaves nothing to
/// reconstruct.
///
/// Nothing here computes a rule. The game comes out of the kernel as
/// `Uttt.code` and goes back into it through `Uttt.load`; this file knows the
/// bytes are a game and not one thing more about them.
public struct UtttWire: Equatable {

    /// A SHIPPED BUBBLE LIVES FOREVER in somebody's transcript, so the version
    /// is the first field read and a reader that meets a number it does not
    /// know refuses the message rather than misreading it. The codec already
    /// changed shape once (it could not decode a position that was still being
    /// played); the next time it does, this is what keeps an old bubble from
    /// decoding into a plausible wrong game.
    public static let version = 1

    /// The two seats, in the order they were taken. Which one is X is the
    /// seed's business, not this enum's.
    public enum Seat: Int { case creator = 0, joiner = 1 }

    /// The moment the first board was composed. Every later bubble copies it.
    public let seed: Int32
    /// The seat tag of whoever sent the empty board.
    public let creator: String
    /// The seat tag of whoever answered it, or nil while the seat is open.
    /// ONCE THIS IS SET THE ROSTER IS SEALED - a third tap in a group chat
    /// matches neither tag and gets a spectator's view, not a seat.
    public let joiner: String?
    /// The whole game, as the kernel codes it. Empty is the empty board.
    public let code: Data

    public init(seed: Int32, creator: String, joiner: String?, code: Data) {
        self.seed = seed
        self.creator = creator
        self.joiner = joiner
        self.code = code
    }

    /// The first bubble: an empty board, one seat taken.
    public static func opening(seed: Int32, creator: String) -> UtttWire {
        Uttt.newGame(seed: seed)
        return UtttWire(seed: seed, creator: creator, joiner: nil, code: Uttt.code)
    }

    // MARK: the seed

    /// THE SEED IS THE SEND TIME OF THE FIRST MESSAGE, and nothing else.
    ///
    /// Not the nicknames: a nickname is under its owner's control, so anybody
    /// who disliked what the seed gave them could rename and reload. The
    /// creator picks the moment, and at that moment they do not yet know who
    /// they are playing.
    ///
    /// (Compose time, strictly. An extension cannot send - `insert()` puts the
    /// bubble in the input field and a human taps the arrow - so the last
    /// moment this code runs is when the board is made, not when it goes.)
    public static func seedNow(_ date: Date = Date()) -> Int32 {
        Int32(truncatingIfNeeded: Int(date.timeIntervalSince1970))
    }

    /// X ALWAYS GOES FIRST, and the seed says which seat is X.
    ///
    /// One bit of a second at which nobody had met their opponent yet. The
    /// joiner cannot re-roll it: they join, and it is either their turn or it
    /// is not, whenever they sent.
    public func mark(of seat: Seat) -> Uttt.Mark {
        let creatorIsX = (seed & 1) == 0
        return (seat == .creator) == creatorIsX ? .x : .o
    }

    // MARK: the roster

    public var isSealed: Bool { joiner != nil }

    /// Which seat a tag holds, or nil for everybody else in the thread.
    public func seat(of tag: String) -> Seat? {
        if tag == creator { return .creator }
        if let joiner, tag == joiner { return .joiner }
        return nil
    }

    /// A seat tag: a value only the device that wrote it can recognise.
    ///
    /// APPLE'S PARTICIPANT UUIDs ARE NOT A SHARED NAME FOR A PERSON. The same
    /// human has a different UUID on their own phone, on their iPad, and in
    /// their opponent's copy of the conversation - so a UUID on the wire is a
    /// value nobody else can compare against anything. That is enough here,
    /// because a seat only ever has to answer one question - "is this me?" -
    /// and the device asking already holds the UUID. It hashes its own and
    /// looks for the result; every other device fails to match, which is the
    /// right answer for every other device.
    ///
    /// Hashed rather than raw so a stable per-device identifier never lands in
    /// a transcript, and salted with the seed so the same device gets a
    /// different tag in every game and the tags cannot be used to follow a
    /// person from one thread into another.
    ///
    /// The cost is a reinstall: a new UUID matches neither seat and its owner
    /// becomes a spectator in their own game. Closing that needs a secret that
    /// outlives the install - the keychain - and is a separate piece of work.
    public static func tag(participant: UUID, seed: Int32) -> String {
        var d = Data("uttt.seat.1|\(seed)|".utf8)
        withUnsafeBytes(of: participant.uuid) { d.append(contentsOf: $0) }
        return b64(Data(SHA256.hash(data: d).prefix(9)))
    }

    // MARK: the game

    /// Put this game into the kernel, and leave it there.
    ///
    /// False means the bytes are not a game: a bubble from a build that does
    /// not exist yet, or one somebody edited. THE KERNEL DECIDES - this never
    /// looks at a byte of the code itself.
    @discardableResult
    public func load() -> Bool {
        Uttt.newGame(seed: seed)
        return code.isEmpty ? true : Uttt.load(code, seed: seed)
    }

    /// How far along this game is, or -1 if it cannot be read.
    ///
    /// LOADS THE GAME to find out, because the kernel is the only thing that
    /// can read the code - so the caller is choosing between two bubbles and
    /// has to load the one it picks afterwards regardless.
    public func plies() -> Int { load() ? Uttt.plyCount : -1 }

    /// The bubble to stage: this game's roster, plus `joining` if the second
    /// seat is still open, over the position the kernel is holding right now.
    public func staging(joining: String? = nil) -> UtttWire {
        UtttWire(seed: seed, creator: creator,
                 joiner: joiner ?? joining, code: Uttt.code)
    }

    /// Whether two bubbles are the same game rather than the same shape.
    public func isSameGame(as other: UtttWire) -> Bool {
        seed == other.seed && creator == other.creator
    }

    // MARK: the URL

    /// `uttt://g?v=1&s=<seed>&a=<tag>&b=<tag>&g=<code>`
    ///
    /// A private scheme rather than an https link to a page that does not
    /// exist. The URL is the payload, not a destination - Messages shows the
    /// layout, and a recipient without the app is offered the App Store - so a
    /// link pointing at a 404 would be a promise the project does not keep.
    public var url: URL {
        var c = URLComponents()
        c.scheme = "uttt"
        c.host = "g"
        var q = [URLQueryItem(name: "v", value: String(Self.version)),
                 URLQueryItem(name: "s", value: String(seed)),
                 URLQueryItem(name: "a", value: creator)]
        if let joiner { q.append(URLQueryItem(name: "b", value: joiner)) }
        if !code.isEmpty { q.append(URLQueryItem(name: "g", value: Self.b64(code))) }
        c.queryItems = q
        // Components carrying a scheme, a host and query items always resolve;
        // the fallback is here so a shipped extension has no force-unwrap.
        return c.url ?? URL(fileURLWithPath: "uttt")
    }

    public static func read(_ url: URL?) -> UtttWire? {
        guard let url,
              let items = URLComponents(url: url, resolvingAgainstBaseURL: false)?.queryItems
        else { return nil }
        func q(_ n: String) -> String? {
            guard let v = items.first(where: { $0.name == n })?.value,
                  !v.isEmpty else { return nil }
            return v
        }
        guard q("v").flatMap({ Int($0) }) == version,
              let seed = q("s").flatMap({ Int32($0) }),
              let creator = q("a") else { return nil }
        let code: Data
        if let g = q("g") {
            guard let d = unb64(g) else { return nil }
            code = d
        } else {
            code = Data()
        }
        return UtttWire(seed: seed, creator: creator, joiner: q("b"), code: code)
    }

    /// base64url, unpadded. `+`, `/` and `=` are all legal in a query value and
    /// all three are re-encoded or re-read differently by somebody along the
    /// way; the URL-safe alphabet has no such argument to lose.
    static func b64(_ d: Data) -> String {
        d.base64EncodedString()
            .replacingOccurrences(of: "+", with: "-")
            .replacingOccurrences(of: "/", with: "_")
            .replacingOccurrences(of: "=", with: "")
    }

    static func unb64(_ s: String) -> Data? {
        var t = s.replacingOccurrences(of: "-", with: "+")
            .replacingOccurrences(of: "_", with: "/")
        while t.count % 4 != 0 { t += "=" }
        return Data(base64Encoded: t)
    }
}
