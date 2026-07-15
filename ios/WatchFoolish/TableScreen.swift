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


    @State private var focusRaw: Double = 0
    @State private var chooser: ChooserSpec?
    @State private var glow: Double = 0        // RejectGlow opacity (§7)

    /// `-focus N` parks the lane on item N so the full ±2 fisheye window can be inspected
    /// without a Crown (the simulator has none).
    static let launchFocus: Int? = {
        let args = ProcessInfo.processInfo.arguments
        guard let i = args.firstIndex(of: "-focus"), i + 1 < args.count else { return nil }
        return Int(args[i + 1])
    }()

    private var focusIndex: Int {
        min(max(Int(focusRaw.rounded()), 0), max(game.focusCount - 1, 0))
    }
    private var focusedItem: FocusItem { game.item(at: focusIndex) }

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
                    FisheyeLane(game: game, focusIndex: focusIndex, sx: sx, sy: sy,
                                onCommit: { commitFocused() },
                                onFocus: { focusRaw = Double($0) })
                        .frame(width: HTuning.laneW * sx, height: h)
                        .position(x: HTuning.laneX * sx, y: h / 2)

                    caption
                        .frame(width: HTuning.captionW * sx)
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
        .modifier(FocusCrown(value: $focusRaw, count: game.focusCount))
        .onAppear {
            focusRaw = Double(Self.launchFocus ?? game.firstLegalIndex)
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
        focusRaw = Double(game.firstLegalIndex)
    }
    private func clampFocus() {
        // Focus never wraps; if the focused card left the hand, fall back to first legal.
        if case .card(let c) = focusedItem, !game.hand.contains(c) {
            focusRaw = Double(game.firstLegalIndex)
        } else {
            focusRaw = Double(min(max(focusIndex, 0), max(game.focusCount - 1, 0)))
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
                label("DISC", 2)
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

    private var rows: some View {
        VStack(spacing: HTuning.tableRowGap * sy) {
            ForEach(Array(battles.enumerated()), id: \.offset) { _, b in row(b) }
        }
    }

    private func row(_ b: BattleView) -> some View {
        let resolved = b.defense != nil
        let isOptimistic = optimistic != nil && (b.attack == optimistic || b.defense == optimistic)
        return HStack(spacing: HTuning.tableColGap * sx) {
            cell(b.defense)
            Text("▸")
                .font(.system(size: HTuning.tableArrowSize, weight: .heavy))
                .foregroundStyle(WColor.arrow)
                .opacity(resolved ? 1 : 0)
                .frame(width: HTuning.tableArrowW * sx)
            cell(b.attack)
        }
        .frame(height: rowH * sy)
        // grayscale + dim-toward-black: on a pure-black canvas opacity IS brightness, so
        // this matches the mock's `grayscale(1) brightness(.62)` exactly.
        .grayscale(resolved ? 1 : 0)
        .opacity(isOptimistic ? HTuning.tableOptimisticOpacity : (resolved ? HTuning.tableResolvedOpacity : 1))
    }

    /// A fixed-width column slot. An empty cover cell must still hold its width, so open
    /// attacks stay in the attack column instead of sliding left.
    private func cell(_ card: Card?) -> some View {
        ZStack {
            Color.clear
            if let c = card { Glyph(card: c, size: HTuning.tableGlyph) }
        }
        .frame(width: HTuning.tableCellW * sx, height: rowH * sy)
    }
}

// MARK: - FisheyeLane (§4.6) — the hand, and the only hand rendering

/// The crown-driven lane hugging the right (crown) edge: the focused chip IS the big
/// card, growing in place; two neighbours shrink away above and below. The terminal item
/// (✓ GOOD / red +n pickup) is the lane's last stop and obeys the same physics.
private struct FisheyeLane: View {
    @ObservedObject var game: WatchGame
    let focusIndex: Int
    let sx: CGFloat
    let sy: CGFloat
    let onCommit: () -> Void
    let onFocus: (Int) -> Void


    /// Ring spec by |offset| from the focus: Glyph size, centre offset above / below the
    /// focus (pt), opacity — all live from HTuning. Owner's calls: the focus is ~36 pt
    /// (suit glyph) — the mock's 42 pt cannot fit a ±2 window on a 197 pt face — and ±2 is
    /// drawn at the ±1 size, separated only by opacity. The down-offsets are the larger
    /// pair because the caption sits in that gap.
    private var ring: [(size: CGFloat, up: CGFloat, down: CGFloat, op: Double)] {
        [(HTuning.focusSize, 0, 0, 1.0),
         (HTuning.ring1Size, HTuning.ring1Up, HTuning.ring1Down, Double(HTuning.ring1Opacity)),
         (HTuning.ring2Size, HTuning.ring2Up, HTuning.ring2Down, Double(HTuning.ring2Opacity))]
    }

    var body: some View {
        ZStack {
            ForEach(-2...2, id: \.self) { off in
                let i = focusIndex + off
                if i >= 0 && i < game.focusCount {
                    let spec = ring[abs(off)]
                    item(at: i, size: spec.size)
                        .opacity(spec.op)
                        .contentShape(Rectangle())
                        .onTapGesture { off == 0 ? onCommit() : onFocus(i) }
                        .position(x: HTuning.laneW / 2 * sx,
                                  y: (HTuning.laneFocusY + (off < 0 ? -spec.up : spec.down)) * sy)
                }
            }
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

/// Binds the Crown to the focus index. Applied only when there's more than one item
/// (a `from: 0, through: 0` range is degenerate and can crash the rotation).
private struct FocusCrown: ViewModifier {
    @Binding var value: Double
    let count: Int
    func body(content: Content) -> some View {
        if count > 1 {
            content.digitalCrownRotation($value, from: 0, through: Double(count - 1),
                                         by: 1, sensitivity: .low, isContinuous: false,
                                         isHapticFeedbackEnabled: true)
        } else {
            content
        }
    }
}
