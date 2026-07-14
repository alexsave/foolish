// TableScreen.swift — Option G page 1 (docs/WATCHOS_G_SPEC.md §2). Play happens here.
// Top→bottom: InfoLine · SeatStrip · TablePager · then a split bottom band — the big
// Crown-FocusSlot on the left, the action Pill and the hand ChipStrip on the right.
// Every value is the kernel's masked GameView + legal menu (via WatchGame); no rule
// logic lives in the view. All frames reference the 40 mm baseline (162×197 pt) and
// scale proportionally.

import SwiftUI

struct TableScreen: View {
    @ObservedObject var game: WatchGame

    @State private var focusRaw: Double = 0
    @State private var chooser: ChooserSpec?
    @State private var glow: Double = 0        // RejectGlow opacity (§7)

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

                infoLine
                    .frame(width: w - 26 * sx, alignment: .trailing)
                    .position(x: w / 2, y: 40 * sy)

                SeatStrip(chips: game.seatStrip, width: w - 10)
                    .position(x: w / 2, y: 61 * sy)

                TablePager(battles: game.battles, optimistic: game.optimistic, sx: sx, sy: sy)
                    .frame(width: w, height: 62 * sy)
                    .position(x: w / 2, y: 104 * sy)

                if game.focusCount > 0 {
                    focusSlot
                        .frame(width: 60 * sx, height: 62 * sy)
                        .position(x: 36 * sx, y: 163 * sy)

                    if let d = game.decision(for: focusedItem) {
                        pill(d, sx: sx)
                            .position(x: 112 * sx, y: 152 * sy)
                    }

                    chipStrip(sx: sx)
                        .frame(width: 88 * sx, height: 28 * sy, alignment: .leading)
                        .position(x: 115 * sx, y: 184 * sy)
                }
            }
            .frame(width: w, height: h)
            // RejectGlow — inset red edge flash on a reject (§7).
            .overlay(
                RoundedRectangle(cornerRadius: w * 0.5, style: .continuous)
                    .strokeBorder(WColor.red, lineWidth: 4 * sx)
                    .blur(radius: 5 * sx)
                    .opacity(glow)
                    .allowsHitTesting(false)
            )
        }
        .background(WColor.bg.ignoresSafeArea())
        .focusable(game.focusCount > 1)
        .modifier(FocusCrown(value: $focusRaw, count: game.focusCount))
        .onAppear {
            focusRaw = Double(game.firstLegalIndex)
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

    private func tap(_ d: PillDecision) {
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
        withAnimation(.easeOut(duration: 0.6)) { glow = 0 }
    }

    // MARK: InfoLine (§2)

    private var infoLine: some View {
        HStack(spacing: 6) {
            if let f = game.flipped {
                Glyph(card: f, size: 11)
            } else if let s = game.trumpSuit {
                Text(s.glyph).font(WFont.token(12)).foregroundStyle(s.glyphColor)
            }
            HStack(spacing: 2) {
                Image(systemName: "rectangle.portrait.on.rectangle.portrait.fill").font(.system(size: 8))
                Text("\(game.deckCount)").font(WFont.label(12))
            }
            HStack(spacing: 2) {
                Image(systemName: "square.stack.3d.down.right.fill").font(.system(size: 8))
                Text("\(game.discardCount)").font(WFont.label(12))
            }
        }
        .foregroundStyle(WColor.info)
    }

    // MARK: FocusSlot (§2) — big preview of the focused item

    @ViewBuilder private var focusSlot: some View {
        switch focusedItem {
        case .card(let c):
            Glyph(card: c, size: 38, dim: !game.isLegal(c))
        case .terminal:
            switch game.terminalKind {
            case .good:
                Image(systemName: "checkmark").font(.system(size: 44, weight: .bold))
                    .foregroundStyle(game.iVoted ? WColor.seat : WColor.green)
            case .take(let n):
                Text("+\(n)").font(WFont.heavy(30)).foregroundStyle(WColor.red)
            }
        }
    }

    // MARK: Pill (§2, §5)

    private func pill(_ d: PillDecision, sx: CGFloat) -> some View {
        Text(d.label)
            .font(WFont.heavy(13))
            .lineLimit(1).minimumScaleFactor(0.75)
            .foregroundStyle(d.style.fg)
            .padding(.horizontal, 12 * sx)
            .frame(minWidth: 86 * sx, minHeight: 28 * sx)
            .background(RoundedRectangle(cornerRadius: 14 * sx, style: .continuous).fill(d.style.bg))
            .contentShape(Rectangle())
            .onTapGesture { tap(d) }
    }

    // MARK: ChipStrip (§2) — hand cards + terminal, Crown-scrollable

    private func chipStrip(sx: CGFloat) -> some View {
        ScrollViewReader { proxy in
            ScrollView(.horizontal, showsIndicators: false) {
                HStack(spacing: 4.5 * sx) {
                    ForEach(Array(game.hand.enumerated()), id: \.element) { i, c in
                        Glyph(card: c, size: 15, focused: i == focusIndex, dim: !game.isLegal(c))
                            .id(i)
                            .onTapGesture { focusRaw = Double(i) }
                    }
                    terminalChip.id(game.hand.count)
                        .onTapGesture { focusRaw = Double(game.hand.count) }
                }
                .padding(.trailing, 15 * sx)   // room for the right-edge fade
            }
            .mask(
                LinearGradient(colors: [.black, .black, .clear],
                               startPoint: .leading, endPoint: .trailing)
            )
            .onChange(of: focusIndex) { i in
                withAnimation(.easeOut(duration: 0.15)) { proxy.scrollTo(i, anchor: .center) }
            }
        }
    }

    @ViewBuilder private var terminalChip: some View {
        switch game.terminalKind {
        case .good:
            Image(systemName: "checkmark")
                .font(.system(size: 13, weight: .bold))
                .foregroundStyle(game.iVoted ? WColor.seat : WColor.green)
                .frame(width: 22, height: 25)
                .overlay(chipOutline(focusIndex == game.hand.count))
        case .take(let n):
            Text("+\(n)").font(WFont.heavy(11)).foregroundStyle(WColor.red)
                .frame(width: 22, height: 25)
                .overlay(chipOutline(focusIndex == game.hand.count))
        }
    }
    private func chipOutline(_ on: Bool) -> some View {
        RoundedRectangle(cornerRadius: 4, style: .continuous)
            .strokeBorder(.white, lineWidth: 1.25).opacity(on ? 1 : 0)
    }
}

// MARK: - SeatStrip (§2)

private struct SeatStrip: View {
    let chips: [SeatChip]
    let width: CGFloat
    private let gap: CGFloat = 3

    var body: some View {
        let n = max(chips.count, 1)
        // Eight 22 pt cells overflow a 40 mm face; size the cell to fit the row so the
        // edge seats (you're always last) never clip.
        let cell = min(22, (width - CGFloat(n - 1) * gap) / CGFloat(n))
        HStack(spacing: gap) {
            ForEach(chips) { c in seat(c, cell: cell) }
        }
        .frame(width: width)
    }
    private func seat(_ c: SeatChip, cell: CGFloat) -> some View {
        ZStack {
            if c.isDefender {
                ShieldShape().stroke(WColor.gold, lineWidth: 1.25)
                    .frame(width: cell * 0.92, height: cell * 1.12)
            }
            Text("\(c.count)")
                .font(WFont.heavy(min(12.5, cell * 0.78)))
                .minimumScaleFactor(0.7)
                .foregroundStyle(color(c))
        }
        .frame(width: cell, height: cell * 1.2)
        .opacity(c.isOut ? 0.3 : 1)
    }
    private func color(_ c: SeatChip) -> Color {
        if c.isSelf { return WColor.gold }
        if c.isGood { return WColor.green }
        return WColor.seat
    }
}

// MARK: - TablePager (§2) — two pairs per page; cover ▸ attack

private struct TablePager: View {
    let battles: [BattleView]
    let optimistic: Card?
    let sx: CGFloat
    let sy: CGFloat

    /// Pages of at most two battles each.
    private var pages: [[Int]] {
        let idx = Array(battles.indices)
        return stride(from: 0, to: max(idx.count, 1), by: 2).map { Array(idx[$0..<min($0 + 2, idx.count)]) }
    }

    var body: some View {
        if battles.isEmpty {
            Text("—").font(WFont.token(18)).foregroundStyle(WColor.faint)
        } else if pages.count == 1 {
            page(pages[0])
        } else {
            TabView {
                ForEach(Array(pages.enumerated()), id: \.offset) { _, p in page(p) }
            }
            .tabViewStyle(.page(indexDisplayMode: .automatic))
        }
    }

    private func page(_ ids: [Int]) -> some View {
        HStack(spacing: 15 * sx) {
            ForEach(ids, id: \.self) { i in pair(battles[i]) }
        }
        .frame(maxWidth: .infinity)
    }

    private func pair(_ b: BattleView) -> some View {
        let resolved = b.defense != nil
        let isOptimistic = optimistic != nil && (b.attack == optimistic || b.defense == optimistic)
        return HStack(spacing: 3 * sx) {
            if let cov = b.defense {
                Glyph(card: cov, size: 20)
                Text("▸").font(.system(size: 9)).foregroundStyle(WColor.arrow)
            }
            Glyph(card: b.attack, size: 20)
        }
        .opacity(isOptimistic ? 0.45 : (resolved ? 0.42 : 1))
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
