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
    /// The rule:
    ///  1. New game always wins — the human just asked for it.
    ///  2. A tapped bubble: decode it (which validates it); its PHASE picks
    ///     lobby vs board.
    ///  3. NO bubble tapped (opened from the app drawer's + button): reopen THIS
    ///     chat's last cached chain, so the drawer button is IDENTICAL to tapping
    ///     the thread's last message. An iMessage extension has no access to the
    ///     transcript — `selectedMessage` is the ONLY message it can read — so the
    ///     App-Group cache is the sole way to answer "what is this thread's game"
    ///     without a tap. One game per thread: `games(chatKey:)` is chat-scoped and
    ///     newest-first, so `.first` is the last chain this device saw (received OR
    ///     sent — both are cached, see `GameSurface.cache` / the onSend persist).
    ///  4. Nothing tapped and nothing cached — a thread with no game yet — offer
    ///     New game (setup).
    ///
    /// ROUND 8 restored the no-selection reopen ROUND 7 had removed. What made
    /// Round 7 remove it was the preferred-chain store's habit of preferring a
    /// STALE cached chain over a fresh tapped one; that whole store is gone, and
    /// this reopen only fires when there is NO tapped chain to prefer, so the two
    /// do not conflict.
    public static func resolve(payload: Data?, startNewGame: Bool, chatKey: String,
                               store: MessageGameStore = .shared,
                               kernel: MessageKernel = .shared) async -> SurfaceScreen {
        if startNewGame { return .setup }
        var incoming = payload
        if incoming == nil,
           let record = store.games(chatKey: chatKey).first,
           let bytes = Base32.decode(record.payloadBase32) {
            incoming = bytes
        }
        guard let bytes = incoming else { return .setup }
        guard let env = try? await MessageEnvelope.decode(payload: bytes, viewer: -1) else {
            return .damaged
        }
        return screen(for: bytes, phase: env.phase)
    }

    private static func screen(for payload: Data, phase: Int) -> SurfaceScreen {
        phase == 0 ? .lobby(payload: payload) : .board(payload: payload)
    }
}
