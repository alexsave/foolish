// MessageSurfaceRouter — WHAT the extension should show, as a value.
//
// This is the decision `GameSurface.load()` used to make inline, writing its
// answer straight into six pieces of view @State (`lobby`, `controller`,
// `showSetup`, `ambiguous`, `spectator`, `damaged`). That is why "the extension
// is stuck on the lobby while a game is in play" kept coming back in different
// shapes: whether the right screen appears depended not only on the rule below
// but on whether SwiftUI happened to re-run the loader, and a screen left over
// from the last run looks exactly like a screen the rule chose.
//
// Now the rule is a function of its inputs — the tapped bubble's bytes, this
// device's cache for THIS chat, and whether the human asked for a new game —
// and the view only renders what it returns. It can be driven end to end in a
// test, with no simulator and no taps, which is what the group-lobby regression
// needed.
//
// It answers no Durak rule: Rule P is the kernel's (`MessageKernel.preferred`),
// and the phase/round/turn it compares are the kernel's own decode.
import Foundation

/// What to put on screen. Deliberately NOT one-to-one with the view's states:
/// seat identity (§6) still happens in the view for the `.board` case, because
/// it needs the participant identity only the host knows.
public enum SurfaceScreen: Equatable {
    /// No game in this chat yet (or the human tapped New game).
    case setup
    /// A WAITING invite — the roster, never a board (a lobby seal leaves a game
    /// dealt at the lobby's CAPACITY resident, so rendering it as a board shows
    /// a phantom 8-seat game; see msg_wire.h's Rule P rule 0).
    case lobby(payload: Data)
    /// A real game to seat the viewer on.
    case board(payload: Data)
    /// The bytes are not a chain we can read.
    case damaged
}

public enum MessageSurfaceRouter {

    /// Resolve the screen for `payload` (the selected bubble's bytes, or nil if
    /// no bubble is selected).
    ///
    /// The order matters and each step is load-bearing:
    ///  1. New game always wins — the human just asked for it.
    ///  2. No bubble selected: reopen this CHAT's newest cached game, if any.
    ///     Chat-scoped (see ChatKey): a device-wide lookup here is how chat B
    ///     used to reopen chat A's board.
    ///  3. A bubble: decode it (which validates it), then let Rule P choose
    ///     between it and whatever chain we already trust for that game id.
    ///  4. The winner's PHASE decides lobby vs board. Never the incoming
    ///     bubble's, and never the cache's — the winner's.
    public static func resolve(payload: Data?, startNewGame: Bool, chatKey: String,
                               store: MessageGameStore = .shared,
                               kernel: MessageKernel = .shared) async -> SurfaceScreen {
        if startNewGame { return .setup }

        guard let incoming = payload else {
            guard let row = store.games(chatKey: chatKey).first,
                  let cached = Base32.decode(row.payloadBase32),
                  let env = try? await MessageEnvelope.decode(payload: cached, viewer: -1)
            else { return .setup }
            return screen(for: cached, phase: env.phase)
        }

        guard let env = try? await MessageEnvelope.decode(payload: incoming, viewer: -1) else {
            return .damaged
        }

        // Rule P against the chain we already hold for this game, if any. The
        // kernel decides; a WAITING lobby never outranks the game started from
        // it (rule 0), which is what stops a joiner's own cached lobby from
        // hiding the game everyone else is already playing. Bubble-anchored
        // lookup (recordForBubble): the tapped bubble's gameId identifies the
        // row even after a group-membership change re-keyed this chat — the
        // strictly-scoped `games(chatKey:)` above stays the no-bubble gate.
        if let row = store.recordForBubble(gameId: env.gameId),
           let cached = Base32.decode(row.payloadBase32), cached != incoming,
           let cachedEnv = try? await MessageEnvelope.decode(payload: cached, viewer: -1),
           ((try? await kernel.preferred(cached, incoming)) ?? 0) < 0 {
            return screen(for: cached, phase: cachedEnv.phase)
        }
        return screen(for: incoming, phase: env.phase)
    }

    private static func screen(for payload: Data, phase: Int) -> SurfaceScreen {
        phase == 0 ? .lobby(payload: payload) : .board(payload: payload)
    }
}
