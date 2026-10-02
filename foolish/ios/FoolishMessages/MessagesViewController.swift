// MessagesViewController — the iMessage extension's entry point (design §11).
//
// Messages instantiates this class (Info.plist NSExtensionPrincipalClass) and
// drives it through the lifecycle below. What it renders comes from FoolishKit
// (the shared board + engine); what it stages comes from MessageComposer. This
// file owns the three things SwiftUI must not: the MSConversation, the insert of
// a staged bubble (§11.4 — the human always presses send), and the App Group
// cache commit that happens exactly when a send actually starts.
//
// THE RULE, restated because this is where it is most tempting to break: no Durak
// rule is answered here. Whose move, whether a move is legal, which chain wins,
// whether a staged move survives — all C (msg_wire.c via MessageKernel). Seat
// identity is the one non-kernel call, and it is SeatIdentity's pure §6 logic.
import Combine
#if RIG_ARRIVE
import os
#endif
import QuartzCore
import UIKit
import Messages
import SwiftUI
import FoolishKit

final class MessagesViewController: MSMessagesAppViewController {

    private var host: UIHostingController<MessagesRootView>?
    /// Set when the user taps New game so the next expanded present deals a
    /// genesis game rather than routing a selected bubble.
    private var startingNewGame = false
    /// The next bubble opens a NEW MSSession, rather than collapsing into the
    /// card of the game it came from.
    ///
    /// Separate from `startingNewGame`, which is a claim about the SURFACE ("the
    /// user asked for the New game screen") and routes straight to setup. A
    /// REMATCH needs the session half and not the surface half: it has already
    /// built its lobby out of the finished board, and asking for setup threw
    /// that lobby away - the rematch bubble staged correctly and the extension
    /// showed New game / Create game behind it. Found on the simulator, 1.0(17).
    private var freshSession = false
    /// Incremented on each New game tap. Threaded into MessagesRootView so an
    /// explicit New game resets the session, while a compact<->expanded style
    /// toggle (same token) preserves the in-progress game.
    private var newGameToken = 0
    /// The name of the player whose LEAVE is about to be staged (round 16), or
    /// nil. Set by the lobby's Exit and consumed by the very next `stage` -
    /// one bubble, one announcement.
    private var pendingLeftName: String?
    /// Incremented on each real SEND (didStartSending). Threaded into
    /// MessagesRootView so the live board can drop the just-sent move from its
    /// pending list (`markSent`) - otherwise the Undo button lingered in the
    /// collapsed view and re-staged an already-sent move (round-6 bug 4).
    private var sentToken = 0
    /// The payload we last staged (via `insert`), awaiting the human's send/cancel
    /// (§7.6). Committed to the cache on didStartSending, dropped on cancel. Carries
    /// the game id so an explicit CANCEL can clear that game's pending ledger
    /// synchronously (round-6 bugs 1 & 2), without a second async decode.
    private var pendingStage: (payload: Data, mySeat: Int, gameId: String)?
    /// note 11: the `payloadURL` the last `present()` call actually used —
    /// what `StagedBubbleRouting.resolvedPayloadURL` falls back to when the
    /// newly-selected message turns out to be our OWN just-staged bubble
    /// (see that type's doc), so the live GameSurface's `loadKey` doesn't
    /// change and the board isn't torn down and rebuilt out from under itself.
    private var lastPayloadURL: URL?
    /// The payload this device most recently SENT. `pendingStage` is cleared at
    /// `didStartSending`, but Messages leaves that bubble selected — so without
    /// this the next `present()` reloaded the whole surface from my own just-sent
    /// chain and replayed the move I had just watched myself play (round-3's
    /// double animation). See StagedBubbleRouting.
    private var lastSentPayload: Data?
    /// The bubble that last ARRIVED while we are on screen (`didReceive`), and a
    /// token bumped per arrival. Apple does not make an arrival the
    /// `selectedMessage`, so `payloadURL`/loadKey never move for it — the surface
    /// folds it in separately (GameSurface.maybeAdoptIncoming), Rule P deciding.
    /// Without this a player stranded on a losing Start fork stayed stranded
    /// until they happened to re-tap a bubble (the 4-player double-Start
    /// deadlock).
    private var incomingURL: URL?
    private var incomingToken = 0
    /// Round-9: bumped when the human deletes the staged bubble
    /// (didCancelSending), so the surface can drop its send reminder.
    private var cancelToken = 0
    /// Round-10d: the collapse arm, delivered in place (no re-present, which
    /// would reload the board mid-transition) - see CollapseSignal.
    private let collapseSignal = CollapseSignal()

    // MARK: - Lifecycle (§11.1)

    /// ROUND 16, the flight recorder. Opened as the very first thing this
    /// extension does, because everything before it is invisible to the trail -
    /// and the failure being chased (the drawer freezing, "even on newer
    /// devices") leaves no other evidence: a memory kill is a SIGKILL, so
    /// nothing runs afterwards to report it. See FlightRecorder.
    /// Keeps the host's fallback colour in step with the TABLE MATERIAL, which
    /// is a preference and so cannot ride the trait collection. Without it,
    /// switching wool<->felt in Settings leaves the old material's colour behind
    /// the drawer until the next `present()`.
    private var prefsSink: AnyCancellable?

    override func viewDidLoad() {
        super.viewDidLoad()
        // A CHAIN, not a broadcast: every bubble carries the whole game in a
        // total order, so a newer chain is the complete truth and a card it does
        // not account for is doomed. The kernel has no default for this
        // (anim_plan.h) and every board built below reads it.
        AnimTransport.declare(.chain)
        FlightRecorder.begin("style \(presentationStyle == .compact ? "compact" : "expanded")")
#if RIG_ARRIVE
        // Pinned now, before any seed is claimed: the door trusts only a claim
        // receipt written after this (a lazy static first read at door time
        // was later than every claim, so no seed was ever "this process's").
        _ = Self.processStart
#endif
        prefsSink = FPrefs.shared.objectWillChange.sink { [weak self] _ in
            // objectWillChange fires BEFORE the value lands, so read it next turn.
            DispatchQueue.main.async { self?.applyTableFallback() }
        }
    }

    /// Paint the host and its hosting view with the current table's flat colour.
    private func applyTableFallback() {
        let colour = Self.tableFallback
        view.backgroundColor = colour
        host?.view.backgroundColor = colour
    }

    /// THE SELECTION MOVED UNDER US - the second way a message can reach this
    /// extension, and the only one left after the host stopped using the first.
    ///
    /// THE BUG. The owner creates a lobby from the app drawer, sends it, and
    /// leaves the drawer open. Eva joins. Her bubble is plainly in the transcript
    /// and his roster never moves. His flight log records NOTHING - no `receive`,
    /// and (since 1.1(62)) no `receive-dropped` either, so it is not ours to
    /// drop. Expanding and collapsing the drawer by hand makes delivery resume,
    /// which is why a live GAME never showed it: playing a move auto-collapses,
    /// so the board is re-armed for free on every single move.
    ///
    /// WHAT IS ESTABLISHED, from disassembling Messages.framework rather than
    /// guessing. `-[_MSMessageAppContext _didReceiveMessage:conversationState:]`
    /// is a bare dispatch to main that updates the conversation and calls
    /// `didReceiveMessage:conversation:`. There is no presentation-style, session
    /// or selection check on the extension side at all (docs/
    /// IMESSAGE_LIVE_ARRIVAL_HOST.md E1, E4). So the decision not to deliver is
    /// the HOST's. Its code IS on disk in the simulator runtime
    /// (MSMessageExtensionBalloonPlugin.bundle, plus ChatKit), and the same doc
    /// reads it: exactly two host methods send the receive (M3, M4), and which
    /// of them reaches a given drawer in practice is still open (U1, U4) until
    /// phase 2 observes it live. Process suspension was my own
    /// theory and it is wrong: XPC messages to a suspended process QUEUE and
    /// deliver on resume, and that session demonstrably resumed twice - it
    /// rendered the diagnostic panel while her join was already in the
    /// transcript, and later ran `didResignActive` - with no receive ever
    /// arriving. The host withheld it.
    ///
    /// WHY THIS SIGNAL EXISTS. `-[MSConversation _updateWithState:]` sets
    /// `selectedMessage` from the host's `activeMessage` on every conversation
    /// state it pushes, and fires willSelect/didSelect when it changes. And
    /// 1.1(61) already PROVED on the owner's device that an arrival moves the
    /// selection - that discovery is the whole reason `reload-is-arrival` exists,
    /// because arrivals were turning into cold reloads onto the newly selected
    /// bubble. So if the host pushes a state for her join while withholding the
    /// receive, this fires and carries the same bytes.
    ///
    /// It is routed EXACTLY as `didReceive` routes one - same `isMine` gate, same
    /// `incomingURL`, same token, same `present` - so everything downstream (Rule
    /// P, the arrival plan, the beats, the `arrivalTaken` dedupe) applies
    /// unchanged and this cannot become a second, divergent adoption path.
    ///
    /// AND IT MUST NOT FIRE FOR A TAP. A human tapping a bubble is a COLD OPEN,
    /// which the owner ruled paints rather than animates ("If you open a lobby
    /// bubble, it should just open the state of that message with no
    /// animations"). A tap arrives as a fresh activation, so `freshlyActive`
    /// stands this down for the first selection after one and the ordinary
    /// willBecomeActive -> present path handles it as it always has. Only a
    /// selection that moves while we are ALREADY active and settled is treated as
    /// an arrival, because that is the only way it can be one.
    ///
    /// SETTLED SINCE: for the create-from-drawer flow this is INERT, and that is
    /// now a fact rather than the open question it was written as. 1.1(65)
    /// shipped it, the owner reproduced, and the log carried no `select` and no
    /// `select-dropped` - the host pushes no conversation state either, because
    /// the browser is bound to no datasource at all (see the block in
    /// `didStartSending`, which has the full call chain).
    ///
    /// KEPT ANYWAY, and not out of sentiment. Where the browser IS bound, the
    /// host moves `activeMessage` on the same replacement that fires
    /// `didReceive`, so this is a second, independent route to an arrival we
    /// would otherwise see once - and `arrivalTaken` in the surface makes a
    /// double delivery free. It costs one guarded branch, it cannot fire for a
    /// tap (`freshlyActive`), and if a future iOS pushes state without a receive
    /// it is already here.
    override func didSelect(_ message: MSMessage, conversation: MSConversation) {
        super.didSelect(message, conversation: conversation)
#if RIG_ARRIVE
        hostTrace("didSelect", message, conversation, "fresh=\(freshlyActive)")
#endif
        let payload = Self.payload(of: message)
        if freshlyActive {
            FlightRecorder.note("select", "\(payload?.count ?? -1)b on a fresh activation - not an arrival")
            return
        }
        if StagedBubbleRouting.isMine(payload, pendingStage: pendingStage?.payload,
                                      lastSentPayload: lastSentPayload) {
            FlightRecorder.note("select-dropped", "isMine")
            return
        }
        FlightRecorder.note("select", "\(payload?.count ?? -1)b - routing as an arrival")
        startingNewGame = false
        freshSession = false
        incomingURL = message.url
        incomingToken += 1
        present(conversation, style: presentationStyle)
    }

    /// When this activation began. A tap that launches or re-activates the
    /// extension brings its own selection WITH it, and that is a cold open, not
    /// an arrival - so a selection landing in the moments right after becoming
    /// active is the tap's own, and is left to the ordinary path.
    ///
    /// A timestamp rather than a flag, because the flag has nowhere honest to be
    /// cleared: `willBecomeActive` calls `present` itself, so clearing it there
    /// clears it before any selection could be weighed against it, and clearing
    /// it on a timer needs a token to survive a rapid re-activation. A window is
    /// the thing actually being expressed, so it is expressed directly.
    ///
    /// Two seconds is generous against a slow launch and far short of the
    /// minutes an arrival takes; the failing report had 153 seconds between the
    /// send and the join.
    private var becameActiveAt: Date?
    private static let activationSettles: TimeInterval = 2
    private var freshlyActive: Bool {
        guard let t = becameActiveAt else { return false }
        return Date().timeIntervalSince(t) < Self.activationSettles
    }

    override func willBecomeActive(with conversation: MSConversation) {
        super.willBecomeActive(with: conversation)
#if RIG_ARRIVE
        hostTrace("willBecomeActive", nil, conversation)
#endif
        FlightRecorder.note("active", "\(conversation.remoteParticipantIdentifiers.count + 1)p chat")
        // A FRESH ACTIVATION OWNS ITS SELECTION. See `didSelect`.
        becameActiveAt = Date()
        // A NEW ACTIVATION IS A NEW AUDIENCE. Any just-sent marker still lying
        // about belongs to a session that has ended - the player closed the
        // drawer and came back - and a reopen they chose is a request to watch
        // the bubble, not to be shown a blank board. See
        // MessageGameStore.clearJustSent for the owner report and why this sits
        // on the way IN rather than on the way out.
        if MessageGameStore.shared.clearJustSent() {
            FlightRecorder.note("quiet-drop", "a new activation replays the bubble")
        }
        present(conversation, style: presentationStyle)
#if RIG_ARRIVE
        rigWatchForArrivals()
#endif
    }

    /// The extension is going away in an orderly fashion. This is the goodbye
    /// line whose ABSENCE is how the next launch knows the previous session was
    /// killed rather than closed.
    override func didResignActive(with conversation: MSConversation) {
        super.didResignActive(with: conversation)
#if RIG_ARRIVE
        hostTrace("didResignActive", nil, conversation)
#endif
        FlightRecorder.end("resigned")
    }

    /// The one warning iOS gives before it starts killing extensions - and until
    /// round 16 this app did not implement it at all, so the warning arrived,
    /// nothing was given back, and the next allocation was fatal.
    ///
    /// Two things happen. The trail records it, which is what turns a later
    /// "ended abruptly" from a shrug into a diagnosis (FlightRecorder.verdict
    /// keys on exactly this). And the baked textures that are NOT on screen are
    /// handed back: measured at ~17.9 MB with every variant resident, of which a
    /// session only ever draws one table's worth - see
    /// FoolishTests/MemoryProfileTests, and FTextures.purgeUnusedTextures for
    /// what it costs to reload one (a file read; the reason it is safe to drop).
    override func didReceiveMemoryWarning() {
        super.didReceiveMemoryWarning()
        let scheme: ColorScheme = traitCollection.userInterfaceStyle == .dark ? .dark : .light
        let dropped = FTextures.purgeUnusedTextures(keeping: FTextures.Variant(scheme))
        FlightRecorder.note("memory-warning", "dropped \(dropped) textures")
    }

    /// A message arrived while we are on screen — an opponent may be live-playing.
    /// Rule P decides progress vs stale (never delivery order, §7.2): the arrival
    /// is threaded to the surface as `incomingURL` (it does NOT become the
    /// `selectedMessage`, so the ordinary payloadURL/loadKey path cannot see it)
    /// and GameSurface adopts it only if it strictly out-ranks what is showing.
    /// A fresh receive cancels any half-started New game.
    override func didReceive(_ message: MSMessage, conversation: MSConversation) {
#if RIG_ARRIVE
        hostTrace("didReceive", message, conversation, "door=\(rigIsDoor(message))")
#endif
        // ROUND 12 #11: an arrival that IS my own chain is not an arrival.
        // Messages delivers a sent bubble back to its sender on the simulator,
        // and to a second device on the same iCloud account for real. Threaded
        // on as new, the surface adopts it and arms the open-replay for the move
        // I just made - so the card I played vanishes under the veil and the
        // attack animates again. The live board already holds this exact chain
        // (these are the bytes it sealed), so there is nothing to fold in:
        // dropping it is not a shortcut, it is the whole of the correct action.
        // See StagedBubbleRouting.isMine.
        if StagedBubbleRouting.isMine(Self.payload(of: message),
                                      pendingStage: pendingStage?.payload,
                                      lastSentPayload: lastSentPayload) {
            // RECORDED, because this return is INVISIBLE and sits above the
            // `receive` note. The owner's first-bubble report - a join landing on
            // a lobby he had just created, with the extension open, and nothing
            // happening - produced a flight log with no `receive` in it at all,
            // and that is consistent with two completely different worlds: iOS
            // never called us, or we called it our own and dropped it here. One
            // is a host limitation with no fix inside the extension; the other is
            // our bug. A blind early return cannot tell them apart, and guessing
            // between them has already cost several builds.
            FlightRecorder.note("receive-dropped", "isMine")
            return
        }
        startingNewGame = false
        freshSession = false
        FlightRecorder.note("receive")
#if RIG_ARRIVE
        let arrived = Self.payload(of: message)
        if rigIsDoor(message), let arrived { rigDoorDelivered.insert(arrived) }
        Task { await rigSaw(arrived) }
#endif
        incomingURL = message.url
        incomingToken += 1
        present(conversation, style: presentationStyle)
    }

#if RIG_ARRIVE
    // MARK: - The rig's arrival door and host trace (RIG_ARRIVE builds only)
    //
    // COMPILED BY NOTHING THAT SHIPS. `RIG_ARRIVE` is set by `rig.sh build` when
    // FOOLISH_ARRIVE=1 and by no configuration in project.yml, so all of this is
    // absent from Debug, Release and the App Store build alike.
    //
    // TWO JOBS (docs/IMESSAGE_LIVE_ARRIVAL_HOST.md, phase 2):
    //
    // 1. THE HOST TRACE. Every callback Messages makes on this controller, with
    //    its thread, both clocks, the payload it carries, the session it names
    //    and the selection beside it - so what the host does on a live arrival
    //    is read off a real run instead of inferred from its binaries. Lines go
    //    to the unified log (subsystem cards.foolish, category host) AND the
    //    flight recorder, where they sit in order beside `receive`, `arrival`
    //    and `anim-open`.
    //
    // 2. THE DOOR. A move made by ANOTHER seat, sealed by the shipping kernel off
    //    the chain this board is showing, and delivered through REAL Messages:
    //    `conversation.send` puts it in this thread, and on the iOS 26 simulator
    //    a message sent in a thread comes back into that same thread as an
    //    INCOMING item (rig README point 2), which is path A's trigger (host doc
    //    M1, M2; M2a: no isFromMe filter). Because the door's bytes are never
    //    registered as `pendingStage` or `lastSentPayload`, `isMine` does not
    //    drop the echo, and it reaches `didReceive` exactly as a second phone's
    //    move would. `direct` delivery skips Messages and threads the bytes
    //    straight into `present()` - the old door, kept for comparison only; it
    //    proves nothing about the host (host doc N5).
    //
    // The file `dev.arrive` is one request, read and deleted every 0.4s:
    //
    //   [send|direct] [gap=MS] [session=inherit|new] ITEM[,ITEM...]
    //
    //   ITEM  join | rules | leave | start          a lobby word
    //         move:SEAT:KIND[:PICK]                 a board move
    //   SEAT  a number, or `any` (the first seat holding KIND)
    //   KIND  good | attack | throwin | cover | pickup | pass
    //   PICK  low (default) | high | a card like QS or 10H
    //
    // Items chain: each is sealed off the one before it, the first off the chain
    // the board is showing (`rigShown`). That is the defect the old door had - it
    // sealed every arrival off `lastPayloadURL`, the bubble first opened, so the
    // second and third `join` were the same bytes ("same chain") and `start`
    // could not seal ("damaged").

    private static let hostLog = Logger(subsystem: "cards.foolish", category: "host")
    private var rigArriveTimer: Timer?
    private var rigSelectTimer: Timer?
    /// The chain this board is showing, as the surface would rank it: every
    /// chain this controller sees (a routed present, a receive, a send, a seed
    /// claim, a door delivery) replaces it when Rule P prefers it, or when it is
    /// another game. Never a staged, unsent bubble - that is not the thread's.
    private var rigShown: Data?
    /// Bytes the door sent, so their send callbacks register nothing.
    /// PROCESS-WIDE, not per controller: closing the drawer resigns this
    /// controller, and a Send pressed with the drawer closed is reported to a
    /// FRESH one (phase 2, run 6), which would otherwise take the door's
    /// bubble for its own send.
    private static var rigDoorBytes: [Data] = []
    private var rigDoorBytes: [Data] {
        get { Self.rigDoorBytes }
        set { Self.rigDoorBytes = newValue }
    }
    /// Door bytes Messages has started sending (didStartSending).
    private static var rigDoorSent: Set<Data> = []
    private var rigDoorSent: Set<Data> {
        get { Self.rigDoorSent }
        set { Self.rigDoorSent = newValue }
    }
    /// Door bytes Messages has delivered back to us (didReceive). One that is
    /// delivered and not yet in `rigDoorSent` is a bubble the rig's Send press
    /// is still carrying OUT of the input field.
    private static var rigDoorDelivered: Set<Data> = []
    private var rigDoorDelivered: Set<Data> {
        get { Self.rigDoorDelivered }
        set { Self.rigDoorDelivered = newValue }
    }

    /// THE FIELD IS THE DOOR'S UNTIL ITS SEND HAS LEFT IT.
    ///
    /// The door delivers by pressing Send in THIS thread's input field, and
    /// Messages calls didReceive on the press (host doc L11) but didStartSending
    /// only about a second later (L4). A bubble the extension stages in that
    /// second - the NOTHING bubble an arrival owes (MessageTableView.
    /// restageNothingAfterArrival) is staged at once - lands in a field that is
    /// still being sent from, and Messages draws it as a zero-height entry: a
    /// divider line and a live Send arrow, no bubble. Staged two seconds later
    /// the very same bubble is the full one an Undo leaves.
    ///
    /// A move from another phone never comes with a Send press in this field
    /// (host doc N7), so that race is the door's own, and the door pays for it
    /// here: a stage waits until every delivered door bubble has started
    /// sending, so what the rig films is what a real arrival would leave.
    /// Bounded, so a send callback that never comes cannot hold a stage.
    @MainActor
    private func rigAwaitFieldFree() async {
        var waited = 0
        while !rigDoorDelivered.subtracting(rigDoorSent).isEmpty, waited < 60 {
            try? await Task.sleep(nanoseconds: 50_000_000)
            waited += 1
        }
        if waited > 0 {
            FlightRecorder.note("rig", "stage waited \(waited * 50)ms for the door's send to leave the field")
        }
    }

    private var rigLastSelected: String = "-"
    private var rigClaimSeen: String?

    /// A short, stable name for a payload: its length and an FNV-1a of its
    /// bytes (two chains can share a link's tail - the seed and roster sit
    /// there - so the tail alone named a join and the Start after it alike).
    private static func tag(_ p: Data?) -> String {
        guard let p else { return "-" }
        var h: UInt32 = 0x811c9dc5
        for b in p { h = (h ^ UInt32(b)) &* 0x01000193 }
        return "\(p.count)b:" + String(format: "%08x", h)
    }

    private static func tag(_ url: URL?) -> String {
        guard let url else { return "-" }
        return tag(try? MessageEnvelope.payloadBytes(url: url))
    }

    private static func sessionTag(_ s: MSSession?) -> String {
        guard let s else { return "-" }
        return String(format: "%08x/%llx", UInt32(truncatingIfNeeded: s.hash),
                      UInt64(UInt(bitPattern: ObjectIdentifier(s).hashValue)) & 0xffffff)
    }

    func hostTrace(_ name: String, _ message: MSMessage? = nil,
                   _ conversation: MSConversation? = nil, _ extra: String = "") {
        let conv = conversation ?? activeConversation
        let sel = conv?.selectedMessage
        // Through KVC: a message built here (the door's) has no sender yet, and
        // the Swift overlay's non-optional UUID traps on the nil.
        let sender = (message?.value(forKey: "senderParticipantIdentifier") as? UUID)?
            .uuidString.prefix(8) ?? "-"
        let local = conv?.localParticipantIdentifier.uuidString.prefix(8) ?? "-"
        let style: String
        switch presentationStyle {
        case .compact: style = "compact"
        case .expanded: style = "expanded"
        case .transcript: style = "transcript"
        @unknown default: style = "other"
        }
        let line = "\(name) thr=\(Thread.isMainThread ? "main" : "bg")"
            + " mt=\(String(format: "%.4f", CACurrentMediaTime()))"
            + " wall=\(String(format: "%.4f", Date().timeIntervalSince1970))"
            + " msg=\(Self.tag(message?.url)) sess=\(Self.sessionTag(message?.session))"
            + " sender=\(sender) local=\(local)"
            + " sel=\(Self.tag(sel?.url)) selsess=\(Self.sessionTag(sel?.session))"
            + " style=\(style) conv=\(conv.map { String(UInt(bitPattern: ObjectIdentifier($0).hashValue) & 0xffffff, radix: 16) } ?? "-")"
            + (extra.isEmpty ? "" : " " + extra)
        Self.hostLog.log("HOST \(line, privacy: .public)")
        FlightRecorder.note("host", line)
    }

    /// Fold a chain this controller has seen into `rigShown`, ranked the way
    /// the surface ranks an arrival (GameSurface.maybeAdoptIncoming).
    @MainActor
    private func rigSaw(_ p: Data?) async {
        guard let p, p != rigShown else { return }
        guard let cur = rigShown else { rigShown = p; return }
        let a = try? await MessageKernel.shared.peek(payload: cur)
        let b = try? await MessageKernel.shared.peek(payload: p)
        guard let b else { return }
        if a?.gameId != b.gameId {
            rigShown = p
            return
        }
        if let pref = try? await MessageKernel.shared.preferred(cur, p), pref > 0 { rigShown = p }
    }

    /// The seed the surface claimed in THIS process, if any (dev.claimed is
    /// written by MessageDevBoard.claimSeededPayload as the board opens).
    private func rigClaimedSeed() -> Data? {
        guard let dir = FileManager.default.containerURL(
                forSecurityApplicationGroupIdentifier: "group.cards.foolish.msg"),
              let raw = try? String(contentsOf: dir.appendingPathComponent("dev.claimed"),
                                    encoding: .utf8)
        else { return nil }
        let hex = raw.trimmingCharacters(in: .whitespacesAndNewlines)
        guard hex != rigClaimSeen else { return nil }
        guard let attrs = try? FileManager.default.attributesOfItem(
                atPath: dir.appendingPathComponent("dev.claimed").path),
              let m = attrs[.modificationDate] as? Date,
              m >= Self.processStart else { return nil }
        rigClaimSeen = hex
        var out = Data(); var i = hex.startIndex
        while i < hex.endIndex {
            let j = hex.index(i, offsetBy: 2, limitedBy: hex.endIndex) ?? hex.endIndex
            guard let b = UInt8(hex[i..<j], radix: 16) else { return nil }
            out.append(b); i = j
        }
        return out
    }
    private static let processStart = Date()

    /// One request from `dev.arrive`. See the block comment above.
    @MainActor
    private func rigArrive(_ request: String) async {
        guard let conversation = activeConversation else { return }
        var words = request.split(whereSeparator: { $0 == " " || $0 == "\n" }).map(String.init)
        var delivery = "send", gapMs = 0, session = "inherit"
        // `hold` is the rig's word (do not press Send); the door ignores it.
        while let w = words.first, w == "send" || w == "direct" || w == "hold"
                || w.hasPrefix("gap=") || w.hasPrefix("session=") {
            if w == "hold" { words.removeFirst(); continue }
            if w == "send" || w == "direct" { delivery = w }
            else if w.hasPrefix("gap=") { gapMs = Int(w.dropFirst(4)) ?? 0 }
            else { session = String(w.dropFirst(8)) }
            words.removeFirst()
        }
        let items = words.joined(separator: ",").split(separator: ",").map(String.init)
        await rigSaw(lastPayloadURL.flatMap { try? MessageEnvelope.payloadBytes(url: $0) })
        await rigSaw(rigClaimedSeed())
        guard var base = rigShown else {
            FlightRecorder.note("rig", "nothing on screen for \(request)")
            return
        }
        hostTrace("door-request", nil, conversation,
                  "delivery=\(delivery) gap=\(gapMs) session=\(session) items=\(items.joined(separator: ",")) base=\(Self.tag(base))")
        for (k, item) in items.enumerated() {
            let bytes: Data
            do {
                bytes = try await rigSeal(item, on: base)
            } catch {
                FlightRecorder.note("rig", "\(item) would not seal: \(error)")
                hostTrace("door-refused", nil, conversation, "item=\(item) error=\(error)")
                return
            }
            if k > 0, gapMs > 0 { try? await Task.sleep(nanoseconds: UInt64(gapMs) * 1_000_000) }
            await rigDeliver(bytes, item: item, delivery: delivery, session: session, conversation)
            base = bytes
            // `send` only STAGES on the simulator (it lands in the input field
            // exactly like `insert`, waiting for Send), and a second stage would
            // replace the first. So the next item waits until this one went out:
            // the rig presses Send (`rig.sh arrive` watches dev.doorstaged).
            if delivery == "send" {
                for _ in 0..<400 where !rigDoorSent.contains(bytes) {
                    try? await Task.sleep(nanoseconds: 50_000_000)
                }
                if !rigDoorSent.contains(bytes) {
                    hostTrace("door-gave-up", nil, conversation, "item=\(item) never sent")
                    return
                }
            }
        }
    }

    /// Seal one item off `base`, by the shipping kernel's own calls.
    @MainActor
    private func rigSeal(_ item: String, on base: Data) async throws -> Data {
        let env = try await MessageEnvelope.decode(payload: base, viewer: -1)
        guard let gid = UInt64(env.gameId) else { throw MessageEnvelope.Failure.damaged(code: -2) }
        let parent = MessageTurnController.firstEight(hex: env.digest)
        let seated = env.joins.sorted { $0.seat < $1.seat }
        let cast = ["Vera", "Boris", "Dima", "Eva", "Fyodor", "Galya", "Igor", "Mira"]
        switch item {
        case "join":
            let free = (0..<env.nPlayers).first { s in !seated.contains { $0.seat == s } } ?? seated.count
            let joins = (seated + [MessageJoin(seat: free, name: cast[free % cast.count])])
                .sorted { $0.seat < $1.seat }
            _ = try await MessageKernel.shared.decode(payload: base, viewer: -1)
            return try await MessageKernel.shared.seal(phase: 0, lastActorSeat: free, gameId: gid,
                                                      parent8: parent, joins: joins)
        case "leave":
            let after = Array(seated.dropLast())
            guard let last = seated.last, !after.isEmpty else { throw MessageEnvelope.Failure.damaged(code: -3) }
            _ = try await MessageKernel.shared.decode(payload: base, viewer: -1)
            return try await MessageKernel.shared.seal(phase: 0, lastActorSeat: last.seat, gameId: gid,
                                                      parent8: parent, joins: after)
        case "rules":
            return try await MessageKernel.shared.resealLobby(
                base, passing: !env.passingAllowed, actingSeat: seated.last?.seat ?? 0,
                gameId: gid, parent8: parent, joins: seated)
        case "start":
            return try await MessageKernel.shared.startFromLobby(
                lobbyPayload: base, gameId: gid, actingSeat: seated.last?.seat ?? 0,
                parent8: parent, joins: seated)
        default:
            let parts = item.split(separator: ":").map(String.init)
            guard parts.count >= 3, parts[0] == "move" else {
                throw MessageEnvelope.Failure.damaged(code: -4)
            }
            return try await MessageKernel.shared.rigMove(
                base: base, seat: parts[1], kind: parts[2], pick: parts.count > 3 ? parts[3] : "low",
                gameId: gid, parent8: parent, joins: seated)
        }
    }

    /// Put sealed bytes in front of this extension, the way `delivery` says.
    @MainActor
    private func rigDeliver(_ bytes: Data, item: String, delivery: String, session: String,
                            _ conversation: MSConversation) async {
        FlightRecorder.note("rig", "\(item) arrives by \(delivery), \(bytes.count)b")
        if delivery == "direct" {
            hostTrace("door-direct", nil, conversation, "item=\(item) bytes=\(Self.tag(bytes))")
            await rigSaw(bytes)
            startingNewGame = false
            freshSession = false
            FlightRecorder.note("receive")
            incomingURL = MessageEnvelope.link(payload: bytes)
            incomingToken += 1
            present(conversation, style: presentationStyle)
            return
        }
        // THE REAL ROUTE: a bubble like any other, sent into this thread.
        let (env, publicView, summary) = await MessageSummary.forStagedBubble(payload: bytes, leftName: nil)
        let scheme: ColorScheme = traitCollection.userInterfaceStyle == .dark ? .dark : .light
        var image: UIImage?
        if let env { image = BubbleSnapshot.render(env: env, publicView: publicView, scheme: scheme) }
        let msg = MessageComposer.message(
            url: MessageEnvelope.link(payload: bytes), snapshot: image,
            caption: MessageSummary.caption(env: env, view: publicView), summary: summary,
            session: session == "new" ? nil : conversation.selectedMessage?.session)
        rigDoorBytes.append(bytes)
        hostTrace("door-send", msg, conversation, "item=\(item) bytes=\(Self.tag(bytes))")
        conversation.send(msg) { [weak self] error in
            DispatchQueue.main.async {
                self?.hostTrace("door-send-done", msg, conversation,
                                "item=\(item) error=\(error.map { "\($0)" } ?? "none")")
                // The rig's cue to press Send: one line per staged door bubble.
                if let dir = FileManager.default.containerURL(
                    forSecurityApplicationGroupIdentifier: "group.cards.foolish.msg") {
                    let f = dir.appendingPathComponent("dev.doorstaged")
                    let line = Data((item + "\n").utf8)
                    if let h = try? FileHandle(forWritingTo: f) {
                        h.seekToEndOfFile(); h.write(line); try? h.close()
                    } else {
                        try? line.write(to: f)
                    }
                }
            }
        }
    }

    /// Is this a door send coming back through a send callback?
    private func rigIsDoor(_ message: MSMessage) -> Bool {
        guard let p = Self.payload(of: message) else { return false }
        return rigDoorBytes.contains(p)
    }

    private func rigWatchForArrivals() {
        if rigSelectTimer == nil {
            // THE SELECTION, WATCHED. `selectedMessage` moves on host-pushed
            // state (host doc E6), with or without a callback; 20 Hz is enough
            // to say whether and roughly when.
            rigSelectTimer = Timer.scheduledTimer(withTimeInterval: 0.05, repeats: true) { [weak self] _ in
                guard let self else { return }
                let now = Self.tag(self.activeConversation?.selectedMessage?.url)
                if now != self.rigLastSelected {
                    self.rigLastSelected = now
                    self.hostTrace("selection-moved")
                }
            }
        }
        guard rigArriveTimer == nil else { return }
        rigArriveTimer = Timer.scheduledTimer(withTimeInterval: 0.4, repeats: true) { [weak self] _ in
            guard let self,
                  let dir = FileManager.default.containerURL(
                      forSecurityApplicationGroupIdentifier: "group.cards.foolish.msg") else { return }
            let file = dir.appendingPathComponent("dev.arrive")
            guard let kind = try? String(contentsOf: file, encoding: .utf8) else { return }
            try? FileManager.default.removeItem(at: file)
            Task { await self.rigArrive(kind.trimmingCharacters(in: .whitespacesAndNewlines)) }
        }
    }

    override func willSelect(_ message: MSMessage, conversation: MSConversation) {
        hostTrace("willSelect", message, conversation)
        super.willSelect(message, conversation: conversation)
    }

    override func didBecomeActive(with conversation: MSConversation) {
        hostTrace("didBecomeActive", nil, conversation)
        super.didBecomeActive(with: conversation)
    }

    override func willResignActive(with conversation: MSConversation) {
        hostTrace("willResignActive", nil, conversation)
        super.willResignActive(with: conversation)
    }
#endif

    /// The user tapped Send on our staged bubble: our chain is now the thread's,
    /// so commit it to the cache (§7.6). This is the ONLY place the cache learns
    /// a chain was actually sent — insert alone is not a commit.
    override func didStartSending(_ message: MSMessage, conversation: MSConversation) {
#if RIG_ARRIVE
        hostTrace("didStartSending", message, conversation, "door=\(rigIsDoor(message))")
        // THE DOOR'S OWN SEND REGISTERS NOTHING: it is another seat's move, and
        // recording it as mine (lastSentPayload) is exactly what would make
        // `isMine` drop its echo. See the door block above.
        if rigIsDoor(message) {
            if let p = Self.payload(of: message) { rigDoorSent.insert(p) }
            return
        }
        let sentBytes = Self.payload(of: message)
        Task { await rigSaw(sentBytes) }
#endif
        // Captured BEFORE the `present` below, which rewrites `lastPayloadURL`.
        // The dismiss itself happens at the END of this method, once the send is
        // fully recorded - see the block there for why it happens at all.
        let drawerIsUnbound = lastPayloadURL == nil
        startingNewGame = false
        freshSession = false
        // ROUND 12 #11: the chain being sent comes from the MESSAGE Messages
        // hands us, not from our own `pendingStage` bookkeeping. They are
        // normally the same bytes, but `pendingStage` can be gone by now (an
        // extension torn down between insert and send; a cancel report for a
        // bubble we already replaced), and when it is, the quiet-open marker
        // below was never written - so reopening my own sent chain replayed the
        // move I had just watched. The message is always authoritative.
        let sent = Self.payload(of: message) ?? pendingStage?.payload
        // WITH THE BYTE COUNT, because a send that reached this function
        // without its bytes is what walked the board back a bubble in 1.0(26)
        // (MessageTurnController.markSent). The board no longer depends on them
        // arriving, but WHERE they go missing - never here, or lost between
        // here and the surface's `onChange` - is still worth knowing, and the
        // trail can only say so if this line counts them.
        FlightRecorder.note("send", sent.map { "\($0.count)b" } ?? "NO PAYLOAD")
        lastSentPayload = sent
        commitPendingStage(sent: sent, chatKey: ChatKey.make(
            local: conversation.localParticipantIdentifier.uuidString,
            remotes: conversation.remoteParticipantIdentifiers.map(\.uuidString)))
        // Round-6 bug 4: signal the live board that its staged move is now sent, so
        // it drops it from `pending` and the collapsed drawer's Undo button (which
        // could otherwise re-stage and re-send the same move) goes away. Bump the
        // token and re-present so the new value reaches MessagesRootView;
        // StagedBubbleRouting keeps the payloadURL (hence loadKey) stable off
        // `lastSentPayload`, so the board is SIGNALLED, not torn down and reloaded.
        sentToken += 1
        present(conversation, style: presentationStyle)
        // ROUND 16 (owner): "if it's collapsed, then sending shouldn't close the
        // extension and go to the keyboard. Just keep it collapsed so they can
        // keep playing without having to tap and reopen anything."
        //
        // Round-6 bug 5 asked for the opposite ("sending should completely
        // close, if possible") and `dismiss()` is the closest Messages offers -
        // it tears the extension down and returns to the transcript, which is
        // also what raises the keyboard. That was the right answer for a send
        // made from the EXPANDED board, where the drawer is covering the thread
        // and the human is done. It is the wrong one from the compact drawer:
        // the ordinary move flow already ends there (a staged move collapses to
        // reach Messages' Send), so every send was closing a strip the player
        // was still using and charging them a tap to get back into the game.
        //
        // So the close is now scoped to the style it was asked for. Nothing
        // else about a send changes: `sentToken` above already tells the live
        // board its move is in the thread (markSent -> `canSend` false), which
        // is what clears the Undo button and the send hint - written as
        // belt-and-braces for the case where Messages kept the drawer open
        // anyway, and now the case that always happens.
        if presentationStyle != .compact {
            dismiss()
        } else if drawerIsUnbound {
            // THE FIRST BUBBLE OF A GAME CLOSES THE DRAWER, AND ONLY THAT ONE.
            //
            // This is a concession, it is the owner's, and it is made against his own
            // standing preference ("We want to keep it open as much as possible", and
            // round 16's decision to keep it open at all). It is here because the
            // alternative is a lobby that can never update, and the whole of that
            // reasoning is written down below so nobody removes this line on the
            // reasonable-sounding grounds that it looks like a papercut.
            //
            // ── WHAT BREAKS ──────────────────────────────────────────────────────
            // `+ > Foolish > New game > create > Send`, drawer left open. The other
            // player joins. Their bubble is visibly in the transcript and this
            // extension is never told: no `didReceive`, and (1.1(65) added the
            // override to check) no `didSelect` either. Nothing arrives at all.
            //
            // ── WHY, FROM THE HOST BINARIES ──────────────────────────────────────
            // Read out of the iOS 26.3 simulator runtime's
            // MSMessageExtensionBalloonPlugin.bundle, ChatKit and
            // iMessageApps.framework, and confirmed against the DEVICE's own ChatKit
            // (iOS DeviceSupport/iPhone16,2 26.5.2) - not inferred from behaviour.
            //
            // Delivery is not a broadcast. The host's browser view controller holds a
            // MESSAGE DATASOURCE, and that binding IS the wire: when a message
            // arrives, the host replaces the payload inside the datasource an
            // extension is bound to, and the replacement is what fires didReceive.
            // Exactly two host paths send `_didReceiveMessage:conversationState:`:
            //
            //   PATH A, not observed to deliver; whether it can is OPEN:
            //     -[CKChatController _handleChatItemDidChange:]
            //       -> -[CKChatInputController
            //           notifyBrowserViewControllerOfMatchingNewMessages:]
            //          requires browserSwitcher.currentViewController to be us
            //          (docs/IMESSAGE_LIVE_ARRIVAL_HOST.md M1, M2).
            //     An earlier reading said `currentViewController` is restored only
            //     through -[CKBrowserTransitionCoordinator setExpanded:withReason:],
            //     which has no call site, so this path is dead. The binary does not
            //     support that: -[CKBrowserSwitcherViewController
            //     _loadBrowserForBalloonPlugin:datasource:] and
            //     _updateVisibleBrowserView set it too (M2c). Our own echo not
            //     arriving in the owner's logs is the observation; the reason is
            //     U1, for phase 2 to settle on the simulator.
            //
            //   PATH B, the live one (M4, M4b):
            //     -[MSMessageExtensionBrowserViewController setBalloonPluginDataSource:]
            //       sets dataSource.delegate = self
            //     -> a same-MSSession message replaces that datasource's payload
            //     -> datasourcePayloadDidChange:updateFlags:  (flags & 0x13)
            //     -> _didReceiveMessage:, activeMessage = the new message.
            //
            // A DRAWER LAUNCH BINDS NOTHING: the browser is created as
            // viewControllerForPluginIdentifier:dataSource:nil - compose mode, not
            // viewing-a-thread mode - and SENDING does not bind it either
            // (didStartSendingPluginPayload: forwards _didStartSendingMessage: and
            // releases the load request, and that is all). So the extension sits
            // running, visible, and the delegate of no datasource. There is nobody
            // for the host to notify. It is not policy; there is no wire.
            //
            // The only host paths that bind a datasource to an ALREADY-PRESENTED card
            // are showBrowserForPlugin:dataSource:style: -> overseer
            // updateCurrentBrowserWithDataSource:, reached from
            // transcriptCollectionViewController:balloonView:tappedForChatItem: -
            // i.e. A HUMAN TAPPING ONE OF OUR BUBBLES - or a transcript live view's
            // own request. That is why a live GAME never shows this bug: you always
            // arrive by tapping, so you are bound before the first move.
            //
            // ── WHY WE CANNOT ASK FOR IT ─────────────────────────────────────────
            // No public extension->host call binds a datasource. Not `insert`, `send`,
            // `dismiss`, `extensionContext.open`, and not requestPresentationStyle at
            // ANY style. 1.1(66)/(67) tried the same-style request on the theory that
            // the host would do its re-presentation bookkeeping with nothing to
            // animate; it does not:
            //     -[MSMessageExtensionBrowserViewController _requestPresentationStyle:]
            //       -> main-queue block
            //       -> -[CKChatInputController requestPresentationStyleExpanded:forPlugin:]
            //       -> performSelector:afterDelay: _deferredRequestPresentationStyleExpanded:
            //       -> CKAppCardPresentationOverseer.requestPresentationStyle(_:animated:)
            //     which compares the requested detent with
            //     sheetPresentationController.selectedDetentIdentifier and logs
            //     "App requested a presentation style change but is already in that
            //      state. Doing nothing."
            // That string is in the device's own ChatKit. The call is removed here.
            //
            // A detent DRAG does not bind either - notifyBrowserOfTransitionStarting/
            // Ending only sends viewWill/DidTransitionTo…Presentation. An earlier
            // reading of the owner's logs concluded "a presentation transition
            // re-arms delivery"; that was wrong. What re-armed his working run was
            // TAPPING HIS OWN LOBBY BUBBLE while the card was open, which reuses the
            // same browser (hence no resign/activate pair in the log) and binds the
            // datasource. The `style expanded` line was the consequence of that tap,
            // not its cause.
            //
            // ── SO: DISMISS, AND ONLY HERE ───────────────────────────────────────
            // Closing the drawer puts the human back in the transcript, where their
            // next natural action - tapping the bubble - is precisely the thing that
            // binds the datasource. Every message after that arrives normally, which
            // satisfies "create a game and finish it without closing the drawer"
            // from that one tap onward.
            //
            // `lastPayloadURL == nil` is "nothing is open", i.e. this is a drawer
            // launch that has never opened a bubble - the create flow and nothing
            // else. It is the value the diagnostic panel prints as
            // `Last message: no message open`, so the scope can be confirmed from a
            // screenshot without reading code, and it is true in every failing log
            // the owner captured and false in every working one. Owner: "ONLY the
            // first send, DO NOT DISMISS AUTOMATICALLY AFTER ANY OTHER SEND other
            // than the create game bubble."
            //
            // NOT scoped on `startingNewGame`, which is the obvious choice and is
            // wrong: that flag is only set when a human taps the New game BUTTON, and
            // a drawer launch into a thread with no game routes straight to setup, so
            // nobody calls onNewGame and it is false for the whole create flow. 66
            // was scoped on it and never fired once.
            FlightRecorder.note("dismiss", "first bubble of a game - the drawer cannot be bound")
            dismiss()
        }
    }

    /// The user deleted the staged bubble before sending: drop the pending record
    /// so the cache never claims a chain nobody will see (§17.2). ROUND 9: the
    /// durable pending ledger this used to clear is gone entirely (owner call).
    override func didCancelSending(_ message: MSMessage, conversation: MSConversation) {
#if RIG_ARRIVE
        hostTrace("didCancelSending", message, conversation, "door=\(rigIsDoor(message))")
        if rigIsDoor(message) { return }
#endif
        // ROUND 12 #11: clear the staging only if THIS is the bubble that was
        // cancelled. Staging a second move (a throw-in, a re-stage after Undo)
        // replaces the input-field bubble, and Messages reports the replaced one
        // as cancelled - after we have already recorded its successor. Clearing
        // unconditionally threw away a live staging, and with it the quiet-open
        // marker its send would have written.
        if let p = pendingStage,
           let cancelled = Self.payload(of: message), cancelled != p.payload { return }
        // OBSERVED, not assumed. "The callback does not fire" was the first
        // theory for the hint that would not go out, and it was wrong - this
        // line is what says so, in a trail that survives the extension being
        // torn down. `selectedMessage` comes with it because whether Messages
        // leaves the deleted bubble selected is what decides if the surface
        // reloads out from under the cancel.
        FlightRecorder.note("cancel", "sel "
            + (conversation.selectedMessage == nil ? "nil" : "set"))
        // Round-9: tell the surface nothing awaits Send any more, so the send
        // reminder (which now also covers lobby join/invite/start bubbles)
        // doesn't keep pointing at a bubble the human just deleted.
        // 1.0(37): …and the BOARD's half of that reminder is
        // `controller.canSend`, which this token now also reaches
        // (MessageTableView.cancelStagedBubble). Clearing only the surface's
        // flag is why the arrow survived a cancel over a game board.
        cancelToken += 1
        // THE PIN IS STILL NEEDED FOR THIS ONE PRESENT, which is why
        // `pendingStage` is cleared BELOW it rather than above (1.1(68)).
        // `conversation.insert` made the staged bubble the SELECTION, and a
        // cancel does not always take that back - so a present with the pin
        // already dropped routes the surface at the cancelled bubble's own URL,
        // which moves `loadKey`, which reloads the surface onto the very chain
        // the human just discarded. That is the opposite of an undo, and it
        // would land on top of `revertStagedSurface` with no way to tell which
        // won. `StagedBubbleRouting` recognises the bytes as mine and keeps
        // `lastPayloadURL`, so the surface is left alone and the revert is the
        // only thing that moves it.
        present(conversation, style: presentationStyle)
        pendingStage = nil
    }

    override func willTransition(to presentationStyle: MSMessagesAppPresentationStyle) {
        super.willTransition(to: presentationStyle)
#if RIG_ARRIVE
        hostTrace("willTransition", nil, nil,
                  "to=\(presentationStyle == .compact ? "compact" : "expanded")")
#endif
        // ROUND 47: the FIRST thing, because on a cold open this callback is
        // the earliest moment Messages will honour an `.expanded` request - see
        // c/src/msg_expand.h for the eight-run measurement.
        nameExpandSaw(presentationStyle)
        // ROUND 30: the sheet is about to MOVE. An open replay started now spends
        // its first beat behind the edge of the screen - see
        // `CollapseTween.isPresenting`, which the board waits on.
        CollapseTween.isPresenting = true
        // Round-7 #5 ("when we auto-collapse it should be the same as if we swiped
        // to collapse"; "it rearranges the display right before the auto collapse").
        // A plain compact<->expanded toggle must NOT re-present: the board is laid
        // out purely from the HEIGHT Messages gives its GeometryReader (a continuous
        // `collapseFraction`), never from the `style` prop, so it follows the drawer
        // resize smoothly on its own. Re-presenting here swapped the entire hosting
        // rootView at the START of the transition - that swap is the "display
        // rearranges right before the collapse" jump, and it is what made the
        // auto-collapse look different from a manual grabber swipe (a swipe the
        // human drives frame-by-frame hit the same swap but masked it under the
        // drag). Now BOTH just resize.
        //
        // The ONE case that still needs a routed present is New game: it flips
        // `startingNewGame` and requests .expanded, and the incoming transition is
        // where that intent has to be rendered (reading `self.presentationStyle`
        // mid-transition would still report the old style). So present only then.
        if startingNewGame, let c = activeConversation {
            present(c, style: presentationStyle)
        }
    }

    /// Round-10b: continuations parked by `awaitTransitionSettled`, resumed
    /// when `didTransition` reports the style change has completed.
    private var transitionWaiters: [Int: CheckedContinuation<Void, Never>] = [:]
    private var transitionWaiterSeq = 0

    override func didTransition(to presentationStyle: MSMessagesAppPresentationStyle) {
        super.didTransition(to: presentationStyle)
#if RIG_ARRIVE
        hostTrace("didTransition")
#endif
        nameExpandSaw(presentationStyle)
        CollapseTween.isPresenting = false
        FlightRecorder.note("style", presentationStyle == .compact ? "compact" : "expanded")
        let waiters = transitionWaiters
        transitionWaiters.removeAll()
        // In KEY order, which is why the key is a monotonic sequence number.
        // `waiters.values` walks the dictionary, and Swift seeds Dictionary
        // hashing per process - so the waiters woke in an order that had nothing
        // to do with the order they parked in, and which send or collapse path
        // resumed first changed from launch to launch.
        for seq in waiters.keys.sorted() { waiters[seq]?.resume() }
    }

    /// Wait until the in-flight presentation-style transition finishes
    /// (didTransition), or a timeout if none ever fires - the caller must
    /// never hang on a transition Messages decided not to run.
    @MainActor
    private func awaitTransitionSettled(timeoutNs: UInt64 = 1_200_000_000) async {
        transitionWaiterSeq += 1
        let id = transitionWaiterSeq
        await withCheckedContinuation { (c: CheckedContinuation<Void, Never>) in
            transitionWaiters[id] = c
            Task { @MainActor [weak self] in
                try? await Task.sleep(nanoseconds: timeoutNs)
                if let waiter = self?.transitionWaiters.removeValue(forKey: id) {
                    waiter.resume()
                }
            }
        }
    }

    /// ROUND 47: the drawer a name screen asks for, and the ONE retry that
    /// makes Messages honour it.
    ///
    /// Threaded down to FoolishKit as `requestExpand`. `MessagesRootView`
    /// decides WHETHER to ask (only when a name is owed - `needsNameEntry`);
    /// the KERNEL decides WHEN the ask is issued, because on a cold open the
    /// host discards a request made before it has installed our view in the
    /// drawer. c/src/msg_expand.h carries the flight log that measured it and
    /// c/tests/msg_expand_test.c pins it; this holds the state and performs the
    /// effect, which is all a host does.
    private var nameExpand = GateWire.NameEntryExpand()

    private func requestNameEntryExpand() {
        // Already open: requesting `.expanded` while expanded fires no
        // transition, so there would be nothing to answer the retry either.
        guard presentationStyle != .expanded else { return }
        if nameExpand.note(.wanted, now: CACurrentMediaTime()) { requestPresentationStyle(.expanded) }
    }

    /// Every style callback, both phases. The compact one is the retry that
    /// lands; the expanded one is what stops us asking.
    private func nameExpandSaw(_ style: MSMessagesAppPresentationStyle) {
        if nameExpand.note(.transition(toCompact: style == .compact), now: CACurrentMediaTime()) {
            requestPresentationStyle(.expanded)
        }
    }

    // MARK: - Presentation

    private func present(_ conversation: MSConversation, style: MSMessagesAppPresentationStyle) {
        let selected = conversation.selectedMessage
        // note 11: `conversation.insert` (in `stage`, below) makes the
        // just-staged bubble `selectedMessage`; the auto-collapse that
        // follows fires `willTransition` -> here with THAT bubble now
        // selected. Route it through `StagedBubbleRouting` so a match against
        // `pendingStage` reuses `lastPayloadURL` instead of tearing down the
        // live board to "adopt" the move it just watched itself play — see
        // that type's doc for the full chain.
        let route = StagedBubbleRouting.route(
            selectedURL: selected?.url, startingNewGame: startingNewGame,
            pendingStage: pendingStage.map { (payload: $0.payload, mySeat: $0.mySeat) },
            lastPayloadURL: lastPayloadURL,
            lastSentPayload: lastSentPayload)
        let payloadURL = route.url
        // 1.0(37): THE MARKERS DIE WITH THE BOARD THEY PIN. Both exist to keep
        // ONE chain on screen when my own bubble becomes the selection; once
        // this surface is presenting something else they are pinning a board
        // that is gone, and the next tap back onto the pinned bubble would be
        // handed the URL of the game we left. See StagedBubbleRouting.Route.
        if route.clearMarkers, lastSentPayload != nil || pendingStage != nil {
            FlightRecorder.note("markers-spent",
                "presentation moved - dropping the staged/just-sent pins")
            lastSentPayload = nil
            pendingStage = nil
        }
        lastPayloadURL = payloadURL
#if RIG_ARRIVE
        let routed = payloadURL.flatMap { try? MessageEnvelope.payloadBytes(url: $0) }
        Task { await rigSaw(routed) }
#endif
        // §6.2 S1's exact half: did THIS device send the tapped bubble? Only the
        // extension can answer — the participant UUIDs never travel in the payload.
        let senderIsLocal = selected?.senderParticipantIdentifier != nil
            && selected?.senderParticipantIdentifier == conversation.localParticipantIdentifier

        // Chat shape (B4 feedback): a 1:1 DM can only ever be a 2-player game; a
        // group chat defaults to its participant count but still allows 2-8. Total
        // participants = remote + me, clamped to the wire's 2-8.
        let participants = min(max(conversation.remoteParticipantIdentifiers.count + 1, 2), 8)
        let isDM = conversation.remoteParticipantIdentifiers.count <= 1

        // The chat-scoping security fix: scope every MessageGameStore lookup to
        // THIS conversation, so chat B can never reopen chat A's board (and never
        // stage chat A's deal-seed-bearing payload into it). Keyed on the whole
        // PARTICIPANT SET, not `localParticipantIdentifier` alone — that one is
        // the same UUID in every thread on a device, so it scoped by device and
        // the leak survived. See ChatKey for the full reasoning.
        let chatKey = ChatKey.make(local: conversation.localParticipantIdentifier.uuidString,
                                   remotes: conversation.remoteParticipantIdentifiers.map(\.uuidString))

        let root = MessagesRootView(
            payloadURL: payloadURL,
            senderIsLocal: senderIsLocal,
            startNewGame: startingNewGame,
            newGameToken: newGameToken,
            sentToken: sentToken,
            // ROUND 16: the chain that went out, so the live controller can
            // rebase onto it. It used to be rebuilt from these same bytes by the
            // teardown a send caused; the drawer survives now, so the bytes have
            // to travel instead of the teardown.
            sentPayload: lastSentPayload,
            chatKey: chatKey,
            chatIsDM: isDM,
            chatPlayers: participants,
            incomingURL: incomingURL,
            incomingToken: incomingToken,
            cancelToken: cancelToken,
            collapseSignal: collapseSignal,
            requestExpand: { [weak self] in self?.requestNameEntryExpand() },
            slideCollapse: { [weak self] travel, duration, response in
                self?.slideCollapse(travel: travel, duration: duration,
                                    response: response)
            },
            endSlide: { [weak self] in self?.endCollapseSlide() },
            // A LIVE read, deliberately: the name-field autofocus asks this at
            // the moment a resize lands, and the answer has to be where the
            // sheet is THEN. See NameFieldAutofocus.
            hostIsExpanded: { [weak self] in self?.presentationStyle == .expanded },
            onNewGame: { [weak self] in
                guard let self else { return }
                self.startingNewGame = true
                self.freshSession = true
                self.newGameToken += 1
                // If already expanded, requesting .expanded fires no transition, so
                // present now; otherwise expand and let willTransition present with
                // startNewGame set.
                if self.presentationStyle == .expanded {
                    if let c = self.activeConversation { self.present(c, style: .expanded) }
                } else {
                    self.requestPresentationStyle(.expanded)
                }
            },
            // A REMATCH starts a new game without a teardown: it builds its
            // lobby in place from the finished board. All it needs from here is
            // the session half of `onNewGame` - a FRESH MSSession, so the
            // rematch's first bubble does not collapse the result card of the
            // game it grew out of (see the session note in `stage`). Cleared by
            // didStartSending/didReceive, exactly like the New game tap's.
            onFreshChain: { [weak self] in self?.freshSession = true },
            // Round 16: who just walked out of the lobby. Only the leaver's own
            // device knows - the join that carried the name is what the leave
            // removed - so it says so here, and `stage` puts it in the
            // transcript line before clearing it.
            onAnnounceLeave: { [weak self] name in self?.pendingLeftName = name },
            onSend: { [weak self] payload, mySeat, fromUndo in
                await self?.stage(payload: payload, mySeat: mySeat, fromUndo: fromUndo)
            },
            onUnstage: { [weak self] in
                // Messages provides no API to remove an already-inserted input-field
                // bubble — the human deletes it manually, or the next stage() call
                // replaces it. All we can retract is our own bookkeeping, so a
                // resumed undo-to-empty doesn't later commit a stale chain on send.
                // (ROUND 9: the durable ledger this also used to clear is gone.)
                self?.pendingStage = nil
            },
            // The result screen's Replay Link. `extensionContext.open` is the
            // ONLY way out of an iMessage extension - there is no
            // `UIApplication.shared` to ask (this target is built
            // extension-API-only, so reaching for one would not even compile),
            // and SwiftUI's `openURL` has no host to fall back on here. The
            // extension is torn down as Safari comes up, which is why the code
            // behind the link is captured when the game ends rather than read
            // on the way out (MessageTurnController.publish).
            //
            // ROUND 20 stopped throwing the ANSWER away. The completion handler
            // was `nil`, so a system that declined to open the URL - which is
            // what iOS does with an arbitrary https link from an extension, see
            // FGameOverList.onOpenURL - was indistinguishable from one that
            // opened it, and the tap did nothing with nothing to say. It is
            // reported up now, and the board falls back to the pasteboard.
            onOpenURL: { [weak self] url in
                FlightRecorder.note("open-url", url.host ?? "?")
                guard let ctx = self?.extensionContext else { return false }
                return await withCheckedContinuation { k in
                    ctx.open(url) { ok in k.resume(returning: ok) }
                }
            })
        setRoot(root)
    }

    /// Compose the staged bubble and insert it into the input field (§11.3/§11.4).
    /// The picture is the PUBLIC table (§10) rendered from the resident game the
    /// seal just left in place. Insert only STAGES — the human sends.
    @MainActor
    private func stage(payload: Data, mySeat: Int, fromUndo: Bool = false) async {
        guard let conversation = activeConversation else { return }
        // UNDO STAYS OUT OF SIGHT FOR ALL OF WHAT FOLLOWS - the picture being
        // baked, the rest, the collapse (CollapseTween.autoCollapses, read by
        // UndoGate). From the first line, and released on every way out.
        let collapsing = !fromUndo && presentationStyle == .expanded
        if collapsing { CollapseTween.autoCollapses += 1 }
        defer { if collapsing { CollapseTween.autoCollapses -= 1 } }
        // NEWEST STAGE WINS, and the losers stop where they stand.
        //
        // This function is re-entrant and its expanded tail is over a second
        // long (a settle wait, a collapse, a transition wait), while a second
        // move - a throw-in, a re-stage after Undo, a fast double tap - starts
        // another one immediately. Nothing serialised them, so two runs raced to
        // `conversation.insert`, and the input field kept whichever landed LAST:
        // routinely the older bubble, because a run that starts while the drawer
        // is already collapsing skips the whole tail and inserts at once. That
        // is not a flicker - `pendingStage` and the inserted message are the
        // bytes Send actually transmits, so the move the player watched
        // themselves make would not be the move that went out.
        //
        // A generation counter, checked after every suspension: the run that
        // has been superseded neither records itself nor inserts.
        stageGeneration += 1
        let generation = stageGeneration
        func current() -> Bool { stageGeneration == generation }
        // READ the bubble and describe it, in one kit call (MessageSummary.
        // forStagedBubble): the read must not ADOPT - see there - and keeping
        // it beside the caption is what lets a test walk this exact path. The
        // leave name is this device's alone (round 16), and is spent here.
        let (env, publicView, summary) = await MessageSummary.forStagedBubble(
            payload: payload, leftName: pendingLeftName)
        // Spent only by the bubble that can say it - a lobby re-seal. Any other
        // bubble leaves it standing for the one that follows.
        if env?.phase == 0 { pendingLeftName = nil }
        // The picture is BubbleSnapshot's call, not this file's: a WAITING lobby
        // previews as its roster, everything else as the public table. Shared
        // with the harness's transcript so a preview can never disagree with the
        // extension (see BubbleSnapshot.render(env:)).
        // Round-7 #3: bake the bubble in THIS device's scheme so a dark-mode
        // sender's bubble is dark. traitCollection is the extension's live
        // appearance; map it onto SwiftUI's ColorScheme for BubbleSnapshot.
        let scheme: ColorScheme = traitCollection.userInterfaceStyle == .dark ? .dark : .light
        var image: UIImage?
        if let env { image = BubbleSnapshot.render(env: env, publicView: publicView, scheme: scheme) }

        // §12, revised by batch 6 item B: the FINISHED bubble stays a normal /m/
        // payload link, NOT `MessageEnvelope.replayLink`'s bare foolish.cards/<code>.
        // That bare link is unparseable by `MessageEnvelope.payloadBytes` (it has
        // no `/m/1<base32>` shape), so the RECEIVER of the final move tapped it
        // into the damaged-link screen and never saw the final board or its
        // animation (batch-3 finding). The replay funnel moves one hop out
        // instead: the web `/m/` page (src/app/m/[payload]/page.tsx) decodes the
        // FINISHED payload itself — it already runs the same kernel — and derives
        // the replay code THERE, rendering its own "Watch the replay" CTA
        // alongside the install/play ones. This bubble only needs the fool
        // announcement — `residentReplayCode()`/`replayLink` (sdk/swift/
        // MessageEnvelope.swift) still exist and are still exercised by
        // MessageTurnControllerTests (the underlying kernel capability the web
        // page's replay derivation mirrors), just no longer called from here.
        let url = MessageEnvelope.link(payload: payload)

        // §11.3/note 21: ONE session per game, and a NEW game must never collapse
        // the PREVIOUS game's final bubble. Messages collapses every older bubble
        // in the same `MSSession` down to its summaryText, keeping only the
        // latest interactive — which is exactly what we want WITHIN one game
        // (so the thread doesn't fill with 60 bubbles), but is wrong across two:
        // reusing `selectedMessage?.session` for the first bubble of a brand-new
        // game folds the just-finished game's result card into it, so the fool
        // announcement vanishes from the transcript the instant the next game
        // starts. `startingNewGame` is exactly "is this the first bubble of a
        // game that didn't exist a moment ago" — it is set on the New game tap
        // and only cleared by didReceive/didStartSending (see the property doc
        // above) — so passing `session: nil` there starts Messages a FRESH
        // session/bubble; every continuation after that (startingNewGame already
        // false) still reuses `selectedMessage?.session` to collapse within the
        // SAME game, unchanged.
        // The caption row is the TABLE's state, not the app's name - see
        // MessageSummary.caption. Same (env, view) the picture and the summary
        // above were made from, so the three cannot disagree.
        let msg = MessageComposer.message(
            url: url,
            snapshot: image,
            caption: MessageSummary.caption(env: env, view: publicView),
            summary: summary,
            session: freshSession ? nil : conversation.selectedMessage?.session)

        // gameId comes from the same decode above so didStartSending's commit
        // can persist the seat without re-decoding. "" only if the payload
        // failed to decode - the commit then skips the seat write.
#if RIG_ARRIVE
        await rigAwaitFieldFree()
#endif
        // Superseded while the picture was being baked: a newer move is already
        // staged, and this one must not claim the input field back off it.
        guard current() else { return }
        pendingStage = (payload, mySeat, env?.gameId ?? "")

        // An UNDO re-stages only to refresh the input bubble - it is NOT a move the
        // player is trying to send, it is them backing up to pick a DIFFERENT move.
        // Collapsing the board out from under them there is exactly wrong (owner:
        // "undo should NOT collapse the screen... best to keep it expanded for
        // moves"), so insert now, stay expanded, and skip the drop-to-Send tail.
        // Every insert goes through here, so a receipt cannot be forgotten by a
        // path added later (DEBUG only - see MessageDevBoard.noteStaged).
        func insertStaged() {
            conversation.insert(msg) { _ in }
            #if DEBUG || SOLO_TESTING
            MessageDevBoard.noteStaged(payload)
            #endif
        }

        if fromUndo { insertStaged(); return }

        // Already in the compact drawer (an ordinary in-drawer move): no style
        // transition will run, so there is no preview flyover to avoid - stage
        // the bubble immediately, exactly the pre-round-10b timing.
        if presentationStyle != .expanded {
            insertStaged()
            return
        }

        // Drop the user straight at Messages' Send (§11.4): the expanded board has
        // no send control of its own — Send lives in the compose area — so once a
        // move is staged, collapse to compact instead of making them drag down. To
        // add more cards (throw-ins, a second cover) they just re-open the game;
        // the staged chain survives the style change (GameSurface @State).
        //
        // note 8: this USED TO be a flat 900ms sleep, tuned for a single card's
        // spring settle — a bout-ending "good" plays a whole discard+draw
        // cascade (one step per drawing player) that routinely runs longer,
        // and got guillotined mid-flight. A short lead-in first (the
        // `BoardAnimator.sequenceDepth` increment for such a cascade happens
        // inside a Task the SwiftUI `onChange` callback schedules, which can
        // lag a beat behind this function starting, so checking `isSequencing`
        // with zero lead-in would sometimes race it and see false); THEN
        // `waitForSettle()` for however long the real sequence takes (a plain
        // attack/cover has no sequence at all, so this returns almost at
        // once); THEN a rest so the settled result reads, not a flicker.
        try? await Task.sleep(nanoseconds: 250_000_000)
        await BoardAnimator.waitForSettle()
        try? await Task.sleep(nanoseconds: 500_000_000)
        guard current() else { return }

        // Round-10b (the residual "self cards go a bit under the screen"):
        // COLLAPSE FIRST, insert AFTER the transition settles. Inserting while
        // still expanded made Messages animate the brand-new input-field
        // bubble from a large preview into its compose slot ON TOP of the
        // collapsing drawer - and since the bubble's picture is the PUBLIC
        // table (no hand, no buttons), the board's bottom looked like it dove
        // under the screen until the preview landed. Filmed with a debug
        // ruler drawn on the live surface: the flying rect carried no ruler
        // lines, so it was never our view - it was the bubble preview. With
        // the insert deferred until didTransition, the collapse animates the
        // LIVE board alone (exactly like a manual swipe), and the bubble
        // simply appears in its slot at the end. (The already-compact case
        // returned above - this path is expanded-only.)
        //
        // Round-10c, the LAST piece: ruler-instrumented films proved the
        // style transition itself is SNAPSHOT compositing we cannot influence
        // (mid-flight imagery our live tree cannot produce), and it visibly
        // dropped the board's bottom half. So first PACK the board into a
        // compact-sized box at the drawer's bottom under our own animation -
        // the visible collapse, fully controlled, hand pinned - and only then
        // change style: both snapshot endpoints now share an identical bottom
        // strip, and all that shrinks away above it is featureless wool.
        // Round-10d: ARM the surface (no pre-animation of any kind - the
        // round-10c "pack the board up first" WAS the owner's "goes up, then
        // goes back down"), then request the collapse. The surface's own
        // height tween takes it from there; see MessagesRootView.follow.
        collapseSignal.token += 1
        requestPresentationStyle(.compact)
        await awaitTransitionSettled()
        // The last gate, and the one that matters: the newer run has already
        // put its own bubble in the field, so inserting here would replace it
        // with this older one.
        guard current() else { return }
        insertStaged()
    }

    /// Which `stage` run owns the input field - see the note at the top of it.
    private var stageGeneration = 0

    /// The chain a message Messages reports actually carries. The message is the
    /// authority on its own bytes; our `pendingStage` bookkeeping is not (round
    /// 12 #11).
    private static func payload(of message: MSMessage) -> Data? {
        guard let u = message.url else { return nil }
        return try? MessageEnvelope.payloadBytes(url: u)
    }

    /// Commit a sent chain to the App Group cache (§6.1/§7.6): our seat becomes
    /// durable and this chain is the preferred one for the game. `chatKey` comes
    /// from `didStartSending`'s own conversation, not a stored property, because
    /// it must be the SAME conversation the bubble was staged/sent into (the
    /// whole point of the chat-scoping fix).
    ///
    /// ROUND-9 #5: fully SYNCHRONOUS. This used to decode the payload in a
    /// fire-and-forget Task just to learn the gameId - but `pendingStage`
    /// already carries it (round 6), and the Task raced the `dismiss()` /
    /// VC-swap teardown that follows a send. When the clear lost that race, the
    /// reopen of my OWN sent chain still saw the ledger rows, Rule R discarded
    /// them against a chain that already contains those moves, and every send
    /// ended in a "move superseded" toast + a replay of my own move. Three
    /// UserDefaults writes need no Task and cannot lose the race.
    ///
    /// ROUND 12 #11: `sent` is the chain the MESSAGE carries. The seat write
    /// still needs `pendingStage` (only it knows which seat and game we staged
    /// as), but the quiet-open marker does not - and it is the one that must
    /// never be skipped, so it is written from `sent` outside the guard.
    private func commitPendingStage(sent: Data?, chatKey: String) {
        // The durable half of `lastSentPayload` (round-9 #5): if the send tears
        // this VC down, the reopen consumes this and opens my own chain QUIETLY
        // (no self-replay) instead of treating it as a new arrival.
        if let sent { MessageGameStore.shared.markJustSent(payload: sent) }
        guard let (_, mySeat, gameId) = pendingStage else { return }
        pendingStage = nil
        if !gameId.isEmpty {
            // Round 7: the preferred-chain cache is gone — commit only the durable
            // SEAT (§6.1). The chain the human just sent is now the thread's, and
            // reopening it re-renders it from its own bytes. (ROUND 9: the pending
            // ledger this also used to clear is gone entirely - owner call.)
            // The claim-time NAME is carried forward, not re-derived: this is a
            // re-affirmation of a seat MessagesRootView.cache already claimed
            // (with the name that game's roster carries at it), and this VC
            // knows only a seat number - `pendingStage` has no roster and this
            // function is deliberately synchronous, so it cannot decode one.
            // Passing the device nickname here would put the bug straight back:
            // one send in game B would stamp B's name onto A's row. Nil when
            // there is no row yet, which is permissive, and the next adopt of
            // this device's own sent chain fills the name in.
            MessageGameStore.shared.setSeat(gameId: gameId, chatKey: chatKey, seat: mySeat,
                                            name: MessageGameStore.shared.claimName(gameId: gameId))
            // ROUND 20: and it is the newest chain this device has seen, by
            // construction - it was built ON the board that was open, which had
            // already been ranked against whatever was on file (GameSurface
            // .rankAgainstHighWater). Without this, tapping back to an older
            // bubble in the same session would not be recognised as a branch
            // until the next bubble arrived. No Rule P call: nothing this device
            // can hold beats a chain it just extended.
            if let sent { MessageGameStore.shared.setLatestChain(gameId: gameId, chatKey: chatKey, payload: sent) }
        }
    }

    /// Round-7 (background gap): a FALLBACK table colour painted on the host view
    /// behind the SwiftUI content, so if the table ever fails to reach an edge
    /// for a frame - e.g. the post-send reopen with the keyboard up - the
    /// exposed strip reads as a duller patch of the SAME board, never a
    /// system-black void.
    ///
    /// ROUND 22: it asks `FTextures`, which is the ONE place that knows what the
    /// table looks like right now, instead of naming wool's hexes itself.
    /// FTextures' own header says nothing outside it may name a resource or a
    /// hex, and this file was the exception - so a player on the FELT table got
    /// a strip of WOOL BROWN across the top of a green drawer (owner, 1.0(24):
    /// "little brown in the lobby top"). The material is a preference, not a
    /// trait, which is why the dynamic provider alone could not have been right:
    /// it re-resolves on a scheme change and never on a settings change.
    private static var tableFallback: UIColor {
        UIColor { tc in
            let scheme: ColorScheme = tc.userInterfaceStyle == .dark ? .dark : .light
            let hex = FTextures.tableFallbackHex(FTextures.Variant(scheme))
            return UIColor(red: CGFloat((hex >> 16) & 0xFF) / 255.0,
                           green: CGFloat((hex >> 8) & 0xFF) / 255.0,
                           blue: CGFloat(hex & 0xFF) / 255.0, alpha: 1)
        }
    }

    // MARK: The collapse, carried on the layer
    //
    // `CollapseTween.slideDuration` has the measurement behind this. The short
    // of it: the host moves our view perfectly smoothly at the composite rate,
    // and every bit of the auto-collapse's judder is our own height arriving a
    // render late - which it must, because SwiftUI renders at 60Hz while the
    // render server composites at 86-94Hz. So the slide pins the box at its
    // expanded height and moves the CONTENT instead, on a keyframe animation
    // the render server evaluates on every frame it composites, ours or not.
    //
    // The animation goes on the hosting view's own layer, NOT on this view
    // controller's: the host is animating this one's position, and the two
    // motions have to compose rather than fight. `transform.translation.y` and
    // `position` are separate properties, so they do.
    private static let slideKey = "cards.foolish.collapse.slide"

    /// Slide the board up by `travel` over `duration`, on the host's own curve.
    func slideCollapse(travel: CGFloat, duration: Double,
                       response: Double = CollapseTween.hostResponse) {
        guard let layer = host?.view.layer, travel > 1 else { return }
        layer.removeAnimation(forKey: Self.slideKey)
        let a = CAKeyframeAnimation(keyPath: "transform.translation.y")
        a.values = CollapseTween.slideOffsets(travel: travel, duration: duration,
                                              response: response)
        a.duration = duration
        a.calculationMode = .linear
        // HELD AT THE END, not removed. The box only becomes the compact one
        // when the release runs; until then the expanded box translated up by
        // its own travel IS the compact box, and letting the animation snap back
        // to zero first would drop the board `travel` points for a frame.
        a.fillMode = .forwards
        a.isRemovedOnCompletion = false
        layer.add(a, forKey: Self.slideKey)
    }

    /// Take the translation off, for the release or for a drag that interrupts.
    /// Actions disabled: an implicit animation on the way out is the same
    /// one-frame drop the fill above exists to prevent.
    func endCollapseSlide() {
        guard let layer = host?.view.layer else { return }
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        layer.removeAnimation(forKey: Self.slideKey)
        CATransaction.commit()
    }

    private func setRoot(_ root: MessagesRootView) {
        applyTableFallback()
        if let host {
            host.rootView = root
        } else {
            let h = UIHostingController(rootView: root)
            h.view.backgroundColor = Self.tableFallback
            addChild(h)
            h.view.translatesAutoresizingMaskIntoConstraints = false
            view.addSubview(h.view)
            NSLayoutConstraint.activate([
                h.view.leadingAnchor.constraint(equalTo: view.leadingAnchor),
                h.view.trailingAnchor.constraint(equalTo: view.trailingAnchor),
                h.view.topAnchor.constraint(equalTo: view.topAnchor),
                h.view.bottomAnchor.constraint(equalTo: view.bottomAnchor),
            ])
            h.didMove(toParent: self)
            host = h
        }
    }
}

#if RIG_ARRIVE
/// THE DOOR'S BOARD MOVE, in one actor call: adopt the chain, read the seat's
/// menu, pick the move, rebuild and seal. One call because the resident game
/// is one slot and decoding IS adopting - a read and a seal split across an
/// await would describe whichever chain landed in between.
extension MessageKernel {
    func rigMove(base: Data, seat: String, kind: String, pick: String,
                 gameId: UInt64, parent8: Data, joins: [MessageJoin]) throws -> Data {
        _ = try decode(payload: base, viewer: -1)
        guard let view = residentView(viewer: -1) else { throw MessageEnvelope.Failure.damaged(code: -5) }
        let type: MoveType
        switch kind {
        case "good": type = .good
        case "attack", "throwin": type = .attack
        case "cover": type = .cover
        case "pickup": type = .pickup
        case "pass": type = .pass
        default: throw MessageEnvelope.Failure.damaged(code: -6)
        }
        let seats = seat == "any" ? Array(0..<view.players.count) : [Int(seat) ?? -1]
        for s in seats where s >= 0 && s < view.players.count {
            let menu = residentLegal(seat: s).filter { $0.type == type }
            guard let move = Self.rigPick(menu, pick) else { continue }
            return try resealFromBase(.continuation(payload: base), replaying: [move], seat: s,
                                      gameId: gameId, parent8: parent8, joins: joins)
        }
        throw MessageEnvelope.Failure.rejected(reason: -1)
    }

    /// low / high by the first card (single-card moves first), or the move that
    /// plays a named card: 2..10, J, Q, K, A then S H C D (value 1 is a two).
    static func rigPick(_ menu: [Move], _ pick: String) -> Move? {
        if menu.first?.cards.isEmpty ?? true { return menu.first }
        let singles = menu.filter { $0.cards.count == 1 }
        let pool = singles.isEmpty ? menu : singles
        switch pick {
        case "low": return pool.min { ($0.cards.first?.v ?? 0) < ($1.cards.first?.v ?? 0) }
        case "high": return pool.max { ($0.cards.first?.v ?? 0) < ($1.cards.first?.v ?? 0) }
        default:
            let ranks = ["2", "3", "4", "5", "6", "7", "8", "9", "10", "J", "Q", "K", "A"]
            guard let suitChar = pick.last, let s = Array("SHCD").firstIndex(of: suitChar),
                  let r = ranks.firstIndex(of: String(pick.dropLast())) else { return nil }
            let card = Card(s: s, v: r + 1)
            return pool.first { $0.cards.contains(card) }
        }
    }
}
#endif
