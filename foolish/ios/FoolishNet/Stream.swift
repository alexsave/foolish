// Stream.swift - the one WebSocket to c_base's push stream.
//
// The kernel's client decides what goes on it (the subscriptions, their
// retries, the ping) and which pushes are stale; this file opens the socket,
// passes every frame in, sends every frame the client asks for (Backend's loop
// wakes it), and reopens the socket after the delay the client names. A view is
// handed to the game it is of.

import Foundation
import FoolishKit

@MainActor
final class Stream {
    static let shared = Stream()

    /// Per game: what to do with its newest envelope.
    private var watchers: [String: (Data) -> Void] = [:]
    private var task: URLSessionWebSocketTask?

    /// A player hears every view of theirs (one subscription for all their
    /// games); a spectator hears the game's casts, each of which the client
    /// turns into the game's board (NetClient.showBoards).
    func watch(_ gameId: String, spectator: Bool, view: @escaping (Data) -> Void) {
        watchers[gameId] = view
        NetClient.subscribe(spectator ? NW_SUB_GAME : NW_SUB_GAMES, spectator ? gameId : nil)
        open()
        Backend.shared.pump()
    }

    func unwatch(_ gameId: String, spectator: Bool) {
        watchers[gameId] = nil
        NetClient.subscribe(spectator ? NW_SUB_GAME : NW_SUB_GAMES, spectator ? gameId : nil, false)
        Backend.shared.pump()
    }

    /// Sends every frame the client has for the socket.
    func send() {
        guard let task else { return }
        while let bytes = NetClient.streamNext() { task.send(.data(bytes)) { _ in } }
    }

    /// What the client says changed, to the game it is of.
    func deliver(_ ev: NcEventSnap, _ payload: Data) {
        if ev.kind == NC_EV_VIEW, !payload.isEmpty { watchers[ev.gid]?(payload) }
    }

    private func open() {
        guard task == nil, let url = Backend.shared.streamURL else { return }
        let t = URLSession.shared.webSocketTask(with: url, protocols: [NetClient.streamProtocol])
        task = t
        t.resume()
        NetClient.streamOpened()
        receive(t)
    }

    private func receive(_ t: URLSessionWebSocketTask) {
        t.receive { [weak self] result in
            Task { @MainActor in
                guard let self, self.task === t else { return }
                switch result {
                case .success(let m):
                    self.handle(m)
                    self.receive(t)
                case .failure:
                    self.closed()
                }
            }
        }
    }

    private func handle(_ m: URLSessionWebSocketTask.Message) {
        let frame: (Data, Bool)
        switch m {
        case .data(let d): frame = (d, false)
        case .string(let s): frame = (Data(s.utf8), true)
        @unknown default: return
        }
        NetClient.streamIn(frame.0, isText: frame.1)
        Backend.shared.changed()
        Backend.shared.pump()
    }

    private func closed() {
        task = nil
        let ms = NetClient.streamClosed()
        Task { [weak self] in
            try? await Task.sleep(nanoseconds: UInt64(ms) * 1_000_000)
            guard let self, !self.watchers.isEmpty else { return }
            self.open()
            Backend.shared.pump()
        }
    }

    /// Signed out: the socket goes, and nothing reopens it.
    func stop() {
        watchers.removeAll()
        task?.cancel(with: .goingAway, reason: nil)
        task = nil
    }
}
