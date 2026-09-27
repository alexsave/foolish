// DiceTray.swift - the five dice in the middle of the felt (T10).
//
// A tap on a die toggles its keep mark, and only when the kernel says the
// tray may be touched (TrayModel.canKeep); a refused tap is a reject haptic
// and nothing else. Every frame the tray asks its DiceMotion what each die
// looks like (TumblePlayer today, the kernel's BeatPlayer later) and draws
// that; with nothing playing, the dice are at rest.
//
// T12: 52pt dice, 10pt apart, so the row is 300pt and fits the 304pt board
// of a 320pt phone (pickemup's 8pt side insets).

import QuartzCore
import SwiftUI

public struct DiceTray: View {
    public let tray: TrayModel
    public let motion: DiceMotion
    public let onToggle: (Int) -> Void
    /// Whether the tray's timeline runs; the caller observes the motion.
    public let animating: Bool

    public static let gap: CGFloat = 10
    public static var width: CGFloat {
        DiceFace.side * CGFloat(TrayModel.diceCount) + gap * CGFloat(TrayModel.diceCount - 1)
    }

    public init(tray: TrayModel, motion: DiceMotion, animating: Bool, onToggle: @escaping (Int) -> Void) {
        self.tray = tray
        self.motion = motion
        self.animating = animating
        self.onToggle = onToggle
    }

    public var body: some View {
        TimelineView(.animation(paused: !animating)) { context in
            let t = context.date.timeIntervalSinceReferenceDate
            let clock = CACurrentMediaTimeOffset.now(t)
            HStack(spacing: Self.gap) {
                ForEach(0..<TrayModel.diceCount, id: \.self) { i in
                    let f = motion.frame(die: i, at: clock) ?? .rest
                    let value = i < tray.dice.count ? tray.dice[i] : 0
                    DiceFace(value: f.face ?? value, kept: i < tray.kept.count && tray.kept[i] && f.face == nil)
                        .scaleEffect(f.scale)
                        .rotationEffect(.degrees(f.rotation))
                        .contentShape(Rectangle())
                        .onTapGesture {
                            if tray.canKeep {
                                Haptics.fire(.pickUp)
                                onToggle(i)
                            } else {
                                Haptics.fire(.reject)
                            }
                        }
                        .accessibilityAddTraits(tray.canKeep ? .isButton : [])
                        .accessibilityLabel(tray.kept[safe: i] == true ? "kept" : "die")
                        .tbAnchor("die.\(i)")
                }
            }
            .frame(width: Self.width, height: DiceFace.side)
        }
    }
}

/// TimelineView hands a wall-clock Date; the motion's clock is
/// CACurrentMediaTime. This maps one to the other for the frame being drawn.
enum CACurrentMediaTimeOffset {
    static func now(_ reference: TimeInterval) -> CFTimeInterval {
        CACurrentMediaTime() + (reference - Date().timeIntervalSinceReferenceDate)
    }
}

extension Array {
    subscript(safe i: Int) -> Element? { i >= 0 && i < count ? self[i] : nil }
}
