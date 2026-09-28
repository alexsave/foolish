// Backend.swift — the Supabase client, configured from the app's Info.plist
// (SupabaseURL / SupabaseKey, substituted from Config/*.xcconfig at build,
// §16.D1). The anon key is public by design (README), but it lives in xcconfig,
// not source. When the two values are blank (v1 offline builds), `client` is nil
// and every online affordance stays disabled — the app is fully usable offline.
//
// NOTE (Mac compile pass): this file imports `Supabase` (supabase-swift). The
// SDK surface used here (SupabaseClient init, auth, functions, realtimeV2) is
// stable in 2.x; verify exact call shapes against the resolved package version
// on first `xcodebuild`.

import Foundation
import Supabase
import FoolishKit

public final class Backend {
    public static let shared = Backend()

    public let client: SupabaseClient?

    private init() {
        let urlStr = (Bundle.main.object(forInfoDictionaryKey: "SupabaseURL") as? String) ?? ""
        let key = (Bundle.main.object(forInfoDictionaryKey: "SupabaseKey") as? String) ?? ""
        if !urlStr.isEmpty, !key.isEmpty, let url = URL(string: urlStr) {
            client = SupabaseClient(supabaseURL: url, supabaseKey: key)
        } else {
            client = nil
        }
    }

    /// True when online play is available (config present). Home gates the PLAY
    /// button on this so an offline build never shows a dead online path.
    public var isConfigured: Bool { client != nil }
}
