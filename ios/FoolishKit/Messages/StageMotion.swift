// StageMotion — the measured, per-frame correction that keeps the board's
// bottom-anchored content pinned to the drawer's VISIBLE bottom edge through a
// host-animated style transition (round-10b, "the self cards go a bit under
// the screen briefly during the collapse").
//
// Why it exists: on an animated style change Messages snaps the extension
// view's MODEL frame to the target in one step and animates the PRESENTATION
// in our process (measured with a CADisplayLink probe: the presented height
// runs 762 -> 369 over ~0.5s on a fast spring while the model is already 369).
// Mid-flight the presented frame's bottom edge SINKS below the screen by
// ~100pt+ before converging - and everything bottom-anchored in the model
// (the hand, the action bar, the settings squares) rides that sink under the
// screen edge. Nothing knowable in advance (the sink is the host's own
// origin/size lead-lag), but everything SAMPLABLE: the host that owns the
// MSMessagesAppViewController runs a display link during the transition,
// measures each frame where the model's bottom anchor actually RENDERS, and
// publishes the correction here. MessagesRootView lifts its content by
// exactly that much, so the hand stays glued to the visible bottom edge the
// whole way - the same guarantee a manual grabber drag gives for free.
//
// The default instance never publishes (lift stays 0), so the harness and any
// host that does not run the sampler get today's behavior unchanged.
import SwiftUI

public final class StageMotionTracker: ObservableObject {
    /// How many points the surface must be lifted above its MODEL bottom
    /// anchor right now so it renders at the drawer's visible bottom edge.
    /// 0 at rest in both styles; briefly positive while the host's transition
    /// animation carries the model anchor below the screen.
    @Published public var lift: CGFloat = 0

    public init() {}
}
