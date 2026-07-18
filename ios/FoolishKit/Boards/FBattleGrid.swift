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

    public init(battles: [BattleView], trumpSuit: Suit?, coverable: Set<Int> = [],
                onTapBattle: @escaping (Int) -> Void = { _ in }) {
        self.battles = battles
        self.trumpSuit = trumpSuit
        self.coverable = coverable
        self.onTapBattle = onTapBattle
    }

    private let cardSize = CGSize(width: 50, height: 70)   // web card 50x70
    private let slot = CGSize(width: 62, height: 84)       // web 60x80 (+room to rotate)
    private let coverAngle: Double = 11.25                 // web PI/16
    // Adaptive columns wrap the pairs into as many per row as fit — the web's
    // flex-wrap, capped so a wide screen keeps them grouped in the middle.
    private var columns: [GridItem] { [GridItem(.adaptive(minimum: 66), spacing: 10)] }

    public var body: some View {
        LazyVGrid(columns: columns, spacing: 12) {
            ForEach(Array(battles.enumerated()), id: \.offset) { idx, battle in
                pair(battle, index: idx)
                    .contentShape(Rectangle())
                    .onTapGesture { onTapBattle(idx) }
            }
        }
        .frame(maxWidth: 320)   // web max-width 300px — keep the cluster centred
    }

    private func pair(_ battle: BattleView, index: Int) -> some View {
        let covered = battle.defense != nil
        return ZStack(alignment: .bottom) {
            FCard(card: battle.attack,
                  trump: trumpSuit != nil && battle.attack.suit == trumpSuit,
                  size: cardSize)
                .rotationEffect(.degrees(covered ? -coverAngle : 0), anchor: .bottom)
                .zIndex(covered ? 1 : 2)
                .matchedGeometryEffect(id: battle.attack.identity, in: ns, isSource: true)

            if let defense = battle.defense {
                FCard(card: defense,
                      trump: trumpSuit != nil && defense.suit == trumpSuit,
                      size: cardSize)
                    .rotationEffect(.degrees(coverAngle), anchor: .bottom)   // laid across (§5.4)
                    .zIndex(2)
                    .matchedGeometryEffect(id: defense.identity, in: ns, isSource: true)
            }
        }
        .frame(width: slot.width, height: slot.height, alignment: .bottom)
        .background(
            RoundedRectangle(cornerRadius: 8)
                .strokeBorder(coverable.contains(index) ? FColor.win : .clear, lineWidth: 2.5)
                .padding(-2)
        )
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

    @Namespace private var ns
}
