// FHandFan.swift — the local player's hand, matching the WEB self-hand: a FLAT
// ROW (not a fan). Cards share the width, clamped 20-50pt wide at a fixed 70pt
// tall, no overlap, so every card is directly tappable. Tapping toggles
// multi-selection; DRAGGING a card lifts it out to play (the board resolves the
// drop against the table/battles). The board turns selection+gesture into a move,
// never this view.
//
// Round-5 additions (M5b, M6, drag-feel, drag-hint centering — see each below):
// a compact caller can crop every card to its top half; a very wide hand splits
// into two rows instead of thinning past the touch-target minimum; the drag now
// grabs on first touch instead of after a minimum travel; and the dragged card's
// live centre is reported so the board can centre its verb hint on the card
// instead of the fingertip. `rowCount`/`height` are static and pure specifically
// so MessageTableView can reserve the SAME room this view will actually use,
// rather than carrying a second hard-coded constant that can drift out of sync.
//
// Round-16: the cards are placed ABSOLUTELY from `slotFrames` instead of by a
// VStack of HStacks, and the in-hand reorder is no longer confined to the row
// the card started in. The two go together - dragging a card into the other row
// bumps one card the other way, and a card cannot be seen to travel between two
// containers, only within one.

import SwiftUI

public struct FHandFan: View {
    public let cards: [Card]
    public let trumpSuit: Suit?
    /// Cards currently locked (in-flight / illegal in this context) — dimmed.
    public let disabled: Set<String>
    /// ROUND 43: cards that must not respond to touch and must LOOK EXACTLY AS
    /// THEY DID - no dimming, no selection ring, nothing to notice.
    ///
    /// `disabled` above cannot serve: `FCard` renders it at opacity 0.5, which
    /// is the correct affordance for "you may not play this" and the wrong one
    /// for the message board's `handHoldback`, whose whole purpose is that a
    /// card you have already played keeps sitting in your hand looking ordinary
    /// right up to the moment its replay flies it out. A dimmed card there
    /// announces a state the player cannot act on and did not cause.
    ///
    /// So this is the gesture half of `disabled` with none of the paint. One
    /// gesture covers both interactions - the tap is synthesized inside the drag
    /// gesture's `onEnded` (see `cardView`), so gating the drag gates the tap.
    public let locked: Set<String>
    @Binding public var selection: Set<String>
    /// Tap a card — the board toggles it in the selection.
    public let onTap: (Card) -> Void
    /// Drag updates: the dragged card + the current point in `boardSpace` (for the
    /// live drop-target highlight). Ended: resolve + play the drop.
    public let onDragChanged: (Card, CGPoint) -> Void
    public let onDragEnded: (Card, CGPoint) -> Void
    /// Shared card-flight namespace: a card keeps its identity when it moves from
    /// the hand to the table, so matchedGeometry animates the flight.
    public let namespace: Namespace.ID?
    /// Cards currently in overlay flight (a draw landing) — rendered invisible so
    /// only the flying ghost shows.
    public let hidden: Set<String>
    /// Round-5 M5b ("in the collapsed view, show only the top half of the
    /// cards"): how much of each hand card to hide off the bottom edge, as a
    /// CONTINUOUS fraction — 0 shows the whole card (the full board), 1 reserves
    /// only the top half so the lower half falls past the drawer's bottom edge
    /// (the compact drawer). Any value between the two is a partly-descended
    /// card, which is what lets `MessageTableView` drive this off its continuous
    /// `collapse` fraction so the hand slides DOWN smoothly as the drawer height
    /// changes (round-6 bugs 2/4 — the self cards "gradually go down as we
    /// collapse") instead of the card height halving in one frame at a single
    /// height threshold. Round-6: this replaced the old `topHalfOnly: Bool`; the
    /// Bool is still accepted as an init convenience (it maps to 0 or 1) so every
    /// pre-existing call site (TableView, GalleryView, the snapshot tests) keeps
    /// rendering exactly the full-height card it always did.
    public let crop: CGFloat
    /// Round-5 finding 5 ("the little mid-drag text... should be centered
    /// horizontally on the card(s)"): the dragged card's own LIVE visual centre
    /// in `boardSpace`, delivered on every `onDragChanged` alongside the raw
    /// finger point the board already tracks. Defaulted no-op so every
    /// pre-existing call site compiles unchanged.
    public let onDragCardMoved: (CGPoint) -> Void
    /// Round-6 bug 10: cards present in `cards` (and in the kernel hand) but which
    /// must reserve NO layout width YET — a deal whose flight has not started, so
    /// its slot is not yet cut and the present cards do not slide over to make
    /// room for it "in anticipation". These are still opacity-hidden via `hidden`
    /// if they are also in there; this set governs LAYOUT only. A card leaves this
    /// set the instant its own flight begins (BoardAnimator.openSlots), and the
    /// fan opens for it then. Defaulted empty so every other call site (the
    /// offline board, gallery, snapshot tests) lays out the whole hand unchanged.
    public let reserveNoSlot: Set<String>
    /// Round-8: when the CALLER owns card flights through an overlay (the message
    /// board's `BoardAnimator` - a played card flies as a ghost while its hand
    /// copy is `hidden`), a card must EXIT this fan instantly, never by a
    /// cross-fade. It cannot be decided from `hidden` at render time: SwiftUI
    /// captures a leaving view's transition from its LAST rendered body, which is
    /// the frame BEFORE `preHide` marked it hidden - so a `hidden`-keyed
    /// transition is always read as the visible-card `.opacity` fade and the card
    /// lingers half-visible in its old slot beside its own flying ghost (the "two
    /// of the same card" bug). A STATIC flag has no such capture race. Defaulted
    /// false so the offline board / gallery / snapshots keep their cross-fade and
    /// their hand↔table matchedGeometry flight, exactly as before.
    public let instantExit: Bool
    /// Round-8 #4: how the caller hears about an in-hand reorder — the full
    /// display order (card identities) after each splice. The message board
    /// persists it per game (MessageGameStore.setHandOrder) so a sorted hand
    /// survives closing and reopening the game; defaulted no-op so every other
    /// call site (offline board, gallery, snapshots) is unchanged.
    public let onOrderChanged: ([String]) -> Void

    public init(cards: [Card], trumpSuit: Suit?, disabled: Set<String> = [],
                locked: Set<String> = [],
                selection: Binding<Set<String>>, onTap: @escaping (Card) -> Void,
                onDragChanged: @escaping (Card, CGPoint) -> Void = { _, _ in },
                onDragEnded: @escaping (Card, CGPoint) -> Void = { _, _ in },
                namespace: Namespace.ID? = nil, hidden: Set<String> = [],
                topHalfOnly: Bool = false, crop: CGFloat? = nil,
                onDragCardMoved: @escaping (CGPoint) -> Void = { _ in },
                reserveNoSlot: Set<String> = [], instantExit: Bool = false,
                initialOrder: [String] = [],
                onOrderChanged: @escaping ([String]) -> Void = { _ in }) {
        self.instantExit = instantExit
        self.cards = cards
        self.trumpSuit = trumpSuit
        self.disabled = disabled
        self.locked = locked
        self._selection = selection
        self.onTap = onTap
        self.onDragChanged = onDragChanged
        self.onDragEnded = onDragEnded
        self.namespace = namespace
        self.hidden = hidden
        // `crop` wins when a caller passes a continuous value (the compact
        // drawer, off its collapse fraction); otherwise the `topHalfOnly` Bool
        // maps to the two endpoints, so every existing call site is unchanged.
        self.crop = crop ?? (topHalfOnly ? 1 : 0)
        self.onDragCardMoved = onDragCardMoved
        self.reserveNoSlot = reserveNoSlot
        // Round-8 #4: seed the cosmetic order from the caller's persisted copy.
        // @State takes an initial value only when the view IDENTITY is created
        // (a board reload gives a fresh board via its `.id`, which is exactly
        // when the seed should re-apply); later re-inits of the same identity
        // keep the live @State, so mid-game re-renders never reset a reorder.
        self._order = State(initialValue: initialOrder)
        self.onOrderChanged = onOrderChanged
    }

    @State private var dragId: String?
    @State private var dragOffset: CGSize = .zero
    /// Has THIS gesture travelled far enough to be a drag rather than a tap?
    /// Gates the in-hand reorder: with `minimumDistance: 0` the gesture starts
    /// on touch-down, so without this a tap reorders the hand under your
    /// finger. Reset when the gesture ends, not when it starts, so a new
    /// gesture always begins as a tap.
    @State private var dragMoved = false
    /// note 5 (hand reordering): the local player's cosmetic display order —
    /// card identities, reconciled against `cards` every body evaluation via
    /// `displayCards`. This is PURELY a UI convenience, but it is no longer
    /// merely a convenience: it is now the ONLY thing that decides where a card
    /// sits. It used to rest on "the kernel hand order never changes", which
    /// round 30 found to be false for any board rendering a replay-derived
    /// snapshot - see `remembering` for the whole finding. Grown from every
    /// hand this fan draws; reordered only by a drag.
    /// Round-8 #4: seeded from `initialOrder` at view creation
    /// and reported through `onOrderChanged` on every splice, so a caller can
    /// persist it per game (the message board does, via MessageGameStore) —
    /// a board reload swaps in a fresh view whose seed restores the
    /// arrangement instead of resetting it. Never sent anywhere off-device.
    @State private var order: [String]
    /// This view's own frame in `boardSpace`, mirrored from the same
    /// `HandFrameKey` preference it publishes below — read locally so a
    /// reorder can tell "drag point still inside the hand" from "dragged out
    /// to play" without needing the board to feed it back down.
    @State private var handFrameSelf: CGRect = .zero
    /// Each card's own frame in `boardSpace`, mirrored from `HandCardFramesKey`
    /// the same way `handFrameSelf` mirrors `HandFrameKey` just above. AT REST
    /// this is the card's slot, which is what `reorder` hit-tests against.
    ///
    /// ROUND-6 BUG 5 CORRECTION: while a card is being dragged, ITS entry here
    /// is NOT the resting slot — it already carries the live `.offset`. The
    /// round-5 comment that used to sit here claimed the opposite ("`.offset()`
    /// is a render-only transform and does not feed back into a GeometryReader's
    /// own layout-time frame, so the offset has to be added back in by hand"),
    /// and the drag-hint code believed it. It is wrong: `.offset` is applied by
    /// an ANCESTOR modifier of this reader (see `cardView`'s modifier order), so
    /// `frame(in: .named(boardSpace))` — a query about where this view sits in
    /// another space, not about its own proposed size — reports the DISPLACED
    /// rect. Adding `dragOffset` on top therefore counted the drag twice and the
    /// verb hint flew off at double speed: "sometimes appearing quite far away
    /// from the cards" (traced live: rest=151,397 while off=151,-322, i.e. the
    /// published rect had already moved by almost exactly the offset).
    ///
    /// The live centre is now derived from the GESTURE instead (`grabDelta`
    /// below), which is exact and, unlike a preference, cannot lag a frame
    /// behind the finger.
    @State private var handCardFramesSelf: [String: CGRect] = [:]
    /// Round-6 bug 5: the vector from the finger to the dragged card's SLOT
    /// centre, captured once when the gesture claims a card (translation is
    /// still ~0 then, so the published frame above is still the honest resting
    /// slot). The card tracks the finger rigidly for the rest of the drag, so
    /// `finger + grabSlot` IS the card's slot centre at every later moment — no
    /// double counting, no preference lag. Zero when the card's frame has not
    /// published yet, which degrades to "the card rides the finger", the
    /// pre-round-5 behavior.
    ///
    /// Round-16: the slot centre rather than the VISIBLE centre, because two
    /// callers now want it and they want different things - the reorder compares
    /// it against slots, the verb-hint pill wants the card you can see, and
    /// under `crop` those differ by half the hidden part. Storing the slot and
    /// adding the crop drop at the one use that wants it keeps the stored value
    /// honest while `crop` itself changes mid-drag (the drawer collapsing).
    @State private var grabSlot: CGSize = .zero
    /// Round-5 M6: this view's own proposed width, measured once via a
    /// preference round-trip (see `HandWidthKey`'s doc) so the row-split
    /// height below can react to it. A plain `GeometryReader` has no notion of
    /// an "ideal" height independent of what its parent proposes — there is no
    /// `.fixedSize` escape hatch for it — so this is the standard two-step fix:
    /// measure width, THEN size height from it.
    @State private var measuredWidth: CGFloat = 0
    /// Cumulative compensation from in-flight reorders, subtracted from
    /// `dragOffset` so the DRAGGED card's own `.offset` doesn't jump when its
    /// slot in `order` moves out from under it (only the OTHER cards should
    /// visibly slide — the dragged one keeps tracking the finger). It is exactly
    /// `slot(now) - slot(at grab)`, which is why it telescopes: each splice adds
    /// the step it just took.
    ///
    /// Round-16: two-dimensional, now that a splice can move a card's slot down
    /// a row as well as along one.
    @State private var reorderShift: CGSize = .zero

    private static let cardH: CGFloat = 72   // CONSTANT height — a skinny (many-card) hand stays this tall

    /// How tall a slice of each card to actually reserve, for a given `crop`
    /// fraction: `crop` 0 shows the whole card, 1 reserves only the top half
    /// (the card is still drawn full-height and top-aligned, so its lower part
    /// simply falls off the bottom — see `cardView`). Continuous between the two
    /// so the compact drawer can drive it off a smooth collapse fraction and the
    /// hand descends gradually (round-6 bugs 2/4) instead of halving in one frame.
    static func shownCardHeight(crop: CGFloat) -> CGFloat {
        Self.cardH * (1 - 0.5 * max(0, min(1, crop)))
    }
    private static let maxCardW: CGFloat = 52   // never wider than ~proper aspect, so cards never go "superwide"
    private static let gap: CGFloat = 4
    private static var rowH: CGFloat { cardH + 8 }
    /// Vertical gap between the two rows once M6 splits the hand.
    private static let rowGap: CGFloat = 6
    /// Round-5 M6: below this per-card width, split into two rows rather than
    /// keep thinning — Durak routinely leaves a defender holding 15-20 cards
    /// after two pickups, which fell under Apple's 44pt hit-target minimum in
    /// a single row (M6's own finding). ~34pt still reads as a proper card,
    /// just a narrow one; the web's own answer to the same problem ("that's
    /// what we do if we have a lot of cards in the replay on the website") is
    /// exactly a second row, not a hard floor on width.
    private static let twoRowThreshold: CGFloat = 34
    /// Round-5 drag feel ("you need to drag a bit of a distance before it
    /// actually grabs the card. Should be immediate right?"): with
    /// `DragGesture(minimumDistance: 0)` a plain tap and a real drag are both
    /// the SAME gesture stream now, distinguished only by how far the finger
    /// actually travelled before lifting. Comfortably below both a human's
    /// natural "still basically a tap" wobble and the ~12pt the old
    /// `minimumDistance` used to require before anything happened at all.
    private static let tapThreshold: CGFloat = 8

    // MARK: - Row-split math (round-5 M6) — static and pure so MessageTableView
    // and Round5BoardTests share EXACTLY this arithmetic; nothing here reads
    // `self`, so none of it can drift from what `body` actually renders.

    /// Per-card width if `count` cards shared `availableWidth` in ONE row — the
    /// same formula `body` used before M6 (`min(52, max(22, avail/count))`),
    /// pulled out so the split threshold below can reuse it without a live view.
    private static func singleRowCardWidth(count: Int, availableWidth: CGFloat) -> CGFloat {
        let n = max(count, 1)
        let avail = availableWidth - Self.gap * CGFloat(n + 1)
        return min(Self.maxCardW, max(22, avail / CGFloat(n)))
    }

    /// How many rows this hand needs at `availableWidth`: two once the
    /// single-row math above would put a card below `twoRowThreshold`, else
    /// one — except a 0/1-card hand is ALWAYS one row no matter what the width
    /// math says, so a degenerate (zero or negative) `availableWidth` can
    /// never report a split there is nothing to actually split (`rowSizes`
    /// relies on that: it needs more than one card before it will ever hand
    /// back two rows).
    public static func rowCount(cards: [Card], availableWidth: CGFloat) -> Int {
        Self.rowCount(count: cards.count, availableWidth: availableWidth)
    }

    /// Count-only form of `rowCount` - every layout question below is about
    /// POSITIONS, not identities, so the split is expressed in card COUNTS.
    public static func rowCount(count: Int, availableWidth: CGFloat) -> Int {
        guard count > 1 else { return 1 }
        return Self.singleRowCardWidth(count: count, availableWidth: availableWidth) < Self.twoRowThreshold ? 2 : 1
    }

    /// How many cards each display row holds at `availableWidth`. The BOTTOM row
    /// gets the extra card on an odd count.
    ///
    /// WHICH ROW GETS THE ODD CARD is the owner's call, and they reversed it:
    /// "If we have 11 cards, do 5 up top and 6 below". It used to be `ceil` -
    /// six up top, five below - which stands the hand on its point. A hand fans
    /// out from the hand that holds it, so the wider row belongs at the BOTTOM,
    /// nearest the player; the narrow row reads as sitting behind it. Under the
    /// collapsed drawer's crop it matters even more, because the bottom row is
    /// the one whose faces are least occluded.
    ///
    /// Round-16: this split is what makes the cross-row drag work at all. Rows
    /// are DERIVED from a flat order by cutting it at `floor(n/2)`, they are not
    /// storage - so moving a card across the boundary is an ordinary splice into
    /// the flat array, and the cut then falls in a different place. Sliding a
    /// bottom card up to slot 1 pushes everything from 1 onward right by one, and
    /// the card that was last in the top row lands first in the bottom row. The
    /// "bump" the owner asked for is not a special case; it is what a fixed cut
    /// through a shifted array already does. Moving the cut from ceil to floor
    /// changes WHERE it falls, not that it is a cut, so all of that still holds.
    public static func rowSizes(count: Int, availableWidth: CGFloat) -> [Int] {
        guard Self.rowCount(count: count, availableWidth: availableWidth) == 2 else { return [count] }
        let first = count / 2   // floor: the extra card goes BELOW on odd counts
        return [first, count - first]
    }

    /// The fan's total on-screen height at `availableWidth` — one row (full
    /// `rowH`, or half that under `topHalfOnly` — round-5 M5b) normally, or two
    /// such rows stacked with `rowGap` between once M6 splits the hand. Public
    /// and static so MessageTableView can reserve exactly this much room above
    /// the hand (see its `handLift`) instead of guessing at a second constant.
    public static func height(cards: [Card], availableWidth: CGFloat, crop: CGFloat) -> CGFloat {
        Self.height(count: cards.count, availableWidth: availableWidth, crop: crop)
    }

    /// Count-only form, for the same reason as `rowCount(count:)`.
    public static func height(count: Int, availableWidth: CGFloat, crop: CGFloat) -> CGFloat {
        let oneRow = Self.shownCardHeight(crop: crop) + 8
        return Self.rowCount(count: count, availableWidth: availableWidth) == 2 ? oneRow * 2 + Self.rowGap : oneRow
    }

    /// `topHalfOnly` convenience overload — the pre-round-6 spelling, kept so
    /// TableView/GalleryView/the snapshot tests that reserve room off this
    /// function compile unchanged. `false` → crop 0 (full card), `true` → crop 1
    /// (top half), so every pinned number in Round5BoardTests still holds exactly.
    public static func height(cards: [Card], availableWidth: CGFloat, topHalfOnly: Bool = false) -> CGFloat {
        Self.height(cards: cards, availableWidth: availableWidth, crop: topHalfOnly ? 1 : 0)
    }

    /// The resting SLOT rect of every card in a hand of `cards`, laid out in a
    /// container `width` wide at collapse `crop` — the geometry `body` renders,
    /// keyed by card.
    ///
    /// Round-7 ("fix this once and for all"): an overlay flight into the hand
    /// needs the card's FINAL slot as its landing target. Reading it off the live
    /// `HandCardFramesKey` fails the moment the make-room is ANIMATING — the row
    /// re-centres as the present cards slide, so the published frame is a MOVING
    /// target and the flight lands on a mid-slide spot, then snaps (the "bunch").
    /// The old cures were to either SNAP the make-room (the "cards jump" the owner
    /// hated) or SETTLE before flying (the "make-room THEN flight, not together"
    /// the owner also hated). Computing the final slot here instead lets the
    /// make-room animate AND the card fly to its true resting place at the SAME
    /// time. Round-16: it no longer MIRRORS `body`, it IS what `body` lays out
    /// from (see `slotFrames`), so the two cannot drift apart.
    ///
    /// Rects are in the container's LOCAL space (origin at its top-leading); the
    /// caller offsets by the hand's own frame origin in `boardSpace`. `cards` is
    /// the set the fan actually lays out at that instant (present + whatever this
    /// step just opened) - the incoming card sits at the END, exactly where the
    /// fan puts a freshly dealt card, so its slot matches even if the player has
    /// cosmetically reordered the cards already in hand.
    public static func slotRects(cards: [Card], width: CGFloat, crop: CGFloat) -> [String: CGRect] {
        let frames = Self.slotFrames(count: cards.count, width: width, crop: crop)
        guard frames.count == cards.count else { return [:] }
        var out: [String: CGRect] = [:]
        for (i, card) in cards.enumerated() { out[card.identity] = frames[i] }
        return out
    }

    /// The same geometry indexed by SLOT rather than by card - slot 0 is the
    /// leftmost of the top row, and the array runs left-to-right, top row then
    /// bottom.
    ///
    /// Round-16: this is now the layout ITSELF, not a mirror of it. `body` used
    /// to render a VStack of HStacks and this function re-derived the same
    /// numbers alongside it, with a comment ("it mirrors `body` exactly")
    /// admitting the drift hazard that arrangement carries. It also could not
    /// give the owner what they asked for: a card that changes row changes which
    /// HStack it belongs to, and SwiftUI has no way to read that as a MOVE - it
    /// is a removal from one container and an insertion into another, so the
    /// bumped card popped rather than travelled. Placing every card absolutely
    /// from these rects makes a row change an ordinary change of offset, which
    /// animates like every other slide in the fan, and leaves exactly one copy of
    /// the arithmetic for the flight targeting and the layout to share.
    public static func slotFrames(count: Int, width: CGFloat, crop: CGFloat) -> [CGRect] {
        guard width > 0, count > 0 else { return [] }
        let rows = Self.rowSizes(count: count, availableWidth: width)
        // ONE card width for BOTH rows, sized by the FULLER row - sizing each row
        // by its own count made the shorter row's cards visibly WIDER than the
        // other's, which read as two different decks rather than one hand that
        // wrapped. This asks `rows.max()` rather than `rows[0]` because the odd
        // card now lands in the SECOND row (see `rowSizes`); hard-coding row 0 as
        // the fuller one was true only while the cut was a ceil, and would have
        // sized an 11-card hand off 5 and then overflowed the row of 6.
        let cardW = Self.singleRowCardWidth(count: rows.max() ?? count, availableWidth: width)
        let cardH = Self.shownCardHeight(crop: crop)
        let containerH = Self.height(count: count, availableWidth: width, crop: crop)
        let stackH = CGFloat(rows.count) * cardH + CGFloat(rows.count - 1) * Self.rowGap
        let vTop = (containerH - stackH) / 2
        var out: [CGRect] = []
        out.reserveCapacity(count)
        for (r, n) in rows.enumerated() {
            let rowW = CGFloat(n) * cardW + CGFloat(max(0, n - 1)) * Self.gap
            let rowLeft = (width - rowW) / 2
            let y = vTop + CGFloat(r) * (cardH + Self.rowGap)
            for c in 0..<n {
                out.append(CGRect(x: rowLeft + CGFloat(c) * (cardW + Self.gap),
                                  y: y, width: cardW, height: cardH))
            }
        }
        return out
    }

    /// The slot a dragged card is asking for: the one whose CENTRE is nearest
    /// the card's own centre, in two dimensions.
    ///
    /// Round-16. Two dimensions is the whole change - the row-local version this
    /// replaces compared x only, because a card could not leave its row anyway.
    /// Plain Euclidean distance needs no per-axis weighting to feel right here:
    /// row centres sit ~78pt apart while neighbouring slots sit ~38pt apart, so a
    /// row change asks for a deliberate half-card lift while sliding sideways
    /// stays as light as it always was. Ties break to the LOWER slot so the
    /// result is total (the same lesson as `BoardDrop.target`: an arbitrary
    /// winner is a bug that only shows up as flicker).
    public static func slotIndex(at centre: CGPoint, slots: [CGRect]) -> Int? {
        var best: Int?
        var bestDistance = CGFloat.greatestFiniteMagnitude
        for (i, r) in slots.enumerated() {
            let d = hypot(centre.x - r.midX, centre.y - r.midY)
            if d < bestDistance { bestDistance = d; best = i }
        }
        return best
    }

    /// THE POINT THE BOARD IS TOLD ABOUT - and the whole answer to "a drag that
    /// ends INSIDE THE HAND must never play a card".
    ///
    /// ROUND 40, owner on a real device in the COMPACT DRAWER: "In collapsed
    /// mode, now I can't rearrange cards. It seems to trigger attack." Sliding a
    /// card sideways to reorder it threw it onto the table as an attack instead.
    ///
    /// WHY it happens. The board resolves a release with `BoardDrop.target`,
    /// which hit-tests the BATTLE SLOTS BEFORE the hand (deliberately - see
    /// `MessageTableView.handDropFrame`). In the compact drawer the two regions
    /// genuinely overlap. Measured on the rig, iPhone 16, a 20-card hand in a
    /// ~276pt drawer (`board-compact` with the hand two rows deep):
    ///
    ///     hand      = (16, 106) 343x166      -> rows centred at y=150 and y=228
    ///     battle[0] = (156, 101)  62x84      -> y 101...185, x 156...218
    ///
    /// The hand's whole TOP ROW lies inside the battle grid. The resting centre
    /// of hand slot 4, (170,150), is a point the player is holding a card at,
    /// and `BoardDrop.target` answers `.battle(0)` for it. `CardPlay.resolve`
    /// then ignores the target entirely for an attacker and returns the plain
    /// attack - the card is played. That is the owner's "it seems to trigger
    /// attack", and it is compact-only because that is the only place the two
    /// regions touch.
    ///
    /// WHY THE FIX IS HERE and not in `BoardDrop.target`. Two other cures were
    /// tried on paper and rejected:
    ///
    ///  - ORDERING (hand beats battles). The board does not hand `BoardDrop` the
    ///    hand's real rect: it hands `handDropFrame`, the CANCEL BAND, which is
    ///    the hand grown 64pt UPWARD (round-7 #3: in the compact drawer a
    ///    rearrange whose finger drifts off the thin cropped strip used to land
    ///    on `.table` and get rejected with "move not allowed"). On the measured
    ///    board that band is (16,42) 343x254 - it SWALLOWS battle[0] whole, so
    ///    letting it win first would make every cover in the compact drawer
    ///    unreachable. `BoardDrop` cannot tell the band from the hand, because it
    ///    is given one rect and only the band ever reaches it.
    ///  - GEOMETRY (stop the battle slack reaching down into the hand). The 8pt
    ///    slack is not the cause: (170,150) is inside battle[0]'s RAW rect, not
    ///    just its inflated one. Removing the slack changes nothing.
    ///
    /// This view is the one place that holds the hand's TRUE frame at the instant
    /// the finger lifts (`handFrameSelf`), so this is where the rule can be
    /// stated. Round-7 #3's 64pt widening is left exactly as it was: a release
    /// ABOVE the hand is not inside it, so it is passed through untouched and
    /// still falls through battles into the cancel band, exactly as before. All
    /// that changes is the case that band was never meant to cover - a finger
    /// still ON the cards.
    ///
    /// The projection is onto the hand's own FLOOR, keeping x so the point stays
    /// over the same card. Reporting nothing at all instead was tried and
    /// rejected: `onDragEnded` is the ONLY thing that clears the board's
    /// `dragCard`/`dragPoint`, so swallowing it leaves the verb-hint pill stuck
    /// on screen and the cover highlights lit after every rearrange. The floor is
    /// the one line inside the hand that the table cannot be sitting on - the
    /// battle grid grows DOWNWARD from the top of the board into the hand, so a
    /// battle reaching the hand's bottom edge would mean the table is drawn over
    /// the entire hand, at which point no drop is meaningful anyway.
    public static func boardPoint(_ point: CGPoint, hand: CGRect) -> CGPoint {
        guard hand.contains(point) else { return point }
        // `nextDown`, not a whole point: this must stay inside `hand` for a
        // `contains` test (which excludes maxY) while moving the point as
        // little as possible - it is still the player's finger.
        return CGPoint(x: point.x, y: hand.maxY.nextDown)
    }

    /// `cards` reordered by the local `order` state: identities still present
    /// keep their relative order from `order`; any identity in `cards` not yet
    /// in `order` (a fresh hand, or a card just dealt in) is appended in
    /// kernel order. Pure — never mutates `order` itself, so it's safe to read
    /// from `body` on every evaluation.
    private var displayCards: [Card] { Self.displayOrder(cards: cards, order: order) }

    /// The reconcile itself, static + pure so it can be asserted directly (the
    /// same contract as the web's displayedHand, src/state/clientReconcile.ts):
    /// only cards actually in `cards` render; `order` decides the relative
    /// order of the ones it knows; unknown cards append in kernel order; stale
    /// ids (played cards the sticky memory still remembers) and duplicates
    /// drop out by construction.
    public static func displayOrder(cards: [Card], order: [String]) -> [Card] {
        guard !order.isEmpty else { return cards }
        let byId = Dictionary(uniqueKeysWithValues: cards.map { ($0.identity, $0) })
        var seen = Set<String>()
        var result: [Card] = []
        result.reserveCapacity(cards.count)
        for id in order {
            if let c = byId[id], seen.insert(id).inserted { result.append(c) }
        }
        for c in cards where !seen.contains(c.identity) {
            result.append(c); seen.insert(c.identity)
        }
        return result
    }

    /// THE LOCAL ARRANGEMENT AFTER SEEING `cards` — grow-only, and the whole
    /// answer to "what decides where my cards sit".
    ///
    /// Every identity already remembered keeps its place, INCLUDING ones no
    /// longer in the hand: a card you covered with and then picked back up
    /// resumes the slot it had, which is what the owner asked for and what a
    /// sticky memory is for. Anything never seen before is appended at the
    /// RIGHT, in the order the caller presents it — a pickup or a draw lands
    /// rightmost. Nothing here ever REORDERS: only a drag does that
    /// (`splice`), which is the rule stated in full.
    ///
    /// ROUND 30, and it exists because the assumption `order` was built on -
    /// "the kernel hand order never changes" (see `order`'s own doc) - stopped
    /// being true. Since 368e666 a staged bout end renders a snapshot taken
    /// from the EVENT STREAM rather than the live game, and that stream is
    /// produced by replaying the game's v6 code from scratch
    /// (`lastMoveEvents` -> `fio_replay_last_events_packed`). A replay reaches
    /// the same position by a different route, so its hand ARRAY comes out in a
    /// different order than the continuation-decoded live game - same cards,
    /// shuffled. With an empty `order` the fan renders that array directly, so
    /// the owner's hand visibly rearranged itself as a pickup flew home and
    /// snapped back on undo (the hold is what undo drops). A cover that does
    /// not end the bout has no settlement to withhold, no snapshot to render,
    /// and never showed the bug - which is exactly how it was found.
    ///
    /// Remembering from the FIRST hand we ever see makes the arrangement the
    /// board's own, so no later disagreement between two derivations of the
    /// same game can move a card that is already on screen. New cards are the
    /// only thing either derivation gets to place, and they go at the right.
    public static func remembering(_ order: [String], cards: [Card]) -> [String] {
        var out = order
        var seen = Set(order)
        for c in cards where seen.insert(c.identity).inserted { out.append(c.identity) }
        return out
    }

    /// The result of an in-hand reorder: the display order after the move, plus
    /// the slots the card left and landed in (the view needs those to keep the
    /// dragged card pinned to the finger - see `reorderShift`).
    public struct HandSplice: Equatable {
        public let order: [String]
        public let from: Int
        public let to: Int
        public init(order: [String], from: Int, to: Int) {
            self.order = order; self.from = from; self.to = to
        }
    }

    /// THE WHOLE REORDER DECISION, pure: where does `dragged` belong now that its
    /// centre is at `centre` (this container's local space), and what does the
    /// order look like once it goes there? `nil` when nothing should move.
    ///
    /// It lives out here rather than inside the gesture because it is the only
    /// part worth asserting, and a test that drove the arithmetic alongside the
    /// view would be green against a view that clamps differently. Everything
    /// the gesture still does - spring, compensation, persist - is bookkeeping
    /// on top of this answer.
    ///
    /// Round-16 (the owner: "if there are two rows, you can't drag to rearrange
    /// between them"). There is deliberately NO row constraint here. Round-5
    /// clamped the target into the dragged card's own row, on the theory that
    /// crossing would "read as it randomly resorting which cards sit on top vs
    /// bottom" - but the rows are a cut through one flat order, so a cross is
    /// the plainest possible splice and the resulting bump is legible: drag up
    /// and the top row's rightmost card comes down to the head of the bottom
    /// row; drag down and the bottom row's leftmost goes up to the tail of the
    /// top. Exactly one card ever moves besides yours.
    ///
    /// `deferred` are cards holding no slot yet (round-6 bug 10, a deal whose
    /// flight has not started). They are invisible to the hit test - they have
    /// no slot to hit - and are stitched back into the positions they held, so a
    /// reorder mid-deal cannot quietly relocate a card that is not on screen.
    public static func splice(order: [String], deferred: Set<String> = [],
                              dragged: String, centre: CGPoint,
                              slots: [CGRect]) -> HandSplice? {
        let laid = deferred.isEmpty ? order : order.filter { !deferred.contains($0) }
        guard laid.count == slots.count,
              let from = laid.firstIndex(of: dragged),
              let to = Self.slotIndex(at: centre, slots: slots),
              to != from else { return nil }
        var moved = laid
        moved.remove(at: from)
        moved.insert(dragged, at: min(to, moved.count))
        if moved.count == order.count { return HandSplice(order: moved, from: from, to: to) }
        // Stitch: walk the full order handing out the reordered ids in sequence
        // wherever a laid-out card stood, so the deferred ids keep their places.
        var it = moved.makeIterator()
        let full = order.map { deferred.contains($0) ? $0 : (it.next() ?? $0) }
        return HandSplice(order: full, from: from, to: to)
    }

    /// In-hand reorder (note 5): ask `splice` where the dragged card belongs
    /// now, and if that is somewhere new, take it there under a spring - the
    /// other cards slide apart, mirroring the web's live reorder feel.
    ///
    /// `centre` is the dragged card's own live slot centre in `boardSpace`, not
    /// the fingertip. That distinction is what makes this stable: the card ends
    /// up AT the slot it asked for, so the very next evaluation asks for that
    /// same slot again and nothing moves. Hit-testing the finger instead let the
    /// answer depend on where inside the card you happened to grab.
    private func reorder(_ card: Card, centre: CGPoint, slots: [CGRect]) {
        // The FULL remembered order, not just what is in my hand this instant:
        // a card that is out on the table (I covered with it and it may yet come
        // back) holds no slot to hit-test, but its PLACE is still mine to keep.
        // Splicing against the current hand alone dropped those ids from the
        // order every time you rearranged, so the card came home rightmost
        // instead of to the slot it left - the owner's "notice that the pickup
        // does not change the local order of that card you covered with".
        // `splice` already knows how to stitch slotless ids back where they
        // were; this just tells it about the second kind.
        let full = Self.remembering(order, cards: cards)
        let inHand = Set(cards.map(\.identity))
        let slotless = Set(full.filter { !inHand.contains($0) }).union(reserveNoSlot)
        guard let s = Self.splice(order: full,
                                  deferred: slotless,
                                  dragged: card.identity,
                                  centre: CGPoint(x: centre.x - handFrameSelf.minX,
                                                  y: centre.y - handFrameSelf.minY),
                                  slots: slots) else { return }
        // Both mutations animate together (same spring, same transaction) so
        // the compensation tracks the layout's own transition instead of
        // stepping instantly while the slide is still mid-spring. In two
        // dimensions now: a card that changes row moves down a whole row as
        // well as across, and a compensation that only knew about x would let
        // the card jump a row out from under the finger.
        withAnimation(FMotion.card) {
            order = s.order
            reorderShift.width += slots[s.to].minX - slots[s.from].minX
            reorderShift.height += slots[s.to].minY - slots[s.from].minY
        }
        // Round-8 #4: report the arrangement OUTSIDE the animation transaction
        // (persisting is not a visual change). It is the full display order,
        // sticky ids included, exactly what a later seed restores.
        onOrderChanged(s.order)
    }

    /// Round-6 bug 10: the cards this fan actually lays out — `displayCards`
    /// minus any deal still deferring its slot. A deferred card stays in `cards`
    /// / `order` (so its position is settled the moment it is revealed) but takes
    /// up no width until then. Empty `reserveNoSlot` (every non-message board) is
    /// the whole hand, unchanged.
    private var laidOutCards: [Card] {
        reserveNoSlot.isEmpty ? displayCards : displayCards.filter { !reserveNoSlot.contains($0.identity) }
    }

    public var body: some View {
        GeometryReader { geo in
            let width = geo.size.width
            let laid = laidOutCards
            // Round-16: absolute placement from the shared slot geometry, in
            // place of the VStack-of-HStacks this used to be. See `slotFrames`
            // for why - in short, a card that changes row has to be able to
            // TRAVEL there, and a card cannot travel between two containers.
            // `dy` re-centres the block in whatever height the outer frame
            // actually handed us, which matters only on the first paint (before
            // the width probe lands, that frame still assumes one row); at rest
            // it is zero and these are exactly the rects `slotRects` publishes
            // for flights to aim at.
            let dy = (geo.size.height - Self.height(count: laid.count,
                                                    availableWidth: width, crop: crop)) / 2
            let slots = Self.slotFrames(count: laid.count, width: width, crop: crop)
                .map { $0.offsetBy(dx: 0, dy: dy) }
            ZStack(alignment: .topLeading) {
                // A bed, so the stack fills the container even when the hand is
                // empty and the cards below are placed against a stable origin.
                Color.clear
                ForEach(Array(laid.enumerated()), id: \.element.identity) { idx, card in
                    if idx < slots.count {
                        cardView(card, slot: slots[idx], slots: slots)
                            .offset(x: slots[idx].minX, y: slots[idx].minY)
                    }
                }
            }
            .frame(width: width, height: geo.size.height, alignment: .topLeading)
            // Round-8: animate the fan's own re-layout when the laid-out SET
            // changes, so present cards SLIDE to their new slots as one is
            // added/removed (a deal makes room, a play closes the gap) instead of
            // snapping. INSIDE the GeometryReader and ON the card stack on purpose:
            // the outer frame is below the reader, and a value-animation there does
            // not reliably propagate through GeometryReader to the row reflow it is
            // meant to drive (the cards just jumped). Keyed on the SET, not its
            // order, so a drag-reorder (same members, different order) keeps its own
            // FMotion.card spring and rigid finger tracking, and `crop` is excluded
            // so the compact-drawer collapse stays the board's animation. A leaving
            // card still EXITS instantly (`instantExit` / the identity transition),
            // so only the survivors slide - no fade, no ghost.
            .animation(.timingCurve(0.25, 0.46, 0.45, 0.94, duration: flightTime),
                       value: Set(laidOutCards.map(\.identity)))
        }
        // Before the width probe lands (`measuredWidth == 0`, the very first
        // paint) assume ONE row: at an unknown-but-real width the single-row
        // math with width 0 would report every 2+-card hand as split, so a
        // small hand ballooned to two-row height for a frame. Infinite width
        // reproduces the pre-M6 default (one full-width row) until the real
        // number arrives a paint later.
        .frame(height: Self.height(cards: cards.filter { !reserveNoSlot.contains($0.identity) },
                                   availableWidth: measuredWidth > 0 ? measuredWidth : .greatestFiniteMagnitude,
                                   crop: crop))
        // Round-5 M6: measure this view's own proposed WIDTH once — see
        // `measuredWidth`'s doc — so the `.frame(height:)` just above can size
        // itself for a possibly-two-row hand. Unaffected by whatever height
        // that frame picks, since `.frame(height:)` never touches width; the
        // measurement therefore converges immediately regardless of which
        // height this renders with first.
        .background(GeometryReader { g in
            Color.clear.preference(key: HandWidthKey.self, value: g.size.width)
        })
        .onPreferenceChange(HandWidthKey.self) { measuredWidth = $0 }
        // Publish the hand's frame so a drop back inside it cancels the play.
        .background(GeometryReader { g in
            Color.clear.preference(key: HandFrameKey.self, value: g.frame(in: .named(boardSpace)))
        })
        // Mirror that same frame locally (note 5) — this doesn't stop it from
        // also bubbling up to the board's own `onPreferenceChange`.
        .onPreferenceChange(HandFrameKey.self) { handFrameSelf = $0 }
        // Round-5 finding 5: mirror each card's own resting frame too, the
        // same way — `onDragCardMoved` needs it (see `cardView`).
        .onPreferenceChange(HandCardFramesKey.self) { handCardFramesSelf = $0 }
        // ROUND 30: the arrangement is remembered from the FIRST hand this fan
        // ever draws, and grows by every card that arrives after it (see
        // `remembering`). Before this the memory was opt-in - empty until you
        // dragged something - so an un-dragged hand rendered whatever array
        // order the board handed over, and two derivations of the same game do
        // not agree about that.
        //
        // `onAppear` FIRST and on the identities, not the cards: this must land
        // before anything can hand over a differently-ordered copy of the same
        // hand, and re-firing on a value change that is not a membership change
        // would be pure churn.
        .onAppear { remember() }
        .onChange(of: cards.map(\.identity)) { _ in remember() }
    }

    /// Fold the hand on screen into the remembered arrangement, and tell the
    /// caller only when that actually moved (the message board persists every
    /// report; a no-op write per paint is not worth making it swallow).
    private func remember() {
        let grown = Self.remembering(order, cards: cards)
        guard grown != order else { return }
        // NOT animated, and never inside a transaction: this adds ids, it never
        // reorders one (that is `splice`'s job alone), so there is no motion to
        // animate - and a card arriving into its slot is the board's flight,
        // which would fight a spring started here.
        order = grown
        onOrderChanged(grown)
    }

    /// One hand card, wired for tap/drag/reorder. `slot` is this card's own
    /// resting rect in the container; `slots` is every slot, which is what the
    /// reorder hit-tests against.
    @ViewBuilder
    private func cardView(_ card: Card, slot: CGRect, slots: [CGRect]) -> some View {
        FCard(card: card,
              selected: selection.contains(card.identity),
              disabled: disabled.contains(card.identity),
              trump: trumpSuit != nil && card.suit == trumpSuit,
              size: CGSize(width: slot.width, height: Self.cardH))
            // Round-5 M5b: the compact drawer shows only the TOP HALF of each
            // hand card. The card is drawn WHOLE and pushed DOWN so its lower
            // half falls past the drawer's own bottom edge — it is NOT cut in
            // half. The first attempt did literally clip each card to cardH/2,
            // which put a hard horizontal edge across every card mid-face
            // (owner, on device: "don't ACTUALLY crop the cards, just move them
            // down so that we only see the top half. right now they look
            // fucking ridiculous"). Reserving half the height with a .top
            // alignment and NO `.clipped()` gets the same compactness from a
            // real card that happens to be partly off-screen — which is what a
            // hand of cards held below the edge of a table actually looks like.
            //
            // The touch target follows the RESERVED half (`.contentShape`
            // below sizes to this frame), which is exactly the part you can
            // see and therefore the only part you could sensibly aim at.
            // Round-16: the width is pinned as well as the height, so this
            // card's layout rect IS its slot - the stack places it absolutely
            // now, and the frame it publishes through `HandCardFramesKey` is
            // what the board's slot self-check compares against.
            .frame(width: slot.width, height: slot.height, alignment: .top)
            .opacity(hidden.contains(card.identity) ? 0 : 1)
            // Round-8: the veil SNAPS - opacity never animates (belt to the exit
            // transition below), so a card cannot half-fade under the fan's own
            // slide animation or the board spring.
            .animation(nil, value: hidden.contains(card.identity))
            // Round-8: an overlay-flight board exits a card INSTANTLY (no fade);
            // see `instantExit`. Everyone else keeps the default cross-fade.
            .transition(instantExit ? .identity : .opacity)
            // Round-7 #2 dropped the namespace for a card the overlay was flying
            // (`hidden.contains(id) ? nil : namespace`), to stop SwiftUI flying it
            // a SECOND time into this fan - the "double animation" for pickup.
            // Round 43 states that rule where it belongs instead: a board either
            // veils cards or shares a namespace, never both, so the two could not
            // be simultaneously non-trivial and the ternary never chose. See
            // `FlightID`, which carries the invariant and the test that pins it.
            .modifier(FlightID(id: card.identity, namespace: namespace))
            .background(GeometryReader { g in
                Color.clear.preference(key: HandCardFramesKey.self,
                                       value: [card.identity: g.frame(in: .named(boardSpace))])
            })
            .contentShape(Rectangle())
            .offset(dragId == card.identity
                    ? CGSize(width: dragOffset.width - reorderShift.width,
                             height: dragOffset.height - reorderShift.height)
                    : .zero)
            .zIndex(dragId == card.identity ? 1000 : 0)
            .gesture(
                // Round-5 drag feel: `minimumDistance: 0` claims the touch the
                // instant a finger lands, which is also why the separate
                // `.onTapGesture` this view used to carry is GONE — a
                // 0-distance drag and a sibling tap gesture compete for the
                // same touch, and the drag always wins, so the tap gesture
                // simply stopped firing the moment this changed. The tap is
                // synthesized in `onEnded` below instead (translation magnitude
                // under `tapThreshold` ⇒ it was a tap, not a drag).
                DragGesture(minimumDistance: 0, coordinateSpace: .named(boardSpace))
                    .onChanged { g in
                        guard !disabled.contains(card.identity) else { return }
                        // …and the silent lock (round 43). Held-back replay
                        // cards are drawn as ordinary hand cards on purpose, so
                        // there is nothing here to distinguish - the touch is
                        // simply not taken, and `dragId` therefore never names
                        // this card, which is what makes `onEnded`'s drag branch
                        // a no-op for it as well.
                        guard !locked.contains(card.identity) else { return }
                        if dragId != card.identity {
                            dragId = card.identity; reorderShift = .zero
                            // Round-6 bug 5: capture finger -> card ONCE, while
                            // the published frame is still the resting slot (see
                            // `grabSlot` / `handCardFramesSelf`).
                            grabSlot = handCardFramesSelf[card.identity].map {
                                CGSize(width: $0.midX - g.startLocation.x,
                                       height: $0.midY - g.startLocation.y)
                            } ?? .zero
                        }
                        dragOffset = g.translation
                        // Only tell the consumer a DRAG is happening once the finger
                        // has actually travelled past the tap/drag threshold — the
                        // SAME line `onEnded` uses to tell a tap from a drag. With
                        // `minimumDistance: 0`, onChanged fires the instant a finger
                        // lands, so an ungated `onDragChanged` set the board's
                        // `dragCard` on every TAP; the tap branch of onEnded then
                        // returns without an `onDragEnded`, so that `dragCard` was
                        // never cleared and lingered. `highlightBattles` prefers
                        // `dragCard` over the real selection, so the last-tapped
                        // card's cover targets stayed lit no matter what you then
                        // selected or deselected (a stale trump lit every six; a
                        // stale J♦ lit only the 6♦). Gating here means a tap never
                        // sets `dragCard` at all. `dragMoved` keeps it firing for the
                        // rest of a real drag even if the finger drifts back near the
                        // start. `dragOffset` above is untouched, so the card still
                        // follows the finger from the first pixel.
                        if dragMoved || hypot(g.translation.width, g.translation.height) >= Self.tapThreshold {
                            // Round 40: through `boardPoint`, the same as the
                            // release below. The board's live verb hint resolves
                            // the SAME `BoardDrop.target` the release will (see
                            // `MessageTableView.dragPreview` - "so neither can
                            // disagree with what actually happens on release"),
                            // so reporting the raw point here and the projected
                            // one there would put an "Attack" pill on screen for
                            // the whole of a rearrange that then plays nothing.
                            onDragChanged(card, Self.boardPoint(g.location, hand: handFrameSelf))
                        }
                        // The dragged card's live SLOT centre, and from it the
                        // two things that want it.
                        //
                        // Straight off the gesture: the card is pinned rigidly to
                        // the finger by `grabSlot`, and `reorderShift` cancels
                        // whatever the splice did to its slot, so at every frame
                        // the card sits exactly where it started plus the
                        // translation - `location + grabSlot`, with no
                        // `reorderShift` term of its own. (Round-16 correction:
                        // this used to subtract `reorderShift` here as well as in
                        // the `.offset`, which counted the compensation twice and
                        // walked the verb hint off the card by one slot for every
                        // reorder made on the way out of the hand.)
                        //
                        // Deliberately NOT `restingFrame + dragOffset` any more:
                        // that double-counted the drag (see
                        // `handCardFramesSelf`), which is round-6 bug 5.
                        let centre = CGPoint(x: g.location.x + grabSlot.width,
                                             y: g.location.y + grabSlot.height)
                        // `crop` matters for the HINT: the slot is
                        // `shownCardHeight` tall while the card is drawn full
                        // `cardH` tall and TOP-aligned in it, so the card's
                        // visible middle sits half the cropped-away part lower
                        // than the slot's. The reorder wants the slot centre, the
                        // pill wants the visible one (round-5 finding 5, round-6
                        // bug 13 - the flight starts where the finger let go).
                        let cropDrop = (Self.cardH - Self.shownCardHeight(crop: crop)) / 2
                        onDragCardMoved(CGPoint(x: centre.x, y: centre.y + cropDrop))
                        // A TAP MUST NEVER REORDER. `minimumDistance: 0` means
                        // onChanged fires the instant a finger lands, before it
                        // has moved at all, so an ungated reorder ran on every
                        // tap — "a single tap will like cause a swap". The
                        // gesture only becomes a reorder once it has travelled
                        // past the same `tapThreshold` that `onEnded` uses to
                        // tell a tap from a drag, so the two can never disagree
                        // about which one this was.
                        if hypot(g.translation.width, g.translation.height) >= Self.tapThreshold {
                            dragMoved = true
                        }
                        // note 5: live-reorder only while the point is still
                        // inside the hand's own frame — once it leaves, this is
                        // a play-drag and today's behavior is untouched.
                        if dragMoved, handFrameSelf.contains(g.location) {
                            reorder(card, centre: centre, slots: slots)
                        }
                    }
                    .onEnded { g in
                        let c = card
                        let wasDragging = dragId == c.identity
                        // Round-6 bug 13, the CANCEL half: letting go over the
                        // hand puts the card back in its slot, and that return
                        // trip is now sprung instead of teleporting (the offset
                        // used to drop to zero in one frame). The PLAY half is
                        // the board's: it hides the card the same instant it
                        // takes it (MessageTableView.playAt pre-hides, then
                        // flies it from the release point), so for a real play
                        // this spring animates an already-invisible card and
                        // costs nothing. Both mutations must sit inside the one
                        // transaction — `.offset` reads `dragId` as well as
                        // `dragOffset`, so animating only one of them still
                        // snaps.
                        withAnimation(FMotion.card) {
                            dragId = nil; dragOffset = .zero; reorderShift = .zero
                        }
                        dragMoved = false
                        if hypot(g.translation.width, g.translation.height) < Self.tapThreshold {
                            // A tap, not a drag — same disabled-check + haptic
                            // + `onTap` the old `.onTapGesture` used to run.
                            guard !disabled.contains(c.identity) else { Haptics.fire(.reject); return }
                            // A LOCKED card swallows the tap and says nothing.
                            // Not even the reject haptic `disabled` fires: a
                            // rejection is feedback about a rule the player
                            // broke, and this card is mid-animation on its way
                            // out of the hand - there is no rule and nothing for
                            // them to do differently.
                            guard !locked.contains(c.identity) else { return }
                            Haptics.fire(.pickUp)
                            onTap(c)
                            return
                        }
                        // Round 40: `boardPoint`, not the raw finger - a release
                        // still on the cards is a rearrange and must never play
                        // one, whatever the battle grid claims about that spot.
                        // See `boardPoint` for the measurement and for the two
                        // cures (reordering `BoardDrop`, shrinking the battle
                        // slack) that were rejected.
                        if wasDragging { onDragEnded(c, Self.boardPoint(g.location, hand: handFrameSelf)) }
                    }
            )
    }
}

/// Round-5 M6: width-measurement probe for the row-split math — see
/// `measuredWidth`'s doc on why `FHandFan` needs this two-step preference
/// round-trip instead of computing its outer `.frame(height:)` directly.
private struct HandWidthKey: PreferenceKey {
    static let defaultValue: CGFloat = 0
    static func reduce(value: inout CGFloat, nextValue: () -> CGFloat) {
        let n = nextValue()
        if n > 0 { value = n }
    }
}
