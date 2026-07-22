// StagedBubbleRouting.swift — note 11 (HARNESS_NOTES_R2): don't adopt my own
// just-staged bubble as if it were new.
//
// `MessagesViewController.stage` inserts the staged bubble via
// `conversation.insert`, which Apple makes the conversation's
// `selectedMessage`. The auto-collapse that follows (note 8's `waitForSettle`
// then `.compact`) fires `willTransition` -> `present()`, and without this
// check `present()` would route that selection as though a brand-new bubble
// had arrived: `payloadURL` changes, `GameSurface.loadKey` changes, the whole
// live controller gets torn down and rebuilt from the URL. The App Group
// cache hasn't committed yet at that point — only `didStartSending` does that
// (§7.6) — so the freshly-rebuilt controller's delta-since-cache looks like a
// genuine new move, and it REPLAYS THE ONE I JUST WATCHED MYSELF PLAY.
//
// Pure decision, zero MSConversation/MSMessage coupling, so it's testable
// without the Messages framework (mirrors SeatIdentity's own reasoning) —
// the extension only wires MSConversation/MSMessage values into it.
import Foundation

public enum StagedBubbleRouting {
    /// What `payloadURL` `present()` should hand to `MessagesRootView`, given
    /// what's currently selected in the conversation and what (if anything)
    /// this device is still waiting to see actually sent.
    ///
    /// - `startingNewGame` short-circuits to nil regardless (New game always
    ///   routes to setup, same as before this fix).
    /// - Otherwise, if `pendingStage` is set AND `selectedURL` decodes to
    ///   EXACTLY those payload bytes, `selectedURL` is recognised as my own
    ///   staged bubble becoming selected (not a new incoming one) — the
    ///   caller keeps presenting with `lastPayloadURL`, the URL already in
    ///   use, so `GameSurface.loadKey` never changes and the live board
    ///   survives untouched (same as a plain compact<->expanded style
    ///   toggle). Byte-equality is safe here: `pendingStage.payload` is a
    ///   value only THIS device just sealed (a fresh hash chain + digest),
    ///   so a genuinely different incoming bubble can never collide with it.
    /// - Otherwise `selectedURL` is used as-is (the ordinary path: a real
    ///   bubble tap, a genuinely new incoming message, or nil/no selection).
    public static func resolvedPayloadURL(selectedURL: URL?, startingNewGame: Bool,
                                          pendingStage: (payload: Data, mySeat: Int)?,
                                          lastPayloadURL: URL?,
                                          lastSentPayload: Data? = nil) -> URL? {
        if startingNewGame { return nil }
        guard let selectedURL else { return nil }
        guard let incoming = try? MessageEnvelope.payloadBytes(url: selectedURL) else {
            return selectedURL
        }
        // Mine, still in the input field — or mine, already SENT. Both keep the
        // board exactly as it is.
        if incoming == pendingStage?.payload || incoming == lastSentPayload {
            return lastPayloadURL
        }
        return selectedURL
    }
}
