// FoolishApp.swift — @main entry, dependency injection root, routing.
// Dark-first (§5.1): the whole app runs in the Gosizdat dark palette; the
// Info.plist forces UIUserInterfaceStyle=Dark so system chrome matches.

import SwiftUI
import FoolishKit

@main
struct FoolishApp: App {
    // The billing-ready seam (§10). v1 injects only FreeEntitlements; the future
    // Oracle swaps in StoreKitEntitlements here with no other change.
    @StateObject private var entitlements = FreeEntitlements()
    // Online auth (§9). Nil-backed when the build has no Supabase config; the
    // app stays fully usable offline (guest-first, §2).
    @StateObject private var auth = AuthService()

    var body: some Scene {
        WindowGroup {
            RootView()
                .environmentObject(entitlements)
                .environmentObject(auth)
                .tint(FColor.accent)
                .preferredColorScheme(.dark)
                .task { await auth.restore() }
        }
    }
}
