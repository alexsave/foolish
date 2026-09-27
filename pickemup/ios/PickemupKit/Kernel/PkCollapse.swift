// PkCollapse.swift - the auto-collapse's slide on the kernel's numbers
// (pickemup/c/ios/include/pk_api.h, PK_LAY_COLLAPSE_*, pk_lay_collapse_push).
//
// The machinery is the shared CollapseSlide (shared/swift/MessagesKit), which
// has no numbers of its own; when and how the sheet moves is a decision, so
// the curve, its length, its keyframes and the flip are C's, as uttt's are
// its kernel's. A Debug build switches it on with the rig's `dev.slide` file
// (PickemupDev.slide) until Messages has judged it (ANIMATION_DECISIONS A14).

import CoreGraphics
import CPickemup

#if canImport(UIKit)
public extension CollapseSlide {
    /// The auto-collapse, pushed from the whole travel to nothing on the
    /// host's own drawer spring.
    static func pickemup() -> CollapseSlide {
        CollapseSlide(duration: Double(PK_LAY_COLLAPSE_MS) / 1000,
                      steps: Int(PK_LAY_COLLAPSE_STEPS),
                      flip: CGFloat(PK_LAY_COLLAPSE_FLIP)) { travel, t in
            CGFloat(pk_lay_collapse_push(Float(travel), Int32((t * 1000).rounded())))
        }
    }
}
#endif
