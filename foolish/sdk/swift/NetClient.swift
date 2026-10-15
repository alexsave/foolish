// NetClient.swift - the backend's client (c/src/netclient.h), as the online app
// reaches it.
//
// Every decision is the kernel's: what a request says and which token it
// carries, when the token is refreshed, what an answer means, which pushes are
// stale, when a refused subscription is retried and when to ping. This file only
// moves bytes across (CFoolish fio_net_*) and reads the answers through the
// generated readers (sdk/swift/gen, readNcRequest and the rest). The sockets
// are FoolishNet's.
//
// It lives in the SDK, though only the online app calls it, for AskWire's reason:
// CFoolish is imported nowhere else (ios/scripts/lint_architecture.sh). The
// client is one set of statics in C, so every call is on the main actor.

import Foundation
import CFoolish

@MainActor
public enum NetClient {

    /// Epoch milliseconds, the clock every call that can act on time is given.
    public static var now: Int64 { Int64(Date().timeIntervalSince1970 * 1000) }

    // MARK: the session

    public static func reset() { fio_net_reset() }

    /// The session as a blob to keep (nil when signed out).
    public static func sessionBlob() -> Data? {
        var out = [UInt8](repeating: 0, count: 2400)
        let n = fio_net_session_save(&out, Int32(out.count))
        return n > 0 ? Data(out.prefix(Int(n))) : nil
    }

    /// Adopt a kept blob; false (and signed out) when it does not read.
    public static func load(_ blob: Data) -> Bool {
        bytes(blob) { fio_net_session_load($0, $1) } == 0
    }

    public static var session: NcSessionInfoSnap? { fio_net_session_info().flatMap { try? readNcSessionInfo($0) } }

    /// An answer to sign-up, sign-in or refresh: 0 and the session is kept, or the refusal's why.
    public static func readSession(_ status: Int, _ body: Data) -> Int {
        Int(bytes(body) { fio_net_session_read(Int32(status), $0, $1) })
    }

    /// Why the last answer read was refused.
    public static var error: NcErrorSnap? { fio_net_error().flatMap { try? readNcError($0) } }

    // MARK: requests

    public static func signUp(_ name: String, _ password: String) -> NcRequestSnap? {
        two(name, password) { fio_net_req_signup($0, $1, $2, $3) }
    }
    public static func signIn(_ name: String, _ password: String) -> NcRequestSnap? {
        two(name, password) { fio_net_req_signin($0, $1, $2, $3) }
    }
    public static func signOut() -> NcRequestSnap? { request(fio_net_req_signout(1, now)) }
    public static func deleteAccount() -> NcRequestSnap? { request(fio_net_req_delete_account(now)) }
    public static func create() -> NcRequestSnap? { request(fio_net_req_create(now)) }
    public static func meta(_ ask: Data) -> NcRequestSnap? {
        request(bytes(ask) { fio_net_req_meta($0, $1, now) })
    }
    public static func action(_ gameId: String, _ wire: [UInt8], intent: Int64) -> NcRequestSnap? {
        let g = chars(gameId)
        return request(g.withUnsafeBufferPointer { gp in
            wire.withUnsafeBufferPointer { wp in
                fio_net_req_action(gp.baseAddress, Int32(g.count - 1), wp.baseAddress, Int32(wire.count), intent, now)
            }
        })
    }
    public static func game(_ gameId: String) -> NcRequestSnap? {
        let g = chars(gameId)
        return request(g.withUnsafeBufferPointer { fio_net_req_game($0.baseAddress, Int32(g.count - 1), now) })
    }
    public static func stream() -> NcRequestSnap? { request(fio_net_req_stream()) }

    // MARK: answers

    /// 0 for an accepted request, else the refusal's why (error says more).
    public static func status(_ status: Int, _ body: Data) -> Int {
        Int(bytes(body) { fio_net_status_read(Int32(status), $0, $1) })
    }
    public static func action(_ status: Int, _ body: Data) -> NcActionSnap? {
        bytes(body) { fio_net_action_read(Int32(status), $0, $1) }.flatMap { try? readNcAction($0) }
    }
    /// One game's view: its envelope, sliced out of the body.
    public static func view(_ status: Int, _ body: Data) -> Data? {
        guard let row = bytes(body, { fio_net_view_read(Int32(status), $0, $1) }).flatMap({ try? readNcGameRow($0) }) else { return nil }
        return body.subdata(in: body.startIndex + row.envOff ..< body.startIndex + row.envOff + row.envLen)
    }

    // MARK: the stream

    public static func subscribe(_ what: Int, _ gameId: String?) {
        let g = chars(gameId ?? "")
        _ = g.withUnsafeBufferPointer { fio_net_stream_sub(Int32(what), $0.baseAddress, Int32(g.count - 1)) }
    }
    public static func unsubscribe(_ what: Int, _ gameId: String?) {
        let g = chars(gameId ?? "")
        _ = g.withUnsafeBufferPointer { fio_net_stream_unsub(Int32(what), $0.baseAddress, Int32(g.count - 1)) }
    }
    public static func streamOpened() { fio_net_stream_opened(now) }
    /// The socket closed: how long to wait before reopening it, in ms.
    public static func streamClosed() -> Int { Int(fio_net_stream_closed(now)) }
    /// One frame as it arrived: what to do, and the payload (an envelope or a cast).
    public static func streamIn(_ frame: Data, isText: Bool) -> (NcEventSnap, Data)? {
        guard let p = bytes(frame, { fio_net_stream_in($0, $1, isText ? 1 : 0, now) }),
              let ev = try? readNcEvent(p) else { return nil }
        let payload = fio_net_event_payload().map { Data(bytes: $0, count: ev.payloadLen) } ?? Data()
        return (ev, payload)
    }
    /// The next frame to send as text, nil for none now.
    public static func streamNext() -> String? { fio_net_stream_next(now).flatMap { try? readNcOut($0).text } }
    /// How long until there is one (-1: the socket is shut).
    public static func streamDueMs() -> Int { Int(fio_net_stream_due_ms(now)) }
    public static func gateReset(_ gameId: String) {
        let g = chars(gameId)
        g.withUnsafeBufferPointer { fio_net_gate_reset($0.baseAddress, Int32(g.count - 1)) }
    }

    // MARK: marshaling

    private static func request(_ p: UnsafeRawPointer?) -> NcRequestSnap? { p.flatMap { try? readNcRequest($0) } }

    private static func bytes<T>(_ d: Data, _ call: (UnsafePointer<UInt8>?, Int32) -> T) -> T {
        d.withUnsafeBytes { call($0.bindMemory(to: UInt8.self).baseAddress, Int32(d.count)) }
    }

    /// UTF-8 and one zero byte of tail, so an empty string still has an address
    /// (the count passed is without it).
    private static func chars(_ s: String) -> [CChar] { s.utf8.map { CChar(bitPattern: $0) } + [0] }

    private static func two(_ a: String, _ b: String, _ call: (UnsafePointer<CChar>?, Int32, UnsafePointer<CChar>?, Int32) -> UnsafeRawPointer?) -> NcRequestSnap? {
        let x = chars(a), y = chars(b)
        return request(x.withUnsafeBufferPointer { xp in
            y.withUnsafeBufferPointer { yp in call(xp.baseAddress, Int32(x.count - 1), yp.baseAddress, Int32(y.count - 1)) }
        })
    }
}
