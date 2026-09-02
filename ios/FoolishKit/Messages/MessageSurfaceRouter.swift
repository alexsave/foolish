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
    /// ROUND 7 (owner: "the last text has everything we need"): the extension now
    /// renders EXACTLY the tapped bubble. Rule P (prefer a cached chain over a
    /// stale-looking tapped one) and the no-selection reopen from cache are gone
    /// with the preferred-chain store — so this is now just:
    ///  1. New game always wins — the human just asked for it.
    ///  2. No bubble selected, nothing to render — offer New game (setup).
    ///  3. A bubble: decode it (which validates it); its PHASE picks lobby vs board.
    ///
    /// `store`/`kernel` are still accepted so every call site and test compiles
    /// unchanged; neither is consulted any more.
    public static func resolve(payload: Data?, startNewGame: Bool, chatKey: String,
                               store: MessageGameStore = .shared,
                               kernel: MessageKernel = .shared) async -> SurfaceScreen {
        if startNewGame { return .setup }
        guard let incoming = payload else { return .setup }
        guard let env = try? await MessageEnvelope.decode(payload: incoming, viewer: -1) else {
            return .damaged
        }
        return screen(for: incoming, phase: env.phase)
    }

    private static func screen(for payload: Data, phase: Int) -> SurfaceScreen {
        phase == 0 ? .lobby(payload: payload) : .board(payload: payload)
    }
}
