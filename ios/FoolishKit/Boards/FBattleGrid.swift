// FBattleGrid.swift — the centre of the table, matching the WEB TableBattles:
// attack/cover pairs that WRAP into rows (never a single column), each pair a
// 62x84 slot holding 50x70 cards. The cover card fans +11.25° and the attack
// tilts -11.25° once covered (web COVER_ROTATION = PI/16), both pivoting about
// their bottom-centre so the defender's card lies across the attacker's. Uncovered
// attacks are the cover drop targets (highlighted via `coverable`); the tap
// handler is owned by the board.

import SwiftUI

public struct FBattleGrid: View {
    public let battles: [BattleView]
    public let trumpSuit: Suit?
    /// Battle indices the local defender may currently cover (highlight them).
    public let coverable: Set<Int>
    public let onTapBattle: (Int) -> Void
    /// Shared card-flight namespace (a card matches its hand slot as it lands).
    public let namespace: Namespace.ID?
    /// Card identities the board is rendering as NOT YET THERE — either in
    /// overlay flight right now (so the flying ghost is the only copy, web
    /// CardFace opacity:0 while animating) or about to be, on a board whose
    /// opening replay has not started yet. One set, one authority: a cover in
    /// here has not landed, so the attack under it does not lie across.
    public let hidden: Set<String>
    /// note 34: while a drag over open table space would resolve to a PASS,
    /// the board shows this empty preview slot instead of highlighting any
    /// existing battle (nothing on the table is about to be covered). Defaulted
    /// false so every existing call site (MessageBoardView, TableView, the
    /// gallery/snapshot tests) keeps compiling unchanged.
    public let showGhostSlot: Bool

    public init(battles: [BattleView], trumpSuit: Suit?, coverable: Set<Int> = [],
                onTapBattle: @escaping (Int) -> Void = { _ in }, namespace: Namespace.ID? = nil,
                hidden: Set<String> = [], showGhostSlot: Bool = false) {
        self.battles = battles
        self.trumpSuit = trumpSuit
        self.coverable = coverable
        self.onTapBattle = onTapBattle
        self.namespace = namespace
        self.hidden = hidden
        self.showGhostSlot = showGhostSlot
    }

    private let cardSize = CGSize(width: 50, height: 70)   // web card 50x70
    private let slot = CGSize(width: 62, height: 84)       // web 60x80 (+room to rotate)
    /// The laid-across tilt (web PI/16). Public + static so a bout-end / open
    /// replay flight (MessageTableView) can rotate a cover ghost INTO exactly
    /// this angle mid-flight (round-6 bug 1) rather than hard-coding a second copy.
    public static let coverAngle: Double = 11.25
    private let gap: CGFloat = 10
    // Round-5 M5 ("maybe we do rows of 3 instead of 4?"): deliberately NOT web
    // parity any more. The web's ~4-across assumes its own wider board; this
    // extension's stage is narrower (M5's own finding: at 6-8 players the
    // table drew straight through the seat names and badges), and 3 across is
    // what keeps a battle pair clear of the seat ring at every player count.
    private let perRow = 3

    public var body: some View {
        // CENTERED wrapped rows (web flex-wrap + justify-center). A LazyVGrid left-
        // aligns its columns, so a single battle sat at the left; chunking into
        // centered HStacks keeps the cluster centered at any count, and the VStack
        // self-sizes (no GeometryReader).
        //
        // note 34: the simplest correct way to fit the ghost slot into this same
        // wrap math is to chunk over `battles.count + 1` (a virtual extra index)
        // rather than special-casing the last row — it lands wherever the next
        // real battle would, wrapping to a new row exactly like a real one would.
        let total = battles.count + (showGhostSlot ? 1 : 0)
        let rows = stride(from: 0, to: total, by: perRow).map { Array($0..<min($0 + perRow, total)) }
        VStack(spacing: 12) {
            ForEach(Array(rows.enumerated()), id: \.offset) { _, row in
                HStack(spacing: gap) {
                    ForEach(row, id: \.self) { idx in
                        if idx < battles.count {
                            pair(battles[idx], index: idx)
                                .contentShape(Rectangle())
                                .onTapGesture { onTapBattle(idx) }
                        } else {
                            ghostSlot()
                        }
                    }
                }
            }
        }
    }

    /// note 34: the pass-preview slot — same 62x84 footprint as a real battle
    /// pair (so the wrap/centering math above doesn't need to treat it
    /// specially), with a dashed win-colored 50x70 placeholder previewing
    /// where the passed card would land. Deliberately does NOT publish a
    /// `BattleFramesKey` entry — it must never become a drop target or shift
    /// `BoardDrop.target`'s hit-testing — and carries no tap gesture.
    private func ghostSlot() -> some View {
        RoundedRectangle(cornerRadius: 7)
            .strokeBorder(FColor.win, style: StrokeStyle(lineWidth: 2.5, dash: [6, 4]))
            .background(RoundedRectangle(cornerRadius: 7).fill(FColor.win.opacity(0.12)))
            .frame(width: cardSize.width, height: cardSize.height)
            .frame(width: slot.width, height: slot.height, alignment: .bottom)
            .allowsHitTesting(false)
            .accessibilityHidden(true)
    }

    /// Should the attacked card lie across (tilted)? Exactly when it has a
    /// defender AND that defender is really on the table — not in flight, and
    /// not queued for a flight the board has not started yet. Pulled out of the
    /// view body so the sequence this got wrong can be asserted directly:
    /// upright on arrival, upright while the cover flies, tilted only once it
    /// lands. The caller owns what goes in `hidden`; see MessageTableView's
    /// `veiledCardIds`, which is why this needs no second "have we settled yet"
    /// input to stay honest on the first paint.
    public static func coverLanded(defense: Card?, hidden: Set<String>) -> Bool {
        guard let d = defense else { return false }
        return !hidden.contains(d.identity)
    }

    private func pair(_ battle: BattleView, index: Int) -> some View {
        let covered = battle.defense != nil
        // The attacked card tilts only once the cover has LANDED — i.e. the cover's
        // flight has cleared the in-flight (`hidden`) set — NOT the instant the model
        // says covered. So while the cover is still flying in, the attacked card
        // stays upright, then both lay across together (web behavior).
        //
        // That covers the FIRST paint too, because `hidden` already carries the
        // cards a not-yet-started open-replay is going to fly (MessageTableView
        // .veiledCardIds derives them synchronously from the controller, rather
        // than waiting for the pre-hide that an onChange delivers a paint late).
        // Without that a pair arriving already covered flashed tilted, snapped
        // upright as the pre-hide landed, then tilted again as the cover flew
        // in — and a cover from an OLDER bubble, which this open does not
        // replay at all, animated its tilt from scratch on load ("I see that
        // the first cover rotates a bit as soon as we load").
        let coverLanded = Self.coverLanded(defense: battle.defense, hidden: hidden)
        return ZStack(alignment: .bottom) {
            FCard(card: battle.attack,
                  trump: trumpSuit != nil && battle.attack.suit == trumpSuit,
                  size: cardSize)
                // Cover-target highlight, drawn ON the card so it stays centred on
                // it (an uncovered attack is upright, so a plain inset ring lines
                // up exactly - the old slot-level ring floated above the card).
                .overlay {
                    if coverable.contains(index) && !covered {
                        RoundedRectangle(cornerRadius: 7)
                            .strokeBorder(FColor.win, lineWidth: 2.5)
                            .padding(-3)
                    }
                }
                .opacity(hidden.contains(battle.attack.identity) ? 0 : 1)
                .rotationEffect(.degrees(coverLanded ? -Self.coverAngle : 0), anchor: .bottom)
                .animation(.easeOut(duration: 0.22), value: coverLanded)
                .zIndex(covered ? 1 : 2)
                .modifier(FlightID(id: battle.attack.identity, namespace: namespace))

            if let defense = battle.defense {
                FCard(card: defense,
                      trump: trumpSuit != nil && defense.suit == trumpSuit,
                      size: cardSize)
                    .opacity(hidden.contains(defense.identity) ? 0 : 1)
                    .rotationEffect(.degrees(Self.coverAngle), anchor: .bottom)   // laid across (§5.4)
                    .zIndex(2)
                    .modifier(FlightID(id: defense.identity, namespace: namespace))
            }
        }
        .frame(width: slot.width, height: slot.height, alignment: .bottom)
        // Publish this slot's frame so a drag can hit-test the drop against it.
        .background(GeometryReader { g in
            Color.clear.preference(key: BattleFramesKey.self,
                                   value: [index: g.frame(in: .named(boardSpace))])
        })
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(a11y(battle))
    }

    // Round-5 m2 ("VoiceOver labels are hard-coded English while all visible
    // strings are localized"): this pair used to build its own English
    // sentence out of `CardRank.spoken` (which is not localized — it's a
    // debug/log helper, not a VoiceOver one) and a hand-rolled suit array.
    // FStrings.spokenCard is the ONE shared builder every board component now
    // routes through (FHandFan's cards go through FCard's own a11y label,
    // which already used it), so "queen of spades" / "дама, пики" / "스페이드
    // 퀸" cannot drift apart between the hand and the battles.
    private func a11y(_ b: BattleView) -> String {
        let atk = name(b.attack)
        if let d = b.defense {
            return FStrings.t("ios.a11y.covered", ["attack": atk, "defense": name(d)])
        }
        return FStrings.t("ios.a11y.uncovered", ["attack": atk])
    }
    private func name(_ c: Card) -> String {
        guard let suit = c.suit else { return FStrings.t("ios.a11y.hiddencard") }
        return FStrings.spokenCard(c.v, suit)
    }
}
