// Stream.swift - the one WebSocket to c_base's push stream.
//
// The kernel's client decides what goes on it (the token, the subscriptions,
// their retries, the ping) and which pushes are stale; this file opens the
// socket, passes every frame in, sends every frame the client asks for, wakes
// when the client says something is due, and reopens the socket after the delay
// the client names. A view is handed to the game it is of.

import Foundation
import FoolishKit

@MainActor
final class Stream {
    static let shared = Stream()

    /// Per game: what to do with its newest envelope, and with news that it moved.
    private var watchers: [String: (view: (Data) -> Void, moved: () -> Void)] = [:]
    private var task: URLSessionWebSocketTask?
    private var wake: Task<Void, Never>?

    /// A player hears every view of theirs (one subscription for all their
    /// games); a spectator hears the game's casts and refetches on each.
    func watch(_ gameId: String, spectator: Bool, view: @escaping (Data) -> Void, moved: @escaping () -> Void) {
        watchers[gameId] = (view, moved)
        NetClient.subscribe(spectator ? NW_SUB_GAME : NW_SUB_GAMES, spectator ? gameId : nil)
        open()
        flush()
    }

    func unwatch(_ gameId: String, spectator: Bool) {
        watchers[gameId] = nil
        if spectator { NetClient.unsubscribe(NW_SUB_GAME, gameId) }
        flush()
    }

    /// Sends whatever the client has due, and sleeps until the next thing is.
    func flush() {
        guard let task else { return }
        while let text = NetClient.streamNext() { task.send(.string(text)) { _ in } }
        wake?.cancel()
        let ms = NetClient.streamDueMs()
        guard ms >= 0 else { return }
        wake = Task { [weak self] in
            try? await Task.sleep(nanoseconds: UInt64(ms) * 1_000_000)
            if !Task.isCancelled { self?.flush() }
        }
    }

    private func open() {
        guard task == nil, let url = Backend.shared.streamURL else { return }
        let t = URLSession.shared.webSocketTask(with: url)
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
        guard let (ev, payload) = NetClient.streamIn(frame.0, isText: frame.1) else { return }
        if ev.kind == NC_EV_VIEW, !payload.isEmpty { watchers[ev.gid]?.view(payload) }
        if ev.kind == NC_EV_CAST { watchers[ev.gid]?.moved() }
        flush()
    }

    private func closed() {
        task = nil
        wake?.cancel()
        let ms = NetClient.streamClosed()
        Task { [weak self] in
            try? await Task.sleep(nanoseconds: UInt64(ms) * 1_000_000)
            guard let self, !self.watchers.isEmpty else { return }
            self.open()
            self.flush()
        }
    }

    /// Signed out: the socket goes, and nothing reopens it.
    func stop() {
        watchers.removeAll()
        wake?.cancel()
        task?.cancel(with: .goingAway, reason: nil)
        task = nil
    }
}
