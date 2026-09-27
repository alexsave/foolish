// GameSurface - the state machine behind the extension's one surface (§5/§6/§7).
//
// MessagesRootView routes the PRESENTATION style (compact drawer vs expanded
// sheet) and drives the collapse tween; this is what it shows inside either
// one. Split out of MessagesRootView.swift, which held both: the routing is
// ~180 lines of drawer geometry and this is ~1000 lines of "which screen does
// the tapped bubble resolve to, and what plays on the way there" - a lobby, a
// name gate, a seat picker, a spectator board or MessageTableView.
//
// No Durak rule is answered here either - MessageTurnController relays the
// kernel and MessageComposer only stages. Seat identity is the one non-kernel
// decision, and it is SeatIdentity's pure §6 logic, fed the conversation's
// `senderIsLocal`.

import SwiftUI

struct GameSurface: View {
    let payloadURL: URL?
    let senderIsLocal: Bool
    let startNewGame: Bool
    let newGameToken: Int
    let sentToken: Int
    let sentPayload: Data?
    let chatKey: String
    let chatIsDM: Bool
    let chatPlayers: Int
    let incomingURL: URL?
    let incomingToken: Int
    let cancelToken: Int
    let requestExpand: () -> Void
    let onNewGame: () -> Void
    /// Start a NEW MSSession for whatever is staged next, WITHOUT the teardown
    /// `onNewGame` does. The rematch path needs exactly this half: its first
    /// bubble must not collapse the finished game's result card (see
    /// MessagesViewController's session note), but it has no name to ask for
    /// and no surface to rebuild.
    let onFreshChain: () -> Void
    /// Name the player who just left, for the bubble about to be staged. The
    /// envelope cannot say it - the join carrying the name is exactly what the
    /// leave removed - so the one device that still knows tells the host, which
    /// writes it into the transcript line. Cleared once staged.
    let onAnnounceLeave: (String) -> Void
    let onSend: (Data, Int, Bool) async -> Void
    let onUnstage: () -> Void
    let onOpenURL: (URL) async -> Bool

    /// A phase-0/handoff lobby the extension shows instead of the board (§5.2).
    private struct Lobby { let env: MessageEnvelope; let payload: Data }
    /// A resolved seat waiting on the human's name (§B3). Since lobby v3 EVERY
    /// seated player named themselves on the way in (the creator in setup, every
    /// joiner — DM opponent included — at the lobby's Join field), so this fires
    /// only on §6.2 cache-loss recovery: a reinstall or second device, where the
    /// seat resolves from an exact signal but the stored nickname is gone with
    /// the cache. Any player count. Ask once, store it, then seat them.
    private struct NameGate { let env: MessageEnvelope; let payload: Data; let seat: Int
                              var quietOpen = false }    // round-9 #5: my just-sent chain

    @State private var controller: MessageTurnController?
    /// The setup/lobby screens' Settings + Help squares present these
    /// (durak-rules-redesign) — the board keeps its own pair of flags inside
    /// MessageTableView, so the two never fight over one sheet.
    @State private var showSettings = false
    @State private var showRules = false
    @State private var ambiguous: (env: MessageEnvelope, payload: Data)?
    /// RELEASE-ONLY substitute for `ambiguous` (§6.3): an unresolved identity in
    /// Release must never offer a seat picker (anyone could claim any hand), so we
    /// show the same PUBLIC spectator board a delivered bubble's snapshot uses,
    /// instead. DEBUG keeps the real picker (single-simulator testing needs it).
    @State private var spectator: (view: GameView, names: [Int: String])?
    /// Round 20: the finished spectator board's Replay Link, captured with the
    /// board itself - see where it is set. nil unless the game is over.
    @State private var spectatorReplayURL: URL?
    /// ROUND 21: A WATCHER GETS THE REAL BOARD FOR THE LAST MOVE.
    ///
    /// The owner: "spectators opening final move goes straight to rank board, no
    /// final animation. We should show the final move still." Round 20 gave a
    /// spectator the RESULT of a finished chain and skipped how it got there,
    /// because the spectator branch draws `MessageBoardView` - a still picture of
    /// a `GameView`, with no animator in it.
    ///
    /// So a finished chain hands the watcher a `MessageTurnController` instead,
    /// seated at -1. Everything a player gets then falls out of machinery that
    /// already exists and is already tested: `begin()` resolves the kernel's
    /// evwire for the last move, the board replays it, and `settleResults` gives
    /// way to the same `FGameOverList`. Read-only by construction rather than by
    /// a flag - the kernel refuses a legal-move query for seat -1, so `legal` is
    /// empty, so `iCanAct` and `canStage` are both false, and every control on
    /// the board is gated on one of those two.
    ///
    /// PUBLIC-SAFE for the same reason the still picture was: viewer -1 is the
    /// masked view (§10), so there are no hands in it to leak - the fan simply
    /// has nothing to draw.
    ///
    /// nil while the chain is still running; that case still gets the still
    /// picture and the "spectating" caption, which is all there is to say about
    /// a game nobody here is playing.
    @State private var spectatorBoard: MessageTurnController?
    @State private var lobby: Lobby?
    /// ONE BEAT OF AN ARRIVING STREAM, held on screen (1.1(56)).
    ///
    /// A single text can carry several actions - `conversation.insert` replaces
    /// an unsent draft rather than queueing a second one, so Join, then the
    /// rules box, then Start go out as ONE envelope. The kernel says what those
    /// actions were, in what order, how long each rests and which idiom it
    /// wears (SurfacePlan); this is where a beat that is not the last one is
    /// drawn, over the surface, until the next one is due. nil the rest of the
    /// time, which is nearly always.
    private struct ArrivalStill {
        /// The roster and rule THIS beat shows.
        let env: MessageEnvelope
        let passing: Bool
        /// Which lobby the CONTROLS answer to: this beat's own state, or the
        /// one the human already had. `SurfacePlan.Controls`, spent here - see
        /// anim_plan.h for the owner's four cases.
        let controls: MessageEnvelope
    }
    @State private var arrivalStill: ArrivalStill?
    /// How opaque the held beat is. 1 while it is showing (every beat SNAPS in),
    /// eased to 0 by a BOARD beat's fade. See `playArrival` for why it is an
    /// opacity of the view's own and not a `.transition`.
    @State private var stillFade: Double = 1
    /// THE WHOLE SURFACE's own opacity, for the two changes that have no second
    /// copy to cross-fade against - see `fadeSurface`. 1 at rest, always.
    @State private var surfaceFade: Double = 1
    /// The rules beat's turn, handed to the live lobby's checkbox once the beat
    /// has adopted. See `FCheckbox.Turn` for why it is a token and not `isOn`.
    @State private var rulesTurn: FCheckbox.Turn?
    @State private var nameGate: NameGate?
    @State private var showSetup = false
    @State private var toast: String?
    @State private var damaged = false
    /// 1.0(6) DIAGNOSTIC: the last decode error, shown in the on-screen dump so a
    /// message that fails to open can be captured by screenshot (temporary).
    @State private var diagError: String?
    /// 1.0(6) DIAGNOSTIC: the FULL raw FMSG envelope bytes as hex (the iMessage
    /// format itself - magic/format/flags/phase/game_id/seed/digest/joins/body -
    /// not just the replay code), and the parsed header fields when decode works.
    @State private var diagHex = ""
    @State private var diagInfo = ""
    /// Round 12: the dump is showing, summoned by a 5-second hold on the gear
    /// (see `diagnosticPanel`). Deliberately NOT cleared by `reloadForInput`:
    /// leaving it up across an arriving bubble is the useful case, since the
    /// fields under it refresh to the message that just landed.
    @State private var showDiagnostics = false
    /// Round-9: the SURFACE just staged a sendable bubble (create/join/invite/
    /// start) that the human has not sent yet - what the send reminder shows
    /// for on the lobby screens, and (via `alsoStaged`) on the starter's
    /// handoff board, where the controller's own pending list is empty. Reset
    /// when the bubble is sent (sentToken), cancelled (cancelToken), or a new
    /// input reloads the surface.
    @State private var surfaceStaged = false
    /// ROUND 16: the previous session ended badly and the owner has not looked
    /// at it yet. Drives the one-line banner (`healthBanner`); cleared by
    /// tapping it (which opens the dump) or by dismissing it.
    @State private var healthAlarm: FlightSession?

    /// A style toggle keeps this key stable, so the session is NOT reloaded and
    /// the in-progress game survives. A new bubble (payloadURL) or a New game tap
    /// (newGameToken) changes it, which resets and reloads. `chatKey` is in here
    /// too, defensively: this view's state must never survive a conversation
    /// change (the chat-scoping fix's whole point), even though in practice one
    /// extension instance presents one conversation for its lifetime.
    ///
    /// `startNewGame` IS DELIBERATELY NOT IN HERE, and leaving it in was the
    /// 1.1(57) lobby bug. It is set when the human taps New game and cleared
    /// again by `didReceive` - so the FIRST text to arrive after you created a
    /// game flipped it true -> false, changed this key, and fired
    /// `reloadForInput`, which nils the lobby and reloads from `payloadURL`:
    /// YOUR OWN INVITE, because an arrival never becomes `selectedMessage`.
    /// That raced `maybeAdoptIncoming`, and whichever finished last won.
    ///
    /// The owner's report is the shape of the race exactly: "the extension that
    /// was open didn't update at all… although SOMETIMES it would update on
    /// live arrival. But when it did, it only ever snapped, no rotate" - the
    /// reload also clears `rulesTurn` and rebuilds the checkbox, and a freshly
    /// built view runs no `onChange`, so the turn was dropped even in the runs
    /// where the roster survived.
    ///
    /// And why it was only ever seen in a LOBBY, which is what made it look
    /// like the lobby's own bug (owner: "it works just fine for the board and
    /// game moves"): by the time a board is taking moves the flag has long been
    /// false, so an arrival does not move this key at all. Only the first text
    /// after a New game does.
    ///
    /// Nothing is lost by dropping it: `newGameToken` is incremented in the
    /// same closure that sets the flag, so a New game still changes this key.
    private var loadKey: String {
        "\(newGameToken)|\(chatKey)|\(payloadURL?.absoluteString ?? "")"
    }

    var body: some View {
        // One game per chat, one surface for both presentation styles: always the
        // table (or the New-game setup when the thread has no game yet). There is no
        // "A game in this thread / Open the game" menu — collapsing to compact just
        // shows the same table in the short strip, with Messages' Send in the
        // compose area above. Keeping a single `expandedContent` root also means the
        // board's @State + .task survive the expanded<->compact toggle.
        expandedContent
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            // Round 12: the hold-summoned dump rests on top of whatever is
            // showing - board, lobby or setup - and nothing underneath it
            // changes. Above `fToast` so a toast cannot land on top of it.
            // ROUND 16, the owner: "make it appear as a diagnostic dump in the
            // UI so I can check next time it happens." The dump itself already
            // existed behind a five-second hold on the gear, which is fine for
            // asking a question and useless for being TOLD something - so when
            // the previous session ended in a way it should not have, the
            // surface says so on its own, in one line, and that line opens the
            // dump. Only for an ALARMING end (FlightRecorder.isAlarming): an
            // ordinary abrupt teardown is something Messages does routinely, and
            // a banner that cries wolf on those would train the owner to ignore
            // the one that matters.
            .overlay(alignment: .top) { healthBanner }
            .overlay { if showDiagnostics { diagnosticPanel } }
            .fToast($toast)
            // The setup/lobby Settings + Help squares present these — same
            // sheets as the board's own pair (MessageTableView).
            #if DEBUG
            // The rig's way in, matching the board's own pair. Without it the
            // only way to ask this surface for its Settings sheet is to hit a
            // 40pt square with a synthetic tap, and a probe that can miss cannot
            // tell "the sheet is broken" from "the tap was off".
            .onAppear {
                if ProcessInfo.processInfo.environment["HARNESS_OPEN_SETTINGS"] != nil { showSettings = true }
                if ProcessInfo.processInfo.environment["HARNESS_OPEN_RULES"] != nil { showRules = true }
            }
            #endif
            .sheet(isPresented: $showSettings) {
                MessageSettingsView { showSettings = false }
            }
            .sheet(isPresented: $showRules) {
                // Round-9 (owner): this sheet only serves the PRE-GAME screens
                // (the board presents its own pair inside MessageTableView), so
                // the rulebook here is the simpler lobby page: how the lobby
                // works + the goal. The full rules stay one tap away in-game.
                RulesView(scope: .lobby) { showRules = false }
            }
            .task(id: loadKey) {
                await reloadForInput()
                await autoDriveLobby()
            }
            // Round-6 bug 4: the human just SENT the staged bubble (the host bumped
            // `sentToken` from didStartSending). Tell the live controller its move
            // is now in the thread so it drops it from `pending` - `canSend`/
            // `canUndo` go false and the collapsed drawer's Undo button, which
            // otherwise lingered and re-staged an already-sent move, disappears.
            // `sentToken` is deliberately absent from loadKey, so this fires WITHOUT
            // reloading the surface (the game is unchanged, only its staged move is
            // no longer pending).
            //
            // ROUND 16: it also carries the sent BYTES now, which rebase the
            // controller onto its own bubble. That used to happen by itself,
            // because the send tore the extension down and the next move was
            // played by a controller rebuilt from those bytes; the drawer stays
            // open now (owner: "just keep it collapsed so they can keep
            // playing"), so this signal is the only thing left that does it.
            .onChange(of: sentToken) { _ in
                AnimLog.say("surface sent token=\(sentToken) bytes=\(sentPayload?.count ?? -1)")
                // The other end of the host's `send` note: what the bytes look
                // like AFTER a root-view rebuild and a SwiftUI diff have
                // carried them here. `send 107b` followed by `send-signal none`
                // is a value lost in the view layer; both saying none is a host
                // that never had it.
                FlightRecorder.note("send-signal", sentPayload.map { "\($0.count)b" } ?? "none")
                surfaceStaged = false   // round-9: the staged bubble is sent
                stagedDraft.clear()     // …and the draft it belonged to is now the thread's
                // SYNCHRONOUSLY, in this same SwiftUI transaction: the Undo pill
                // goes now, not after a Task hop and a decode (owner: "should
                // probably disappear the second you hit send"). `markSent` below
                // does the rest and clears the flag.
                controller?.markSending()
                Task { await controller?.markSent(payload: sentPayload) }
            }
            // Round-9: the human deleted the staged bubble from the input field
            // (didCancelSending) - nothing is awaiting Send any more.
            //
            // The board's half of a cancel - the undo, and whether a shorter
            // chain goes back into the input field - is
            // `MessageTableView.cancelStagedBubble`, on the same token: the send
            // hint over a BOARD is drawn off `controller.canSend`, so clearing
            // `surfaceStaged` here never dimmed it (1.0(37): "if I stage then X
            // the staged bubble, the send hint arrow doesn't go away").
            //
            // 1.1(68): and the SURFACE's half is no longer just three flags.
            // The owner asked for the X to undo a staged lobby action the way it
            // undoes a staged move, so this reverts the table too - see
            // `revertStagedSurface`, which is where the flags are now cleared.
            .onChange(of: cancelToken) { _ in Task { await revertStagedSurface() } }
            // A bubble ARRIVED while this surface is open (didReceive). Apple
            // does not move `selectedMessage` for an arrival, so loadKey does
            // not change and the .task above will not re-run - this one does.
            .task(id: incomingToken) { await maybeAdoptIncoming() }
            // Read once, on the first paint of this surface. Deliberately not in
            // `loadKey`'s task: the question "how did last time end" is answered
            // once per launch, not once per bubble.
            .task {
                guard healthAlarm == nil, let p = FlightRecorder.previousSession(),
                      FlightRecorder.isAlarming(p) else { return }
                healthAlarm = p
            }
            #if RIG_RESEED
            .task { await watchForReseed() }
            #endif
    }

    /// The banner. One line, at the top, tappable - and gone for good once it
    /// has been acted on, because its job is to be noticed once and not to
    /// decorate the board.
    @ViewBuilder private var healthBanner: some View {
        if let alarm = healthAlarm, !showDiagnostics {
            HStack(spacing: 6) {
                Text(FlightRecorder.verdict(alarm))
                    .font(.system(size: 10, weight: .semibold, design: .monospaced))
                    .foregroundColor(.white)
                    .lineLimit(2)
                    .minimumScaleFactor(0.7)
                Spacer(minLength: 4)
                Text("✕").font(.system(size: 12, weight: .bold)).foregroundColor(.white)
                    .onTapGesture { healthAlarm = nil }
            }
            .padding(.horizontal, FSpace.s)
            .padding(.vertical, 5)
            .background(FColor.accent.opacity(0.94))
            .clipShape(RoundedRectangle(cornerRadius: 6))
            .padding(.horizontal, FSpace.s)
            .padding(.top, 4)
            .contentShape(Rectangle())
            .onTapGesture { showDiagnostics = true; healthAlarm = nil }
            .transition(.move(edge: .top).combined(with: .opacity))
        }
    }

    // MARK: - the stale-branch gate (round 20)

    /// The newer chain this board was found to be behind, kept so the bar below
    /// has something to open. Cleared whenever an adopt comes back live.
    @State private var supersededBy: Data?
    /// The verdict from the last `adopt`, spent by `seatOnBoard` - which is
    /// where the controller finally exists, and is reached from the name gate
    /// and the DEBUG seat picker as well as straight from `adopt`.
    @State private var staleBranch = false

    /// THE BAR OVER A READ-ONLY BOARD. Says why nothing can be tapped, and
    /// offers the one thing that fixes it.
    ///
    /// Offering the newer chain by BUTTON rather than adopting it silently is
    /// the whole difference between this and the round-7 payload cache the owner
    /// removed: the extension still renders exactly the bubble you tapped, until
    /// you ask it not to.
    @ViewBuilder private func supersededBar(_ c: MessageTurnController) -> some View {
        if c.superseded {
            HStack(spacing: 8) {
                Text(FStrings.t("ios.msg.stale"))
                    .font(.system(size: 11, weight: .semibold))
                    .foregroundColor(.white)
                    .lineLimit(2)
                    .minimumScaleFactor(0.7)
                Spacer(minLength: 4)
                if supersededBy != nil {
                    FButton(FStrings.t("ios.msg.opennewest"), kind: .wood, compact: true) {
                        Task { await openNewest() }
                    }
                }
            }
            .padding(.horizontal, FSpace.s)
            .padding(.vertical, 5)
            .background(FColor.accent.opacity(0.94))
            .clipShape(RoundedRectangle(cornerRadius: 6))
            .padding(.horizontal, FSpace.s)
            .transition(.move(edge: .top).combined(with: .opacity))
        }
    }

    /// Take the newer chain, through the SAME `adopt` a tap or an arrival goes
    /// through - so seat identity, the phase-0 lobby route and the open-replay
    /// all behave exactly as they would have if this bubble had been tapped.
    private func openNewest() async {
        guard let bytes = supersededBy,
              let env = try? await MessageEnvelope.decode(payload: bytes, viewer: -1) else { return }
        AnimLog.say("surface opens the newest chain by request")
        await adopt(winner: bytes, env: env)
    }

    /// IS THIS BOARD A BRANCH OFF AN OLD BUBBLE, and record it if it is not.
    ///
    /// The decision itself is `StaleBranchGate.rank` - two authorities, Rule P
    /// and "does the chain on file actually show more of the game", both of
    /// which must agree before the board goes read-only. It lives there and not
    /// here because it can be driven from a test with real sealed chains; this
    /// only spends the answer.
    @discardableResult
    private func rankAgainstHighWater(_ payload: Data, env: MessageEnvelope) async -> Bool {
        let verdict = await StaleBranchGate.rank(payload: payload, env: env, chatKey: chatKey)
        supersededBy = verdict.newest
        return verdict.superseded
    }

    /// Fold an ARRIVING bubble into the live surface, Rule P deciding (§7.2).
    ///
    /// Why this exists: `didReceive` fires while the extension is open, but the
    /// arrival does not become `selectedMessage`, so nothing reloaded and the
    /// surface sat on whatever chain it last adopted until the human re-tapped
    /// a bubble. Mostly that was just staleness (an opponent's move not showing
    /// until reopen; a lobby roster missing the join that just arrived). In the
    /// double-Start race it was a DEADLOCK: two players tap Start off different
    /// lobby states, two LIVE handoffs exist, Rule P (kernel rule 3) picks the
    /// fuller one - but the losing starter's own device was already sitting on
    /// its fork's board and never re-compared, so if the real game's first
    /// attacker was that player, every screen in the chat waited forever.
    ///
    /// Rule P still decides everything: a stale or duplicate arrival loses to
    /// the chain on screen and changes NOTHING (no teardown, no replay). Only a
    /// strictly-preferred arrival is adopted - through the same `adopt` a tap
    /// goes through, so seat identity and the phase-0 lobby route both
    /// hold. With nothing on screen to compare (spectator / picker / name
    /// gate), the arrival simply renders - round 7 keeps no cached chain to
    /// weigh it against. `showSetup` is exempt: the human explicitly asked for
    /// a new game.
    /// The arrival this surface has already taken. ONE arrival, handled ONCE.
    ///
    /// There are two callers now - `.task(id: incomingToken)`, and
    /// `reloadForInput` when it recognises that the chain it was about to load
    /// IS the chain that just arrived - and in 1.1(61) both of them ran. The
    /// owner's flight log shows it as a doubled line at an identical timestamp:
    ///
    ///     88.57s  arrival  beats=1 showing=yes phase=0 joins=2
    ///     88.57s  arrival  beats=1 showing=yes phase=0 joins=2
    ///
    /// Two `playArrival`s over one surface, each seeding the still and adopting.
    /// The loser finishes second and lands the END STATE on top of a sequence
    /// that is still running - which is a snap wearing a sequence's timing, the
    /// exact symptom this whole chain of builds has been chasing.
    ///
    /// Keyed on the BYTES rather than on `incomingToken`, because the two callers
    /// are reached by different routes and only one of them is the token's; the
    /// bytes are what they agree on. Cleared by a new input, so the same chain
    /// arriving again on a fresh surface is still played.
    @State private var arrivalTaken: Data?

    private func maybeAdoptIncoming(showingBefore: Data? = nil) async {
        guard let url = incomingURL, !showSetup, !startNewGame,
              let bytes = try? MessageEnvelope.payloadBytes(url: url) else {
            // The gate that produces NOTHING on screen and no trace of itself,
            // which is the owner's "it does not update at all" on the first
            // bubble of a chain. `setup`/`newGame` are the two states that
            // deliberately refuse arrivals; if one of them is stuck on, this is
            // the only line that will ever say so.
            if incomingURL != nil {
                FlightRecorder.note("arrival-ignored",
                    "setup=\(showSetup) newGame=\(startNewGame) bytes=\(incomingURL == nil ? "-" : "ok")")
            }
            return
        }
        // A reload is mid-flight and the surface is momentarily empty. Stand
        // down: `reloadForInput` calls this again the moment it has settled, and
        // passes what was on screen before it started. See `reloading`.
        if reloading, showingBefore == nil {
            AnimLog.say("arrival held - a reload owns the surface")
            return
        }
        let current = controller?.basePayload ?? lobby?.payload
                      ?? showingBefore ?? lastShownChain
        if bytes == current {
            AnimLog.say("arrival ignored - same chain")
            FlightRecorder.note("arrival-ignored", "same chain")
            return
        }
        // ONE ARRIVAL, ONE HANDLER - see `arrivalTaken`.
        if bytes == arrivalTaken {
            FlightRecorder.note("arrival-ignored", "already taken")
            return
        }
        arrivalTaken = bytes
        if let current {
            // A refusal here is SILENT to the player, and that is what made the
            // rule-4 hole (a chain tying with its own child on round/turn, see
            // msg_wire.c msg_rule_p) so hard to see: the board just sat one
            // move behind until the bubble was re-tapped. So a refusal now says
            // which chains it weighed - `peek` reads the headers without
            // touching the resident game, and the whole block is skipped when
            // the trace is off.
            var pref = -999
            do { pref = try await MessageKernel.shared.preferred(current, bytes) }
            catch { AnimLog.say("arrival Rule P threw \(error)") }
            if pref <= 0 {
                if AnimLog.on {
                    let ce = try? await MessageKernel.shared.peek(payload: current)
                    let be = try? await MessageKernel.shared.peek(payload: bytes)
                    AnimLog.say("arrival ignored - Rule P pref=\(pref) "
                        + "cur=[t\(ce?.turn ?? -1) r\(ce?.round ?? -1) actor\(ce?.lastActorSeat ?? -1)] "
                        + "new=[t\(be?.turn ?? -1) r\(be?.round ?? -1) actor\(be?.lastActorSeat ?? -1)]")
                }
                // The SHAPE of both chains, because "pref=-1" alone cannot tell
                // a correct refusal from a broken one. My own invite coming back
                // with a fresh send clock is state-identical and SHOULD lose;
                // a genuine join losing is a bug. They differ only in these
                // numbers, and without them the log makes both look the same.
                let ce = try? await MessageKernel.shared.peek(payload: current)
                let be = try? await MessageKernel.shared.peek(payload: bytes)
                FlightRecorder.note("arrival-ignored",
                    "rule P pref=\(pref) mine=[p\(ce?.phase ?? -1) j\(ce?.joins.count ?? -1) "
                    + "t\(ce?.turn ?? -1)] new=[p\(be?.phase ?? -1) j\(be?.joins.count ?? -1) "
                    + "t\(be?.turn ?? -1)]")
                return
            }
        }
        guard let env = try? await MessageEnvelope.decode(payload: bytes, viewer: -1) else {
            AnimLog.say("arrival ignored - decode failed")
            FlightRecorder.note("arrival-ignored", "decode failed")
            return
        }
        // This runs under `.task(id: incomingToken)`, so a NEWER arrival CANCELS
        // this one - but cancellation only lands at an await, and nothing above
        // rethrows it (`try?` + do/catch swallow it), so a superseded task used
        // to sail on and adopt with facts read BEFORE the newer arrival moved
        // the base: at best a duplicate adopt of the same chain (the stranded
        // open-replay veil this file's round-18 fix is about), at worst an OLDER
        // chain adopted over the newer one. The newer task owns the surface now.
        guard !Task.isCancelled else {
            AnimLog.say("arrival ignored - superseded by a newer arrival")
            return
        }
        AnimLog.say("surface adopts arrival phase=\(env.phase) joins=\(env.joins.count) turn=\(env.turn)")
        // WHAT THIS ARRIVAL IS, AS A SEQUENCE. Asked of the kernel off the two
        // envelopes, and asked BEFORE `adopt` decodes anything - `surfacePlan`
        // is a header read for the same reason `preferred` above is, so the
        // resident game is not moved out from under the board being asked about.
        let plan = await MessageKernel.shared.surfacePlan(showing: current ?? bytes,
                                                          arriving: bytes)
        // THE ONE LINE THAT SAYS WHY THERE WAS NO ANIMATION, and it is on the
        // always-compiled recorder rather than in AnimLog on purpose: AnimLog is
        // DEBUG plus an environment variable, so the build a human actually
        // plays on has no animation trace at all, and three rounds of this bug
        // were diagnosed by inference because nobody could see this number.
        // `showing` is the half that goes wrong silently - a plan asked with
        // nothing on screen diffs the arrival against itself and answers "no
        // beats", which is indistinguishable from "nothing to animate".
        FlightRecorder.note("arrival",
            "beats=\(plan.beats.count) showing=\(current == nil ? "NONE" : "yes") "
            + "phase=\(env.phase) joins=\(env.joins.count)")
        await playArrival(plan, winner: bytes, env: env)
        // WHAT THE SURFACE ENDED UP AS, which is the one thing the notes above
        // cannot say. Everything before this reports what we DECIDED; this
        // reports what the human is now looking at. Without it, "the arrival was
        // accepted with N beats" and "…and the screen still shows the old roster"
        // are the same log line, and telling those apart is another round trip.
        FlightRecorder.note("arrival-done", showingWhat)
    }

    /// PLAY AN ARRIVAL, one beat at a time (1.1(56)).
    ///
    /// The owner: "LOBBY DID NOT UPDATE LIVE! I was in lobby, got a start game
    /// text, and it was stuck on lobby! I think it should fade from lobby to the
    /// game in this case." And then, on a text that carries more than one
    /// action: "it should snap to the state where there are two or whatever
    /// people in the lobby, wait a bit, then fade."
    ///
    /// ONE LOOP, and every decision in it is read rather than made. WHAT the
    /// stream contained, the ORDER, the REST between beats and the IDIOM each
    /// wears are all `plan`'s (c/src/anim_plan.c, c/src/msg_wire.c). What is
    /// left here is the only part a C function cannot do: the easing curve, and
    /// putting a view on screen.
    ///
    /// THE LAST BEAT IS THE ADOPT. Every beat before it is a STILL - the
    /// arriving roster with that beat's rule, drawn over the surface - and the
    /// last one shows the arriving chain exactly, which is what the surface was
    /// going to become anyway. So there is no second code path for "and then
    /// actually take the message": adopting IS the last beat.
    ///
    /// An EMPTY plan is the ordinary case - a board folding a move in, or a
    /// change that is simply true now (a lone join or leave, a lone rules move
    /// with nothing before it). The adopt on its own is that snap.
    private func playArrival(_ plan: SurfacePlan, winner: Data, env: MessageEnvelope) async {
        // THE SURFACE COMING BACK, which is the one beat that plays over a
        // BOARD (1.1(68)). It is the X on a staged Start - see
        // `revertStagedSurface` - and the kernel hands it over as one whole
        // LOBBY beat, never mixed with the others, so it is taken first and
        // alone.
        if let beat = plan.beats.first, beat.kind == .lobby {
            await fadeBackToLobby(beat, winner: winner, env: env)
            return
        }
        guard !plan.beats.isEmpty, let showing = lobby else {
            // A plan WITH beats and no lobby to play them over is not the
            // ordinary empty-plan case - it is the surface having been cleared
            // out from under a sequence, and it degrades to exactly the same
            // silent snap the 1.1(58) report was about. Say so, so the two are
            // never again indistinguishable in a log.
            if !plan.beats.isEmpty {
                AnimLog.say("surface DROPPED \(plan.beats.count) beat(s) - no lobby to play them over")
                FlightRecorder.note("beats-dropped", "\(plan.beats.count) with no lobby under them")
            }
            await adopt(winner: winner, env: env)
            return
        }
        AnimLog.say("surface plays the arrival as \(plan.beats.count) beat(s) "
            + "over \(Int(plan.total * 1000))ms")
        // SEED THE STILL WITH WHAT IS ALREADY ON SCREEN. Invisible by
        // construction - it is a copy of the lobby underneath it - and it is
        // what gives the first beat something to replace and the last something
        // to fade out of. A plan is only ever non-empty over a lobby (the
        // kernel's `on_a_lobby`), which is why `showing` above is a guard and
        // not a fallback.
        arrivalStill = ArrivalStill(env: showing.env, passing: showing.env.passingAllowed,
                                    controls: showing.env)
        stillFade = 1
        let began = Date()
        var turns = rulesTurn?.token ?? 0
        let last = plan.beats.count - 1
        for (i, beat) in plan.beats.enumerated() {
            // WAIT FOR THE BEAT'S OWN START, rather than sleeping a duration of
            // ours: the rest between beats is the gap between their `start`s, so
            // a beat that took longer than the plan expected (an adopt behind a
            // busy kernel actor) does not push the whole sequence out.
            let due = began.addingTimeInterval(beat.start).timeIntervalSinceNow
            if due > 0 { try? await Task.sleep(nanoseconds: UInt64(due * 1_000_000_000)) }
            // A NEWER ARRIVAL OWNS THE SURFACE. Same rule, and the same reason,
            // as `maybeAdoptIncoming`'s own cancellation guard: this runs under
            // `.task(id: incomingToken)`, and a sleep is exactly where a newer
            // token lands. Drop the still rather than leaving it over a surface
            // the next task is about to rebuild.
            guard !Task.isCancelled else { arrivalStill = nil; stillFade = 1; return }
            if beat.transition == .turn { turns += 1 }
            guard i == last else {
                // A beat on the way: the arriving roster, under the rule as it
                // stood at that moment. Set outside any animation, because
                // every one of these is a SNAP - "the roster changing is not a
                // transition, it is a fact arriving".
                // The roster and rule are the BEAT's; the controls are the
                // beat's own state only while the stream ends in the lobby.
                arrivalStill = ArrivalStill(env: env, passing: beat.passing,
                                            controls: beat.controls == .held ? showing.env : env)
                continue
            }
            await adopt(winner: winner, env: env)
            // The live lobby's own checkbox does the turning from here - the
            // still is about to go, and the box under it is the real one.
            rulesTurn = beat.transition == .turn
                ? FCheckbox.Turn(token: turns, seconds: beat.duration) : nil
            guard beat.transition == .fade, beat.duration > 0 else {
                arrivalStill = nil
                return
            }
            // THE CROSS-FADE, as an explicit opacity rather than a `.transition`
            // on the `if let`. A removal transition is only honoured when
            // SwiftUI is left to diff the branch alone, and this branch is
            // removed in the SAME transaction that swaps the whole surface
            // underneath it (lobby -> board) - filmed at 20fps, the still
            // vanished in one frame and the "fade" was a cut. An opacity the
            // view owns is animated whatever else changes around it.
            withAnimation(.easeInOut(duration: beat.duration)) { stillFade = 0 }
            try? await Task.sleep(nanoseconds: UInt64(beat.duration * 1_000_000_000))
            arrivalStill = nil
            stillFade = 1
        }
    }

    /// THE LOBBY COMING BACK OVER THE BOARD - the lobby->board cross-fade seen
    /// from the other side, and rendered as one because the kernel calls it one
    /// (anim_plan.h's ANIM_SURFACE_LOBBY: an idiom is a fact about the change,
    /// not about its direction).
    ///
    /// The still is the LOBBY here rather than the thing being left, so it fades
    /// IN over the board instead of out of it - a board cannot be drawn into
    /// `ArrivalStill`, and it does not need to be: at full opacity the still IS
    /// the lobby the surface is about to become, so swapping the real one in
    /// underneath it is invisible. Same reason the forward fade uses an explicit
    /// opacity rather than a `.transition`: the branch is removed in the same
    /// transaction that swaps the whole surface, and SwiftUI cuts rather than
    /// animates that.
    private func fadeBackToLobby(_ beat: SurfacePlan.Beat, winner: Data,
                                 env: MessageEnvelope) async {
        guard beat.duration > 0 else {
            await adopt(winner: winner, env: env)
            return
        }
        arrivalStill = ArrivalStill(env: env, passing: env.passingAllowed, controls: env)
        stillFade = 0
        // ONE FRAME AT ZERO, so there is a `from` for the fade to come out of.
        // Setting an opacity and animating it away from that value in the same
        // update gives SwiftUI one value to render and it cuts - this is a
        // render barrier, not a duration, which is why it is a yield and not a
        // number (the timings are all the kernel's; see `holdSurface`).
        await Task.yield()
        withAnimation(.easeInOut(duration: beat.duration)) { stillFade = 1 }
        try? await Task.sleep(nanoseconds: UInt64(beat.duration * 1_000_000_000))
        // The real lobby, under a still that is already showing it.
        await adopt(winner: winner, env: env)
        arrivalStill = nil
        stillFade = 1
    }

    /// HOLD THE SURFACE until the tap the human just made has been SEEN, and
    /// only then let the caller stage its bubble - which collapses the drawer.
    ///
    /// 1.1(68), owner: "do lobby animation (fade/rotate/snap) THEN collapse. I
    /// notice that the leave snap and the collapse also seem to happen at the
    /// same time." All three lobby actions had the same shape: `await onSend`
    /// first, which stages, collapses, and waits out the host's transition
    /// before returning - so the fade, the turn and the snap were all being
    /// played into a drawer that was already moving, or after it had finished.
    ///
    /// THE LENGTH IS `plan.settle` AND NOTHING ELSE. It is the kernel's answer
    /// to "how long must this surface be looked at", which is not the same as
    /// how long its beats take: a lone roster snap has NO beats (the adopt is
    /// the snap) and still has to be read, and two chains that describe the same
    /// table settle in zero - the owner's no-op rule, which is why this can be
    /// called unconditionally. A number typed here instead would be a second
    /// timing policy that nothing compares against the first (anim_plan.h).
    ///
    /// NOTHING HERE TOUCHES THE COLLAPSE ITSELF. The tween, its curve and the
    /// box geometry are untouched and must stay so; the only thing that moved is
    /// WHEN the host is asked for the style change.
    private func holdSurface(_ plan: SurfacePlan, since began: Date) async {
        let due = began.addingTimeInterval(plan.settle).timeIntervalSinceNow
        guard due > 0 else { return }
        try? await Task.sleep(nanoseconds: UInt64(due * 1_000_000_000))
    }

    /// A WHOLE-SURFACE CHANGE THE STILL CANNOT DRAW: fade the surface out, swap
    /// it, fade it back in.
    ///
    /// `ArrivalStill` renders a LOBBY, which is all a cross-fade between two
    /// lobbies or a lobby and a board ever needs - there is always a lobby on
    /// one side of those. The New game screen is on one side of these two, and
    /// there is no second copy of it to cross-fade against. What both sides DO
    /// share is the table under them, so the change is played through it: out
    /// over half the beat, swap while nothing but felt is showing, back in over
    /// the other half. The whole thing measures exactly the beat the kernel
    /// handed over, which is the only number that matters.
    ///
    /// A nil beat is "the kernel does not call this a transition", and swaps.
    ///
    /// The swap is `async` because one of them - the rematch reversal, which
    /// puts a whole BOARD back - is `adopt`, and adopting is a trip through the
    /// kernel. It still happens in the dark half, which is the only thing the
    /// shape of this function promises.
    private func fadeSurface(_ beat: SurfacePlan.Beat?,
                             _ swap: @MainActor () async -> Void) async {
        guard let beat, beat.duration > 0 else { await swap(); return }
        let half = beat.duration / 2
        withAnimation(.easeIn(duration: half)) { surfaceFade = 0 }
        try? await Task.sleep(nanoseconds: UInt64(half * 1_000_000_000))
        await swap()
        withAnimation(.easeOut(duration: half)) { surfaceFade = 1 }
        try? await Task.sleep(nanoseconds: UInt64(half * 1_000_000_000))
        surfaceFade = 1
    }

    /// THE X ON THE STAGED BUBBLE, on a lobby surface: put the table back the
    /// way the thread still has it (1.1(68)).
    ///
    /// Owner: "while there isn't an undo for this specific scenario, it should
    /// still 'fade back' to the lobby state it was in previously if I X on the
    /// staged bubble. kinda like how X on the stage bubble undoes a staged move
    /// mid game", and then for the other two lobby actions: "if i leave, it
    /// stages a bubble. then if I X on that bubble, it should snap me back in.
    /// and same for toggling passing, it should 'unrotate'."
    ///
    /// ONE MECHANISM, AND IT IS THE ARRIVAL'S. The reversal is not a fourth
    /// thing to build: it is `surfacePlan` asked the other way round - what is
    /// on screen as `showing`, the chain the draft was started from as
    /// `arriving` - and then the same `playArrival` that plays a text. Each
    /// action therefore comes back in the idiom it was made in without anything
    /// here knowing which action it was: a rules change TURNS back, a roster
    /// change SNAPS back, a start FADES back.
    ///
    /// WHICH IS ALSO THE NO-OP RULE, for free. Owner: "if you toggle, then
    /// toggle back, then X the staged, it should detect that the resulting state
    /// is the same, and do zero animation. same if you leave then join AND END
    /// UP IN SAME ORDER IN GAME." Nothing is replayed and no tap is remembered -
    /// two chains that describe the same table diff to no beats and a zero
    /// settle, and a zero settle is the one case that returns without touching
    /// the surface at all. The pair the owner set as the test of it (leave and
    /// rejoin as the JOINER, which lands you back in the same place, against
    /// leave and rejoin as the CREATOR, which lands you behind the player who
    /// held their seat) differ in no tap at all - only in the roster - so a
    /// reversal that replayed taps would have to get one of them wrong.
    ///
    /// `StagedDraft.origin` is the state to go back to and `threadParent8` is
    /// what records it - the FIRST edit of a draft captures it and later edits
    /// of the same draft do not, so a Join, a rules move and a Start staged
    /// together revert as one, exactly as the single bubble the X deletes
    /// carried them.
    ///
    /// AND AN ORIGIN IS NOT ALWAYS A DELTA - 1.1(69), U2. A base belonging to a
    /// DIFFERENT game cannot be diffed at all: `msg_surface_delta` returns on
    /// `showing->game_id != arriving->game_id`, deliberately and correctly, so
    /// the plan comes back with no beats and a ZERO settle - which the no-op
    /// rule below then reads as "nothing changed" and returns, having already
    /// cleared the flags. The bubble is gone and the surface is stranded. A
    /// REMATCH is the case that reaches it (`createRematchLobby` mints a fresh
    /// random game id, so the result card it was created over shares no line of
    /// continuity with it), and a plain create over a thread that already held a
    /// finished game reaches it too. `StagedRevert` splits that out as the swap
    /// it is, and the kernel still says how: `surfaceSwap`, the one beat the
    /// create and its mirror already use.
    private func revertStagedSurface() async {
        let wasStaged = surfaceStaged
        let draft = stagedDraft
        // The flags go first and unconditionally: whatever happens below, this
        // surface no longer has a bubble waiting to be sent.
        surfaceStaged = false
        stagedDraft.clear()
        // ONLY WHAT THIS SURFACE STAGED. A cancel over a BOARD is the move-level
        // retraction and belongs to `MessageTableView.cancelStagedBubble`, on
        // this same token; an origin can also be left behind by a local chain
        // that never staged anything at all (`addSoloSeat`, in DEBUG). Neither
        // is a lobby action of mine waiting to be undone, and reverting to a
        // base either one recorded would put a table on screen that nobody
        // asked for.
        guard wasStaged, let origin = draft.origin else { return }
        // What is on screen, by the same reading `maybeAdoptIncoming` uses - and
        // `lastShownChain` last, because a board's own base is the truth while
        // there is one.
        let showing = controller?.basePayload ?? lobby?.payload ?? lastShownChain
        // PEEKED, NEVER DECODED. All that is wanted here is which game each side
        // names, and a decode is an ADOPTION - it would move the resident game
        // out from under the very surface being asked about, which is the
        // phantom-seal shape (see `resealFromBase`). The arm that goes on to
        // play something decodes there, once it knows it is going to.
        var showingId: String?
        if let showing, let e = try? await MessageKernel.shared.peek(payload: showing) {
            showingId = e.gameId
        }
        var baseId: String?
        if let base = origin.payload, let e = try? await MessageKernel.shared.peek(payload: base) {
            baseId = e.gameId
        }
        switch StagedRevert.route(staged: wasStaged, origin: origin,
                                  showingGameId: showingId, baseGameId: baseId) {
        case .nothing:
            return

        // THE DRAFT MADE THE GAME, so discarding it takes the game with it and
        // there is no chain to diff against - the surface goes back to the New
        // game screen it was created from. The kernel still says HOW (see
        // `surfaceSwap`): a whole surface giving way to another is a fade in
        // every direction, and this is the direction that used to be a cut.
        case .newGame:
            let plan = await MessageKernel.shared.surfaceSwap(passing: true)
            FlightRecorder.note("unstage", "the create is discarded - back to New game")
            await fadeSurface(plan.beats.first) {
                controller = nil
                lobby = nil
                lastShownChain = nil
                staleBranch = false
                showSetup = true
            }

        // TWO GAMES, NOT TWO STATES OF ONE. Nothing here is a step backwards
        // along a chain, so nothing is diffed: the surface the draft covered up
        // is simply put back, through the same fade every other whole-surface
        // change wears. `adopt` is what puts it back, because the thing being
        // restored is usually a BOARD (a rematch's result card) and `playArrival`
        // renders a LOBBY - there is no second copy of a board to cross-fade
        // against, which is exactly the argument `fadeSurface` was written for.
        case .swap(let back):
            guard let env = try? await MessageEnvelope.decode(payload: back, viewer: -1)
            else { return }
            let plan = await MessageKernel.shared.surfaceSwap(passing: env.passingAllowed)
            FlightRecorder.note("unstage",
                "a different game - swapping back to the thread's own chain")
            AnimLog.say("surface swaps a staged \(env.phase == 0 ? "lobby" : "board") back")
            await fadeSurface(plan.beats.first) {
                lobby = nil
                showSetup = false
                damaged = false
                await adopt(winner: back, env: env)
            }

        case .delta(let base):
            guard let showing,
                  let env = try? await MessageEnvelope.decode(payload: base, viewer: -1)
            else { return }
            let plan = await MessageKernel.shared.surfacePlan(showing: showing, arriving: base)
            FlightRecorder.note("unstage",
                "beats=\(plan.beats.count) settle=\(Int(plan.settle * 1000))ms")
            // THE NO-OP. Nothing changed, so nothing plays and nothing is
            // adopted: re-adopting an identical table would still cost a
            // rebuild, and a rebuild is a frame the owner asked not to see.
            // Trustworthy ONLY on this arm - a zero here really is "the same
            // table", because the two chains have already been established to
            // be the same game.
            guard plan.settle > 0 else { return }
            AnimLog.say("surface reverts a staged lobby action as \(plan.beats.count) beat(s)")
            await playArrival(plan, winner: base, env: env)
            // AND THE SEAT COMES BACK WITH ME. `leaveLobby` forgets this
            // device's seat so a later open cannot re-seat me in a lobby I
            // walked out of; reverting the leave has to put it back, or the
            // lobby I snap into offers me Join rather than the chair I am
            // sitting in.
            if env.phase == 0, let mine = lobbySeat(env) {
                cache(seat: mine, env: env, payload: base)
            }
        }
    }

    /// 1.0(6): the graceful failure screen - shown ONLY when a message fails to
    /// open (decode error / damaged), never during normal play. It gives the
    /// human a way out (New game) and dumps the full payload + versions so a
    /// recurrence can be captured by screenshot. A HARD process crash cannot show
    /// any UI; this covers the graceful "gray screen" failures the extension can
    /// still render through.
    private var diagnosticFailView: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 6) {
                Text("Couldn’t open this game").font(FType.title(18)).onTableText()
                FButton(FStrings.t("ios.msg.newgame"), kind: .wood, action: onNewGame)
                diagnosticDump
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding()
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .background(TableBackground().ignoresSafeArea())
    }

    /// The dump itself, with no framing of its own — ONE version of these
    /// fields, shown both by the failure screen above and by the hold-summoned
    /// panel below, so the two can never drift into disagreeing about what a
    /// message contains.
    @ViewBuilder private var diagnosticDump: some View {
        let hex = dumpHex
        let url = dumpURL
        Group {
            if let e = diagError { Text("ERR: \(e)").foregroundColor(.red) }
            // The version fields: FMSG format byte (byte 1), flags byte
            // (byte 2 - 0x04 = 1.0(3) passing bit), the URL text version,
            // and the replay-body ENCODING version (5/6/7 - the real
            // cross-version signal).
            if !hex.isEmpty {
                Text("VER msgFmt=0x\(hex.dropFirst(2).prefix(2)) flags=0x\(hex.dropFirst(4).prefix(2)) urlVer=\(url?.pathComponents.last?.first.map(String.init) ?? "?")")
            }
            if let c = controller {
                Text("seat \(c.mySeat) · \(c.pending.count) staged\(c.isGenesis ? " · genesis" : "")")
            }
            if !diagInfo.isEmpty { Text("opened: \(diagInfo)") }
            // THE PAYLOAD IS THE WHOLE GAME, SO IT PRINTS ONLY WHEN THE
            // GAME IS ALREADY BROKEN.
            //
            // Every envelope carries `seed[32]` (msg_wire.h), repeated by every
            // seal, and deal_rng makes the WHOLE DEAL a deterministic function
            // of it - "a whole deal is a function of one seed... reproducible
            // from a stored seed". These bytes are not this reader's view of
            // the game, they ARE the game: every opponent's hand and the order
            // of the rest of the deck. Printed as selectable text, and again as
            // a foolish.cards/m/ link, a five-second hold on the gear handed a
            // player the table face-up in a form they could paste anywhere.
            //
            // That the bytes are already ON the device is not a defence. A
            // serverless design means every client CAN compute every hand; the
            // game is honest because the client does not SHOW you what it can
            // compute, and this panel was the one place that broke that.
            //
            // But suppressing it outright would take away the thing it is FOR.
            // Owner: "I still think we should dump it if we encounter an error,
            // not not allow for cheating in release builds." So the release
            // gate is the error itself - `mayDumpPayload`. A chain that failed
            // to open is not a game anybody is playing, its bytes are what a
            // bug report needs, and there is nothing to cheat at. A chain that
            // opened fine gets the health report and the version lines and no
            // payload.
            if mayDumpPayload {
                if !hex.isEmpty {
                    Text("HEX (\(hex.count / 2) bytes):")
                    Text(hex).textSelection(.enabled)
                }
                if let u = url?.absoluteString {
                    Text("URL:")
                    Text(u).textSelection(.enabled)
                }
            }
        }
        .font(.system(size: 10, design: .monospaced))
        .onTableText()
    }

    /// The chain to dump: the one the board is ACTUALLY on, not the one the
    /// surface happened to load from.
    ///
    /// They diverge routinely. An arrival is folded into the live controller
    /// without ever becoming `selectedMessage`, so `payloadURL` - and with it
    /// the `diagHex` captured at load - still describes the bubble the human
    /// last tapped, possibly several moves ago. A dump that reports that is
    /// worse than no dump: it answers a question about the wrong message while
    /// looking authoritative. `basePayload` is the controller's own adopted
    /// chain, so it moves with every adopt. Falls back to the load-time capture
    /// when there is no controller, which is exactly the damaged case the
    /// failure screen covers.
    private var dumpHex: String {
        guard let p = controller?.basePayload else { return diagHex }
        return p.map { String(format: "%02x", $0) }.joined()
    }

    private var dumpURL: URL? {
        guard let p = controller?.basePayload else { return payloadURL }
        return MessageEnvelope.link(payload: p)
    }

    /// ROUND 12 (owner): "I know we have like a last message diagnostics view
    /// that is not enabled. How about this though - if you hold the settings
    /// button for 4 seconds, it pops up. And if you tap again it goes away."
    /// Five, on a second pass - the owner's call, and it reads as an easter egg
    /// rather than a slow tap.
    ///
    /// The dump used to be reachable only by FAILING to open a bubble, which is
    /// exactly when you cannot ask it about a bubble that opened fine. This is
    /// the same fields, on demand, over whatever is on screen.
    ///
    /// Not DEBUG-gated on purpose: its whole value is reading the real bytes of
    /// a real message on a real phone, in the build that shipped. A five-second
    /// hold on an unlabelled square is not something a player finds by accident,
    /// and what it shows is the reader's own game.
    ///
    /// It floats OVER the surface rather than replacing it, so summoning it
    /// never disturbs the board underneath: no reload, no teardown, and the
    /// staged bubble is exactly where it was when you dismiss.
    /// May this build put the raw payload on screen right now?
    ///
    /// Release: only when the surface is reporting an ERROR. See the note at
    /// the printers in `diagnosticDump` for why the bytes are a cheat and why
    /// the error case is nonetheless the one that must keep them.
    ///
    /// Debug and SOLO_TESTING: always, because that is where the bytes are
    /// read on purpose and there is no opponent to deceive.
    private var mayDumpPayload: Bool {
        #if DEBUG || SOLO_TESTING
        return true
        #else
        return diagError != nil || damaged
        #endif
    }

    private var diagnosticPanel: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 6) {
                healthDump
                Text("Last message").font(FType.title(16)).onTableText()
                diagnosticDump
                if dumpHex.isEmpty && diagInfo.isEmpty && diagError == nil {
                    // Reached from the setup screen, or after a New game tap:
                    // there is no message behind this surface to dump. Say so
                    // rather than showing an empty panel that reads as broken.
                    Text("no message open").font(.system(size: 10, design: .monospaced))
                        .onTableText()
                }
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding()
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        // Nearly opaque, not fully: enough of the board shows through to make
        // it obvious this is a panel resting on top of a game, not a screen the
        // extension has navigated to.
        .background(TableBackground().opacity(0.97).ignoresSafeArea())
        // "Tap again it goes away" - anywhere, since the panel covers the gear
        // that summoned it. `contentShape` so the gaps between the lines are
        // dismissible too, not just the text.
        .contentShape(Rectangle())
        .onTapGesture { showDiagnostics = false }
    }

    /// ROUND 16 (owner): "sometimes it just hangs. Even on newer devices. Can't
    /// tell why... if there's a crash or something make it appear as a
    /// diagnostic dump in the UI so I can check next time it happens."
    ///
    /// The trail the PREVIOUS session left, plus this one so far, plus the live
    /// footprint - see FlightRecorder for why a trail is the only possible
    /// evidence (the failure being chased kills the process outright, so nothing
    /// survives to report itself). Rendered above the message dump because when
    /// this section has something to say it is the more urgent of the two.
    @ViewBuilder private var healthDump: some View {
        let previous = FlightRecorder.previousSession()
        VStack(alignment: .leading, spacing: 6) {
            Text("Health").font(FType.title(16)).onTableText()
            if let p = previous, FlightRecorder.isAlarming(p) {
                // The one line the owner is looking for, in the accent, so it is
                // not something to be found in a wall of monospace.
                Text(FlightRecorder.verdict(p))
                    .font(.system(size: 11, weight: .bold, design: .monospaced))
                    .foregroundColor(FColor.accent)
            }
            Text(FlightRecorder.report(previous: previous,
                                       current: FlightRecorder.currentSession()))
                .font(.system(size: 9, design: .monospaced))
                .onTableText()
                .textSelection(.enabled)
            FButton("Clear", kind: .wood, compact: true) {
                FlightRecorder.reset()
                showDiagnostics = false
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }

    /// The surface, plus the STILL an arriving stream holds over it (1.1(56)).
    ///
    /// A ZStack rather than a state of `resolvedContent`'s own, because the two
    /// have to be on screen AT ONCE for the last beat: the board is built
    /// underneath while the lobby is still showing, and then the lobby fades off
    /// it. It carries its own `TableBackground` for the same reason - a
    /// transparent still would let the table read through the roster instead of
    /// covering it, and there would be nothing to cross-fade.
    @ViewBuilder private var expandedContent: some View {
        ZStack {
            resolvedContent
                .opacity(surfaceFade)
            if let still = arrivalStill {
                LobbyView(env: still.env, mySeat: lobbySeat(still.controls),
                          nickname: "", onJoin: { _ in }, onStart: {}, onInvite: {},
                          stillPassing: still.passing, controlsEnv: still.controls)
                    .background(TableBackground().ignoresSafeArea())
                    // Nothing here is live: the chain it describes has already
                    // moved on, and a tap landing on a beat would act on it.
                    .allowsHitTesting(false)
                    // A beat SNAPS in (this is set outside any animation) and
                    // fades out only when the plan's last beat says to fade.
                    .opacity(stillFade)
            }
        }
    }

    @ViewBuilder private var resolvedContent: some View {
        if let controller {
            MessageTableView(controller: controller,
                             onSend: { payload, fromUndo in await onSend(payload, controller.mySeat, fromUndo) },
                             // A finished game's New game is a REMATCH: same
                             // table, built right here from the board still on
                             // screen. Anything else - a mid-game board, a
                             // roster with an unnamed seat - is an ordinary new
                             // game and punishes nobody.
                             onNewGame: {
                                 guard let r = rematchRoster(from: controller) else {
                                     onNewGame(); return
                                 }
                                 onFreshChain()
                                 // The table's RULES carry over with the table.
                                 // A rematch is the same people playing again,
                                 // so it starts as the game they were just
                                 // playing - and the checkbox is still there to
                                 // change it before anyone starts.
                                 let passing = controller.passingAllowed
                                 Task { await createRematchLobby(joins: r.joins,
                                                                 foolSeat: r.foolSeat,
                                                                 passing: passing) }
                             },
                             onUnstage: onUnstage,
                             alsoStaged: surfaceStaged,
                             // The board runs the UNDO a cancel means; this
                             // view only clears its own `surfaceStaged` below.
                             cancelToken: cancelToken,
                             onOpenURL: onOpenURL)
                // 1.0(4) live-receive blink: a received bubble reloads the surface
                // with a NEW controller. Tying the board's identity to the
                // controller INSTANCE (not just the `if let` slot) means a reload
                // still gets a fresh board with fresh @State - so the open-move
                // replay fires exactly as before - but WITHOUT the controller ever
                // going nil, which is what flashed `Color.clear` between the old
                // board and the new one. A style toggle keeps the same controller
                // instance, so the id is stable and the in-progress board survives
                // (same guarantee as before). See reloadForInput / load.
                .id(ObjectIdentifier(controller))
                // ROUND 20: the board is read-only because a newer chain for this
                // game has already been through this device. An overlay rather
                // than a row in the stack, so the bar appearing does not
                // re-lay-out the board underneath it - a stale board is still a
                // board, and the cards must not move because it grew a caption.
                .overlay(alignment: .top) { supersededBar(controller) }
        } else if let lob = lobby {
            LobbyView(env: lob.env, mySeat: lobbySeat(lob.env),
                      nickname: MessageGameStore.shared.nicknamePrefill,
                      onJoin: { name in Task { await joinLobby(lob, nickname: name) } },
                      onStart: { Task { await startGame(lob) } },
                      onExit: { Task { await leaveLobby(lob) } },
                      onInvite: { Task {
                          await onSend(lob.payload, lobbySeat(lob.env) ?? 0, false)
                          surfaceStaged = true   // round-9: the invite awaits Send
                      } },
                      onSetPassing: { on in Task { await setLobbyPassing(lob, passing: on) } },
                      passingBaseline: passingBaseline[lob.env.gameId],
                      // nil in every shipping build: the closure only exists
                      // where `addSoloSeat` is compiled at all.
                      onAddSoloSeat: soloSeatAction(lob),
                      // Somebody else moved the checkbox and the arriving
                      // stream's rules beat has just landed (1.1(56)).
                      rulesTurn: rulesTurn)
                // The JOIN row is a name field too, and it is the screen the
                // second player lands on. `.join` is exactly "no seat yet, and
                // there is still room" (LobbyControls.offered) - the other
                // states show buttons, which work compact. See
                // expandForNameEntry.
                .onAppear {
                    if lobbySeat(lob.env) == nil,
                       lob.env.joins.count < lob.env.nPlayers { expandForNameEntry() }
                }
                // Keep the corner pair's own footprint clear - the lobby is
                // centred in whatever height it is given and the pair is an
                // overlay, so a tall lobby lays out straight through it.
                .padding(.bottom, SettingsHelpSquares.reservedHeight)
                .overlay(alignment: .bottomLeading) { settingsHelpCorner }
                // Round-9: the send reminder covers EVERY staged bubble, not
                // just board moves - a join/invite/start left unsent stalls the
                // whole thread the same way. Collapsed view only, same as the
                // board's; full-bleed container, so the screen-edge axis.
                //
                // Round-10 #2: gated on the surface's LIVE height (the same
                // collapseFraction the board uses), NOT the `style` prop -
                // present() only runs on discrete host events, so `style` goes
                // stale across a grabber drag or an auto-transition, which is
                // exactly how the arrow leaked into the EXPANDED lobby.
                .overlay(alignment: .topTrailing) {
                    GeometryReader { g in
                        StagedSendHint(staged: surfaceStaged,
                                       visible: MessageTableView.collapseFraction(height: g.size.height) > 0.95)
                            .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topTrailing)
                    }
                }
        } else if let g = nameGate {
            NameGateView(prefill: MessageGameStore.shared.nicknamePrefill) { name in
                Task { await nameThenSeat(name, gate: g) }
            }
            .onAppear(perform: expandForNameEntry)   // see expandForNameEntry
        } else if showSetup {
            // chatPlayers is threaded through unused (see NewGameSetup's own doc)
            // — kept only so this call site, the harness, and
            // MessagesViewController (which all compute a real participant
            // count) keep compiling unchanged.
            NewGameSetup(nickname: MessageGameStore.shared.nicknamePrefill,
                         isDM: chatIsDM, chatPlayers: chatPlayers) { name in
                Task { await start(nickname: name) }
            }
            .padding(.bottom, SettingsHelpSquares.reservedHeight)
            .overlay(alignment: .bottomLeading) { settingsHelpCorner }
            .onAppear(perform: expandForNameEntry)   // see expandForNameEntry
        } else if let a = ambiguous {
            SeatPicker(nPlayers: a.env.nPlayers, joins: a.env.joins) { seat in
                Task { await choose(seat: seat, from: a) }
            }
        } else if let s = spectator {
            // ROUND 20: A FINISHED GAME IS A RESULT, WHATEVER SEAT YOU HOLD -
            // including none (owner: "spectators should still be able to see win
            // screen"). Until now this branch drew the public board and the
            // "spectating" caption at every phase, so the one bubble a spectator
            // most wants to open - the last one - showed them a swept, empty
            // table and a line telling them they could not play on it.
            //
            // The SAME `FGameOverList` a player gets, ranked by the same
            // function: who came first and who was the fool is public (§10 - it
            // is on the bubble's own picture), so there is nothing here a
            // spectator may not see. `mySeat: -1` is what says "none of these
            // rows is yours", so no row is tagged (You).
            //
            // New game works from here for the same reason it works anywhere: a
            // spectator watching a table finish is exactly somebody who might
            // want to deal the next one.
            if let board = spectatorBoard {
                // ROUND 21: the last move, then the ranks - the same board a
                // player watches, seated at nobody's seat (see `spectatorBoard`).
                // `MessageTableView` owns both halves: it replays the chain's
                // final move and then gives way to `FGameOverList` itself, so
                // there is no second copy of "when does the result appear" here.
                //
                // `onSend` can never fire (nothing is sendable from a seat the
                // kernel will not compute a move for), and is written as a
                // no-op rather than a fatalError for the same reason every other
                // unreachable branch in this file is: a screen a watcher is
                // looking at must not be the thing that takes the extension down.
                MessageTableView(controller: board,
                                 onSend: { _, _ in },
                                 onNewGame: onNewGame,
                                 onOpenURL: onOpenURL)
                    .id(ObjectIdentifier(board))
            } else if s.view.isOver {
                // The board could not be built - the ranks alone, as round 20
                // left them. Never reached in practice; kept because losing the
                // result screen is a worse failure than losing the animation.
                FGameOverList(rows: MessageTableView.finishRows(s.view, names: s.names, mySeat: -1),
                              onNewGame: onNewGame,
                              replayURL: spectatorReplayURL,
                              onOpenURL: onOpenURL)
            } else {
                // Release-only §6.3 fallback: read-only, public (no hand), with a
                // caption explaining why there is nothing to tap (§ release security).
                VStack(spacing: 4) {
                    MessageBoardView(view: s.view, names: s.names)
                    // Round-5 M10: full-opacity ink + a LIGHT shadow, not 55%
                    // black — the busy wool weave has no fixed-opacity foreground
                    // that survives it - full-opacity ink and a LIGHT shadow,
                    // never 55% black.
                    // Round-6 #17 added the weight: `onTableText` (Tokens.swift).
                    Text(FStrings.t("ios.msg.spectating"))
                        .font(.footnote).onTableText()
                        .multilineTextAlignment(.center).padding(.horizontal).padding(.bottom, 8)
                }
            }
        } else if damaged || diagError != nil {
            // 1.0(6): a message that FAILS to open shows a graceful diagnostic
            // (the full payload/version dump + New game) instead of a gray screen.
            // This branch is reached ONLY on a real decode failure - normal play,
            // and the transient reload below, never show it.
            diagnosticFailView
        } else {
            // 1.0(4) live-receive blink: while a received bubble reloads the
            // surface (controller briefly nil), a ProgressView spinner flashed
            // over the wool for a frame or two - the "slight blink". The reload is
            // sub-frame in the common case, so show the steady wool (Color.clear
            // over GameSurface's TableBackground) instead of a spinner that
            // announces the reload. NOTE: the board still tears down and remounts
            // on a live receive (that remount is what drives the incoming-move
            // replay off the view nil->value transition); removing the remount
            // entirely needs frame-by-frame harness verification, tracked
            // separately, since it would otherwise kill that replay.
            Color.clear
        }
    }

    /// The Settings + Rulebook squares on the setup and lobby screens (owner
    /// ask, durak-rules-redesign): the SAME 40pt pair the board floats
    /// bottom-left (`SettingsHelpSquares`), at the same corner inset — 4 outer
    /// + the pair's own FSpace.m inner = 16pt off the edge, exactly the board's
    /// line — so Settings and the rules are reachable before a game exists at
    /// all. Round-9 (owner: "we need to bring them back"): shown in EVERY
    /// presentation style — the old expanded-only gate meant the pair was
    /// invisible in the compact drawer, which is where the extension actually
    /// opens, so in practice it read as removed.
    private var settingsHelpCorner: some View {
        SettingsHelpSquares(onSettings: { showSettings = true },
                            onHelp: { showRules = true })
            .padding(.leading, 4)
            .padding(.bottom, 4)
    }

    /// ASK THE HOST TO EXPAND, BECAUSE A NAME FIELD CANNOT WORK COMPACT.
    ///
    /// Compact IS the keyboard's area (§3.5, and `NewGameSetup`'s own doc says
    /// so): a `TextField` drawn there can never become first responder, because
    /// there is nowhere for the keyboard to go. Every name screen in this
    /// extension is therefore only usable EXPANDED - but nothing ever asked to
    /// be. `requestExpand` has been threaded down from MessagesViewController
    /// since M1 and was never called anywhere in FoolishKit (HarnessRootView's
    /// own comment says as much); expansion was entirely host-driven, i.e. it
    /// happened only if the player happened to drag the grabber.
    ///
    /// The result was a dead end on the app's FIRST screen: tap Foolish in the
    /// drawer, get "New game" with a nickname field and a disabled "Enter
    /// Nickname" button, and neither the field nor the button responds. The
    /// same dead end sat on the JOIN row, which is the screen the second player
    /// - and an App Store reviewer's second device - lands on.
    ///
    /// Unconditional on purpose. `style` goes stale across a grabber drag or an
    /// auto-transition (see the note in `expandedContent`), so testing it would
    /// re-introduce the bug in exactly the case that matters; requesting
    /// `.expanded` while already expanded fires no transition and is a no-op
    /// (MessagesViewController's `onNewGame` relies on the same property).
    private func expandForNameEntry() {
        // ROUND 46: only when the field will be EMPTY. The 2.1 fix made this
        // unconditional, which was right for the dead end it cured (a field you
        // cannot focus in compact) but wrong for the common case: a device that
        // already knows its name shows one filled-in field and a button, which
        // reads fine compact, and taking the conversation over to show it is a
        // worse first impression than leaving the drawer alone. `needsNameEntry`
        // is the same predicate the autofocus uses, so the drawer and the
        // keyboard cannot disagree about whether a name is owed.
        guard MessageGameStore.shared.needsNameEntry else { return }
        requestExpand()
    }

    /// Reset + (re)load for a NEW input. A compact<->expanded toggle leaves
    /// loadKey unchanged, so `.task(id:)` does not fire and the game persists.
    private func reloadForInput() async {
        AnimLog.say("surface reload key=[\(loadKey)]")
        // WHAT THE HUMAN WAS LOOKING AT, read before the reset below throws it
        // away. An arrival landing during a reload has to be diffed against
        // THIS, not against whatever the reload settles on - see `reloading`.
        let wasShowing = controller?.basePayload ?? lobby?.payload ?? lastShownChain

        // A RELOAD ONTO THE ARRIVING CHAIN IS NOT A RELOAD - IT IS THE ARRIVAL.
        //
        // This file has asserted since round 7 that "Apple does not move
        // `selectedMessage` for an arrival, so loadKey does not change and the
        // .task above will not re-run". ON A REAL DEVICE THAT IS FALSE. The
        // owner's 1.1(60) flight log says so in three places, and says it the
        // same way every time:
        //
        //     22.26s  receive
        //     22.26s  reload-vs-arrival
        //
        // `loadKey` is newGameToken | chatKey | payloadURL; the first two cannot
        // move on a receive, so `payloadURL` did - the arriving bubble HAD become
        // the selection. So every arrival was really a cold reload onto the new
        // chain, which is a PAINT, which is a snap by construction; and
        // `maybeAdoptIncoming` then found `bytes == current` and returned without
        // a sound. That is why no lobby beat has ever played on a phone in 57,
        // 58, 59 or 60, while the same stream animates correctly in the harness -
        // the harness delivers an arrival WITHOUT moving the selection, which is
        // the one thing the real host does differently.
        //
        // So the two are told apart by their bytes rather than by a belief about
        // what the host does with the selection: if the chain this reload would
        // load IS the chain that just arrived, and it is not already what we are
        // showing, then this is an arrival and it plays as one - diffed against
        // what was on screen, which is exactly the sequence the human was owed.
        if let url = incomingURL,
           let arriving = try? MessageEnvelope.payloadBytes(url: url),
           let loading = payloadURL.flatMap({ try? MessageEnvelope.payloadBytes(url: $0) }),
           arriving == loading, let was = wasShowing, was != arriving {
            FlightRecorder.note("reload-is-arrival", "\(arriving.count)b")
            await maybeAdoptIncoming(showingBefore: was)
            return
        }
        if incomingURL != nil { FlightRecorder.note("reload-vs-arrival") }
        reloading = true
        // Do NOT tear the board down to nil up front: on a live receive that
        // blank (Color.clear) between the old controller and the new one is the
        // "blink". Reset only the NON-board transient screens here; the resolved
        // screen sets `controller` - a fresh instance for a board (the `.id` in
        // expandedContent gives it fresh @State), or nil in the branches below
        // that show something other than a board.
        lobby = nil; nameGate = nil; showSetup = false
        ambiguous = nil; spectator = nil; spectatorReplayURL = nil
        spectatorBoard = nil; damaged = false
        // A held beat belongs to the surface it was played over (1.1(56)). A new
        // input is a different surface, so it goes - and without an animation,
        // since there is nothing left underneath for it to fade into.
        arrivalStill = nil; stillFade = 1; rulesTurn = nil
        surfaceStaged = false   // round-9: a new input owes nothing to Send yet
        stagedDraft.clear()
        arrivalTaken = nil
        await load()
        AnimLog.say("surface showing \(showingWhat)")
        // AND THEN RE-OFFER THE ARRIVAL, if one is outstanding. 1.1(57).
        //
        // `load()` shows what `payloadURL` names, and an arrival is NEVER that:
        // a received bubble does not become `selectedMessage`, which is the whole
        // reason `incomingURL` exists as a separate channel. So a reload that
        // happens to run alongside `maybeAdoptIncoming` shows the OLD chain, and
        // whichever of the two finishes last wins the surface. The owner, on a
        // lobby he had just created: "the extension that was open didn't update
        // at all… although SOMETIMES it would update on live arrival" - a
        // coin-flip is what an unsequenced race looks like from the outside.
        //
        // Rather than hunt every input that can move `loadKey` under an arrival
        // (`startNewGame` was one and is gone; `StagedBubbleRouting` can move
        // `payloadURL` as the staged/just-sent markers are spent), this makes the
        // order not matter: a reload ENDS by handing the arrival back to the one
        // function that knows how to weigh it. Rule P still decides, so a stale
        // or duplicate arrival changes nothing, and an arrival already adopted
        // returns immediately on the `bytes == current` check.
        //
        // It cannot recurse: adopting changes `lobby`/`controller`, and none of
        // those is an input to `loadKey`.
        reloading = false
        if incomingURL != nil { await maybeAdoptIncoming(showingBefore: wasShowing) }
    }

    /// THE PARENT A STAGED DRAFT KEEPS, across every edit made to it.
    ///
    /// `conversation.insert` REPLACES an unsent draft rather than queueing a
    /// second one, so a human who taps the rules checkbox and then Exit sends ONE
    /// bubble - and until now that bubble named the intermediate rules chain as
    /// its parent. That chain was never sent. It exists on no other device, so
    /// Rule P's rule 4 ("a chain's own DIRECT CHILD outranks it") could not fire
    /// on the receiver, and the comparison fell through to rule 3, "the fuller
    /// roster wins the turn-0 tie" - which a LEAVE loses by construction, because
    /// leaving is what makes a roster smaller.
    ///
    /// So the arrival lost to the chain already on screen and was discarded in
    /// silence. Owner, 1.1(61): "a passing toggle + exit (which is fine, just the
    /// other order isn't allowed) DID NOT update the view", and his read of it -
    /// "makes me wonder if its something to do about game membership" - is
    /// exactly right: it is the membership COUNT, spent as a tiebreak.
    ///
    /// Proved in C rather than argued: with the intermediate as parent,
    /// `msg_rule_p(showing, arriving)` is -1 (showing wins, arrival ignored);
    /// with the thread's own chain as parent it is +1 (adopted). Same two
    /// chains, same rosters, one field.
    ///
    /// Rule P is not the thing to change here - rule 3 is right for what it is
    /// for, picking between two Starts dealt from different lobbies. What was
    /// wrong is the CLAIM the draft made about its own ancestry. A draft that
    /// replaces itself on the way out is still one link in the thread, so it
    /// names the link the thread actually has, for every edit made to it.
    /// …and WHERE AN X GOES BACK TO, which is the other half of the same fact.
    /// Both live in `StagedDraft` (FoolishKit/Messages/StagedDraft.swift), which
    /// also holds the record-once rule that relates them - it was inline here
    /// until 1.1(69), which is why nothing could test it and U11 sat in it: the
    /// rule was keyed on the PARENT being recorded, and a create records an
    /// origin while having no parent at all, so the next edit of the same draft
    /// overwrote the create's `.noGame` with the lobby the create had just made.
    @State private var stagedDraft = StagedDraft()

    /// The parent8 for a lobby reseal - one edit of the staged draft. See
    /// `StagedDraft.parent8(staged:digest:threadChain:)` for the rule.
    private func threadParent8(_ env: MessageEnvelope) -> Data {
        stagedDraft.parent8(staged: surfaceStaged, digest: env.digest,
                            threadChain: lastShownChain)
    }

    /// THE LAST CHAIN THIS SURFACE ACTUALLY SHOWED, and the only thing here that
    /// a reset does not clear.
    ///
    /// `maybeAdoptIncoming` has to answer "what is this arrival a change FROM",
    /// and it reads that from what is on screen RIGHT NOW - `controller` or
    /// `lobby`. Both are transient: a reload nils them and then awaits, screens
    /// swap, branches come and go. Every one of those windows turns the question
    /// into "a change from nothing", and a plan asked with nothing to compare
    /// against diffs the arrival with ITSELF and answers "no beats" - which is
    /// indistinguishable, at the call site and on screen, from "this arrival
    /// genuinely has nothing to animate". That is how the rotate and the fade
    /// went missing in 1.1(57), (58) and (59) while every test stayed green: the
    /// beats were never computed, so there was nothing to fail.
    ///
    /// So the surface remembers, separately from what it is currently drawing.
    /// It is written wherever a chain goes on screen and nowhere else, so the
    /// worst it can be is one chain stale - which still yields a CORRECT plan,
    /// because a plan against a slightly older chain is exactly the stale-surface
    /// case the kernel already handles (msg_wire.h: the diff is against what is
    /// on screen, and a gap replays as one sequence).
    @State private var lastShownChain: Data?

    /// A RELOAD OWNS THE SURFACE WHILE IT RUNS. 1.1(59).
    ///
    /// `reloadForInput` clears `lobby` and then awaits `load()`, so for the
    /// length of that await the surface HAS no chain - and `maybeAdoptIncoming`
    /// reads exactly that to decide what an arrival is a change FROM. An arrival
    /// landing in the window computed its plan as `surfacePlan(showing: bytes,
    /// arriving: bytes)`: a diff of a chain against itself, which is zero beats,
    /// which is a plain adopt.
    ///
    /// That is not a missing animation, it is the animation being computed
    /// against nothing, and it degrades SILENTLY - the roster still arrives, so
    /// a join (which is a snap anyway) looks perfect while a rules change and a
    /// Start lose their rotate and their fade. Owner on 1.1(58): "incoming join
    /// looks good, is a simple snap. incoming pass toggle unfortunately is a
    /// snap too, not a rotate. incoming start game is also a snap."
    ///
    /// So the two are sequenced instead of racing. A concurrent call defers, and
    /// the reload hands the arrival back itself once the surface has settled -
    /// with `wasShowing`, so the beats are computed against the chain that was
    /// really on screen rather than against the one the reload happened to land
    /// on. `showingBefore` is what distinguishes that call from the racing one.
    @State private var reloading = false

    /// What the surface resolved to, for the trace. "Why is it showing a lobby
    /// when the thread is mid-game" is only answerable if the surface says which
    /// branch it took and off which bytes.
    private var showingWhat: String {
        if controller != nil { return "board" }
        if let l = lobby { return "lobby(joins=\(l.env.joins.count) phase=\(l.env.phase) game=\(l.env.gameId))" }
        if nameGate != nil { return "nameGate" }
        if showSetup { return "setup" }
        if ambiguous != nil { return "seatPicker" }
        if spectator != nil { return "spectator" }
        if damaged { return "damaged" }
        return "nothing"
    }

    /// Ask the router what to show, then put it on screen. The DECISION —
    /// setup vs lobby vs board, and which chain wins Rule P — is not made here
    /// any more (MessageSurfaceRouter): it is a function of the selected
    /// bubble, this chat's cache, and the New-game intent, so it can be driven
    /// in a test without a simulator. What stays here is the part that genuinely
    /// needs the host: seat identity (§6) and the name gate.
    private func load() async {
        AnimLog.say("surface load url=\(payloadURL?.absoluteString.suffix(12) ?? "nil") startNew=\(startNewGame)")
        diagError = nil; diagInfo = ""   // 1.0(6) diagnostic
        #if DEBUG || SOLO_TESTING
        // Dev hook (owner: "use build flags to skip the create game / join game /
        // start game stuff and jump straight to the game state"). With a
        // `dev.fatboard` file in the App Group, this chain IS the surface: no
        // setup screen, no lobby, no Start, seated as the DEFENDER so the very
        // first tap can be Pickup. Compiled out of every Release build; the
        // chain itself is searched offline by `msg_wire_test --fatboard` — see
        // MessageDevBoard for why it is a constant and not a search.
        if await openSeededBoard() { return }
        #endif
        var incoming: Data?
        if let url = payloadURL {
            do { incoming = try MessageEnvelope.payloadBytes(url: url) }
            catch { diagError = "payloadBytes: \(error)"; damaged = true; return }
        }
        // 1.0(6): the raw envelope bytes (the iMessage format, header + body).
        diagHex = incoming.map { $0.map { String(format: "%02x", $0) }.joined() } ?? ""
        let screen = await MessageSurfaceRouter.resolve(payload: incoming,
                                                        startNewGame: startNewGame,
                                                        chatKey: chatKey)
        AnimLog.say("surface router -> \(screen)")
        // reloadForInput no longer clears `controller` up front (blink fix), so a
        // resolution that is NOT a board must clear the old one itself, or the
        // stale board would win expandedContent's `if let controller` over the
        // lobby/setup/damaged screen.
        switch screen {
        case .setup:
            controller = nil
            showSetup = true
        case .damaged:
            controller = nil
            damaged = true
        case .lobby(let payload):
            controller = nil
            // Decoding also ADOPTS, so the lobby's locked seed is resident for a
            // join/start seal — same as before this was routed.
            guard let env = try? await MessageEnvelope.decode(payload: payload, viewer: -1) else {
                damaged = true
                return
            }
            noteRulesBaseline(env)
            lobby = Lobby(env: env, payload: payload); lastShownChain = payload
        case .board(let payload):
            let env: MessageEnvelope; let bodyVer: Int
            do { (env, bodyVer) = try await MessageKernel.shared.decodeWithBodyVersion(payload: payload) }
            catch { diagError = "board decode: \(error)"; damaged = true; return }
            diagInfo = "phase \(env.phase) turn \(env.turn) round \(env.round) n \(env.nPlayers) actor \(env.lastActorSeat) game \(env.gameId) joins \(env.joins.count) · bodyVer=\(bodyVer)"
            await adopt(winner: payload, env: env)
        }
    }

    #if DEBUG || SOLO_TESTING
#if RIG_RESEED
    /// DEV ONLY (`dev.reseed`): let a LIVE appex pick up a new seed.
    ///
    /// Inert unless the flag file exists - the guard is read once, so a DEBUG
    /// run without it never starts a loop and never touches the filesystem
    /// again. Release has neither: the whole block is `#if DEBUG`.
    ///
    /// Why it exists: `claimSeededPayload()` is once per process, and the only
    /// thing that ends an appex process is leaving the thread. So a rig that
    /// wants the next board has to leave, blind-probe a conversation row to get
    /// back in, and re-open - about ten seconds of simulator driving for what
    /// is really a file write. This watches the flag instead.
    ///
    /// `openSeededBoard()` is self-sufficient (it clears setup and lobby,
    /// stages if `dev.stage`, and seats the board), so re-entering it is the
    /// whole of the reload. Under `.task` it is cancelled with the surface, so
    /// it cannot outlive what it is driving.
    private func watchForReseed() async {
        guard MessageDevBoard.reseeds else { return }
        AnimLog.say("dev.reseed: watching for a new seed")
        while !Task.isCancelled {
            try? await Task.sleep(nanoseconds: 150_000_000)
            if Task.isCancelled { return }
            guard MessageDevBoard.hasUnclaimedReseed else { continue }
            _ = await openSeededBoard()
        }
    }
#endif

    /// DEV ONLY (`dev.fatboard`): open a canned chain directly, as its defender.
    /// Returns true when it took over the surface, so `load()` stops.
    ///
    /// The seat is the DEFENDER's, resolved from the chain rather than from the
    /// seat cache or the picker: this board exists to be picked up from, and
    /// only the defender may do that. That is the "seat yourself as defender"
    /// half of the owner's instruction, done for you.
    private func openSeededBoard() async -> Bool {
        guard let payload = MessageDevBoard.claimSeededPayload() else { return false }
        guard let env = try? await MessageKernel.shared.decode(payload: payload, viewer: -1),
              let view = await MessageKernel.shared.residentView(viewer: -1),
              view.defender >= 0 || view.isOver else {
            AnimLog.say("dev.fatboard present but not a decodable chain - ignoring")
            return false
        }
        // `dev.seat` overrides the chair. Default is the defender's (only they
        // may pick up); the deal case wants an ATTACKER, since it is an attacker
        // saying good that closes the bout and deals.
        // A FINISHED chain (the `endgame` board, for verifying the fool's
        // penalty) has no defender to sit at, so it defaults to seat 0.
        let seat = MessageDevBoard.seededSeat.map { max(0, min($0, view.players.count - 1)) }
            ?? (view.defender >= 0 ? view.defender : 0)
        AnimLog.say("dev.fatboard: seating as \(seat) (defender=\(view.defender)), \(view.battles.count) battles")
        showSetup = false
        lobby = nil
        // `quietOpen`: this is a seeded state, not a move anyone just watched -
        // opening it must not replay whatever its last action happened to be, or
        // the film starts with an animation nobody asked for. Unless the REPLAY
        // is the point (round 16's bubble delta: `dev.replay`), in which case
        // this opens exactly as a tapped bubble does.
        // `dev.stage`: put THIS board in the transcript too, so a photograph's
        // last bubble is the board underneath it rather than a leftover from
        // another game. Ordinary stage path, ordinary bubble; the rig presses
        // Send.
        //
        // BEFORE `seatOnBoard`, and that order is the whole point. `stage` reads
        // the payload back through `MessageSummary.forStagedBubble` -> one
        // `publicRead` - and a read of the resident slot is only as good as what
        // last wrote it. `seatOnBoard` rebuilds that slot for the BOARD's sake
        // (a quiet open, a replay, a seat), and a read taken after it came back
        // with an event window from earlier in the game: a four-bubble chain
        // photographed as "SEATONE attacks with 8 of C / SEATZERO covers 10 of C
        // with 8 of S / SEATONE attacks with 8 of C" over entries that were
        // really cover 8S, attack 9C, cover 7S. Two bubbles of a transcript
        // showing the SAME sentence is not a sentence bug - no two consecutive
        // moves can read alike - and it was chased as one for three shoots.
        //
        // Staged first, the read happens in a freshly launched appex whose
        // resident slot this payload is the only writer of, which is exactly the
        // situation a real device stages in: seal, then describe what you sealed.
        if MessageDevBoard.seededStages {
            await onSend(payload, seat, false)
        }
        seatOnBoard(seat: seat, env: env, winner: payload,
                    quietOpen: !MessageDevBoard.seededReplays)
        return true
    }
    #endif

    /// DEV ONLY (HARNESS_AUTOGAME): press the setup/lobby buttons a human would,
    /// so an unattended run can actually reach a board. Lobby v3 put three human
    /// taps — Create game, Join, Start — between launch and a dealt game, and the
    /// harness's auto-play only knows how to make MOVES, so an auto-run just sat
    /// on the setup screen forever and the animation trace it exists to produce
    /// was four lines long. Each participant's turn through here does the one
    /// thing that seat can do; HARNESS_AUTOGAME's own deliver+become carries it
    /// to the next. Never compiled into Release.
    ///
    /// Runs INSIDE `.task(id: loadKey)`, not as a Task of its own, and that is
    /// load-bearing: a detached one outlives the surface that started it. The
    /// first version was detached, and its 400ms sleep regularly finished after
    /// the harness had already switched to the next participant — so a joiner's
    /// pending drive ran with the PREVIOUS player's captured lobby and started
    /// the game as them. An 8-player run reached a 2-player board with a seat
    /// nobody at that keyboard held. Under `.task` it is cancelled with the
    /// surface, so a stale drive cannot act at all.
    ///
    /// It also waits for the lobby to FILL. Starting at two is what a human may
    /// do, but an auto-run that does it turns "8 players" into a 2-player game
    /// and never exercises the seat count being asked about.
    private func autoDriveLobby() async {
        #if DEBUG
        guard ProcessInfo.processInfo.environment["HARNESS_AUTOGAME"] != nil else { return }
        try? await Task.sleep(nanoseconds: 400_000_000)
        if Task.isCancelled { return }
        if showSetup { await start(nickname: MessageGameStore.shared.nickname); return }
        guard let lob = lobby else { return }
        // The lobby's capacity is the WIRE's max (8) for a group, not how many
        // people are in the chat — so the target is the chat's own size.
        let target = min(lob.env.nPlayers, max(2, chatPlayers))
        if lobbySeat(lob.env) == nil {
            if lob.env.joins.count < lob.env.nPlayers {
                await joinLobby(lob, nickname: MessageGameStore.shared.nickname)
            }
        } else if lob.env.joins.count >= target {
            // Calls startGame() directly — bypasses LobbyControls.offered's
            // round-5 M9 gate (that gate only governs the UI's Start
            // button). Fine here: this is a scripted driver racing to a
            // dealt board for a screenshot, not a human who could be locked
            // out of one.
            await startGame(lob)
        }
        #endif
    }

    // MARK: the fool's penalty (Rule F)

    /// The rematch roster, read STRAIGHT OFF the finished board: the same table,
    /// in the same cycle, rotated so this device sits at seat 0. nil when this
    /// is not a game a rematch can be built from.
    ///
    /// Rotated because seat 0 is the creator's by construction (`createWaiting`)
    /// and whoever taps New game is the creator. Preserving the CYCLE is what
    /// matters, not the numbers - the wire keys a roster rotation-canonically
    /// for exactly this reason - so the same table comes back as the same
    /// table however it is spun.
    ///
    /// My own name comes from the store, not from the old game's join: this
    /// device may have been renamed since, and the name it seals now is the one
    /// its seat will be recognised by.
    private func rematchRoster(from controller: MessageTurnController)
        -> (joins: [MessageJoin], foolSeat: Int)? {
        guard let v = controller.view, v.isOver, v.gameOver >= 0 else { return nil }
        let n = v.players.count
        let me = controller.mySeat
        guard n >= 2, me >= 0, me < n, v.gameOver < n else { return nil }

        // Names BY SEAT. A seat with no name cannot be recognised by its owner
        // on the other device (SeatIdentity.seatClaimedByName is what lets a
        // prefilled lobby seat people who never tapped Join), so a roster
        // missing one is not a rematch roster at all - the tap falls back to an
        // ordinary new game rather than seating somebody as a blank.
        var joins: [MessageJoin] = []
        for s in 0..<n {
            let old = (s + me) % n
            let name = old == me ? MessageGameStore.shared.nickname : (controller.names[old] ?? "")
            guard !name.isEmpty else { return nil }
            joins.append(MessageJoin(seat: s, name: name))
        }
        return (joins, (v.gameOver - me + n) % n)
    }

    /// "New game" on a FINISHED board: create the rematch lobby HERE, from the
    /// game still on screen, and stage it. No intent is written down and
    /// nothing is read back - the roster, the fool and my seat are all in hand
    /// at the moment of the tap, and a cache of them would only be a second
    /// place for them to be wrong.
    ///
    /// It also means no teardown: the ordinary New game path bumps
    /// `newGameToken`, which re-ids this whole view and routes through the name
    /// prompt. A rematch has nothing to ask - this device just played a game
    /// under its name - so it goes straight to a lobby. `onFreshChain` is the
    /// one thing it still needs from the host: start a NEW MSSession, so the
    /// rematch's first bubble does not collapse the result card of the game it
    /// came from.
    ///
    /// The lobby stays OPEN at the usual capacity, deliberately: someone else
    /// in the chat may join a rematch, and if they do, the wire's guard sees a
    /// roster that no longer keys equal and the penalty does not fire. That is
    /// the owner's "if the players do not change at all".
    ///
    /// `passing` is the finished game's own rule, carried across: `newGame`
    /// resets the kernel's rules to the classic transfer game, so a rematch of a
    /// podkidnoy table would otherwise silently deal a perevodnoy one. The
    /// lobby's checkbox is still live - this sets where it STARTS, not what it
    /// must be.
    private func createRematchLobby(joins: [MessageJoin], foolSeat: Int,
                                    passing: Bool) async {
        var seed = Data(count: 32)
        for i in 0..<32 { seed[i] = UInt8.random(in: 0...UInt8.max) }
        let gameId = UInt64.random(in: 1...UInt64.max)
        let capacity = max(chatIsDM ? 2 : 8, joins.count)
        do {
            try await MessageKernel.shared.newGame(seed: seed, players: capacity)
            await MessageKernel.shared.setPassing(passing)
            let armed = await MessageKernel.shared.armRematchCarry(joins: joins,
                                                                   foolSeat: foolSeat)
            AnimLog.say("rematch lobby: n=\(joins.count) fool@\(foolSeat) armed=\(armed)")
            let payload = try await MessageKernel.shared.seal(
                phase: 0, lastActorSeat: 0, gameId: gameId,
                parent8: Data(repeating: 0, count: 8), joins: joins)
            let env = try await MessageEnvelope.decode(payload: payload, viewer: -1)
            showSetup = false
            damaged = false
            cache(seat: 0, env: env, payload: payload)
            // The finished game is what an X goes back to here - the result
            // card, not a dead lobby - and unlike the create there IS a chain
            // for it.
            //
            // BUT IT IS A DIFFERENT GAME, which is U2 and what 1.1(68) missed:
            // the id above is freshly random, so the result card and this lobby
            // share no line of continuity and `msg_surface_delta` will not diff
            // them (it returns on the game-id mismatch, deliberately). Recording
            // the chain is still right - that IS where the X goes - but the
            // reversal has to play it as a whole-surface SWAP rather than as a
            // delta, which is `StagedRevert`'s job and no longer this one's.
            stagedDraft.created(from: lastShownChain.map(StagedOrigin.chain) ?? .noGame)
            let plan = await MessageKernel.shared.surfaceSwap(passing: passing)
            let began = Date()
            await fadeSurface(plan.beats.first) {
                controller = nil
                lobby = Lobby(env: env, payload: payload); lastShownChain = payload
            }
            await holdSurface(plan, since: began)
            await onSend(payload, 0, false)
            surfaceStaged = true
        } catch {
            damaged = true
        }
    }

    // MARK: creation + lobby (§5.2)

    /// Finish the New game setup: persist the nickname (B3), then create a
    /// lobby — every chat shape now goes through the SAME lobby machinery
    /// (lobby v3, note 2: "2p — creator creates the game and sends the first
    /// chat. The other player can join, or do join+start... the same hand
    /// because the seed was set by the first chat the creator sent"). A DM
    /// used to deal LIVE straight to the board here (`startGenesis`, now
    /// removed) — that let the creator see their hand before committing and
    /// reroll by tapping New game until it was good; a locked-seed lobby
    /// closes that.
    private func start(nickname: String) async {
        // Round-5 B1: NewGameSetup only calls this from its `.ok` branch, so
        // `nickname` is already NicknameGate-valid and trimmed — the "You"
        // fallback that used to live here is unreachable now. Re-check
        // defensively anyway (never trust a caller's promise past the type
        // system) and, per M2, fall back to the STORED nickname rather than
        // a placeholder if it somehow is not: skipping the write below just
        // leaves whatever this device already had on file.
        if case .ok(let name) = NicknameGate.check(nickname) {
            MessageGameStore.shared.nickname = name
        }
        // THE SETUP SCREEN GOES WITH THE SWAP, not before it. It used to be
        // dropped here and the lobby appear a few awaits later - a cut with a
        // blank frame in the middle of it, which is only invisible because both
        // screens sit on the same felt. `createWaiting` now fades between them
        // (see `fadeSurface`), and a fade needs the outgoing screen to still be
        // there when it starts.
        await createWaiting(nickname: MessageGameStore.shared.nickname)
    }

    /// Create a game as seat 0 and open its lobby (lobby v3): lock the seed +
    /// game id in NOW — that is the whole "seed locked at create" guarantee —
    /// and seal a WAITING bubble seating only me. The kernel is dealt at the
    /// lobby's CAPACITY, not a chosen player count: nobody has picked how many
    /// will play yet. For a group chat that capacity is the wire's max, 8 (a
    /// WAITING envelope with n_players==8 renders as an open lobby, not 8
    /// literal seats — see LobbyView) — not a real 8-player game. A DM's
    /// capacity is 2 (note 2): the chat has exactly two people, so "lobby
    /// full" must read correctly once the one possible opponent has joined,
    /// not "waiting for 6 more". Start (below) later re-derives the SAME seed
    /// at however many actually joined. Auto-stages the invite (notes 14/16):
    /// the human still presses Messages' own Send, but there is no separate
    /// "Send invite" button offering the same action a second time.
    private func createWaiting(nickname: String) async {
        var seed = Data(count: 32)
        for i in 0..<32 { seed[i] = UInt8.random(in: 0...UInt8.max) }
        #if DEBUG
        // Dev hook (owner: "a fixed seed, enabled by some flags - dev build
        // flags only"): a `dev.seed` file in the App Group container pins the
        // genesis deal, so a verification run can choose a deal where the
        // CREATOR opens the bout instead of hoping. Seed 3 is such a deal at
        // 2 players. A file, not a UserDefaults key: `defaults write` from
        // outside lands in the wrong domain and cfprefsd caches group prefs
        // until a reboot. Compiled out of every Release build.
        if let n = MessageDevBoard.genesisSeed {
            seed = Data(repeating: n, count: 32)
        }
        #endif
        let gameId = UInt64.random(in: 1...UInt64.max)
        let capacity = chatIsDM ? 2 : 8
        do {
            try await MessageKernel.shared.newGame(seed: seed, players: capacity)
            let joins = [MessageJoin(seat: 0, name: nickname)]
            let payload = try await MessageKernel.shared.seal(
                phase: 0, lastActorSeat: 0, gameId: gameId,
                parent8: Data(repeating: 0, count: 8), joins: joins)
            let env = try await MessageEnvelope.decode(payload: payload, viewer: -1)
            cache(seat: 0, env: env, payload: payload)
            // WHAT AN X GOES BACK TO: the New game screen this was created
            // from, always. A create seals with a zero parent - a new chain has
            // no ancestry to claim - so it never goes near `threadParent8`,
            // which is the only other writer of this.
            //
            // UNCONDITIONALLY `.noGame` since 1.1(69). It used to fall back to
            // whatever chain the surface last showed, which in a thread that
            // already held a finished game is a DIFFERENT game - so the X
            // reverted to a chain the kernel refuses to diff against, which is
            // the same dead end as U2's rematch. The destination was never in
            // doubt either way: a create is reached from the New game screen,
            // and that is the screen it is discarded back to. A rematch is
            // reached from a result card, and records that instead.
            stagedDraft.created(from: .noGame)
            // …AND IT ARRIVES AS A FADE, like every other whole-surface change.
            // This edge and its mirror were the only two that cut.
            let plan = await MessageKernel.shared.surfaceSwap(passing: env.passingAllowed)
            let began = Date()
            await fadeSurface(plan.beats.first) {
                showSetup = false
                lobby = Lobby(env: env, payload: payload); lastShownChain = payload
            }
            await holdSurface(plan, since: began)
            await onSend(payload, 0, false)
            surfaceStaged = true   // round-9: the created lobby awaits Send
        } catch {
            damaged = true
        }
    }

    /// My seat in a lobby, or nil if I have not claimed one yet (§6). Note 14:
    /// gated through `SeatIdentity.resolveInLobby`, not the plain `resolve` the
    /// live board uses — see that function's doc for the bug this closes (a
    /// stale lobby bubble granting Start/Send to a seat it doesn't list, and
    /// the flip side, a fresh join not showing as joined). This used to add
    /// that note 15's Rule-P-for-lobbies fix in `load()` showed the NEWEST
    /// bubble here in the first place; round 7 removed that (and its cache),
    /// so what arrives here is exactly the bubble that was tapped, stale or
    /// not - which is precisely why the membership gate below has to hold.
    ///
    /// Bubble-anchored lookup (`seatForBubble`): this env came off a real
    /// bubble, whose gameId identifies my seat even after a group-membership
    /// change re-keyed the chat. `recordedName` extends note 14's membership
    /// gate by name: a lobby carrying someone ELSE's name at my cached seat is
    /// a claim race this device lost - nil here brings the Join button back so
    /// I re-claim the next free seat instead of squatting on theirs.
    ///
    /// The name is the ROW's (`claimName`), not the device nickname. The
    /// nickname is one device-wide value while a lobby is per game, so two
    /// lobbies joined under two names left the older one reading as a claim
    /// race this device lost the instant the second was joined - which is the
    /// owner's "I cannot play two large group games at the same time with
    /// different nicknames". See `SeatRow.name`.
    private func lobbySeat(_ env: MessageEnvelope) -> Int? {
        let me = MessageGameStore.shared.identity(gameId: env.gameId)
        return SeatIdentity.resolveInLobby(
            cachedSeat: me.seat,
            senderIsLocal: senderIsLocal, nPlayers: env.nPlayers,
            lastActorSeat: env.lastActorSeat, joins: env.joins, chatIsDM: chatIsDM,
            recordedName: me.name)
    }

    /// Claim the lowest free seat (§5.2, lobby v3). Always reseals WAITING and
    /// stays in the lobby — joining NEVER starts the game, no matter how many
    /// have joined or that the lobby's own capacity (8 for a group, 2 for a DM
    /// — see `createWaiting`) is reached; Start (below) is the one, explicit
    /// action that flips the game LIVE. Auto-stages the reseal (notes 14/16):
    /// the human still presses Messages' own Send, there is no separate "Send
    /// invite" button.
    private func joinLobby(_ lob: Lobby, nickname: String) async {
        let env = lob.env
        guard let free = (0..<env.nPlayers).first(where: { s in !env.joins.contains { $0.seat == s } }),
              let gid = UInt64(env.gameId) else { return }
        // Round-5 B1: LobbyView's join button is only reachable from its
        // `.ok` branch, so `nickname` is already NicknameGate-valid and
        // trimmed — the "You" fallback that used to live here is unreachable
        // now. Re-check defensively anyway and, per M2, fall back to the
        // STORED nickname (never a placeholder) if it somehow is not.
        let nick: String
        if case .ok(let name) = NicknameGate.check(nickname) {
            nick = name
        } else {
            nick = MessageGameStore.shared.nickname
        }
        // Names must stay unique WITHIN a chain (they are the only identity
        // the payload carries, §6 — see NicknameGate.isTaken). LobbyView's
        // join button already refuses a taken name; this re-check covers the
        // fallback path above landing on a stored nickname that collides.
        guard !NicknameGate.isTaken(nick, in: env.joins) else { return }
        MessageGameStore.shared.nickname = nick   // remember it for the next game (B3)
        let joins = (env.joins + [MessageJoin(seat: free, name: nick)]).sorted { $0.seat < $1.seat }
        do {
            // Re-adopt the lobby so the LOCKED seed + open capacity are resident
            // for the seal.
            _ = try await MessageKernel.shared.decode(payload: lob.payload, viewer: -1)
            let parent = threadParent8(env)
            let payload = try await MessageKernel.shared.seal(
                phase: 0, lastActorSeat: free, gameId: gid, parent8: parent, joins: joins)
            let newEnv = try await MessageEnvelope.decode(payload: payload, viewer: -1)
            cache(seat: free, env: newEnv, payload: payload)
            // THE SNAP, AND THEN THE DRAWER - the rule `leaveLobby`,
            // `stageLobbyPassing` and `startGame` have all kept since 1.1(68), and
            // the one lobby action that was missed (U12). `onSend` is not a
            // notification: it is the host's `stage(payload:mySeat:)`, which
            // composes the bubble, bumps `collapseSignal`, asks for `.compact`
            // and does not return until that transition has settled - so a
            // roster assigned BELOW it landed after the drawer had already gone.
            //
            // A join's delta is the one the kernel folds to NO beats and one
            // REST (the adopt is the snap, and it still has to be read); that is
            // exactly the number `holdSurface` spends, and exactly why it is
            // asked for rather than typed. See `holdSurface`.
            let plan = await MessageKernel.shared.surfacePlan(showing: lob.payload,
                                                              arriving: payload)
            let began = Date()
            lobby = Lobby(env: newEnv, payload: payload); lastShownChain = payload
            await holdSurface(plan, since: began)
            await onSend(payload, free, false)
            surfaceStaged = true   // round-9: the join reseal awaits Send
        } catch {
            damaged = true
        }
    }

    /// Leave the lobby (round 16): reseal it WITHOUT me and stage that, so the
    /// thread's newest bubble is a table I am no longer at.
    ///
    /// SEATS ARE COMPACTED, not holed. Start deals at `joins.count` and the
    /// kernel seats 0..<n contiguously (`fio_reseat_game`), an invariant the
    /// whole lobby rests on - "seats are claimed lowest-first, so it is always
    /// a contiguous 0..<n". A hole would seal a join whose seat is >= the
    /// dealt player count and simply not replay. Compacting preserves the
    /// CYCLE, which is all the seat numbers ever meant; everyone finds
    /// themselves again by name (SeatIdentity.seatClaimedByName), and the
    /// numbers were never identity.
    ///
    /// WHAT `lastActorSeat` BECOMES. It has to be a seat, and mine no longer
    /// exists - so it points at the first FREE slot, which after a compaction
    /// is always in range and is never a seated player. That matters twice:
    /// nobody left behind is wrongly read as "you sent the newest bubble" and
    /// withheld from Start (M9), and "the actor is not in the joins" is exactly
    /// how a reader tells a leave from a join.
    ///
    /// THE RACE, ACCEPTED (owner's call). If someone taps Start off the lobby
    /// that still lists me at the same moment I leave, one of the two is
    /// silently dropped: Messages hands every device whichever bubble arrives
    /// last, there is no way to read past it, and Rule P ranks the fuller
    /// roster higher - so a device already sitting on the lobby keeps showing
    /// me until it reopens the newer bubble. No priority scheme is layered on
    /// top of that; it would only be a second opinion about an order the
    /// platform has already decided.
    private func leaveLobby(_ lob: Lobby) async {
        let env = lob.env
        guard let me = lobbySeat(env), let gid = UInt64(env.gameId) else { return }
        guard LobbyControls.canExit(mySeat: me, joined: env.joins.count) else { return }
        let myName = env.joins.first { $0.seat == me }?.name ?? ""

        let remaining = env.joins.filter { $0.seat != me }.sorted { $0.seat < $1.seat }
        let joins = remaining.enumerated().map { MessageJoin(seat: $0.offset, name: $0.element.name) }
        guard !joins.isEmpty else { return }

        do {
            // LEAVING DISCARDS A RULES CHANGE MADE IN THE SAME BREATH. Owner's
            // ruling, and it is a RULE now rather than whatever fell out:
            // "if you toggle and leave, it's the same as if you just left. No
            // affect on toggle. Leaving then toggling shouldn't even be
            // possible. Lets make it unambiguous."
            //
            // The other order was already impossible and needs no clause -
            // leaving spends the seat that moving the rules requires
            // (`msg_lobby_can_set_rules`), so a leaver simply has no checkbox.
            // This is the order that WAS possible, and it was the worst of the
            // three outcomes: the chain carried her new rule, so the table
            // silently changed its rules on somebody who had walked away from
            // it, and the only signal was a rotate that may or may not play.
            //
            // Enforced at the SENDER, deliberately. A receiver could suppress
            // the rule instead, and that would be worse: her chain would say one
            // thing and every screen reading it another, which is a fork rather
            // than a rule. So the leave seals from the chain the draft STARTED
            // from - the table as everyone else still has it - and her local
            // toggle goes out with the draft it belonged to.
            //
            // The roster is still taken from `env`, which is correct either way:
            // a draft carries at most one roster action and leaving IS it, so a
            // preceding rules edit cannot have moved anybody.
            let base = (surfaceStaged ? stagedDraft.origin?.payload : nil) ?? lob.payload
            // Re-adopt so the LOCKED seed and the open capacity are resident -
            // and, per the rule above, the rules the TABLE agreed.
            _ = try await MessageKernel.shared.decode(payload: base, viewer: -1)
            let parent = threadParent8(env)
            let payload = try await MessageKernel.shared.seal(
                phase: 0, lastActorSeat: joins.count, gameId: gid,
                parent8: parent, joins: joins)
            let newEnv = try await MessageEnvelope.decode(payload: payload, viewer: -1)
            // Forget the seat I no longer hold, or the next open of this game
            // would resolve me back into a lobby I left.
            MessageGameStore.shared.forgetSeat(gameId: env.gameId)
            AnimLog.say("lobby exit: \(myName) left, \(joins.count) remain")
            // The sender names the leaver in the transcript line; the envelope
            // cannot (the join that carried the name is exactly what was
            // removed), so the one device that still knows says it.
            onAnnounceLeave(myName)
            // THE SNAP, AND THEN THE DRAWER. 1.1(68), owner, off the device: "i
            // just confirmed the leave snap happens mid collapse." It did: the
            // seat only left the roster on the line BELOW `onSend`, which does
            // not return until the collapse it starts has settled - so the one
            // frame the human asked to see was drawn into a moving drawer. See
            // `holdSurface` for why the length is the kernel's and not a number
            // typed here.
            let plan = await MessageKernel.shared.surfacePlan(showing: lob.payload,
                                                              arriving: payload)
            let began = Date()
            lobby = Lobby(env: newEnv, payload: payload); lastShownChain = payload
            await holdSurface(plan, since: began)
            await onSend(payload, joins.count, false)
            surfaceStaged = true
        } catch {
            damaged = true
        }
    }

    /// THE RULES THE TABLE HAS AGREED, per lobby (keyed by game id): the
    /// passing value on the newest bubble that somebody ELSE put on the chain.
    /// `LobbyControls.rulesChanged` compares it with what the lobby says now to
    /// answer "have I just changed this", which is what withholds Start from
    /// whoever moved the checkbox.
    ///
    /// A dictionary rather than one value because a chat can hold more than one
    /// lobby, and this view is reused across them; it is small (one Bool per
    /// game this device has looked at) and dies with the extension.
    @State private var passingBaseline: [String: Bool] = [:]

    /// Adopt a lobby bubble's rules as the agreed baseline - unless it is MINE,
    /// in which case it may be the change itself and the older agreement still
    /// stands. Called wherever a lobby arrives from the chain.
    private func noteRulesBaseline(_ env: MessageEnvelope) {
        guard env.lastActorSeat != lobbySeat(env) else { return }
        passingBaseline[env.gameId] = env.passingAllowed
    }

    /// CHANGE THE TABLE'S RULES: reseal this lobby with the passing checkbox
    /// moved, and stage that, so the change reaches everyone the same way a
    /// join does - as a bubble on the chain.
    ///
    /// It is `joinLobby` with the roster left alone: re-adopt the lobby (the
    /// locked seed and the open capacity have to be resident to seal), tell the
    /// kernel the rule, seal, stage. `lastActorSeat` is mine, which is what
    /// takes Start away from me until somebody else acts - the owner's rule,
    /// and the reason the reseal is a real bubble rather than a local flag: the
    /// others must be able to see the rules they are about to play under before
    /// anyone can start.
    ///
    /// A no-op if I hold no seat (the checkbox is disabled there anyway - a
    /// reseal has to name an actor seat) or if the rule is already what was
    /// asked for, so a double tap cannot stage a bubble that changes nothing.
    ///
    /// ONE AT A TIME (round 21). The checkbox now moves the instant it is
    /// touched (`LobbyView.passingWish`), which makes it easy to tap twice
    /// before the first reseal has landed - and two of these running at once
    /// would interleave through the kernel actor and seal each other's rule.
    /// `passingStaging` holds the lane and `passingWanted` holds the newest
    /// request, so taps COLLAPSE: whatever the box says when the lane frees is
    /// what gets sealed, and every tap in between costs nothing.
    private func setLobbyPassing(_ lob: Lobby, passing: Bool) async {
        passingWanted = passing
        guard !passingStaging else { return }
        passingStaging = true
        defer { passingStaging = false }
        // Re-read `lobby` each pass rather than trusting the `lob` this call was
        // handed: an earlier iteration has already replaced it, and staging
        // against the payload from before that would fork the chain.
        while let want = passingWanted {
            passingWanted = nil
            guard let current = lobby else { return }
            await stageLobbyPassing(current, passing: want)
        }
    }

    /// True while a rules reseal is in the kernel. See `setLobbyPassing`.
    @State private var passingStaging = false
    /// The newest rule asked for while the lane was busy, or nil for none.
    @State private var passingWanted: Bool?

    /// One rules reseal, start to finish. Always called from the single lane
    /// `setLobbyPassing` owns.
    private func stageLobbyPassing(_ lob: Lobby, passing: Bool) async {
        let env = lob.env
        guard let me = lobbySeat(env), let gid = UInt64(env.gameId) else { return }
        guard env.passingAllowed != passing else { return }
        do {
            // Remember what the table had agreed BEFORE this change, unless
            // this device has already staged one on this lobby (then the
            // baseline is still the older, agreed value - see
            // LobbyControls.rulesChanged, and note that ticking the box back
            // must clear the gate rather than double it).
            if passingBaseline[env.gameId] == nil {
                passingBaseline[env.gameId] = env.passingAllowed
            }
            // Decode, set, seal - ONE actor call (round 21). Three separate
            // hops left two suspension points in which any other decode could
            // repoint the resident game, which is the phantom-seal shape all
            // over again; see `MessageKernel.resealLobby`.
            let parent = threadParent8(env)
            let payload = try await MessageKernel.shared.resealLobby(
                lob.payload, passing: passing, actingSeat: me,
                gameId: gid, parent8: parent, joins: env.joins)
            let newEnv = try await MessageEnvelope.decode(payload: payload, viewer: -1)
            AnimLog.say("lobby rules: passing=\(passing) by seat \(me)")
            cache(seat: me, env: newEnv, payload: payload)
            // THE TURN, AND THEN THE DRAWER - the same rule `leaveLobby` and
            // `startGame` keep. The box began turning under the finger
            // (`FCheckbox` runs it before it calls this), so what is held here
            // is the REST of that turn: without it the collapse started in the
            // same breath as the rotation and ate it.
            let plan = await MessageKernel.shared.surfacePlan(showing: lob.payload,
                                                              arriving: payload)
            let began = Date()
            lobby = Lobby(env: newEnv, payload: payload); lastShownChain = payload
            await holdSurface(plan, since: began)
            await onSend(payload, me, false)
            surfaceStaged = true
        } catch {
            damaged = true
        }
    }

    /// The lobby's "Add player" action, or nil when solo seating is not
    /// compiled in — one `#if` here instead of one at the call site, so the
    /// view code above reads the same in every configuration.
    private func soloSeatAction(_ lob: Lobby) -> (() -> Void)? {
        #if DEBUG || SOLO_TESTING
        return { Task { await addSoloSeat(lob) } }
        #else
        return nil
        #endif
    }

    #if DEBUG || SOLO_TESTING
    /// Testing-only (MessageDebugFlags.soloSeats): seat a PUPPET player from this
    /// device so a lobby with nobody else in the chat can still reach two seats
    /// and start. Mechanically `joinLobby` minus the two things that would be
    /// wrong here:
    ///
    ///   - it does NOT overwrite this device's stored nickname (the puppet is
    ///     not me renaming myself — I keep my own name on my own seat), and
    ///   - it does NOT re-cache MY seat as the puppet's, so identity stays
    ///     whatever it already was; `pickSeatOnAdopt` is what switches which
    ///     hand you are playing, one bubble at a time.
    ///
    /// It also deliberately does not stage/send the reseal: a puppet is local
    /// scaffolding, and `startGame` seals the LIVE handoff off this same
    /// in-memory lobby payload, so the chat only ever sees the real game.
    private func addSoloSeat(_ lob: Lobby) async {
        let env = lob.env
        guard let free = (0..<env.nPlayers).first(where: { s in !env.joins.contains { $0.seat == s } }),
              let gid = UInt64(env.gameId) else { return }
        let keepSeat = lobbySeat(env) ?? 0
        let puppet = MessageDevBoard.soloName ?? "Solo \(free + 1)"
        let joins = (env.joins + [MessageJoin(seat: free, name: puppet)])
            .sorted { $0.seat < $1.seat }
        do {
            _ = try await MessageKernel.shared.decode(payload: lob.payload, viewer: -1)
            let parent = threadParent8(env)
            let payload = try await MessageKernel.shared.seal(
                phase: 0, lastActorSeat: free, gameId: gid, parent8: parent, joins: joins)
            let newEnv = try await MessageEnvelope.decode(payload: payload, viewer: -1)
            cache(seat: keepSeat, env: newEnv, payload: payload)
            lobby = Lobby(env: newEnv, payload: payload); lastShownChain = payload
        } catch {
            damaged = true
        }
    }
    #endif

    /// Start the game at the ACTUAL joined count (§5.2, lobby v3). Any JOINED
    /// player may do this once 2+ have joined (LobbyView gates the button on
    /// that; nothing re-checks it here — the kernel would happily reseat and
    /// seal a 1-player "game" too, but the design never offers the button for
    /// it). Re-derives the resident game from the seed LOCKED at create, at
    /// `joins.count` seats — contiguous 0..<k because seats are always claimed
    /// lowest-free-first — then seals the LIVE handoff (turn 0, parent8 =
    /// first8(lobby digest), the same joins) and drops the starter onto the
    /// board: mechanically identical to what the OLD "last joiner auto-starts"
    /// branch of `joinLobby` used to do, just triggered explicitly instead of
    /// implicitly by seat count. Uses the shared `MessageKernel.startFromLobby`
    /// primitive so this reseat/seal is provably the deal locked at create.
    private func startGame(_ lob: Lobby) async {
        let env = lob.env
        guard let seat = lobbySeat(env), let gid = UInt64(env.gameId) else { return }
        do {
            let parent = threadParent8(env)
            let payload = try await MessageKernel.shared.startFromLobby(
                lobbyPayload: lob.payload, gameId: gid, actingSeat: seat,
                parent8: parent, joins: env.joins)
            let newEnv = try await MessageEnvelope.decode(payload: payload, viewer: -1)
            cache(seat: seat, env: newEnv, payload: payload)
            // MY OWN START FADES, exactly as an arriving one does. Owner: "our
            // own start (when possible) should fade." Before this the lobby was
            // simply gone in the frame the board appeared, which is the same cut
            // the 1.1(56) report was about - it was only ever noticed from the
            // receiving side because that is the side a human sits and watches.
            //
            // THE DURATION IS THE KERNEL'S, and so is the decision that there is
            // a fade at all: the plan is asked over my own two chains (the lobby
            // on screen, the LIVE handoff I just sealed) exactly as it is asked
            // over a text's, so there is one answer to "what does a lobby giving
            // way to a board look like" rather than one per direction. A plan
            // that comes back without a fade - a kernel that stops calling this
            // a transition - simply swaps, as it always did.
            //
            // AND IT FADES FIRST. 1.1(68), owner: "if I tap a lobby where I am
            // legally allowed to start, I see it collapsed THEN fade on my
            // screen. It should be the opposite. Fade to game, then collapse."
            // `onSend` is not a notification - it stages the bubble, and staging
            // collapses the drawer and does not return until the host's
            // transition has settled - so awaiting it first spent the whole
            // transition before the first frame of the fade was drawn. It now
            // sits below `holdSurface`, which is where every lobby action in
            // this file puts it, and for the same reason.
            let plan = await MessageKernel.shared.surfacePlan(showing: lob.payload,
                                                              arriving: payload)
            let began = Date()
            let fade = plan.beats.last.flatMap { $0.transition == .fade ? $0 : nil }
            if fade != nil {
                arrivalStill = ArrivalStill(env: env, passing: env.passingAllowed,
                                            controls: env)
                stillFade = 1
            }
            controller = MessageTurnController(parentPayload: payload, parent: newEnv, mySeat: seat)
            lobby = nil
            if let fade, fade.duration > 0 {
                withAnimation(.easeInOut(duration: fade.duration)) { stillFade = 0 }
                try? await Task.sleep(nanoseconds: UInt64(fade.duration * 1_000_000_000))
                arrivalStill = nil
                stillFade = 1
            }
            await holdSurface(plan, since: began)
            await onSend(payload, seat, false)
            surfaceStaged = true   // round-9: the LIVE handoff awaits Send (alsoStaged)
        } catch {
            damaged = true
        }
    }

    /// Adopt `winner` as the game and open the board.
    private func adopt(winner: Data, env: MessageEnvelope) async {
        // A WAITING envelope is an INVITE, and this function opens a BOARD. They
        // are never interchangeable: a lobby seal leaves a game dealt at the
        // lobby's CAPACITY resident (8 for a group chat — see `createWaiting`),
        // so adopting one as a board shows a phantom 8-player game whose unjoined
        // seats read "Seat N", with a different first attacker than the real
        // game — the round-3 "some see a 5-player game, some see 8" fork, which
        // deadlocks the thread. Rule P now ranks any started chain above a lobby
        // (msg_rule_p rule 0), so nothing should reach here at phase 0 any more;
        // this is the structural guarantee behind that, not a second opinion
        // about which chain wins.
        if env.phase == 0 {
            controller = nil
            noteRulesBaseline(env)
            lobby = Lobby(env: env, payload: winner); lastShownChain = winner
            return
        }
        // Round-9 #5: is this the chain THIS DEVICE just pressed Send on? The
        // send can tear the extension down (dismiss / VC swap), so the reopen
        // arrives here as a cold load of my own bubble - without this it
        // REPLAYED the move I had just watched myself play. One-shot: consumed
        // (cleared) whether it matches or not, so a stale marker can never
        // silence a later genuine replay.
        lastShownChain = winner
        let justSent = MessageGameStore.shared.consumeJustSent(matching: winner)
        // ROUND 20: is this bubble the latest this device has seen of this game,
        // or a branch off something older? Asked BEFORE any early return below,
        // so every route to a board carries the same answer, and stored in
        // `@State` because `seatOnBoard` is where the controller finally exists
        // (and is reached from the name gate and the seat picker too).
        staleBranch = await rankAgainstHighWater(winner, env: env)
        // Make the resident game the winner (the round guard/ledger it used to set
        // are gone with Rule R).
        _ = try? await MessageKernel.shared.decode(payload: winner, viewer: -1)
        #if DEBUG || SOLO_TESTING
        // Single-simulator harness: both conversations share ONE App Group cache
        // and participant identity, so a received bubble always resolves to the
        // SENDER's seat and you can never view the receiver ("Waiting for Seat 2"
        // while you ARE seat 2). In DEBUG, ask who you are so both seats are
        // playable on one sim. Release resolves automatically (real devices have
        // separate caches + distinct participant UUIDs) and never shows this.
        if MessageDebugFlags.pickSeatOnAdopt { controller = nil; ambiguous = (env, winner); return }
        #endif
        // ROUND 9 (owner): the durable pending ledger and its Rule R rebase are
        // REMOVED ("caching has caused A LOT of problems... drop the pending
        // ledger altogether"). An adopt no longer replays any stored moves - a
        // staged-but-unsent move survives only in the live controller, and in
        // the staged input-field bubble itself.

        // WHICH SEAT AM I - one kernel call (msg_seat_resolve_on_board). The
        // three §6 layers with the roster's NAMES in front of them, both ways:
        // recovery (the seat carrying MY claim name in THIS chain is mine even
        // when a fork race left the number on a claim that lost) and the ghost
        // guard (a roster naming somebody ELSE at my cached seat means my claim
        // lost, and trusting the number would put their hand face-up on my
        // screen). The composition is C's; this used to be a `??` and a ternary
        // here, which is a rule living in Swift.
        //
        // The claim is read as ONE ROW. Its NAME is the name MY join sealed for
        // THIS game, never the device nickname: the nickname is a single
        // device-wide value the human can change at any time, including by
        // joining a second game under a different one - after which this game's
        // roster disagrees with it and both name gates read as a lost claim
        // race. That is the owner's "I cannot play two large group games at the
        // same time with different nicknames" (see `SeatRow.name`). A row with
        // no name (format 1, or a claim off a chain that did not list the seat)
        // is permissive - the kernel treats a missing side as no disownment.
        //
        // The row is found by gameId alone (`identity`), so it survives a
        // group-membership change re-keying the chat - and with NO row at all
        // the nickname is still offered, which is §6.2 name recovery for a
        // device that has never claimed a seat here (see `identity`).
        let me = MessageGameStore.shared.identity(gameId: env.gameId)
        switch SeatIdentity.resolveOnBoard(
                cachedSeat: me.seat, recordedName: me.name,
                senderIsLocal: senderIsLocal,
                nPlayers: env.nPlayers, lastActorSeat: env.lastActorSeat,
                joins: env.joins, chatIsDM: chatIsDM) {
        case .known(let seat):
            // §B3: a player about to be seated who has never chosen a name is
            // asked once. Since lobby v3 everyone named themselves at setup or
            // the lobby's Join field, so this fires only on §6.2 cache-loss
            // recovery (reinstall/second device — the nickname went with the
            // cache), at any player count; it never re-asks once stored.
            if !MessageGameStore.shared.hasSetNickname {
                controller = nil
                nameGate = NameGate(env: env, payload: winner, seat: seat,
                                    quietOpen: justSent)
            } else {
                seatOnBoard(seat: seat, env: env, winner: winner, quietOpen: justSent)
            }
        case .ambiguous:
            controller = nil
            #if DEBUG || SOLO_TESTING
            // Single-simulator testing keeps the real picker (see the DEBUG note
            // above in this function) — this branch is unreachable in DEBUG anyway
            // because `pickSeatOnAdopt` already returned above, but stays correct
            // if that flag is ever turned off. Round 20: unless the rig asks for
            // the Release route, which is the only way that screen can be
            // reached on a debug build at all - see `spectateWhenAmbiguous`.
            if !MessageDebugFlags.spectateWhenAmbiguous {
                ambiguous = (env, winner)
                return
            }
            #endif
            // RELEASE SECURITY: an ambiguous identity must never offer a seat
            // picker — anyone could claim any hand and see it. Show the same
            // PUBLIC spectator board a delivered bubble's snapshot uses instead
            // (§10, MessageBoardView is public-safe by construction). `winner` was
            // already decoded/adopted above, so the resident game IS this chain.
            let names = Dictionary(env.joins.map { ($0.seat, $0.name) }, uniquingKeysWith: { a, _ in a })
            // Round 20 read the §12 funnel code HERE rather than on the tap,
            // because the resident game is this chain at THIS moment and by the
            // time anyone taps the link something else may have been decoded
            // over it. Round 22 finishes that thought: the board and the code
            // were still two separate trips into the kernel, so the same
            // interloper could land BETWEEN them and hand a watcher one game's
            // table with another game's replay link. `readBoard` rebuilds this
            // chain and answers both in one call - viewer -1 is the public
            // table, which is all a watcher may see anyway.
            if let read = try? await MessageKernel.shared.readBoard(
                    .continuation(payload: winner), replaying: [], seat: -1, sentAt: 0),
               let view = read.view {
                // …with the roster attached, the same as a seated player's link
                // (MessageTurnController.replayURL): a watcher who shares the
                // finished game should not hand out a link that has forgotten
                // who played it. `names` is the joins of the chain that was just
                // decoded, `view` the table it built.
                spectatorReplayURL = read.replayCode.map {
                    MessageEnvelope.replayLink(
                        code: $0,
                        names: ReplayExtras.seatNames(names, count: view.numPlayers))
                }
                spectator = (view, names)
                // ROUND 21: a FINISHED chain also gets a real board to watch the
                // last move on, seated at nobody's seat - see `spectatorBoard`.
                // The still picture stays behind it as the running-game case and
                // as the fallback if the board cannot be built.
                spectatorBoard = view.isOver
                    ? MessageTurnController(parentPayload: winner, parent: env, mySeat: -1)
                    : nil
            } else {
                damaged = true
            }
        }
    }

    /// Open the board for a resolved seat: cache it and hand the winner chain
    /// to a fresh controller. The tail of `adopt`'s `.known` branch, shared
    /// with the name gate. `quietOpen` (round-9 #5): this is my own just-sent
    /// chain, so its last move - mine, watched live - is not replayed.
    private func seatOnBoard(seat: Int, env: MessageEnvelope, winner: Data,
                             quietOpen: Bool = false) {
        cache(seat: seat, env: env, payload: winner)
        // ROUND 12: same game, same seat, board already up -> hand the new chain
        // to the LIVE controller instead of replacing it.
        //
        // The board is keyed on the controller's identity (`expandedContent`'s
        // `.id`), so replacing the controller throws the board away and builds a
        // new one - fresh `@State`, unmeasured geometry, a first paint at
        // defaults. That teardown is what the owner sees as the board flashing
        // when a move arrives on an expanded screen. Nothing about an arriving
        // bubble requires a new board: the seat is the same, the game is the
        // same, only the chain moved on, and `adopt` moves exactly that.
        //
        // A DIFFERENT game (or a different seat in one) still gets a fresh
        // controller - there the teardown is honest, because it really is a
        // different board.
        if let live = controller, live.canAdopt(seat: seat, gameId: env.gameId) {
            // Round 20: re-asked on every adopt, in BOTH directions - the newest
            // bubble arriving on a stale board is what hands it the right to
            // play again, and it must not have to be re-tapped for that.
            live.setSuperseded(staleBranch)
            // OFFERED, not forced (the conflict model, 1.0(28)): a chain
            // arriving over a staged move is visibly retracted first - the
            // staged cards fly home in red against the OLD base - and adopted
            // only when that lands. With nothing staged this is `adopt` as it
            // always was.
            Task { await live.offerArrival(payload: winner, parent: env, quietOpen: quietOpen) }
            return
        }
        let fresh = MessageTurnController(parentPayload: winner, parent: env, mySeat: seat,
                                          suppressOpenReplay: quietOpen)
        fresh.setSuperseded(staleBranch)
        controller = fresh
    }

    /// The human answered the name gate: persist the name, then seat them. The
    /// name is baked into `joins` when they first play (sealJoins). Round-5
    /// B1: NameGateView's Continue/onSubmit are only reachable from their
    /// `.ok` branch, so `raw` is already NicknameGate-valid and trimmed — the
    /// "call me the default" blank fallback this used to have is gone (see
    /// NameGateView's own doc). Re-check defensively anyway and, per M2, fall
    /// back to the STORED nickname (never a placeholder) if it somehow is not
    /// — skipping the write below just leaves whatever this device already
    /// had on file.
    private func nameThenSeat(_ raw: String, gate g: NameGate) async {
        if case .ok(let name) = NicknameGate.check(raw) {
            MessageGameStore.shared.nickname = name
        }
        nameGate = nil
        seatOnBoard(seat: g.seat, env: g.env, winner: g.payload, quietOpen: g.quietOpen)
    }

    /// §6.3 pick resolved: remember the seat, then play. DEBUG-only
    /// single-simulator path (never compiled into Release). It used to be
    /// described as skipping the "open-delta-replay hint" that `adopt` looks
    /// up; round 43 established there was never a lookup to skip - the hint
    /// was nil at its only origin and read nowhere - so this picker opens
    /// exactly the same board every other path does.
    private func choose(seat: Int, from a: (env: MessageEnvelope, payload: Data)) async {
        cache(seat: seat, env: a.env, payload: a.payload)
        let c = MessageTurnController(parentPayload: a.payload, parent: a.env, mySeat: seat)
        c.setSuperseded(staleBranch)   // round 20 - see seatOnBoard
        controller = c
        ambiguous = nil
    }

    /// Round 7: persist ONLY this device's seat (§6.1). The preferred-chain
    /// payload, denormalized display fields and pending ledger the old record
    /// carried are gone — the extension always renders the tapped bubble now, so
    /// the one thing worth keeping is which seat is me in this game.
    ///
    /// …plus the CLAIM-TIME NAME for that seat, read out of `env`'s own roster
    /// rather than off `MessageGameStore.nickname`. Every path that reaches
    /// here has just sealed or adopted a chain in which `seat` is mine, so the
    /// join at `seat` IS the name my identity travels under in this game -
    /// which is what the §6 name gates want, and what the nickname stops being
    /// the moment a second game is joined under a different one. A roster that
    /// does not list the seat yet (a DM receiver who has not sealed a join)
    /// records nil, and the next adopt of my own sent chain fills it in.
    private func cache(seat: Int, env: MessageEnvelope, payload: Data) {
        MessageGameStore.shared.setSeat(gameId: env.gameId, chatKey: chatKey, seat: seat,
                                        name: env.joins.first { $0.seat == seat }?.name)
    }
}
