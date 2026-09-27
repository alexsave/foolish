// KernelSeam.swift - THE ONE SEAM between the screens and the kernel.
//
// Everything a screen draws is a field of `TableModel`, and everything a touch
// does is one method of `Kernel`. Swift decides no rule, composes no sentence
// and derives no game number: legality is `Menu`, which dice count at a
// reveal is `Reveal.counts`, every word is `Kernel.word` or a string field of
// the model. The screens never import the kernel module.
//
// THE SCAFFOLD RUNS ON `FakeKernel` (below), which plays scripted data so
// every screen can be looked at before chuiniu/c exists. The fake is the ONLY
// place a fake value lives. The tie-together step:
//   1. adds a `BridgeKernel: Kernel` beside this file that reads CChuiniu
//      (cn_api.h) and Generated/ChuiniuKernel.swift into a TableModel;
//   2. makes `KernelSeam.make()` return it;
//   3. deletes `FakeKernel` and everything below its MARK.
// Nothing outside this file changes, unless the kernel's model needs a field
// the screens do not draw yet.

import Foundation

// MARK: - the model the screens draw

public enum Phase: Equatable, Sendable {
    /// Seats are filling; nobody has dice.
    case lobby
    /// A round is being bid.
    case bidding
    /// The last bid was called: every die is face up.
    case revealed
    /// One seat holds dice; the game is done.
    case over
}

/// A bid: `quantity` dice showing `face` (2 to 6) among every die on the table.
public struct Bid: Equatable, Sendable {
    public var quantity: Int
    public var face: Int
    public init(quantity: Int, face: Int) {
        self.quantity = quantity
        self.face = face
    }
}

public struct SeatModel: Equatable, Sendable, Identifiable {
    /// The seat index, 0 to 5, in seat order.
    public var id: Int
    public var name: String
    /// How many dice the seat still holds (public: every cup's count is seen).
    public var dice: Int
    public var alive: Bool
    public var isTurn: Bool
    public var isMe: Bool
    public init(id: Int, name: String, dice: Int, alive: Bool, isTurn: Bool, isMe: Bool) {
        self.id = id
        self.name = name
        self.dice = dice
        self.alive = alive
        self.isTurn = isTurn
        self.isMe = isMe
    }
}

/// What the player on turn may do: the kernel's verdict, never Swift's.
public struct Menu: Equatable, Sendable {
    /// The lowest legal raise; the picker opens on it.
    public var minimumRaise: Bid
    /// For each face 2 to 6, the least quantity a raise on that face may name
    /// (index = face; 0 and 1 unused). A face with no legal raise has a value
    /// above `maxQuantity`.
    public var minQuantityByFace: [Int]
    /// The stepper's upper bound: every die on the table.
    public var maxQuantity: Int
    public var callAllowed: Bool

    public init(minimumRaise: Bid, minQuantityByFace: [Int], maxQuantity: Int, callAllowed: Bool) {
        self.minimumRaise = minimumRaise
        self.minQuantityByFace = minQuantityByFace
        self.maxQuantity = maxQuantity
        self.callAllowed = callAllowed
    }

    /// The least quantity the kernel allows on `face`, or nil for a face it
    /// did not rank. A lookup, not a rule.
    public func minQuantity(face: Int) -> Int? {
        minQuantityByFace.indices.contains(face) && face >= 2 ? minQuantityByFace[face] : nil
    }
}

/// Every die face up after a call.
public struct Reveal: Equatable, Sendable {
    /// Each seat's dice (index = seat; empty for a seat that is out).
    public var dice: [[Int]]
    /// Whether each of those dice counts toward the called bid (its face or a
    /// wild 1): the kernel's tally, drawn as it says.
    public var counts: [[Bool]]
    /// The bid that was called.
    public var bid: Bid
    /// The kernel's words for the tally ("Five 3s on the table").
    public var tally: String
    /// The seat that loses a die.
    public var loser: Int
    /// This phone may start the next round.
    public var nextAllowed: Bool

    public init(dice: [[Int]], counts: [[Bool]], bid: Bid, tally: String, loser: Int, nextAllowed: Bool) {
        self.dice = dice
        self.counts = counts
        self.bid = bid
        self.tally = tally
        self.loser = loser
        self.nextAllowed = nextAllowed
    }
}

/// The lobby's one control for this phone (pickemup's pk_lobby_offered).
public enum LobbyOffer: Equatable, Sendable {
    case join, start, waiting, full, alone
}

public struct TableModel: Equatable, Sendable {
    public var phase: Phase
    public var seats: [SeatModel]
    /// My seat, nil for a phone that has not joined (a spectator).
    public var me: Int?
    /// My own dice this round, face values 1 to 6 (empty in the lobby).
    public var myDice: [Int]
    /// The bid on the table, nil before the round's opening bid.
    public var bid: Bid?
    /// The kernel's words for the bid ("four 3s"), "" when there is none.
    public var bidText: String
    /// Who made the bid on the table.
    public var bidder: Int?
    /// Every seat's dice, once the bid is called (and at the end).
    public var reveal: Reveal?
    /// The kernel's caption: the headline under the table, and the bubble's.
    public var caption: String
    /// My choices when it is my turn to bid; nil otherwise.
    public var menu: Menu?
    /// The lobby's control for this phone.
    public var offered: LobbyOffer
    /// Changes when the round's dice change, so the roll plays once per
    /// round. An identity, not a number anything is computed from.
    public var rollID: Int
    /// The winner, when over.
    public var winner: Int?

    public init(phase: Phase, seats: [SeatModel], me: Int?, myDice: [Int], bid: Bid?, bidText: String,
                bidder: Int?, reveal: Reveal?, caption: String, menu: Menu?, offered: LobbyOffer,
                rollID: Int, winner: Int?) {
        self.phase = phase
        self.seats = seats
        self.me = me
        self.myDice = myDice
        self.bid = bid
        self.bidText = bidText
        self.bidder = bidder
        self.reveal = reveal
        self.caption = caption
        self.menu = menu
        self.offered = offered
        self.rollID = rollID
        self.winner = winner
    }

    public static let empty = TableModel(phase: .lobby, seats: [], me: nil, myDice: [], bid: nil, bidText: "",
                                         bidder: nil, reveal: nil, caption: "", menu: nil, offered: .waiting,
                                         rollID: 0, winner: nil)
}

/// Every fixed word a screen shows. The kernel's string table answers; the
/// scaffold's fake answers in English.
public enum Word: CaseIterable, Sendable {
    case gameTitle
    case lobbyTitle, lobbyWaiting, lobbyFull, lobbyAlone
    case join, start
    case raise, call, nextRound
    case quantity, face
    case yourDice, loses, wins, out
    case namePrompt
}

// MARK: - the kernel, as the screens and the conversation see it

/// One call per touch. A method that answers `true` has left a bubble to
/// stage (`stagedURL()`, captioned `table.caption`).
@MainActor
public protocol Kernel: AnyObject {
    /// The resident game as the screens draw it, read fresh.
    var table: TableModel { get }
    func word(_ w: Word) -> String

    /// This device's participant identity in the conversation.
    func me(_ participant: Data)
    /// A new lobby with me in seat 0.
    func newGame(dm: Bool) -> Bool
    /// The kernel's verdict on a nickname typed into the lobby.
    func nameAccepted(_ name: String) -> Bool
    func join(name: String) -> Bool
    func start() -> Bool
    func raise(quantity: Int, face: Int) -> Bool
    func call() -> Bool
    func nextRound() -> Bool

    /// Adopt a bubble's link as the resident game: 0, or the kernel's
    /// negative error, and then nothing changed.
    func adoptBubble(_ url: URL) -> Int
    /// The link of the resident game, to stage.
    func stagedURL() -> URL?
    /// `url` went out: it is the authority (pickemup's didStartSending rule).
    func sent(_ url: URL)
    /// Messages' X on the staged bubble: the draft is taken back.
    func cancelStaged()
    /// Both links are one game.
    func sameGame(_ a: URL, _ b: URL) -> Bool
    /// `a` is later in the game than `b` (the kernel's ranking).
    func isNewer(_ a: URL, than b: URL) -> Bool
}

public enum KernelSeam {
    /// The kernel the extension runs on. The tie-together step returns the
    /// bridge here; until then, the scripted fake.
    @MainActor
    public static func make() -> Kernel {
#if DEBUG
        return FakeKernel(scene: ChuiniuDev.scene ?? .lobby)
#else
        return FakeKernel(scene: .lobby)
#endif
    }
}

// MARK: - FakeKernel: scripted data, deleted by the tie-together step
//
// EVERY VALUE BELOW IS A STAND-IN. The dice are a fixed table, the menu's
// arithmetic and the reveal's tally are placeholders so the screens respond
// to a touch, and the words are English literals. None of it is the rules
// (chuiniu/c is), and nothing outside this section may read or copy it.

@MainActor
public final class FakeKernel: Kernel {
    public enum Scene: String, CaseIterable, Sendable {
        case lobby, invited, bidding, waiting, revealed, over
    }

    private var names = ["Alex", "Bo", "Cy", "Dee"]
    private var diceLeft = [5, 5, 5, 5]
    private var phase: Phase = .lobby
    private var joined = true
    private var turn = 0
    private var bid: Bid?
    private var bidder: Int?
    private var round = 0
    private var reveal: Reveal?
    private var stagedN = 0
    private var mySeat = 0

    /// A fixed table of rolls, one row per round, one entry per seat.
    private static let rolls: [[[Int]]] = [
        [[3, 1, 5, 3, 6], [2, 3, 3, 4, 1], [6, 6, 2, 5, 3], [1, 4, 4, 2, 5]],
        [[2, 2, 6, 1, 4], [5, 3, 1, 6, 6], [4, 4, 3, 2, 2], [3, 6, 5, 5, 1]],
        [[6, 5, 1, 1, 2], [4, 2, 2, 3, 5], [1, 3, 6, 4, 4], [5, 5, 2, 6, 3]],
    ]

    public init(scene: Scene = .lobby) {
        switch scene {
        case .lobby:
            joined = true
        case .invited:
            joined = false
            names = ["Bo", "Cy", "Dee"]
            diceLeft = [5, 5, 5]
            mySeat = -1
        case .bidding:
            phase = .bidding
            bid = Bid(quantity: 4, face: 3)
            bidder = 3
            turn = mySeat
        case .waiting:
            phase = .bidding
            bid = Bid(quantity: 5, face: 3)
            bidder = mySeat
            turn = 1
        case .revealed:
            phase = .bidding
            bid = Bid(quantity: 5, face: 3)
            bidder = 3
            turn = mySeat
            _ = call()
        case .over:
            diceLeft = [2, 0, 0, 0]
            phase = .over
            round = 1
            reveal = revealFor(Bid(quantity: 2, face: 6), caller: 1, bidderSeat: mySeat)
        }
    }

    private func dice(_ seat: Int) -> [Int] {
        Array(Self.rolls[round % Self.rolls.count][seat].prefix(diceLeft[seat]))
    }

    private func nextAlive(after s: Int) -> Int {
        var i = s
        repeat { i = (i + 1) % names.count } while diceLeft[i] == 0 && i != s
        return i
    }

    private static let numberWords = ["zero", "one", "two", "three", "four", "five", "six", "seven", "eight",
                                      "nine", "ten", "eleven", "twelve", "thirteen", "fourteen", "fifteen",
                                      "sixteen", "seventeen", "eighteen", "nineteen", "twenty"]

    private static func words(_ b: Bid) -> String {
        let n = b.quantity < numberWords.count ? numberWords[b.quantity] : "\(b.quantity)"
        return "\(n) \(b.face)s"
    }

    private var totalDice: Int { diceLeft.reduce(0, +) }

    private var menu: Menu? {
        guard phase == .bidding, turn == mySeat else { return nil }
        var mins = [Int](repeating: 0, count: 7)
        if let b = bid {
            for f in 2...6 { mins[f] = f > b.face ? b.quantity : b.quantity + 1 }
            let first = (2...6).min { (mins[$0], $0) < (mins[$1], $1) } ?? 2
            return Menu(minimumRaise: Bid(quantity: mins[first], face: first), minQuantityByFace: mins,
                        maxQuantity: totalDice, callAllowed: true)
        }
        for f in 2...6 { mins[f] = 1 }
        return Menu(minimumRaise: Bid(quantity: 1, face: 2), minQuantityByFace: mins,
                    maxQuantity: totalDice, callAllowed: false)
    }

    private var caption: String {
        switch phase {
        case .lobby:
            return "\(names[0]) wants to play \(word(.gameTitle))"
        case .bidding:
            guard let b = bid, let by = bidder else { return turn == mySeat ? "Your bid" : "\(names[turn]) to bid" }
            let who = by == mySeat ? "You" : names[by]
            let next = turn == mySeat ? "your turn" : "\(names[turn]) to bid"
            return "\(who) bid \(Self.words(b)), \(next)"
        case .revealed:
            guard let r = reveal else { return "" }
            return "\(r.tally). \(r.loser == mySeat ? "You lose" : "\(names[r.loser]) loses") a die"
        case .over:
            let w = diceLeft.firstIndex { $0 > 0 } ?? 0
            return w == mySeat ? "You win" : "\(names[w]) wins"
        }
    }

    public var table: TableModel {
        let seats = names.indices.map { s in
            SeatModel(id: s, name: joined && s == mySeat ? "\(names[s]) (You)" : names[s], dice: diceLeft[s],
                      alive: diceLeft[s] > 0, isTurn: phase == .bidding && s == turn, isMe: joined && s == mySeat)
        }
        return TableModel(phase: phase, seats: seats, me: joined ? mySeat : nil,
                          myDice: phase == .lobby || !joined ? [] : dice(mySeat),
                          bid: bid, bidText: bid.map(Self.words) ?? "", bidder: bidder,
                          reveal: reveal, caption: caption, menu: menu,
                          offered: !joined ? .join : (mySeat == 0 ? .start : .waiting), rollID: round + 1,
                          winner: phase == .over ? diceLeft.firstIndex { $0 > 0 } : nil)
    }

    public func word(_ w: Word) -> String {
        switch w {
        case .gameTitle: return "Chui Niu"
        case .lobbyTitle: return "Chui Niu"
        case .lobbyWaiting: return "Waiting for the host to start"
        case .lobbyFull: return "The table is full"
        case .lobbyAlone: return "Waiting for someone to join"
        case .join: return "Join"
        case .start: return "Start"
        case .raise: return "Raise"
        case .call: return "Call"
        case .nextRound: return "Next round"
        case .quantity: return "How many"
        case .face: return "Of"
        case .yourDice: return "Your dice"
        case .loses: return "loses a die"
        case .wins: return "wins"
        case .out: return "OUT"
        case .namePrompt: return "Your name"
        }
    }

    public func me(_ participant: Data) {}
    public func newGame(dm: Bool) -> Bool { stagedN += 1; return true }
    public func nameAccepted(_ name: String) -> Bool { !name.isEmpty && name.count <= 16 }
    public func join(name: String) -> Bool {
        guard !joined else { return false }
        names.append(name.isEmpty ? "Alex" : name)
        diceLeft.append(5)
        mySeat = names.count - 1
        joined = true
        stagedN += 1
        return true
    }

    public func start() -> Bool {
        guard phase == .lobby else { return false }
        phase = .bidding
        turn = mySeat
        stagedN += 1
        return true
    }

    public func raise(quantity: Int, face: Int) -> Bool {
        guard let m = menu, let least = m.minQuantity(face: face), quantity >= least else { return false }
        bid = Bid(quantity: quantity, face: face)
        bidder = mySeat
        // the scripted table answers at once: the next seat raises by one
        let bo = nextAlive(after: mySeat)
        if quantity + 1 <= totalDice {
            bid = Bid(quantity: quantity + 1, face: face)
            bidder = bo
            turn = mySeat
        } else {
            turn = bo
        }
        stagedN += 1
        return true
    }

    private func revealFor(_ b: Bid, caller: Int, bidderSeat: Int) -> Reveal {
        let all = names.indices.map { dice($0) }
        let counts = all.map { $0.map { $0 == b.face || $0 == 1 } }
        let n = counts.joined().filter { $0 }.count
        let loser = n >= b.quantity ? caller : bidderSeat
        let nw = n < Self.numberWords.count ? Self.numberWords[n] : "\(n)"
        return Reveal(dice: all, counts: counts, bid: b, tally: "\(nw.capitalized) \(b.face)s on the table",
                      loser: loser, nextAllowed: phase != .over)
    }

    public func call() -> Bool {
        guard let b = bid, let by = bidder, menu?.callAllowed == true else { return false }
        reveal = revealFor(b, caller: mySeat, bidderSeat: by)
        phase = .revealed
        stagedN += 1
        return true
    }

    public func nextRound() -> Bool {
        guard phase == .revealed, let r = reveal else { return false }
        diceLeft[r.loser] = max(0, diceLeft[r.loser] - 1)
        if diceLeft.filter({ $0 > 0 }).count <= 1 {
            phase = .over
            return true
        }
        round += 1
        reveal = nil
        bid = nil
        bidder = nil
        turn = diceLeft[r.loser] > 0 ? r.loser : nextAlive(after: r.loser)
        phase = .bidding
        if turn != mySeat {
            // the scripted opener
            bid = Bid(quantity: 2, face: 4)
            bidder = turn
            turn = mySeat
        }
        stagedN += 1
        return true
    }

    public func adoptBubble(_ url: URL) -> Int { url.host == "chuiniu.invalid" ? 0 : -1 }
    public func stagedURL() -> URL? { URL(string: "https://chuiniu.invalid/fake?n=\(stagedN)") }
    public func sent(_ url: URL) {}
    public func cancelStaged() {}
    public func sameGame(_ a: URL, _ b: URL) -> Bool { a.host == b.host }
    public func isNewer(_ a: URL, than b: URL) -> Bool { Self.n(a) > Self.n(b) }
    private static func n(_ u: URL) -> Int {
        Int(URLComponents(url: u, resolvingAgainstBaseURL: false)?.queryItems?.first { $0.name == "n" }?.value ?? "") ?? 0
    }
}
