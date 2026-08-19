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

import SwiftUI

public struct FHandFan: View {
    public let cards: [Card]
    public let trumpSuit: Suit?
    /// Cards currently locked (in-flight / illegal in this context) — dimmed.
    public let disabled: Set<String>
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
    /// `displayCards`. This is PURELY a UI convenience (the kernel hand order
    /// never changes). Round-8 #4: seeded from `initialOrder` at view creation
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
    /// Round-6 bug 5: the vector from the finger to the dragged card's visual
    /// CENTRE, captured once when the gesture claims a card (translation is
    /// still ~0 then, so the published frame above is still the honest resting
    /// slot). The card tracks the finger rigidly for the rest of the drag, so
    /// `finger + grabDelta` IS the card's centre at every later moment — no
    /// double counting, no preference lag. Zero when the card's frame has not
    /// published yet, which degrades to "the hint rides the finger", the
    /// pre-round-5 behavior.
    @State private var grabDelta: CGSize = .zero
    /// Round-5 M6: this view's own proposed width, measured once via a
    /// preference round-trip (see `HandWidthKey`'s doc) so the row-split
    /// height below can react to it. A plain `GeometryReader` has no notion of
    /// an "ideal" height independent of what its parent proposes — there is no
    /// `.fixedSize` escape hatch for it — so this is the standard two-step fix:
    /// measure width, THEN size height from it.
    @State private var measuredWidth: CGFloat = 0
    /// Cumulative x-compensation from in-flight reorders, subtracted from
    /// `dragOffset` so the DRAGGED card's own `.offset` doesn't jump when its
    /// slot in `order` moves out from under it (only the OTHER cards should
    /// visibly slide — the dragged one keeps tracking the finger).
    @State private var reorderShift: CGFloat = 0

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
    /// never report a split there is nothing to actually split (`rowGroups`
    /// relies on that: it needs more than one card before it will ever hand
    /// back two arrays).
    public static func rowCount(cards: [Card], availableWidth: CGFloat) -> Int {
        guard cards.count > 1 else { return 1 }
        return Self.singleRowCardWidth(count: cards.count, availableWidth: availableWidth) < Self.twoRowThreshold ? 2 : 1
    }

    /// Split `cards` into 1 or 2 display rows for `availableWidth`. The FIRST
    /// row gets the extra card on an odd count. Row membership is also what
    /// constrains the in-hand reorder to "within a row" — see `reorder`.
    private static func rowGroups(_ cards: [Card], availableWidth: CGFloat) -> [[Card]] {
        guard Self.rowCount(cards: cards, availableWidth: availableWidth) == 2 else { return [cards] }
        let firstCount = (cards.count + 1) / 2   // ceil: extra card up top on odd counts
        return [Array(cards.prefix(firstCount)), Array(cards.suffix(from: firstCount))]
    }

    /// The fan's total on-screen height at `availableWidth` — one row (full
    /// `rowH`, or half that under `topHalfOnly` — round-5 M5b) normally, or two
    /// such rows stacked with `rowGap` between once M6 splits the hand. Public
    /// and static so MessageTableView can reserve exactly this much room above
    /// the hand (see its `handLift`) instead of guessing at a second constant.
    public static func height(cards: [Card], availableWidth: CGFloat, crop: CGFloat) -> CGFloat {
        let oneRow = Self.shownCardHeight(crop: crop) + 8
        return Self.rowCount(cards: cards, availableWidth: availableWidth) == 2 ? oneRow * 2 + Self.rowGap : oneRow
    }

    /// `topHalfOnly` convenience overload — the pre-round-6 spelling, kept so
    /// TableView/GalleryView/the snapshot tests that reserve room off this
    /// function compile unchanged. `false` → crop 0 (full card), `true` → crop 1
    /// (top half), so every pinned number in Round5BoardTests still holds exactly.
    public static func height(cards: [Card], availableWidth: CGFloat, topHalfOnly: Bool = false) -> CGFloat {
        Self.height(cards: cards, availableWidth: availableWidth, crop: topHalfOnly ? 1 : 0)
    }

    /// The resting SLOT rect of every card in a hand of `cards`, laid out in a
    /// container `width` wide at collapse `crop` — the SAME geometry `body`
    /// renders, expressed as a PURE function.
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
    /// time. It mirrors `body` exactly: `rowGroups` split, `singleRowCardWidth`,
    /// the row centred in `width`, the VStack centred in `height(...)`.
    ///
    /// Rects are in the container's LOCAL space (origin at its top-leading); the
    /// caller offsets by the hand's own frame origin in `boardSpace`. `cards` is
    /// the set the fan actually lays out at that instant (present + whatever this
    /// step just opened) - the incoming card sits at the END, exactly where the
    /// fan puts a freshly dealt card, so its slot matches even if the player has
    /// cosmetically reordered the cards already in hand.
    public static func slotRects(cards: [Card], width: CGFloat, crop: CGFloat) -> [String: CGRect] {
        guard width > 0, !cards.isEmpty else { return [:] }
        let rows = Self.rowGroups(cards, availableWidth: width)
        let cardW = Self.singleRowCardWidth(count: rows[0].count, availableWidth: width)
        let cardH = Self.shownCardHeight(crop: crop)
        let containerH = Self.height(cards: cards, availableWidth: width, crop: crop)
        let rowN = rows.count
        let vstackH = CGFloat(rowN) * cardH + CGFloat(max(0, rowN - 1)) * Self.rowGap
        let vTop = (containerH - vstackH) / 2
        var out: [String: CGRect] = [:]
        for (r, row) in rows.enumerated() {
            let rowW = CGFloat(row.count) * cardW + CGFloat(max(0, row.count - 1)) * Self.gap
            let rowLeft = (width - rowW) / 2
            let y = vTop + CGFloat(r) * (cardH + Self.rowGap)
            for (c, card) in row.enumerated() {
                out[card.identity] = CGRect(x: rowLeft + CGFloat(c) * (cardW + Self.gap),
                                            y: y, width: cardW, height: cardH)
            }
        }
        return out
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

    /// In-hand reorder (note 5): turn the drag's x (within this view's own
    /// width) into a slot index and, if it differs from the dragged card's
    /// current slot, splice `order` there under a spring — the other cards
    /// slide apart, mirroring the web's live reorder feel. `cardW` is this
    /// row's per-evaluation slot width (depends on the row's own width/count).
    ///
    /// Round-5 M6: `rowStart`/`rowCount` locate the dragged card's OWN row
    /// within the flat `order` array, and `to` is clamped to stay inside it —
    /// once a hand splits into two rows, letting a card cross the boundary
    /// would read as it randomly resorting which cards sit on top vs bottom.
    /// For a one-row hand `rowStart` is always 0 and `rowCount` the whole hand,
    /// so this is a strict superset of the pre-M6 behavior (nothing changes
    /// there). This is a deliberate simplification, not an oversight — a
    /// cross-row drag still WORKS as a play-drag once it leaves `handFrameSelf`
    /// entirely; it just can't reorder while still hovering the other row.
    private func reorder(_ card: Card, at point: CGPoint, cardW: CGFloat,
                         rowStart: Int, rowCount: Int) {
        let current = displayCards.map(\.identity)
        guard let from = current.firstIndex(of: card.identity) else { return }
        let slot = cardW + Self.gap
        guard slot > 0, rowCount > 0 else { return }

        // Target slot by HIT-TESTING the row's real card frames, not by
        // recomputing geometry from the hand's left edge. The arithmetic
        // version ("(x - gap) / slot, rounded") silently assumed the cards
        // start at the container's leading edge — but the row is CENTRED in
        // it, so any hand narrow enough not to fill the width (cardW is capped
        // at maxCardW) sits inset by an amount the formula never subtracted.
        // Every slot it computed was shifted, which is how a plain tap on card
        // 3 could resolve to slot 2 and swap two cards under your finger.
        // The published frames are the layout's own answer and cannot drift
        // from it.
        let rowIds = current[rowStart..<min(rowStart + rowCount, current.count)]
        var to = from
        var bestDistance = CGFloat.greatestFiniteMagnitude
        for (offset, id) in rowIds.enumerated() {
            guard let frame = handCardFramesSelf[id] else { continue }
            let distance = abs(frame.midX - point.x)
            if distance < bestDistance {
                bestDistance = distance
                to = rowStart + offset
            }
        }
        guard bestDistance < .greatestFiniteMagnitude else { return }
        guard to != from else { return }
        var next = current
        next.remove(at: from)
        next.insert(card.identity, at: min(to, next.count))
        // Both mutations animate together (same spring, same transaction) so
        // the compensation tracks the layout's own transition instead of
        // stepping instantly while the slide is still mid-spring.
        withAnimation(FMotion.card) {
            order = next
            reorderShift += CGFloat(to - from) * slot
        }
        // Round-8 #4: report the arrangement OUTSIDE the animation transaction
        // (persisting is not a visual change). `next` is the full display
        // order, sticky ids included, exactly what a later seed restores.
        onOrderChanged(next)
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
            let rows = Self.rowGroups(laidOutCards, availableWidth: width)
            // ONE card width for BOTH rows, sized by the fuller first row (it
            // gets the ceil on odd counts) — sizing each row by its own count
            // made the shorter bottom row's cards visibly WIDER than the top
            // row's, which read as two different decks rather than one hand
            // that wrapped.
            let cardW = Self.singleRowCardWidth(count: rows[0].count, availableWidth: width)
            VStack(spacing: Self.rowGap) {
                ForEach(Array(rows.enumerated()), id: \.offset) { rowIdx, rowCards in
                    let rowStart = rows.prefix(rowIdx).reduce(0) { $0 + $1.count }
                    HStack(spacing: Self.gap) {
                        ForEach(rowCards, id: \.identity) { card in
                            cardView(card, cardW: cardW, rowStart: rowStart, rowCount: rowCards.count)
                        }
                    }
                }
            }
            .frame(width: width, height: geo.size.height, alignment: .center)
            // Round-8: animate the fan's own re-layout when the laid-out SET
            // changes, so present cards SLIDE to their new slots as one is
            // added/removed (a deal makes room, a play closes the gap) instead of
            // snapping. INSIDE the GeometryReader and ON the row VStack on purpose:
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
    }

    /// One hand card, wired for tap/drag/reorder within its OWN row (see
    /// `reorder`'s doc for why row membership constrains it).
    @ViewBuilder
    private func cardView(_ card: Card, cardW: CGFloat, rowStart: Int, rowCount: Int) -> some View {
        FCard(card: card,
              selected: selection.contains(card.identity),
              disabled: disabled.contains(card.identity),
              trump: trumpSuit != nil && card.suit == trumpSuit,
              size: CGSize(width: cardW, height: Self.cardH))
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
            .frame(height: Self.shownCardHeight(crop: crop), alignment: .top)
            .opacity(hidden.contains(card.identity) ? 0 : 1)
            // Round-8: the veil SNAPS - opacity never animates (belt to the exit
            // transition below), so a card cannot half-fade under the fan's own
            // slide animation or the board spring.
            .animation(nil, value: hidden.contains(card.identity))
            // Round-8: an overlay-flight board exits a card INSTANTLY (no fade);
            // see `instantExit`. Everyone else keeps the default cross-fade.
            .transition(instantExit ? .identity : .opacity)
            // Round-7 #2: a card the board's overlay is flying (it is in `hidden`)
            // must NOT also carry matchedGeometry, or SwiftUI flies it a SECOND
            // time from wherever it left (a picked-up card's grid slot) into this
            // fan while the overlay is already flying it there - the "double
            // animation" for pickup. A hidden card drops its matched namespace so
            // it just appears here (opacity 0) and the overlay flies it in once. At
            // rest (not hidden) the namespace stays, a no-op. Empty `hidden` (the
            // offline board) keeps every card matched, exactly as before.
            .modifier(FlightID(id: card.identity, namespace: hidden.contains(card.identity) ? nil : namespace))
            .background(GeometryReader { g in
                Color.clear.preference(key: HandCardFramesKey.self,
                                       value: [card.identity: g.frame(in: .named(boardSpace))])
            })
            .contentShape(Rectangle())
            .offset(dragId == card.identity
                    ? CGSize(width: dragOffset.width - reorderShift, height: dragOffset.height)
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
                        if dragId != card.identity {
                            dragId = card.identity; reorderShift = 0
                            // Round-6 bug 5: capture finger -> card-centre ONCE,
                            // while the published frame is still the resting slot
                            // (see `grabDelta` / `handCardFramesSelf`). `crop`
                            // matters here: the reserved frame is
                            // `shownCardHeight` tall while the card is drawn full
                            // `cardH` tall and TOP-aligned in it, so the visible
                            // middle sits half the cropped-away part lower.
                            if let rest = handCardFramesSelf[card.identity] {
                                let cropDrop = (Self.cardH - Self.shownCardHeight(crop: crop)) / 2
                                grabDelta = CGSize(width: rest.midX - g.startLocation.x,
                                                   height: rest.midY + cropDrop - g.startLocation.y)
                            } else {
                                grabDelta = .zero
                            }
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
                            onDragChanged(card, g.location)
                        }
                        // The dragged card's live visual CENTRE, for the board's
                        // verb-hint pill (round-5 finding 5) and for the flight
                        // that now starts where the finger let go (round-6 bug
                        // 13). Straight off the gesture: the card is rigidly
                        // pinned to the finger by `grabDelta`, so this is exact
                        // at every frame. `reorderShift` is the one thing that
                        // moves the card RELATIVE to the finger — the same
                        // compensation the `.offset` above applies when the
                        // dragged card's slot in `order` moves out from under
                        // it — so it comes off the x here too.
                        //
                        // Deliberately NOT `restingFrame + dragOffset` any more:
                        // that double-counted the drag (see
                        // `handCardFramesSelf`), which is round-6 bug 5.
                        onDragCardMoved(CGPoint(x: g.location.x + grabDelta.width - reorderShift,
                                                y: g.location.y + grabDelta.height))
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
                            reorder(card, at: g.location, cardW: cardW,
                                    rowStart: rowStart, rowCount: rowCount)
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
                            dragId = nil; dragOffset = .zero; reorderShift = 0
                        }
                        dragMoved = false
                        if hypot(g.translation.width, g.translation.height) < Self.tapThreshold {
                            // A tap, not a drag — same disabled-check + haptic
                            // + `onTap` the old `.onTapGesture` used to run.
                            guard !disabled.contains(c.identity) else { Haptics.fire(.reject); return }
                            Haptics.fire(.pickUp)
                            onTap(c)
                            return
                        }
                        if wasDragging { onDragEnded(c, g.location) }
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
