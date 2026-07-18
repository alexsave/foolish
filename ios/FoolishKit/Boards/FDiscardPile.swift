// FDiscardPile.swift — the beaten pile, matching the WEB DiscardPile: a small
// messy stack of red card backs with the count centred on it (±20° per-layer
// tilt, deterministic). Beaten cards are out of play, so only the count matters.
// Pinned to the table's top-right corner by the board.

import SwiftUI

public struct FDiscardPile: View {
    public let count: Int
    public init(count: Int) { self.count = count }

    public var body: some View {
        let layers = min(max(count, 1), 5)
        ZStack {
            ForEach(0..<layers, id: \.self) { i in
                FCard(card: nil, backSeed: UInt64(3 + i), size: CGSize(width: 44, height: 62))
                    .rotationEffect(.degrees(rotation(i)))
            }
            Text("\(count)")
                .font(.system(size: 15, weight: .bold))
                .foregroundColor(.white)
                .shadow(color: .black.opacity(0.8), radius: 1, x: 1, y: 1)
        }
        .frame(width: 68, height: 78)
        .accessibilityElement(children: .ignore)
        .accessibilityLabel("\(count) cards discarded")
        // Publish the pile's rect so a bout-end discard flight has a target — even
        // when the pile is empty (count 0), so the FIRST discard can fly to it.
        .background(GeometryReader { g in
            Color.clear.preference(key: DiscardFrameKey.self, value: g.frame(in: .named(boardSpace)))
        })
        .opacity(count > 0 ? 1 : 0.001)
    }

    // Deterministic per-layer tilt (mirrors the web enableRandomRotation).
    private func rotation(_ i: Int) -> Double {
        let s = sin(Double(42 + i * 1000)) * 10000
        return (s - floor(s)) * 40 - 20
    }
}
