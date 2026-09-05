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
//
// DELIBERATELY NOT LIFTED, while its three neighbours were (StaleBranchGate,
// NicknameGate, SeatIdentity all became msg_wire.c rules). Everything here is
// either URL work, which is Swift's by the same rule that keeps Base32 and the
// "/m/1" prefix in Swift, or `isMine` - which is two byte comparisons. A second
// chain client cannot get `a == b || a == c` wrong; what it could get wrong is
// WHY byte equality is enough, and that is the comment below rather than code.
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
        if isMine(incoming, pendingStage: pendingStage?.payload, lastSentPayload: lastSentPayload) {
            return lastPayloadURL
        }
        return selectedURL
    }

    /// ROUND 12 #11 ("sometimes animation replays when the bubble is sent - I
    /// saw this for an attack"): is this chain one THIS DEVICE authored?
    ///
    /// The same question `resolvedPayloadURL` asks of the SELECTED bubble, asked
    /// of an ARRIVING one - because an arrival can be mine too. The simulator
    /// loops a sent message straight back to the sender, and an iCloud account
    /// signed in on two devices does the same thing for real; either way
    /// `didReceive` hands us the bytes we just sealed. Threaded on as an arrival,
    /// the surface adopts it, `MessageTurnController.begin` arms the open-replay
    /// for its last move - MY move, the one I just watched - and the board veils
    /// the cards that are "about to fly". Filmed at 30fps: the card the player
    /// played disappears from the table the instant Send lands, and when the
    /// arming later runs, the attack animates a second time.
    ///
    /// Byte equality is the whole test, and it is safe: a staged/sent payload is
    /// a hash chain THIS device sealed, so no other device's bubble can collide
    /// with it. `pendingStage` covers the moment between insert and send;
    /// `lastSentPayload` the window after.
    public static func isMine(_ payload: Data?, pendingStage: Data?,
                              lastSentPayload: Data?) -> Bool {
        guard let payload else { return false }
        return payload == pendingStage || payload == lastSentPayload
    }
}
