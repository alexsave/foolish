// Backend.swift - the one HTTP transport to c_base's binary surface, the loop
// that runs the kernel's client, and the Keychain item the session lives in.
//
// The base URL comes from Info.plist (BackendURL, set from Config/*.xcconfig);
// blank means an offline build, and every online affordance stays disabled.
// The app says what the user wants (an intent); the kernel's client
// (NetClient) decides the requests, their tokens, refreshes, retries, what an
// answer means and what changed. This file sends what it names with
// URLSession, hands every answer back, and settles the intent it finished.

import Foundation
import Security
import FoolishKit

@MainActor
public final class Backend {
    public static let shared = Backend()

    public let baseURL: URL?

    /// Told whenever the session the client holds changed (signed in, refreshed, ended).
    var onSession: (() -> Void)?

    private var waiting: [Int: CheckedContinuation<Answer, Never>] = [:]
    private var wake: Task<Void, Never>?

    private init() {
        let s = (Bundle.main.object(forInfoDictionaryKey: "BackendURL") as? String) ?? ""
        baseURL = s.isEmpty ? nil : URL(string: s)
    }

    /// True when online play is available. Home gates the PLAY button on it.
    public var isConfigured: Bool { baseURL != nil }

    /// A finished intent: the client's NcDone and the body its spans are into.
    struct Answer {
        let done: NcDoneSnap
        let body: Data
        /// The envelope (or replay moves) the answer carries, nil for none.
        var envelope: Data? { done.aLen > 0 ? body.subdata(in: body.startIndex + done.aOff ..< body.startIndex + done.aOff + done.aLen) : nil }
    }

    /// One intent (NC_DO_*), answered: its finished ticket, or a throw with the refusal.
    func ask(_ kind: Int, gameId: String = "", name: String = "", password: String = "", bytes: [UInt8] = [],
             flag: Int = 0, intent: Int64 = -1) async throws -> Answer {
        guard isConfigured else { throw NetError.notConfigured }
        let t = NetClient.do(kind, gameId: gameId, name: name, password: password, bytes: bytes, flag: flag, intent: intent)
        guard t != 0 else { throw NetError.refused(NetClient.error) }
        let a = await withCheckedContinuation { (c: CheckedContinuation<Answer, Never>) in
            waiting[t] = c
            pump()
        }
        let d = a.done
        guard d.why == 0 else { throw NetError.refused(NcErrorSnap(why: d.why, code: d.code, detail: d.detail, status: d.status)) }
        return a
    }

    /// Sends whatever the client has for the server, settles what it finished,
    /// and sleeps until the client says something is due.
    func pump() {
        while let r = NetClient.nextRequest() { Task { await send(r) } }
        while let d = NetClient.nextDone() { settle(Answer(done: d, body: Data())) }
        Stream.shared.send()
        wake?.cancel()
        let ms = NetClient.dueMs()
        guard ms >= 0 else { return }
        wake = Task { [weak self] in
            try? await Task.sleep(nanoseconds: UInt64(ms) * 1_000_000)
            if !Task.isCancelled { self?.pump() }
        }
    }

    /// What the last answer or frame changed, to whoever it is for.
    func changed() {
        for (ev, payload) in NetClient.events() {
            if ev.kind == NC_EV_SESSION {
                Session.save(NetClient.sessionBlob())
                onSession?()
            } else {
                Stream.shared.deliver(ev, payload)
            }
        }
    }

    private func send(_ r: NcRequestSnap) async {
        var status = 0, body = Data()   // status 0: it never arrived, and the client knows what that means
        if let base = baseURL, let url = URL(string: r.path, relativeTo: base) {
            var q = URLRequest(url: url)
            q.httpMethod = r.method == NC_GET ? "GET" : "POST"
            if !r.auth.isEmpty { q.setValue(r.auth, forHTTPHeaderField: "Authorization") }
            if r.method == NC_POST {
                q.setValue("application/octet-stream", forHTTPHeaderField: "Content-Type")
                q.httpBody = Data(r.body.map { UInt8(truncatingIfNeeded: $0) })
            }
            if let (b, response) = try? await URLSession.shared.data(for: q) {
                status = (response as? HTTPURLResponse)?.statusCode ?? 0
                body = b
            }
        }
        let d = NetClient.onResponse(r.id, status, body)
        changed()
        if let d { settle(Answer(done: d, body: body)) }
        pump()
    }

    private func settle(_ a: Answer) { waiting.removeValue(forKey: a.done.ticket)?.resume(returning: a) }

    /// The stream's URL: the base URL's host over ws or wss.
    var streamURL: URL? {
        guard let base = baseURL, let path = NetClient.streamPath,
              var c = URLComponents(url: URL(string: path, relativeTo: base) ?? base, resolvingAgainstBaseURL: true) else { return nil }
        c.scheme = c.scheme == "https" ? "wss" : "ws"
        return c.url
    }
}

/// A refusal, as the app shows it.
public enum NetError: Error, LocalizedError {
    case notConfigured, unreadable
    case refused(NcErrorSnap?)

    public var errorDescription: String? {
        switch self {
        case .notConfigured: return "Online play isn’t configured in this build."
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
