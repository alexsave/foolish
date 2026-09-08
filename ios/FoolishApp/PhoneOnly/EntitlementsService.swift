// Entitlements — the billing-ready seam (§10). v1 ships ONLY FreeEntitlements.
// The future paid Oracle drops in as `StoreKitEntitlements` implementing this
// same protocol (StoreKit 2 + App Store Server Notifications → entitlements
// table, Oracle doc §5); nothing here imports StoreKit and a CI lint forbids
// `import StoreKit` anywhere outside this module — including this file (§16.E1).
//
// This is its own module boundary on purpose: adding the Oracle later must
// touch only (a) a new implementer of this protocol and (b) flipped Flags.

// MOVED OUT OF FoolishKit (bundle size). FoolishKit is a DYNAMIC framework that
// ships inside FoolishMessagesApp, and every `public` symbol in a dylib is an
// export - which makes it a dead-strip ROOT the linker may never remove. So any
// public API in FoolishKit that only the PHONE app calls is bytes on every
// iMessage install for code that install can never run. This file had no caller
// outside ios/FoolishApp, so it lives with its caller now.

import Foundation
import Combine
import FoolishKit

/// What the user is entitled to. v1 is always all-false.
public struct EntitlementSet: Equatable, Sendable {
    public let oraclePremium: Bool
    /// Where the entitlement came from (e.g. "storekit", "promo") — nil when free.
    public let source: String?

    public static let free = EntitlementSet(oraclePremium: false, source: nil)

    public init(oraclePremium: Bool, source: String?) {
        self.oraclePremium = oraclePremium
        self.source = source
    }
}

/// Injected at the app root (SwiftUI Environment). Observable so views react
/// when the future StoreKit implementation refreshes entitlements.
public protocol EntitlementsService: ObservableObject {
    var current: EntitlementSet { get }
    func refresh() async
}

/// The only v1 implementation: everyone is on the free tier, forever.
public final class FreeEntitlements: EntitlementsService {
    @Published public private(set) var current: EntitlementSet = .free
    public init() {}
    public func refresh() async { /* free tier never changes */ }
}
