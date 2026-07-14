// FBattleGrid.swift — the center of the table (§5.4): attack/cover pairs. Each
// cover lands rotated 12° over its attack (the physical "laid across" look).
// Uncovered attacks are tappable drop targets when the local player is defending
// (the tap handler is owned by TableView; this view just reports the battle).

import SwiftUI

/// Reports each battle's frame (in a named coordinate space) so a drag can
/// hit-test which battle is under the finger.
public struct BattleFramesKey: PreferenceKey {
    public static var defaultValue: [Int: CGRect] = [:]
    public static func reduce(value: inout [Int: CGRect], nextValue: () -> [Int: CGRect]) {
        value.merge(nextValue()) { _, new in new }
    }
}

public struct FBattleGrid: View {
    public let battles: [BattleView]
    public let trumpSuit: Suit?
    /// Battle indices the local defender may currently cover (highlight them).
    public let coverable: Set<Int>
    /// The battle currently under a drag (strong gold highlight), if any.
    public let activeTarget: Int?
    /// Named coordinate space to report battle frames in (for drag hit-testing).
    public let coordinateSpace: String?
    public let onTapBattle: (Int) -> Void

    public init(battles: [BattleView], trumpSuit: Suit?, coverable: Set<Int> = [],
                activeTarget: Int? = nil, coordinateSpace: String? = nil,
                onTapBattle: @escaping (Int) -> Void = { _ in }) {
        self.battles = battles
        self.trumpSuit = trumpSuit
        self.coverable = coverable
        self.activeTarget = activeTarget
        self.coordinateSpace = coordinateSpace
        self.onTapBattle = onTapBattle
    }

    private let cardSize = CGSize(width: 54, height: 76)
    private let columns = [GridItem(.adaptive(minimum: 62), spacing: FSpace.m)]

    public var body: some View {
        LazyVGrid(columns: columns, spacing: FSpace.m) {
            ForEach(Array(battles.enumerated()), id: \.offset) { idx, battle in
                pair(battle, index: idx)
                    .onTapGesture { onTapBattle(idx) }
            }
        }
        .padding(.horizontal, FSpace.m)
    }

    private func pair(_ battle: BattleView, index: Int) -> some View {
        ZStack {
            FCard(card: battle.attack,
                  trump: trumpSuit != nil && battle.attack.suit == trumpSuit,
                  size: cardSize)
                .matchedGeometryEffect(id: battle.attack.identity, in: ns, isSource: true)

            if let defense = battle.defense {
                FCard(card: defense,
                      trump: trumpSuit != nil && defense.suit == trumpSuit,
                      size: cardSize)
                    .rotationEffect(.degrees(12))       // cover laid across (§5.4)
                    .offset(x: 10, y: 8)
                    .matchedGeometryEffect(id: defense.identity, in: ns, isSource: true)
            }
        }
        .frame(width: 74, height: 92)
        .background(
            RoundedRectangle(cornerRadius: FRadius.card)
                .strokeBorder(highlightColor(index), lineWidth: activeTarget == index ? 3 : 2)
                .padding(-3)
        )
        .background(
            GeometryReader { g in
                if let space = coordinateSpace {
                    Color.clear.preference(key: BattleFramesKey.self,
                                           value: [index: g.frame(in: .named(space))])
                } else { Color.clear }
            }
        )
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(a11y(battle))
    }

    private func highlightColor(_ index: Int) -> Color {
        if activeTarget == index { return FColor.win }
        return coverable.contains(index) ? FColor.win.opacity(0.7) : .clear
    }

    private func a11y(_ b: BattleView) -> String {
        let atk = name(b.attack)
        if let d = b.defense { return "\(atk), covered by \(name(d))" }
        return "\(atk), uncovered"
    }
    private func name(_ c: Card) -> String {
        guard let suit = c.suit else { return "hidden card" }
        let rank: String
        switch c.v { case 13: rank = "ace"; case 12: rank = "king"; case 11: rank = "queen"; case 10: rank = "ten"; default: rank = "\(c.v)" }
        return "\(rank) of \(["spades","hearts","clubs","diamonds"][suit.rawValue])"
    }

    @Namespace private var ns
}
