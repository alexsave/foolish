import CoreGraphics

/// What the screen is allowed to know. Every answer comes from the kernel;
/// this only remembers which of them is on screen and how far a stroke has
/// been drawn.
@MainActor
public final class UtttModel {
    /// The position changed and this device can no longer move in it: the
    /// host stages on this. Bumped once a move's ink is down, and every bump
    /// after the first is told to `onPosition`.
    public private(set) var positionKey = 0 {
        didSet { if positionKey != oldValue { onPosition?() } }
    }
    /// The host's ear for `positionKey`.
    public var onPosition: (() -> Void)?
    /// The screen's ear: something it draws changed (the board's position,
    /// the words, the draft).
    public var onChange: (() -> Void)?
    /// EVERY POSITION THE BOARD SHOWS, bumped the moment the kernel's game
    /// changes - before a move's ink starts, not after it lands - so the
    /// cached board under the ink is always the one the ink belongs to. The
    /// board is keyed on this and not on the clock, so it is not rebuilt
    /// every display frame (UtttLiveBoard).
    public private(set) var boardKey = 0 { didSet { onChange?() } }
    /// The one motion loop: what the board looks like this frame is the
    /// kernel's answer to (plan, clock), and nothing here times anything.
    public let clock = UtttMotionClock()
    public private(set) var busy = false

    public private(set) var seed: Int32
    public private(set) var you: Uttt.Mark

    /// NOBODY PLAYS A BOT HERE. The bots exist - `random` through `nib`, in
    /// `uttt/c/src/uttt_bots.c` - and they were written to answer one
    /// question, how long a real game's code is, which needed thousands of
    /// plausible games and no human. They are research. This game is a thing
    /// two people do to each other in a thread, and an opponent that is
    /// always available and never loses interest is the opposite of that.
    ///
    /// So there is no `solo`, no reply, and no name to put on the other side
    /// of the bar: it is whoever has the other seat, drawn as their mark.
    /// THE POSITION IS ALREADY IN THE KERNEL when this is made - the host
    /// read the message first - so the model does not start a game of its
    /// own. It only remembers which seed draws the marks and which mark is
    /// this device's.
    public init(seed: Int32, you: Uttt.Mark = .x) {
        self.seed = seed; self.you = you
    }

    public var active: Int {
        guard Uttt.over == .none else { return -1 }
        let f = Uttt.forced
        return (f != 255 && Uttt.block(f) == .none) ? f : 9
    }
    public var last: Int { Uttt.plyCount > 0 ? Uttt.move(at: Uttt.plyCount - 1) : -1 }

    /// WHAT THE BAR SAYS, and the other side is NAMED BY ITS MARK - drawn,
    /// not spelled. "Waiting on O" puts a letter of the alphabet where the
    /// rest of the screen has a pen stroke, and the same screen is already
    /// showing "you are" over a drawn mark two inches away. A mark is also
    /// the only name this side actually has: a Messages extension gets a
    /// per-device UUID and no display name.
    public enum Headline: Equatable {
        case text(String)
        /// Words, a drawn mark, words: "Waiting on <O>", "<X> wins".
        case mark(String, Uttt.Mark, String)
    }

    /// THE WORDS WAIT FOR THE INK (UI.html: "Your move" once their mark has
    /// landed, not before it is drawn - sheet 5). False from the moment a
    /// move starts to draw until the kernel's frame says it has landed; the
    /// words meanwhile are the position one ply back.
    public private(set) var inked = true { didSet { if inked != oldValue { onChange?() } } }

    /// The words are the kernel's; this only says where the drawn mark goes.
    public var headline: Headline {
        let pre = inked ? Uttt.say(.headlinePre) : Uttt.sayBefore(.headlinePre)
        let post = inked ? Uttt.say(.headlinePost) : Uttt.sayBefore(.headlinePost)
        let m = inked ? Uttt.sayMark : Uttt.sayMarkBefore
        return m == .none ? .text(pre + post) : .mark(pre, m, post)
    }

    /// The line under it. When it is not your turn this is WHERE YOU SENT
    /// THEM, which is the one thing worth reading on a board you cannot touch.
    public var subline: String { inked ? Uttt.say(.subline) : Uttt.sayBefore(.subline) }

    /// Run the clock through `ch` with the words held until the ink lands.
    private func run(_ ch: Uttt.Channel, then: (() -> Void)? = nil) {
        boardKey &+= 1
        inked = false
        clock.run(ch) { [weak self] in
            self?.inked = true
            then?()
        }
    }

    /// The harness loads a position behind the model's back; this is how it
    /// tells the screen to look again.
    public func refresh() {
        positionKey &+= 1
        boardKey &+= 1
        inked = true
        clock.run(.still)
    }

    /// THE POST-SETTLEMENT, on the board already up: the draft was sent, so
    /// the highlighter goes to the outlined block. Nothing else about the
    /// board changes, so the host is not told the position moved and the
    /// screen is not rebuilt - a new screen at the moment of Send was a
    /// re-render the drawer could be seen replaying.
    public func sent() {
        pending = false
        run(.settle)
    }

    /// The last move arriving through `ch` - a bubble opened (C or D) or a
    /// move that landed while this board was up (E).
    public func show(_ ch: Uttt.Channel) {
        positionKey &+= 1
        run(ch)
    }

    /// A MOVE THAT IS STAGED IS STILL A DRAFT. It sits in the input field
    /// with an X on it until a human taps the arrow, so until then the player
    /// is allowed to change their mind - and the way they say so is by
    /// tapping a different square, not by hunting for Apple's little X.
    ///
    /// True while this device's last move is staged and unsent.
    public private(set) var pending = false { didSet { if pending != oldValue { onChange?() } } }

    /// A tap in the board's own 0..1 space. WHICH SQUARE is the kernel's
    /// answer, from the same geometry it draws the board with, and so is
    /// whether it may be played.
    ///
    /// A TAP THAT IS NOT A MOVE DOES NOTHING - no redraw, no replay, no
    /// restage (owner, 2026-09-23): outside the block in play, an occupied
    /// square, the gap between squares, the same square again.
    public func tap(at p: CGPoint) {
        guard !busy, let mv = Uttt.hit(p) else { return }
        if pending {
            /* A CHANGE OF MIND: another free square where the draft was
             * played. The kernel's question, and a winning draft can be
             * changed like any other. */
            guard Uttt.canReplace(mv) else { return }
            Task { await replace(with: mv) }
            return
        }
        guard Uttt.over == .none, Uttt.canMove, Uttt.legal.contains(UInt8(mv)) else { return }
        Task { await playerMove(mv) }
    }

    /// Take the staged move back and play another one.
    ///
    /// IN ONE STEP (owner, 2026-09-23): the old mark - and the big mark, if
    /// it had won its block - is gone at once and the new one draws in, with
    /// its own settlement and its outline. Nothing is un-drawn and the
    /// highlighter never moves (it is on the block both drafts were played
    /// in). The undo and the play are one kernel change before the next
    /// frame, so the board never shows the position between them.
    private func replace(with mv: Int) async {
        busy = true
        guard Uttt.undoMine() else { busy = false; return }
        guard Uttt.playAsMe(mv) else {
            /* canReplace said yes; put the draft back rather than lose it */
            if lastPlayed >= 0 { _ = Uttt.playAsMe(lastPlayed) }
            busy = false
            return
        }
        lastPlayed = mv
        await draw(mv)
        positionKey &+= 1
        busy = false
    }

    /// The move this device staged, so a rejected replacement can be put back.
    private var lastPlayed = -1

    /// The host says whether a draft is outstanding: it is the only thing that
    /// knows whether the bubble is still in the input field.
    public func setPending(_ on: Bool) { pending = on }

    /// ON AN OPEN INVITATION THIS MOVE IS THE JOIN: the kernel seats this
    /// device in X and seals the roster as part of playing it.
    private func playerMove(_ mv: Int) async {
        busy = true
        guard Uttt.playAsMe(mv) else { busy = false; return }
        lastPlayed = mv
        await draw(mv)
        positionKey &+= 1
        busy = false
    }

    /// THE MOVE MOVES, and the host hears about it only when the ink is
    /// down: it stages then, which is what collapses the drawer, and UI.html
    /// says the drawer moves once the ink lands, never during. How long that
    /// is, is the kernel's plan.
    private func draw(_ mv: Int) async {
        _ = mv
        await withCheckedContinuation { (k: CheckedContinuation<Void, Never>) in
            run(.stage) { k.resume() }
        }
    }
}
