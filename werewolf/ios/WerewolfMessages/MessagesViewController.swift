// The iMessage extension's entry point.
//
// Messages instantiates this class (Info.plist NSExtensionPrincipalClass) and
// drives it through the lifecycle below. What it renders comes from WerewolfKit;
// what it stages is the kernel's sealed bytes. This file owns the three things
// SwiftUI must not: the MSConversation, the insert of a staged bubble (the human
// always presses send), and the App Group note of which chain this device has
// seen.
//
// THE RULE, restated because this is where it is most tempting to break: no
// werewolf rule is answered here. Who is a wolf, whose view carries what, whose
// call tonight is, which chain wins a race, whether Send is legal - all C, via
// Kernel. Nothing in this file may read a role.
import Messages
import SwiftUI
import UIKit
import WerewolfKit

final class MessagesViewController: MSMessagesAppViewController {

    private var host: UIHostingController<RootView>?

    /// The bytes of the bubble the resident game came from. The next seal names it
    /// as its parent, so it must be the EXACT bytes that arrived - a re-serialized
    /// "same" envelope hashes differently and would name a chain nobody sent.
    private var parentPayload: Data?

    /// The payload staged and awaiting the human's send. Dropped on cancel.
    private var pendingStage: Data?

    /// Identity, per game. `gameId` comes off the bubble; on creation this device
    /// mints one, because somebody has to and the creator is the only one there.
    private var gameId: UInt64 = 0
    private var mySeat: Int = -1
    private var lastSealAt: UInt16 = 0
    /// Whether the bubble on screen is this device's own. The lobby's "the newest
    /// sender stands aside while there is still room" rule reads it, and Messages is
    /// the only thing that can answer it.
    private var iSentTheNewest = false
    /// The chat shape, which is the one thing lobby capacity varies by.
    private var chatIsDM = false

    // ------------------------------------------------------- the lifecycle ---

    override func willBecomeActive(with conversation: MSConversation) {
        super.willBecomeActive(with: conversation)
        adopt(conversation.selectedMessage, in: conversation)
        present(conversation)
    }

    override func didSelect(_ message: MSMessage, conversation: MSConversation) {
        adopt(message, in: conversation)
        present(conversation)
    }

    /// A bubble that ARRIVED while we are on screen. Apple does not make an
    /// arrival the selectedMessage, so without this a player stranded on a losing
    /// fork stays stranded until they happen to re-tap something.
    override func didReceive(_ message: MSMessage, conversation: MSConversation) {
        guard let incoming = payload(of: message) else { return }
        // RULE P DECIDES, not arrival order. Two devices can transiently disagree
        // about which message is newest, so the comparison is on the bytes.
        if let showing = parentPayload, !Kernel.shared.prefers(incoming: incoming, over: showing) {
            return
        }
        adopt(message, in: conversation)
        present(conversation)
    }

    override func didStartSending(_ message: MSMessage, conversation: MSConversation) {
        // The bubble is really going. From here the chain this device is on is the
        // one it just sent, so the next seal names THAT as its parent.
        if let staged = pendingStage {
            parentPayload = staged
            lastSealAt = nowSeconds()
        }
        pendingStage = nil
    }

    override func didCancelSending(_ message: MSMessage, conversation: MSConversation) {
        // Messages offers no API to remove a staged bubble, so a cancel is the
        // human deleting it. The kernel already holds the record, and that is
        // correct: the chain they staged is a chain they can still send.
        pendingStage = nil
    }

    // ------------------------------------------------------------ adoption ---

    private func adopt(_ message: MSMessage?, in conversation: MSConversation) {
        guard let message, let bytes = payload(of: message) else { return }
        do {
            // DECODING IS ADOPTING, and there is no await in here for exactly that
            // reason: a half-adopted game is one this device could then seal and
            // send.
            try Kernel.shared.adopt(bytes)
            parentPayload = bytes
            gameId = Self.gameId(of: bytes)
            resolveSeat(message, conversation)
        } catch {
            // A damaged link leaves the device on whatever it was already showing.
            // Never a partial recovery: the kernel refuses the whole chain.
            return
        }
    }

    /// Seat identity, which is the one question this file asks that is not about a
    /// rule - and even that answer comes from C (`ww_seat.h`).
    private func resolveSeat(_ message: MSMessage, _ conversation: MSConversation) {
        let senderIsLocal = message.senderParticipantIdentifier == conversation.localParticipantIdentifier
        chatIsDM = conversation.remoteParticipantIdentifiers.count == 1
        iSentTheNewest = senderIsLocal
        let name = Self.myName()
        // A LOBBY SEAT AND A BOARD SEAT ARE DIFFERENT QUESTIONS, and the difference
        // is a rule rather than a caller's convenience (ww_seat.h). A lobby bubble
        // is a snapshot of who had joined when it was sealed, so a seat it does not
        // list is a seat that had not been claimed on this branch - granting Start
        // off it is how a stale invite hands the game to somebody the lobby does
        // not contain. A live board is the opposite: it carries every seated player
        // forward, so a roster that has not caught up is not evidence.
        let inLobby = Kernel.shared.phase(-1) == .lobby
        let seat = inLobby
            ? Kernel.shared.seatInLobby(cached: mySeat, senderIsLocal: senderIsLocal,
                                        lastActor: -1, chatIsDM: chatIsDM, myName: name)
            : Kernel.shared.seatOnBoard(cached: mySeat, senderIsLocal: senderIsLocal,
                                        lastActor: -1, chatIsDM: chatIsDM, myName: name)
        // -1 means the bubble cannot say, and a spectator board is the honest
        // answer. Inventing a seat here would hand somebody else's night to this
        // phone, and in this game that is the whole game.
        mySeat = seat
    }

    // ------------------------------------------------------------- present ---

    private func present(_ conversation: MSConversation) {
        host?.willMove(toParent: nil)
        host?.view.removeFromSuperview()
        host?.removeFromParent()

        let root = RootView(mySeat: mySeat,
                            gameId: gameId,
                            parent: parentPayload,
                            lastSealAt: lastSealAt,
                            iSentTheNewest: iSentTheNewest,
                            stage: { [weak self] payload in self?.stage(payload, in: conversation) },
                            create: { [weak self] in self?.create(in: conversation) },
                            join: { [weak self] in self?.join(in: conversation) },
                            start: { [weak self] in self?.start(in: conversation) },
                            invite: { [weak self] in self?.reseal(in: conversation) },
                            exit: { [weak self] in self?.reseal(in: conversation) })
        let hc = UIHostingController(rootView: root)
        hc.view.backgroundColor = .clear
        addChild(hc)
        view.addSubview(hc.view)
        hc.view.translatesAutoresizingMaskIntoConstraints = false
        NSLayoutConstraint.activate([
            hc.view.leadingAnchor.constraint(equalTo: view.leadingAnchor),
            hc.view.trailingAnchor.constraint(equalTo: view.trailingAnchor),
            hc.view.topAnchor.constraint(equalTo: view.topAnchor),
            hc.view.bottomAnchor.constraint(equalTo: view.bottomAnchor),
        ])
        hc.didMove(toParent: self)
        host = hc
    }

    // -------------------------------------------------------------- create ---

    // CREATING NEVER DEALS. It locks a seed and a game id and seals a WAITING
    // bubble seating only the creator. Nobody has a role until Start - which is why
    // tapping this again is not a reroll: there is nothing yet to look at, so there
    // is nothing to reroll for.
    //
    // There is no "deal immediately in a 1:1" branch here, and there must never be
    // one. The tree this was forked from had one, and it let the creator see their
    // hand before anything committed. For this game that is seeing your ROLE before
    // committing, which is not a fairness bug - it is the whole game.
    private func create(in conversation: MSConversation) {
        do {
            // The hidden deal is 32 bytes from the OS CSPRNG. A player legitimately
            // observes some outputs of the stream it seeds - their own role - so it
            // cannot come from anything reversible.
            chatIsDM = conversation.remoteParticipantIdentifiers.count == 1
            try Kernel.shared.createLobby(seed: Kernel.freshSeed(),
                                          chatIsDM: chatIsDM,
                                          myName: Self.myName())
            gameId = UInt64.random(in: 1...UInt64.max)
            mySeat = 0
            parentPayload = nil
            lastSealAt = nowSeconds()
            present(conversation)
            reseal(in: conversation)
        } catch {
            return
        }
    }

    /// Join the lobby on screen at the lowest free seat, then reseal it. Route one
    /// of the two to Start: the joiner's claim lands as its own WAITING bubble and
    /// anybody already in can then start it.
    private func join(in conversation: MSConversation) {
        do {
            mySeat = try Kernel.shared.joinLobby(myName: Self.myName())
            present(conversation)
            reseal(in: conversation)
        } catch {
            return
        }
    }

    /// START. Both routes land here, which is what makes them one deal:
    /// join-then-start arrives with the joiner's own WAITING bubble as the parent,
    /// and join-and-start arrives having just joined off somebody else's. Either
    /// way Start re-derives the LOCKED seed at the join count.
    private func start(in conversation: MSConversation) {
        guard let lobby = parentPayload else { return }
        do {
            // Handed the LOBBY CHAIN's own bytes, not trusted to what is resident.
            // This is one resident kernel that every chat, lobby and board decodes
            // through, so by the time somebody taps Start the resident game
            // routinely belongs to something else - and starting off it deals the
            // wrong roles with no error anywhere.
            try Kernel.shared.startFromLobby(lobby)
            present(conversation)
            reseal(in: conversation)
        } catch {
            return
        }
    }

    /// Seal the resident state and stage it. What "Send the invite" does, what a
    /// join does after claiming its seat, and what Start does after dealing.
    private func reseal(in conversation: MSConversation) {
        do {
            let payload = try Kernel.shared.seal(gameId: gameId,
                                                sentAt: nowSeconds(),
                                                parent: parentPayload)
            stage(payload, in: conversation)
        } catch {
            return
        }
    }

    // --------------------------------------------------------------- stage ---

    private func stage(_ payload: Data, in conversation: MSConversation) {
        let message = MSMessage(session: conversation.selectedMessage?.session ?? MSSession())
        let layout = MSMessageTemplateLayout()
        // The caption says what every bubble on this game says, whoever sent it and
        // whatever they sent. A caption that varied with the sender's role would
        // undo the entire night in one line of text.
        // The caption says what EVERY bubble on this game says, whoever sent it and
        // whatever they sent. A caption that varied with the sender's role would
        // undo the entire night in one line of text - and a caption that named the
        // wolves' line would do it in one word.
        layout.caption = Kernel.shared.phase(mySeat) == .lobby
            ? "Werewolf - \(Kernel.shared.lobbyJoined) in"
            : "Night \(Kernel.shared.night(mySeat) + 1)"
        layout.subcaption = "Werewolf"
        message.layout = layout
        message.url = Self.url(for: payload)
        message.summaryText = "A werewolf night"
        pendingStage = payload
        conversation.insert(message) { _ in }
    }

    // -------------------------------------------------------------- codecs ---
    //
    // base64url over the raw envelope. NOT base32 and not a JSON wrapper: the
    // payload is packed bytes and a URL query value is the only thing Messages
    // will carry, so the encoding's only job is to survive that trip.

    private static func url(for payload: Data) -> URL {
        var c = URLComponents()
        c.scheme = "werewolf"
        c.host = "n"
        c.queryItems = [URLQueryItem(name: "b", value: base64url(payload))]
        return c.url!
    }

    private func payload(of message: MSMessage) -> Data? {
        guard let url = message.url,
              let items = URLComponents(url: url, resolvingAgainstBaseURL: false)?.queryItems,
              let b = items.first(where: { $0.name == "b" })?.value
        else { return nil }
        return Self.unbase64url(b)
    }

    private static func base64url(_ d: Data) -> String {
        d.base64EncodedString()
            .replacingOccurrences(of: "+", with: "-")
            .replacingOccurrences(of: "/", with: "_")
            .replacingOccurrences(of: "=", with: "")
    }

    private static func unbase64url(_ s: String) -> Data? {
        var t = s.replacingOccurrences(of: "-", with: "+")
                 .replacingOccurrences(of: "_", with: "/")
        while t.count % 4 != 0 { t += "=" }
        return Data(base64Encoded: t)
    }

    /// The game id lives in the envelope at offset 4, little-endian. Read here
    /// rather than asked of the kernel because it is this file's business: it
    /// identifies the THREAD's game, not the game's state.
    private static func gameId(of payload: Data) -> UInt64 {
        guard payload.count >= 12 else { return 0 }
        var v: UInt64 = 0
        for i in (4..<12).reversed() { v = (v << 8) | UInt64(payload[payload.startIndex + i]) }
        return v
    }

    /// The nickname this device claims. Free-form and per-install: the extension
    /// has no account and Messages will not tell it the local participant's name.
    private static func myName() -> String {
        let store = UserDefaults(suiteName: "group.cards.werewolf.msg")
        if let n = store?.string(forKey: "nickname"), !n.isEmpty { return n }
        let fallback = "Player"
        store?.set(fallback, forKey: "nickname")
        return fallback
    }

    /// Unix seconds mod 65536 - the width the envelope's clock carries, so the
    /// kernel's wrap arithmetic is the same arithmetic on both sides.
    private func nowSeconds() -> UInt16 {
        UInt16(truncatingIfNeeded: Int(Date().timeIntervalSince1970))
    }
}
