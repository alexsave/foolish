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
    /// Card identities currently in overlay flight — rendered invisible here so the
    /// flying ghost is the only copy (web CardFace opacity:0 while animating).
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
    private let coverAngle: Double = 11.25                 // web PI/16
    private let gap: CGFloat = 10
    private let perRow = 4                                 // web ~4 across (max-width 300)

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

    private func pair(_ battle: BattleView, index: Int) -> some View {
        let covered = battle.defense != nil
        // The attacked card tilts only once the cover has LANDED — i.e. the cover's
        // flight has cleared the in-flight (`hidden`) set — NOT the instant the model
        // says covered. So while the cover is still flying in, the attacked card
        // stays upright, then both lay across together (web behavior).
        let coverLanded = battle.defense.map { !hidden.contains($0.identity) } ?? false
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
                .rotationEffect(.degrees(coverLanded ? -coverAngle : 0), anchor: .bottom)
                .animation(.easeOut(duration: 0.22), value: coverLanded)
                .zIndex(covered ? 1 : 2)
                .modifier(FlightID(id: battle.attack.identity, namespace: namespace))

            if let defense = battle.defense {
                FCard(card: defense,
                      trump: trumpSuit != nil && defense.suit == trumpSuit,
                      size: cardSize)
                    .opacity(hidden.contains(defense.identity) ? 0 : 1)
                    .rotationEffect(.degrees(coverAngle), anchor: .bottom)   // laid across (§5.4)
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

    private func a11y(_ b: BattleView) -> String {
        let atk = name(b.attack)
        if let d = b.defense { return "\(atk), covered by \(name(d))" }
        return "\(atk), uncovered"
    }
    private func name(_ c: Card) -> String {
        guard let suit = c.suit else { return "hidden card" }
        return "\(CardRank.spoken(c.v)) of \(["spades","hearts","clubs","diamonds"][suit.rawValue])"
    }
}
