// Backend.swift - the one HTTP transport to c_base's binary surface, and the
// Keychain item the session lives in.
//
// The base URL comes from Info.plist (BackendURL, set from Config/*.xcconfig);
// blank means an offline build, and every online affordance stays disabled.
// What each request says is the kernel's (NetClient); this file sends it with
// URLSession and hands the answer back. When the client asks for the token's
// refresh first, it is sent, kept, and the request is built again.

import Foundation
import Security
import FoolishKit

@MainActor
public final class Backend {
    public static let shared = Backend()

    public let baseURL: URL?

    private init() {
        let s = (Bundle.main.object(forInfoDictionaryKey: "BackendURL") as? String) ?? ""
        baseURL = s.isEmpty ? nil : URL(string: s)
    }

    /// True when online play is available. Home gates the PLAY button on it.
    public var isConfigured: Bool { baseURL != nil }

    /// One request, built by `build` as many times as it takes: a refresh the
    /// client asks for first is sent and kept, then the request is built again.
    func call(_ build: () -> NcRequestSnap?) async throws -> (status: Int, body: Data) {
        var refreshes = 0, busy = 0
        while refreshes < 3 {
            guard let base = baseURL else { throw NetError.notConfigured }
            guard let r = build() else { throw NetError.notSignedIn }
            let answer = try await send(r, base)
            let wait = NetClient.retryMs(Int(r.method), busy, answer.status, answer.body)
            if wait > 0 { busy += 1; try await Task.sleep(nanoseconds: UInt64(wait) * 1_000_000); continue }
            guard r.isRefresh != 0 else { return answer }
            refreshes += 1
            try keep(NetClient.readSession(answer.status, answer.body))
        }
        throw NetError.notSignedIn
    }

    /// Throws the refusal, or keeps the session the client now holds (and tells
    /// the stream, which sends a fresh token with its next frame).
    func keep(_ why: Int) throws {
        Session.save(NetClient.sessionBlob())   // nil when the refusal ended the session
        guard why == 0 else { throw NetError.refused(NetClient.error) }
        Stream.shared.flush()
    }

    private func send(_ r: NcRequestSnap, _ base: URL) async throws -> (status: Int, body: Data) {
        guard let url = URL(string: r.path, relativeTo: base) else { throw NetError.notConfigured }
        var q = URLRequest(url: url)
        q.httpMethod = r.method == NC_GET ? "GET" : "POST"
        if !r.auth.isEmpty { q.setValue(r.auth, forHTTPHeaderField: "Authorization") }
        if r.method == NC_POST {
            q.setValue("application/octet-stream", forHTTPHeaderField: "Content-Type")
            q.httpBody = Data(r.body.map { UInt8(truncatingIfNeeded: $0) })
        }
        let (body, response) = try await URLSession.shared.data(for: q)
        return ((response as? HTTPURLResponse)?.statusCode ?? 0, body)
    }

    /// The stream's URL: the base URL's host over ws or wss.
    var streamURL: URL? {
        guard let base = baseURL, let path = NetClient.stream()?.path,
              var c = URLComponents(url: URL(string: path, relativeTo: base) ?? base, resolvingAgainstBaseURL: true) else { return nil }
        c.scheme = c.scheme == "https" ? "wss" : "ws"
        return c.url
    }
}

/// A refusal, as the app shows it.
public enum NetError: Error, LocalizedError {
    case notConfigured, notSignedIn, unreadable
    case refused(NcErrorSnap?)

    public var errorDescription: String? {
        switch self {
        case .notConfigured: return "Online play isn’t configured in this build."
        case .notSignedIn: return "You’re not signed in."
        case .unreadable: return "The server sent an unreadable response."
        case .refused(let e):
            guard let e, e.why == ASK_WHY_ACCOUNT else { return "The server refused that." }
            switch e.code {
            case NW_ACCOUNT_E_BAD_LOGIN: return "Invalid login credentials"
            case NW_ACCOUNT_E_EXISTS: return "User already registered"
            case NW_ACCOUNT_E_WEAK_PASSWORD: return "Password should be at least 6 characters."
            case NW_ACCOUNT_E_LONG_PASSWORD: return "Password cannot be longer than 72 characters"
            case NW_ACCOUNT_E_BAD_NAME: return "That username isn’t allowed."
            default: return "Please sign in again."
            }
        }
    }
}

/// The session blob in the Keychain, so a signed-in player stays signed in.
enum Session {
    private static let query: [String: Any] = [kSecClass as String: kSecClassGenericPassword,
                                               kSecAttrService as String: "cards.foolish.session"]

    static func load() -> Data? {
        var q = query
        q[kSecReturnData as String] = true
        var out: AnyObject?
        return SecItemCopyMatching(q as CFDictionary, &out) == errSecSuccess ? out as? Data : nil
    }

    /// Keeps `blob`, or forgets the session when nil.
    static func save(_ blob: Data?) {
        SecItemDelete(query as CFDictionary)
        guard let blob else { return }
        var q = query
        q[kSecValueData as String] = blob
        q[kSecAttrAccessible as String] = kSecAttrAccessibleAfterFirstUnlock
        SecItemAdd(q as CFDictionary, nil)
    }
}
