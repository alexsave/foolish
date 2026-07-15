// TableScreen.swift — Option H, "all vertical" (docs/WATCHOS_LAYOUT.md §4.6). The only
// screen needed in play. Top→bottom: InfoLine · SeatStrip (tap → Roster) · then a split
// band — the table as a VERTICAL LIST on the left (cover ▸ attack, one pair per row) and
// the hand as a crown-driven FISHEYE LANE hugging the right (crown) edge, with the action
// verb as a caption beneath it. Nothing moves horizontally; there is no pager, no chip
// strip, no focus slot and no pill.
//
// Every value is the kernel's masked GameView + legal menu (via WatchGame); no rule logic
// lives in the view. All frames reference the 40 mm baseline (162×197 pt) and scale
// proportionally.

import SwiftUI

struct TableScreen: View {
    @ObservedObject var game: WatchGame
    let onOpenRoster: () -> Void


    /// `focusDetent` snaps to whole items — it drives legality, the caption and commits.
    /// `focusLive` is the crown's continuous position and drives ONLY the lane's rendering.
    /// See FocusCrown for why both exist.
    @State private var focusDetent: Double = 0
    @State private var focusLive: Double = 0
    @State private var chooser: ChooserSpec?
    @State private var glow: Double = 0        // RejectGlow opacity (§7)

    /// `-focus N` parks the lane at position N so the fisheye can be inspected without a
    /// Crown (the simulator has none). N is FRACTIONAL on purpose: `-focus 2.5` freezes the
    /// lane exactly halfway between two cards, which is the only way to see the
    /// interpolation the crown does while turning.
    static let launchFocus: Double? = {
        let args = ProcessInfo.processInfo.arguments
        guard let i = args.firstIndex(of: "-focus"), i + 1 < args.count else { return nil }
        return Double(args[i + 1])
    }()

    private var focusIndex: Int {
        min(max(Int(focusDetent.rounded()), 0), max(game.focusCount - 1, 0))
    }

    /// Move the lane as one — the snapped value and the drawn value must not drift apart.
    private func setFocus(_ i: Int, animated: Bool = true) {
        focusDetent = Double(i)
        if animated {
            withAnimation(.spring(response: HTuning.laneSettleResponse,
                                  dampingFraction: HTuning.laneSettleDamping)) { focusLive = Double(i) }
        } else {
            focusLive = Double(i)
        }
    }
    private var focusedItem: FocusItem { game.item(at: focusIndex) }

    /// True when the lane has settled onto a card. The verb names what a tap will do, so it
    /// must not be shown mid-scroll — while the lane is moving there is no "the" card, and
    /// a caption flickering through ATTACK/GOOD/nothing as cards slide past reads as noise.
    private var isResting: Bool { abs(focusLive - focusDetent) < 0.02 }

    var body: some View {
        GeometryReader { geo in
            let w = geo.size.width, h = geo.size.height
            let sx = w / 162, sy = h / 197
            ZStack {
                WColor.bg

                // Right-aligned as the clock's column (trailing inset 13).
                infoLine
                    .position(x: HTuning.headerX * sx, y: HTuning.headerY * sy)

                // Tapping the strip is the roster's second door (the page swipe is the
                // other); the system back chevron pops to the games list.
                SeatStrip(chips: game.seatStrip, width: w - 10)
                    .contentShape(Rectangle())
                    .onTapGesture(perform: onOpenRoster)
                    .position(x: w / 2, y: HTuning.stripY * sy)

                TableList(battles: game.battles, optimistic: game.optimistic, sx: sx, sy: sy)
                    .frame(width: HTuning.tableW * sx, height: HTuning.tableH * sy)
                    .position(x: HTuning.tableX * sx, y: HTuning.tableY * sy)

                if game.focusCount > 0 {
                    FisheyeLane(game: game, position: focusLive, sx: sx, sy: sy,
                                onCommit: { commitFocused() })
                        .frame(width: HTuning.laneW * sx, height: h)
                        .position(x: HTuning.laneX * sx, y: h / 2)

                    caption
                        .frame(width: HTuning.captionW * sx)
                        .opacity(isResting ? 1 : 0)
                        .animation(.easeOut(duration: HTuning.captionFade), value: isResting)
                        .position(x: HTuning.laneX * sx, y: HTuning.captionY * sy)
                }
            }
            .frame(width: w, height: h)
            // RejectGlow — inset red edge flash on a reject (§7).
            .overlay(
                RoundedRectangle(cornerRadius: w * 0.5, style: .continuous)
                    .strokeBorder(WColor.red, lineWidth: HTuning.glowWidth * sx)
                    .blur(radius: HTuning.glowBlur * sx)
                    .opacity(glow)
                    .allowsHitTesting(false)
            )
        }
        // H's y-coordinates are absolute on the 197 pt face (the InfoLine tucks under the
        // clock, §2), so the canvas must span the whole screen — not the nav-bar-inset
        // content area a pushed page gets by default.
        .ignoresSafeArea()
        .background(WColor.bg.ignoresSafeArea())
        .focusable(game.focusCount > 1)
        .modifier(FocusCrown(detent: $focusDetent, live: $focusLive, count: game.focusCount))
        .onAppear {
            if let f = Self.launchFocus {
                focusDetent = f.rounded()      // legality still snaps to a real card
                focusLive = f                  // …but the lane can sit between two
            } else {
                setFocus(game.firstLegalIndex, animated: false)
            }
            if ProcessInfo.processInfo.arguments.contains("-chooser") { chooser = .demo }
        }
        .onChange(of: game.hand) { _ in clampFocus() }
        .onChange(of: game.rejectPulse) { _ in flashGlow() }
        // In-view overlay (no system modal chrome); the nav back-chevron is hidden while
        // it's up, so the only top-left control is the ✕ (one habit, §5 G2b).
        .navigationBarBackButtonHidden(chooser != nil)
        .overlay {
            if let spec = chooser {
                ChooserOverlay(spec: spec,
                               onCover: { commit($0) },
                               onPass: { commit($0) },
                               onClose: { chooser = nil })
            }
        }
    }

    // MARK: commit + focus housekeeping

    /// A tap on the focused lane card (or its caption) is the only commit gesture (§4.6).
    private func commitFocused() {
        guard let d = game.decision(for: focusedItem) else { return }
        switch d.action {
        case .commit(let m): commit(m)
        case .chooser(let c): chooser = c
        case .none: break
        }
    }
    private func commit(_ move: Move) {
        game.commit(move)
        chooser = nil
        WHaptics.fire(.confirmed)
        setFocus(game.firstLegalIndex)
    }
    private func clampFocus() {
        // Focus never wraps; if the focused card left the hand, fall back to first legal.
        if case .card(let c) = focusedItem, !game.hand.contains(c) {
            setFocus(game.firstLegalIndex)
        } else {
            setFocus(min(max(focusIndex, 0), max(game.focusCount - 1, 0)), animated: false)
        }
    }
    private func flashGlow() {
        WHaptics.fire(.rejected)
        glow = 1
        withAnimation(.easeOut(duration: HTuning.glowDuration)) { glow = 0 }
    }

    // MARK: InfoLine (§2) — a label line naming the columns, values under it

    /// Owner review: the header carries the words, so the row beneath is just
    /// `card icon / number / number` — no SF-symbol deck/discard glyphs to decode. The
    /// first column names itself: FLIP while the flipped card is still in the deck,
    /// TRUMP once it's been drawn (`game.c:321-333`).
    private var cols: [CGFloat] { [HTuning.colFlip, HTuning.colDeck, HTuning.colDisc] }

    private var infoLine: some View {
        VStack(spacing: HTuning.headerRowGap) {
            HStack(spacing: HTuning.headerColGap) {
                label(game.flipped != nil ? "FLIP" : "TRUMP", 0)
                label("DECK", 1)
                label("DISCARD", 2)
            }
            HStack(spacing: HTuning.headerColGap) {
                Group {
                    if let f = game.flipped {
                        Glyph(card: f, size: HTuning.flipGlyphSize)
                    } else if let s = game.trumpSuit {
                        Text(s.glyph).font(WFont.token(HTuning.trumpGlyphSize)).foregroundStyle(s.glyphColor)
                    }
                }
                .frame(width: cols[0])
                value("\(game.deckCount)", 1)
                value("\(game.discardCount)", 2)
            }
        }
    }
    private func label(_ s: String, _ i: Int) -> some View {
        Text(s).font(.system(size: HTuning.labelSize)).foregroundStyle(WColor.dim)
            .frame(width: cols[i])
    }
    private func value(_ s: String, _ i: Int) -> some View {
        Text(s).font(WFont.label(HTuning.valueSize)).foregroundStyle(WColor.info)
            .frame(width: cols[i])
    }

    // MARK: caption (§4.6) — the whole action UI

    /// The verb, sitting directly under the focused card: uppercase, gray, and the same
    /// size as the header's column labels (owner review — one size for all chrome words).
    /// Blank on a dead card.
    private var caption: some View {
        Text(game.decision(for: focusedItem)?.caption ?? "")
            .font(.system(size: HTuning.captionSize))
            .foregroundStyle(WColor.seat)
            .lineLimit(1).minimumScaleFactor(0.8)
            .frame(maxWidth: .infinity)
            .contentShape(Rectangle())
            .onTapGesture { commitFocused() }
    }
}

// MARK: - TableList (§4.6) — the whole table as rows; no pages

/// One pair per row: cover column ▸ attack column. Open attacks sit alone in the attack
/// column at full brightness; resolved rows desaturate. Rows are simply visible (centred);
/// past that the list scrolls — the app's only drag.
///
/// Owner review made the glyphs and the ▸ larger, which costs a row: four fit now, not
/// five (which is what H §4.6 asked for in the first place).
private struct TableList: View {
    let battles: [BattleView]
    let optimistic: Card?
    let sx: CGFloat
    let sy: CGFloat

    private var rowH: CGFloat { HTuning.tableRowH }

    var body: some View {
        if battles.isEmpty {
            Text("—").font(WFont.token(HTuning.tableEmptySize)).foregroundStyle(WColor.faint)
                .frame(maxWidth: .infinity, maxHeight: .infinity)
        } else if battles.count <= HTuning.tableVisibleRows {
            rows.frame(maxWidth: .infinity, maxHeight: .infinity)   // centred
        } else {
            ScrollView(.vertical, showsIndicators: false) { rows }
                .mask(LinearGradient(stops: [.init(color: .clear, location: 0),
                                             .init(color: .black, location: HTuning.tableFadeInset),
                                             .init(color: .black, location: 1 - HTuning.tableFadeInset),
                                             .init(color: .clear, location: 1)],
                                     startPoint: .top, endPoint: .bottom))
        }
    }

    /// Cards arrive from the direction the play came from: an **attack rises from the
    /// bottom** (thrown in by a player) and a **cover drops from the top** (laid down onto
    /// it). Driven off `battles` itself, so a bot's move animates exactly like your own —
    /// the snapshot is the only source either way.
    private var rows: some View {
        VStack(spacing: HTuning.tableRowGap * sy) {
            ForEach(Array(battles.enumerated()), id: \.offset) { _, b in row(b) }
        }
        .animation(.spring(response: HTuning.tableDealResponse,
                           dampingFraction: HTuning.tableDealDamping),
                   value: battles)
    }

    private func row(_ b: BattleView) -> some View {
        let resolved = b.defense != nil
        let isOptimistic = optimistic != nil && (b.attack == optimistic || b.defense == optimistic)
        return HStack(spacing: HTuning.tableColGap * sx) {
            cell(b.defense, from: .top)        // the cover comes down onto the attack
            Text("▸")
                .font(.system(size: HTuning.tableArrowSize, weight: .heavy))
                .foregroundStyle(WColor.arrow)
                .opacity(resolved ? 1 : 0)
                .frame(width: HTuning.tableArrowW * sx)
            cell(b.attack, from: .bottom)      // the attack comes up off the hand
        }
        .frame(height: rowH * sy)
        // A whole new row IS a new attack, so it enters from below like one. It must NOT
        // leave the same way: a pickup moves these very cards INTO your hand, so any slow
        // exit paints them on the table while the lane already holds them — a card in two
        // places, which this game can never actually do (single 52-card deck,
        // game.c:305-316). Leaving is a quick fade, and it is deliberately not the deal
        // spring.
        .transition(.asymmetric(
            insertion: .move(edge: .bottom).combined(with: .opacity),
            removal: .opacity.animation(.easeOut(duration: HTuning.tableClearFade))))
        // A beaten pair recedes by OPACITY ALONE. The mock desaturated it as well, but that
        // throws away the suit colour — the fastest thing you read a card by — and you can
        // still be thrown more of that rank. Dim, still red.
        .opacity(isOptimistic ? HTuning.tableOptimisticOpacity : (resolved ? HTuning.tableResolvedOpacity : 1))
    }

    /// A fixed-width column slot. An empty cover cell must still hold its width, so open
    /// attacks stay in the attack column instead of sliding left. `edge` is where the card
    /// flies in from when it lands.
    private func cell(_ card: Card?, from edge: Edge) -> some View {
        ZStack {
            Color.clear
            if let c = card {
                Glyph(card: c, size: HTuning.tableGlyph)
                    .transition(.asymmetric(
                        insertion: .move(edge: edge).combined(with: .opacity),
                        removal: .opacity.animation(.easeOut(duration: HTuning.tableClearFade))))
            }
        }
        .frame(width: HTuning.tableCellW * sx, height: rowH * sy)
        .clipped()                              // the card slides in, it doesn't overhang
    }
}

// MARK: - FisheyeLane (§4.6) — the hand, and the only hand rendering

/// The crown-driven lane hugging the right (crown) edge: the focused chip IS the big
/// card, growing in place; two neighbours shrink away above and below. The terminal item
/// (✓ GOOD / red +n pickup) is the lane's last stop and obeys the same physics.
private struct FisheyeLane: View {
    @ObservedObject var game: WatchGame
    /// The crown's CONTINUOUS position, in item indices — 2.4 means "40 % of the way from
    /// item 2 to item 3". Not an index: that's the whole point.
    let position: Double
    let sx: CGFloat
    let sy: CGFloat
    let onCommit: () -> Void


    /// The ring values are STOPS on a continuous curve, sampled at |d| = 0, 1, 2 — not five
    /// fixed slots. Every item's size, offset and opacity is interpolated from its real
    /// distance `d` to the crown's position, so a card grows into the focus as you turn
    /// instead of teleporting between rings. The down-offsets are the larger pair because
    /// the caption sits in that gap. (Owner's calls: focus ~36 pt — the mock's 42 cannot fit
    /// a ±2 window on a 197 pt face — and ±2 draws at the ±1 size, graded by opacity alone.)
    private var sizes: [CGFloat] { [HTuning.focusSize, HTuning.ring1Size, HTuning.ring2Size] }
    private var ups: [CGFloat] { [0, HTuning.ring1Up, HTuning.ring2Up] }
    private var downs: [CGFloat] { [0, HTuning.ring1Down, HTuning.ring2Down] }
    private var opacities: [CGFloat] { [1, HTuning.ring1Opacity, HTuning.ring2Opacity] }

    /// Sample the stops at |d|. Linear between 0→1 and 1→2; past 2 it keeps the last
    /// segment's slope, which is what walks the outer items off the face.
    private func sample(_ stops: [CGFloat], _ d: Double) -> CGFloat {
        let x = CGFloat(abs(d))
        if x <= 1 { return stops[0] + (stops[1] - stops[0]) * x }
        if x <= 2 { return stops[1] + (stops[2] - stops[1]) * (x - 1) }
        return stops[2] + (stops[2] - stops[1]) * (x - 2)
    }

    private func size(_ d: Double) -> CGFloat { max(1, sample(sizes, d)) }
    private func dy(_ d: Double) -> CGFloat { d < 0 ? -sample(ups, d) : sample(downs, d) }
    /// Past the last stop, fade out over one more index so items leave rather than pop.
    private func opacity(_ d: Double) -> Double {
        let x = abs(d)
        let base = Double(sample(opacities, min(x, 2)))
        return x <= 2 ? base : base * max(0, 3 - x)
    }

    /// Only the items near enough to be visible; beyond ±3 they're fully faded anyway.
    private var window: [Int] {
        let lo = max(0, Int((position - 3).rounded(.down)))
        let hi = min(game.focusCount - 1, Int((position + 3).rounded(.up)))
        return lo <= hi ? Array(lo...hi) : []
    }

    var body: some View {
        ZStack {
            // ONE tap target for the whole lane, and it always commits the focused card.
            // Per-card taps used to re-focus, which was worse than useless: the cards are
            // far too small to aim at, so "tap to act" mostly landed on a neighbour and
            // scrolled the lane instead of doing the thing. The crown moves the lane; a tap
            // only ever acts. Sits below the strip so it can't swallow the strip's own tap.
            Color.clear
                .frame(height: (197 - HTuning.laneTapTop) * sy)
                .contentShape(Rectangle())
                .onTapGesture(perform: onCommit)
                .position(x: HTuning.laneW / 2 * sx,
                          y: (HTuning.laneTapTop + (197 - HTuning.laneTapTop) / 2) * sy)

            ForEach(window, id: \.self) { i in
                let d = Double(i) - position
                item(at: i, size: size(d))
                    .opacity(opacity(d))
                    .zIndex(2 - min(abs(d), 2))          // the nearest card draws on top
                    .position(x: HTuning.laneW / 2 * sx,
                              y: (HTuning.laneFocusY + dy(d)) * sy)
            }
            .allowsHitTesting(false)                     // taps belong to the target above
        }
    }

    @ViewBuilder private func item(at index: Int, size: CGFloat) -> some View {
        switch game.item(at: index) {
        case .card(let c):
            Glyph(card: c, size: size, dim: !game.isLegal(c))
        case .terminal:
            terminal(size: size)
        }
    }

    /// Bare ✓ / ↓ — no background, sized off the same scale as a card at this ring.
    @ViewBuilder private func terminal(size: CGFloat) -> some View {
        Group {
            switch game.terminalKind {
            case .good:
                Image(systemName: "checkmark")
                    .font(.system(size: size * HTuning.checkScale, weight: .heavy))
                    .foregroundStyle(game.iVoted ? WColor.seat : WColor.green)
            case .pickup:
                Image(systemName: "arrow.down")
                    .font(.system(size: size * HTuning.arrowScale, weight: .heavy))
                    .foregroundStyle(WColor.red)
            }
        }
        .frame(width: size * HTuning.glyphFrameW, height: size * HTuning.glyphFrameH)
    }
}

// MARK: - SeatStrip (§2)

private struct SeatStrip: View {
    let chips: [SeatChip]
    let width: CGFloat


    /// The shield is drawn ONCE, floating over the row, and slides between seats; the seats
    /// themselves never draw it. `slot` is its rendered position and deliberately leaves
    /// 0..<n mid-wrap — that overshoot is what carries it off the end. See `moveShield`.
    @State private var slot: Int?
    @State private var shieldOpacity: Double = 1

    private var gap: CGFloat { HTuning.stripGap }
    private var cell: CGFloat {
        let n = max(chips.count, 1)
        // Eight cells overflow a 40 mm face; size the cell to fit the row so the edge
        // seats (you're always last) never clip. The 18 pt cap (G specified 22) buys the
        // two-line header its band without pushing the lane off the bottom.
        return min(HTuning.stripCellMax, (width - CGFloat(n - 1) * gap) / CGFloat(n))
    }
    private var pitch: CGFloat { cell + gap }
    private var defenderIndex: Int? { chips.firstIndex { $0.isDefender } }

    /// Centre of seat `i` in the strip's own coordinates. Indices outside 0..<n keep
    /// marching, which is what lets the shield exit one end and re-enter the other.
    private func x(_ i: Int) -> CGFloat {
        let n = max(chips.count, 1)
        let rowW = CGFloat(n) * cell + CGFloat(n - 1) * gap
        return (width - rowW) / 2 + cell / 2 + CGFloat(i) * pitch
    }

    var body: some View {
        ZStack(alignment: .leading) {
            HStack(spacing: gap) {
                ForEach(chips) { c in seat(c, cell: cell) }
            }
            .frame(width: width)

            if defenderIndex != nil, let s = slot {
                ShieldShape()
                    .stroke(WColor.defender, lineWidth: 1.25)
                    .frame(width: cell * HTuning.shieldW, height: cell * HTuning.shieldH)
                    .position(x: x(s), y: cell * HTuning.stripCellAspect / 2)
                    .opacity(shieldOpacity)
            }
        }
        .frame(width: width, height: cell * HTuning.stripCellAspect)
        .onAppear { slot = defenderIndex; shieldOpacity = 1 }
        .onChange(of: defenderIndex) { old, new in moveShield(from: old, to: new) }
    }

    /// The strip is a ring drawn as a line, so when the defence wraps from the last seat to
    /// the first the shield must NOT slide backwards across the whole row. It keeps going:
    /// out past the right edge, then an unanimated teleport (while invisible) to a matching
    /// slot off the left edge, then in from there. Ordinary moves are a plain spring.
    private func moveShield(from old: Int?, to new: Int?) {
        let n = chips.count
        guard let new else { slot = nil; return }
        guard let old, n > 1, old != new else {
            slot = new; shieldOpacity = 1; return
        }
        let forward = (new - old + n) % n          // distance travelling right around the ring
        let wraps = new < old && forward <= n / 2  // a leftward jump that's really a short hop right

        guard wraps else {
            withAnimation(.spring(response: 0.34, dampingFraction: 0.72)) { slot = new }
            return
        }
        withAnimation(.easeIn(duration: 0.17)) {
            slot = old + forward                   // march off the right edge
            shieldOpacity = 0
        }
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.17) {
            slot = new - forward                   // teleport off-screen left, still invisible
            shieldOpacity = 0
            DispatchQueue.main.async {             // next tick, or SwiftUI coalesces the two
                withAnimation(.easeOut(duration: 0.22)) {
                    slot = new
                    shieldOpacity = 1
                }
            }
        }
    }

    /// You are the one seat you must find instantly, and the mark is the digit's **weight**:
    /// your count is heavy, everyone else's is light. Colour never says "you" — it says
    /// state, and only state. (Two dead ends got here: an underline, which the shield sits
    /// on top of when you defend; and an outline, which SwiftUI cannot draw before watchOS
    /// 11's `textRenderer(_:)`.)
    private func seat(_ c: SeatChip, cell: CGFloat) -> some View {
        let size = min(HTuning.stripCountSize, cell * 0.78)
        return Text("\(c.count)")
            .font(.system(size: size,
                          weight: c.isSelf ? HTuning.stripSelfWeight : HTuning.stripOtherWeight,
                          design: .rounded))
            .foregroundStyle(color(c))
            .minimumScaleFactor(0.7)
            .frame(width: cell, height: cell * HTuning.stripCellAspect)
    }
    /// Colour is state and ONLY state (§4.6.1): out ▸ defender ▸ good ▸ opening ▸ plain.
    /// Red is transient — it marks the seat the table is WAITING ON to open the bout, and
    /// clears to white the moment they attack, so it never fights the green tally.
    private func color(_ c: SeatChip) -> Color {
        if c.isOut { return WColor.out }
        if c.isDefender { return WColor.defender }
        if c.isGood { return WColor.green }
        if c.isOpening { return WColor.attacker }
        return WColor.plain
    }
}

// MARK: - Crown focus modifier

/// Binds the Crown to the lane. Applied only when there's more than one item (a
/// `from: 0, through: 0` range is degenerate and can crash the rotation).
///
/// TWO values, and the split is the whole trick:
///
/// - `detent` snaps to whole items. It is what the caption reads and what a tap commits —
///   the game only ever deals in real cards.
/// - `live` is the crown's raw continuous offset, which the lane draws. It is why cards
///   grow smoothly into the focus instead of jumping ring to ring.
///
/// The plain `digitalCrownRotation(_:…by: 1)` binding only ever hands you the snapped
/// value, so the lane could never render anything between two cards. The `detent:` overload
/// gives both at once: `onChange` streams the continuous offset while you turn, and
/// `onIdle` springs `live` onto the settled detent so the lane always comes to rest
/// centred on a card rather than parked at 2.37.
private struct FocusCrown: ViewModifier {
    @Binding var detent: Double
    @Binding var live: Double
    let count: Int

    func body(content: Content) -> some View {
        if count > 1 {
            content.digitalCrownRotation(
                detent: $detent,
                from: 0, through: Double(count - 1), by: 1,
                sensitivity: .low,
                isContinuous: false,
                isHapticFeedbackEnabled: true,
                onChange: { event in live = event.offset },
                onIdle: {
                    withAnimation(.spring(response: HTuning.laneSettleResponse,
                                          dampingFraction: HTuning.laneSettleDamping)) {
                        live = detent
                    }
                })
        } else {
            content
        }
    }
}
