// AXTree.swift - the accessibility tree of a hosted SwiftUI view, as an
// assistive client sees it.
//
// SwiftUI builds its accessibility elements lazily, only once something asks
// for them: VoiceOver, Voice Control, or an XCUITest runner, all of which turn
// on the system's accessibility automation first. A plain unit-test process has
// none of those, so walking a hosting view without it finds an EMPTY tree and a
// test that reads labels off it passes or fails on nothing (the first run of
// ActionCardCornerTests and NoCountLeakTests, 2026-09-27: both found no
// element at all). `AXTree.enable()` flips the same switch an automation client
// does (libAccessibility's _AXSSetAutomationEnabled), once per process.

import Darwin
import SwiftUI
import UIKit

@MainActor
enum AXTree {
    private static var enabled = false

    /// Turn on accessibility automation for this process. Returns false when
    /// the switch cannot be found, so a test can say why its tree is empty.
    @discardableResult
    static func enable() -> Bool {
        if enabled { return true }
        guard let lib = dlopen("/usr/lib/libAccessibility.dylib", RTLD_NOW),
              let sym = dlsym(lib, "_AXSSetAutomationEnabled") else { return false }
        typealias SetEnabled = @convention(c) (Int32) -> Void
        unsafeBitCast(sym, to: SetEnabled.self)(1)
        enabled = true
        return true
    }

    /// Host `view` in a key window of `size`, let SwiftUI settle, and hand the
    /// hosting view to `read` while the window is still on screen.
    static func hosted<V: View, R>(_ view: V, size: CGSize, _ read: (UIView) -> R) -> R {
        enable()
        let host = UIHostingController(rootView: view)
        let window = UIWindow(frame: CGRect(origin: .zero, size: size))
        window.rootViewController = host
        window.makeKeyAndVisible()
        host.view.frame = window.bounds
        host.view.layoutIfNeeded()
        RunLoop.current.run(until: Date().addingTimeInterval(0.2))
        defer { window.isHidden = true }
        return read(host.view)
    }

    /// Every object under `root`, breadth first: accessibility elements
    /// (either protocol) and subviews, capped so a cycle cannot hang a test.
    static func walk(_ root: NSObject, limit: Int = 2000) -> [NSObject] {
        var out: [NSObject] = []
        var queue: [NSObject] = [root]
        while !queue.isEmpty, out.count < limit {
            let o = queue.removeFirst()
            out.append(o)
            if let els = o.accessibilityElements as? [NSObject] { queue += els }
            let n = o.accessibilityElementCount()
            if n != NSNotFound, n > 0 {
                for i in 0..<n { if let e = o.accessibilityElement(at: i) as? NSObject { queue.append(e) } }
            }
            if let v = o as? UIView { queue += v.subviews }
        }
        return out
    }

    /// The elements an assistive client would focus: accessibility elements
    /// other than the root that carry a label.
    static func elements(_ root: NSObject) -> [NSObject] {
        walk(root).filter { $0 !== root && $0.isAccessibilityElement && !($0.accessibilityLabel ?? "").isEmpty }
    }
}
