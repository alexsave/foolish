// FSeatBadge.swift — an opponent seat (§5.4): avatar-less by design — nickname,
// card count (condensed numeral), a thinking indicator, and role marks
// (attacker/defender/"good"). No photos, no color-only state.

import SwiftUI

public struct FSeatBadge: View {
    public let name: String
    public let handCount: Int
    public let isDefender: Bool
    public let isAttacker: Bool
    public let saidGood: Bool
    public let thinking: Bool
    public let isOut: Bool

    public init(name: String, handCount: Int, isDefender: Bool = false,
                isAttacker: Bool = false, saidGood: Bool = false,
                thinking: Bool = false, isOut: Bool = false) {
        self.name = name
        self.handCount = handCount
        self.isDefender = isDefender
        self.isAttacker = isAttacker
        self.saidGood = saidGood
        self.thinking = thinking
        self.isOut = isOut
    }

    public var body: some View {
        VStack(spacing: FSpace.xs) {
            ZStack {
                // A fanned stack silhouette + the count numeral.
                RoundedRectangle(cornerRadius: FRadius.card)
                    .fill(FColor.surface)
                    .frame(width: 44, height: 30)
                    .overlay(RoundedRectangle(cornerRadius: FRadius.card)
                        .strokeBorder(role.borderColor, lineWidth: role == .none ? 0 : 1.5))
                Text("\(handCount)")
                    .font(FType.numeral(20))
                    .foregroundColor(FColor.textPrimary)
            }
            Text(name)
                .font(FType.body(13))
                .foregroundColor(isOut ? FColor.textDim : FColor.textPrimary)
                .lineLimit(1)
                .truncationMode(.tail)
                .frame(maxWidth: 84)
            roleRow
        }
        .opacity(isOut ? 0.45 : 1)
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(a11y)
    }

    private enum Role { case none, attacker, defender
        var borderColor: Color {
            switch self { case .none: return .clear; case .attacker: return FColor.accent; case .defender: return FColor.win }
        }
    }
    private var role: Role { isDefender ? .defender : (isAttacker ? .attacker : .none) }

    private var roleRow: some View {
        HStack(spacing: FSpace.xs) {
            if thinking { ThinkingDots() }
            if saidGood {
                Image(systemName: "checkmark.seal.fill")
                    .font(.system(size: 11)).foregroundColor(FColor.win)
            }
            if isDefender {
                Image(systemName: "shield.fill").font(.system(size: 11)).foregroundColor(FColor.win)
            } else if isAttacker {
                Image(systemName: "flame.fill").font(.system(size: 11)).foregroundColor(FColor.accent)
            }
        }
        .frame(height: 14)
    }

    private var a11y: String {
        var parts = ["\(name), \(handCount) cards"]
        if isDefender { parts.append("defending") }
        else if isAttacker { parts.append("attacking") }
        if saidGood { parts.append("said good") }
        if thinking { parts.append("thinking") }
        if isOut { parts.append("out") }
        return parts.joined(separator: ", ")
    }
}

/// A three-dot "thinking" pulse (Reduce-Motion falls back to a static dot row).
public struct ThinkingDots: View {
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @State private var phase = 0.0
    public init() {}
    public var body: some View {
        HStack(spacing: 2) {
            ForEach(0..<3, id: \.self) { i in
                Circle().fill(FColor.textDim)
                    .frame(width: 4, height: 4)
                    .opacity(reduceMotion ? 0.6 : 0.3 + 0.7 * abs(sin(phase + Double(i) * 0.6)))
            }
        }
        .onAppear {
            guard !reduceMotion else { return }
            withAnimation(.linear(duration: 0.9).repeatForever(autoreverses: false)) { phase = .pi * 2 }
        }
        .accessibilityHidden(true)
    }
}
