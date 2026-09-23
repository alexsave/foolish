import CUttt
import QuartzCore
import SwiftUI

/// THE HEIGHT THE SHEET IS LAID OUT AT, on a display link.
///
/// Messages hands the extension its height in steps: one height about 20ms
/// before an auto-collapse slides, and a new one only every ~200ms while a
/// released drag settles. Laid out at those, the board shrank in one frame
/// and then in 50-90 point steps beside a drawer that moved smoothly. The
/// kernel decides the height to lay out at instead (`uti_drawer_*`, a spring
/// on the host's own response toward the last height handed, a finger's small
/// steps followed at once); this file owns a clock and nothing else.
@MainActor
public final class UtttDrawerClock: ObservableObject {
    /// Bumped once a display frame while the spring runs, so the sheet asks
    /// again. The height itself is `layout(for:)`, never stored here.
    @Published public private(set) var frame = 0
    private var d = UtiDrawer()
    private var link: CADisplayLink?
    private let origin = CACurrentMediaTime()

    public init() {}

    private func ms(_ t: CFTimeInterval) -> Int32 {
        Int32(((t - origin) * 1000).rounded())
    }

    /// The height to lay out at now, `handed` being the one Messages gives
    /// this layout pass. Pure: a pass sees a new height before `report` does,
    /// and must not draw that frame at the raw height.
    public func layout(for handed: CGFloat) -> CGFloat {
        var moving: Int32 = 0
        return CGFloat(uti_drawer_peek(&d, Float(handed), ms(CACurrentMediaTime()), &moving))
    }

    /// The last height Messages handed, so a drop can be judged against it.
    public private(set) var handed: CGFloat = 0

    /// The auto-collapse flipped: lay out at `h` from this frame, no spring -
    /// the slide's push does the moving (UtttDrawerSheet, CollapseSlide).
    public func rest(_ h: CGFloat) {
        handed = h
        uti_drawer_rest(&d, Float(h))
        link?.invalidate()
        link = nil
        frame &+= 1
    }

    /// Every height Messages hands the sheet.
    public func report(_ h: CGFloat) {
        handed = h
        uti_drawer_report(&d, Float(h), ms(CACurrentMediaTime()))
        var moving: Int32 = 0
        _ = uti_drawer_at(&d, ms(CACurrentMediaTime()), &moving)
        if moving != 0 { run() }
    }

    private func run() {
        guard link == nil else { return }
        let l = CADisplayLink(target: Tick(self), selector: #selector(Tick.fire(_:)))
        l.preferredFrameRateRange = CAFrameRateRange(minimum: 60, maximum: 120, preferred: 120)
        l.add(to: .main, forMode: .common)
        link = l
    }

    fileprivate func tick(_ l: CADisplayLink) {
        var moving: Int32 = 0
        _ = uti_drawer_at(&d, ms(l.targetTimestamp), &moving)
        frame &+= 1
        if moving == 0 {
            link?.invalidate()
            link = nil
        }
    }

    /// CADisplayLink retains its target; this breaks the cycle.
    private final class Tick: NSObject {
        weak var clock: UtttDrawerClock?
        init(_ c: UtttDrawerClock) { clock = c }
        @objc func fire(_ l: CADisplayLink) {
            guard let clock else { l.invalidate(); return }
            MainActor.assumeIsolated { clock.tick(l) }
        }
    }
}

public extension CollapseSlide {
    /// The auto-collapse's slide, on the kernel's curve and numbers
    /// (uttt_anim.h UTTT_COLLAPSE_*): the host's spring, pushed from the
    /// whole travel to nothing.
    static func uttt() -> CollapseSlide {
        CollapseSlide(duration: Double(uti_collapse_ms()) / 1000,
                      steps: Int(uti_collapse_steps()),
                      flip: CGFloat(uti_collapse_flip())) { travel, t in
            CGFloat(uti_collapse_push(Float(travel), Int32((t * 1000).rounded())))
        }
    }
}
