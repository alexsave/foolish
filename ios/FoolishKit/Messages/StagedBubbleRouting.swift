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
        route(selectedURL: selectedURL, startingNewGame: startingNewGame,
              pendingStage: pendingStage, lastPayloadURL: lastPayloadURL,
              lastSentPayload: lastSentPayload).url
    }

    /// The full answer: the URL to present, AND whether the markers that pin it
    /// have been spent.
    public struct Route: Equatable {
        public let url: URL?
        /// DROP `pendingStage` AND `lastSentPayload`. Both exist to protect ONE
        /// chain - the one the board is standing on - from being torn down and
        /// re-adopted when my own bubble becomes the selection. The instant the
        /// surface presents something else, they are protecting a board that is
        /// no longer on screen, and a marker that outlives its board is a
        /// pin on the WRONG one.
        ///
        /// THE BUG THIS CLOSES, 1.0(37) (owner: "if you have one game open, and
        /// you scroll up and hit a different game bubble, it should completely
        /// switch to that other game. Not rebase, completely switch"). Send a
        /// move in game B, scroll up, tap game A - that switches, because A's
        /// bytes are neither marker. Now tap game B's bubble again: it IS
        /// `lastSentPayload`, `isMine` says so, and the pin hands back
        /// `lastPayloadURL` - which by then is game A's. The loadKey never
        /// moves, the surface never reloads, and tapping game B leaves game A
        /// on screen. Rare, because it needs a send and then two taps, and
        /// invisible in every test that only ever staged in one game.
        ///
        /// It is also the "make the bubble a noop" half: a draft for a game the
        /// human has left is disowned here, so its Send commits nothing to this
        /// device's cache and its bytes are no longer vouched for as "mine".
        /// What it does NOT do is remove the bubble - Messages offers no call
        /// for that, so the last guard is the kernel refusing to rebase a board
        /// onto another game's chain (msg_wire.h, MSG_TURN_SEND_OTHERGAME).
        public let clearMarkers: Bool
    }

    public static func route(selectedURL: URL?, startingNewGame: Bool,
                             pendingStage: (payload: Data, mySeat: Int)?,
                             lastPayloadURL: URL?,
                             lastSentPayload: Data? = nil) -> Route {
        let url = pinned(selectedURL: selectedURL, startingNewGame: startingNewGame,
                         pendingStage: pendingStage, lastPayloadURL: lastPayloadURL,
                         lastSentPayload: lastSentPayload)
        // The presentation MOVED. Not "a different bubble was tapped" - the
        // whole point of the pin above is that a tap on my own bubble does not
        // move it - but the URL this surface is actually being handed.
        return Route(url: url, clearMarkers: url != lastPayloadURL)
    }

    private static func pinned(selectedURL: URL?, startingNewGame: Bool,
                               pendingStage: (payload: Data, mySeat: Int)?,
                               lastPayloadURL: URL?,
                               lastSentPayload: Data?) -> URL? {
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
