// DiceRoll.swift - one seat's roll: the cup shakes, lifts, and the dice
// tumble out of it and settle into a row showing the given values.
//
// PRODUCT-NEUTRAL (a later lift into shared/swift is possible). The values
// come from outside; nothing here picks a face. The spin each die tumbles
// through is decoration keyed to its position in the row, never a game
// number.
//
// EVERY DURATION AND CURVE IS IN `RollBeats`, so the kernel's beats can
// drive them later by replacing that one enum (DECISIONS I5).

import SwiftUI

/// The roll's timing: the ONE place its numbers live.
public enum RollBeats {
    /// One swing of the cup's shake, and how many swings.
    public static let shakeStep: Double = 0.09
    public static let shakes = 5
    /// How far the cup tips each way, in degrees.
    public static let shakeAngle: Double = 9
    /// The cup rising off the dice and fading.
    public static let lift: Animation = .easeOut(duration: 0.28)
    public static let liftSeconds: Double = 0.28
    /// The dice tumbling out and settling.
    public static let tumble: Animation = .spring(response: 0.42, dampingFraction: 0.72)
    /// The gap between one die leaving the cup and the next.
    public static let stagger: Double = 0.05
    /// The whole roll, start to the last die resting.
    public static var total: Double {
        shakeStep * Double(shakes + 1) + liftSeconds + 0.42 + stagger * 4
    }
}

public struct DiceRoll: View {
    public let values: [Int]
    /// Which roll this is (the model's rollID). A new one plays the roll.
    public let rollID: Int
    /// The newest roll already played on this phone; equal to `rollID`
    /// draws the dice at rest with no motion.
    public let played: Int
    public var dieSize: CGFloat
    public var spacing: CGFloat
    public var onPlayed: (Int) -> Void

    public init(values: [Int], rollID: Int, played: Int, dieSize: CGFloat = 34, spacing: CGFloat = 8,
                onPlayed: @escaping (Int) -> Void = { _ in }) {
        self.values = values
        self.rollID = rollID
        self.played = played
        self.dieSize = dieSize
        self.spacing = spacing
        self.onPlayed = onPlayed
        // a roll not yet played opens under the cup, so its first frame is
        // never the dice at rest
        _stage = State(initialValue: rollID == played ? .settled : .covered)
    }

    enum Stage: Int, Comparable {
        case covered, lifted, settled
        static func < (a: Stage, b: Stage) -> Bool { a.rawValue < b.rawValue }
    }

    @State private var stage: Stage = .settled
    @State private var tilt: Double = 0

    private var rowWidth: CGFloat {
        CGFloat(values.count) * dieSize + CGFloat(max(values.count - 1, 0)) * spacing
    }
    private var cupWidth: CGFloat { max(dieSize * 1.6, min(rowWidth * 0.4, dieSize * 2.0)) }

    public var body: some View {
        ZStack(alignment: .bottom) {
            HStack(spacing: spacing) {
                ForEach(Array(values.enumerated()), id: \.offset) { i, v in
                    let out = stage == .settled
                    Die(face: v, size: dieSize)
                        .rotationEffect(.degrees(out ? 0 : spin(i)))
                        .scaleEffect(out ? 1 : 0.4)
                        .offset(x: out ? 0 : toCentre(i), y: out ? 0 : -dieSize * 0.3)
                        .opacity(out ? 1 : 0)
                        .animation(RollBeats.tumble.delay(RollBeats.stagger * Double(i)), value: stage)
                }
            }
            Cup(width: cupWidth)
                .rotationEffect(.degrees(tilt), anchor: .bottom)
                .offset(y: stage == .covered ? 0 : -cupWidth * 0.7)
                .opacity(stage == .covered ? 1 : 0)
                .animation(RollBeats.lift, value: stage)
                .allowsHitTesting(false)
        }
        .frame(width: max(rowWidth, cupWidth), height: max(dieSize, cupWidth * Cup.aspect), alignment: .bottom)
        .task(id: rollID) { await play() }
    }

    /// Where die `i` starts: under the cup, at the row's centre.
    private func toCentre(_ i: Int) -> CGFloat {
        let mid = CGFloat(values.count - 1) / 2
        return (mid - CGFloat(i)) * (dieSize + spacing)
    }

    /// The decorative turn a die tumbles through, alternating direction.
    private func spin(_ i: Int) -> Double {
        (i % 2 == 0 ? 1 : -1) * (180 + 45 * Double(i))
    }

    @MainActor
    private func play() async {
        guard rollID != played, !values.isEmpty else { stage = .settled; return }
        var t = Transaction()
        t.disablesAnimations = true
        withTransaction(t) { stage = .covered; tilt = 0 }
        let step = UInt64(RollBeats.shakeStep * 1_000_000_000)
        for k in 0..<RollBeats.shakes {
            withAnimation(.easeInOut(duration: RollBeats.shakeStep)) {
                tilt = k % 2 == 0 ? RollBeats.shakeAngle : -RollBeats.shakeAngle
            }
            try? await Task.sleep(nanoseconds: step)
            if Task.isCancelled { return }
        }
        withAnimation(.easeInOut(duration: RollBeats.shakeStep)) { tilt = 0 }
        try? await Task.sleep(nanoseconds: step)
        stage = .lifted
        try? await Task.sleep(nanoseconds: UInt64(RollBeats.liftSeconds * 0.5 * 1_000_000_000))
        if Task.isCancelled { return }
        stage = .settled
        onPlayed(rollID)
    }
}
