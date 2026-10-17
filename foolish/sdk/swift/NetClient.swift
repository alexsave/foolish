// NetClient.swift - the backend's client (c/src/netclient.h), as the online app
// reaches it.
//
// Every decision is the kernel's: which request an intent becomes, its token,
// when the token is refreshed, what is sent again, what an answer means, which
// pushes are stale, what is refetched and what the stream is subscribed to.
// This file only moves bytes across (CFoolish fio_net_*) and reads what comes
// back through the generated readers (sdk/swift/gen, readNcDone and the rest).
// The sockets are FoolishNet's.
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

    /// The app shows boards and never decodes a cast: the client turns each cast into a board.
    public static func showBoards() { fio_net_set_boards(1) }

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

    /// Why the last intent was refused at once.
    public static var error: NcErrorSnap? { fio_net_error().flatMap { try? readNcError($0) } }

    // MARK: the pump

    /// A ticket for what the user wants (NC_DO_*), or 0: refused at once (`error` says why).
    public static func `do`(_ kind: Int, gameId: String = "", name: String = "", password: String = "",
                            bytes: [UInt8] = [], flag: Int = 0, intent: Int64 = -1) -> Int {
        let s = NcIntentSnap(kind: kind, flag: flag, gid: gameId, name: name, password: password,
                             bytes: bytes.map(Int.init), intent: intent)
        let p = UnsafeMutableRawPointer.allocate(byteCount: NcIntentSnap.cSize, alignment: 8)
        defer { p.deallocate() }
        guard (try? writeNcIntent(p, s)) != nil else { return 0 }
        return Int(fio_net_do(p, now))
    }

    /// The next request to send, or nil for none now.
    public static func nextRequest() -> NcRequestSnap? { fio_net_next_request(now).flatMap { try? readNcRequest($0) } }

    /// The answer to request `id` (status 0: it never arrived): the ticket it finished, if any.
    public static func onResponse(_ id: Int, _ status: Int, _ body: Data) -> NcDoneSnap? {
        bytes(body) { fio_net_on_response(UInt32(id), Int32(status), $0, $1, now) }.flatMap { try? readNcDone($0) }
    }

    /// A ticket finished with no answer of its own (the session ended under it).
    public static func nextDone() -> NcDoneSnap? { fio_net_next_done().flatMap { try? readNcDone($0) } }

    /// What the last answer or frame changed, each with its payload.
    public static func events() -> [(NcEventSnap, Data)] {
        var out: [(NcEventSnap, Data)] = []
        var i: Int32 = 0
        while let p = fio_net_event(i), let ev = try? readNcEvent(p) {
            let payload = fio_net_event_payload(i).map { Data(bytes: $0, count: ev.payloadLen) } ?? Data()
            out.append((ev, payload))
            i += 1
        }
        return out
    }

    /// When to call again, in ms (-1: nothing waits).
    public static func dueMs() -> Int { Int(fio_net_due_ms(now)) }

    // MARK: the stream

    public static var streamPath: String? { fio_net_stream_request().flatMap { try? readNcRequest($0).path } }
    /// Hold `what` (NW_SUB_*) for a game, or let one hold go: the client counts them.
    public static func subscribe(_ what: Int, _ gameId: String?, _ held: Bool = true) {
        let g = chars(gameId ?? "")
        _ = g.withUnsafeBufferPointer { (held ? fio_net_stream_sub : fio_net_stream_unsub)(Int32(what), $0.baseAddress, Int32(g.count - 1)) }
    }
    public static func streamOpened() { fio_net_stream_opened(now) }
    /// The socket closed: how long to wait before reopening it, in ms.
    public static func streamClosed() -> Int { Int(fio_net_stream_closed(now)) }
    /// One frame as it arrived; then `events()` says what it changed.
    public static func streamIn(_ frame: Data, isText: Bool) {
        _ = bytes(frame) { fio_net_stream_in($0, $1, isText ? 1 : 0, now) }
    }
    /// The next frame to send as text, nil for none now.
    public static func streamNext() -> String? { fio_net_stream_next(now).flatMap { try? readNcOut($0).text } }

    // MARK: marshaling

    private static func bytes<T>(_ d: Data, _ call: (UnsafePointer<UInt8>?, Int32) -> T) -> T {
        d.withUnsafeBytes { call($0.bindMemory(to: UInt8.self).baseAddress, Int32(d.count)) }
    }

    /// UTF-8 and one zero byte of tail, so an empty string still has an address
    /// (the count passed is without it).
    private static func chars(_ s: String) -> [CChar] { s.utf8.map { CChar(bitPattern: $0) } + [0] }
}
