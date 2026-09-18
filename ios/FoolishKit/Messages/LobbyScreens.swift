// LobbyScreens - everything the extension shows BEFORE a board: the new-game
// setup, the waiting lobby, the name gate and the seat picker.
//
// One surface's worth of pre-game UI, lifted out of MessagesRootView.swift.
// They share a shape (a name field that raises its own keyboard, a wooden
// action row) and they share a test (NameFieldKeyboardTests counts all three
// name fields here). GameSurface decides which of them a bubble resolves to;
// LobbyControls decides which action a lobby offers.

import SwiftUI

/// New game setup (§5.2, rewritten for lobby v3 — notes 2/19/25). The creator
/// names themselves (B3 — the one place a nickname is entered; compact is the
/// keyboard area and cannot host a field, §3.5). There is no player-count
/// picker any more: it was off-theme (a segmented `Picker` reads as glass, not
/// wood/wool) AND wrong, per the owner's own framing — "New game should just
/// stage the new game, lobby style, with unspecified player count until
/// someone hits start".
///
/// Lobby v3 (note 2) unified DM and group behind ONE path: "Create game"
/// always opens a lobby (LobbyView), never a straight-to-board deal — a DM
/// used to deal LIVE immediately here, which let the creator reroll a bad
/// hand by tapping New game until the deck favored them, since nothing
/// committed the seed until they'd already seen it. A DM's lobby capacity is
/// just 2 (`GameSurface.createWaiting`), so "Players: 2" is still shown as a
/// fact, not a picker — nobody has joined yet, and nobody needs to pick a
/// count: whoever has joined when someone taps Start (or Join and start) IS
/// the player count (§5.2/lobby v3).
struct NewGameSetup: View {
    @State private var nickname: String
    let isDM: Bool
    /// No longer displayed (the picker it used to size is gone). Kept only so
    /// this struct's callers — this file's own call site, the harness, and
    /// MessagesViewController, all of which compute a real participant count —
    /// keep compiling unchanged (source compatibility, no Swift compiler here
    /// to re-check call sites across targets).
    let chatPlayers: Int
    let onStart: (String) -> Void

    init(nickname: String, isDM: Bool, chatPlayers: Int, onStart: @escaping (String) -> Void) {
        // Already normalised by MessageGameStore.nicknamePrefill.
        _nickname = State(initialValue: nickname)
        self.isDM = isDM
        self.chatPlayers = chatPlayers
        self.onStart = onStart
    }

    /// Round-5 B1: the three-state verdict on the CURRENT field text, driving
    /// both the Create-game button's label/enabled state and — via `.ok` —
    /// the exact trimmed name `onStart` is called with. A name that fails
    /// either of NicknameGate's caps is REJECTED here, in the UI, rather than
    /// lighting the button up and failing downstream at the seal layer as
    /// "this game link is damaged" (B1's actual bug).
    private var nameVerdict: NicknameGate.Verdict { NicknameGate.check(nickname) }

    /// OUR OWN KEYBOARD, and the one thing that has to happen before this view
    /// is swapped away. The other two name fields (`LobbyView`'s join and
    /// `NameGateView`) carry the same pair and point back here.
    ///
    /// Compact is the keyboard area and cannot host a field (§3.5), so every
    /// name field in this extension is EXPANDED - and the keyboard it raises
    /// belongs to Messages, not to us. Nothing in here can lower it once it is
    /// up: there is no UIApplication to ask (this target is extension-API-only,
    /// see MessagesViewController's `onOpenURL` note) and
    /// MSMessagesAppViewController offers no control over it. The ONE lever we
    /// have is our own first responder - resign it while the field still
    /// exists, and the keyboard goes down with it.
    ///
    /// Until now nothing did. Create game, Join and Continue each hand off to a
    /// closure that replaces this whole view with the lobby or the board, so a
    /// focused field was REMOVED while it was still first responder. When the
    /// keyboard survives that, Messages lays the expanded sheet out into the
    /// half of the screen the keyboard leaves and there is no way left to
    /// dismiss it: the compose bar is hidden while a sheet is expanded, so
    /// there is no field to tap and nothing to swipe. The board itself is not
    /// damaged - roughly 500pt is still well above
    /// `MessageTableView.collapseFraction`'s 440pt anchor, which is why it
    /// reads as half a board rather than a broken one - but half the table and
    /// a hand jammed against the keyboard is the whole complaint.
    @FocusState private var nameFocused: Bool
    /// One hand-off at a time. `handOff` defers its action by a runloop turn,
    /// and this closes the window that opens.
    @State private var handingOff = false

    /// Drop our keyboard, THEN hand off - in that order, and not in the same
    /// runloop turn. Resigning and calling `act()` together races the
    /// resignation against the swap `act()` causes, which is the
    /// removal-while-first-responder above; the hop lets SwiftUI commit the
    /// focus change while the field is still on screen. A turn is ~16ms and
    /// every one of these actions is a screen change, so nothing reads as lag.
    private func handOff(_ act: @escaping () -> Void) {
        guard !handingOff else { return }
        handingOff = true
        nameFocused = false
        DispatchQueue.main.async { act(); self.handingOff = false }
    }

    var body: some View {
        VStack(spacing: 16) {
            // Round-6 #17: `onTableText` (Tokens.swift) is the wool half of
            // the wood/wool text pairing, thickened per the owner's ask.
            Text(FStrings.t("ios.msg.newgame")).font(.headline).onTableText()
            // Round-7 #1: the "Your name" label is dropped - the field's own
            // "your nickname" placeholder already says what it is, and the two
            // together were redundant. The placeholder carries it alone now.
            TextField(FStrings.t("ios.msg.nickname_ph"), text: $nickname)
                .textFieldStyle(.roundedBorder).focused($nameFocused)
                // Owner, round 46: a device that owes us a name lands able to
                // type, with no second tap. See NameFieldAutofocus.
                .modifier(NameFieldAutofocus(
                    active: nickname.isEmpty, focused: $nameFocused))
            switch nameVerdict {
            case .ok(let name):
                // `handOff`, never `onStart` directly - see `handOff`.
                FButton(FStrings.t("ios.msg.creategame"), kind: .wood) { handOff { onStart(name) } }
            case .empty:
                FButton(FStrings.t("ios.msg.entername"), kind: .wood, enabled: false) {}
            case .tooLong:
                FButton(FStrings.t("ios.msg.nametoolong"), kind: .wood, enabled: false) {}
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .padding()
    }
}

/// The WAITING lobby, rewritten for lobby v3 (§5.2/§5.3, docs/IMESSAGE_LOBBY_V3.md,
/// notes 2/14/15/16): an OPEN lobby, not a fixed seat count. `env.nPlayers` is
/// the lobby's CAPACITY — 8 for a group (the wire's max) or 2 for a DM (see
/// `GameSurface.createWaiting`) — display convention only, never rendered as N
/// literal seats: the joined list IS the player count so far, and the game's
/// real size is decided at Start, not now.
///
/// What a viewer can do: Join (name + button, if I have not claimed a seat and
/// the lobby has room), or Start (once I'm already joined and 2+ have —
/// round-5 M9 narrows "any joined player may" to "any joined player except
/// whoever sent the newest bubble, unless the lobby is full" — see
/// `LobbyControls.offered`). Owner decision (this pass): there is NO combined
/// "Join and start" button — a joiner either Joins (which stages the WAITING
/// lobby for the human to send) or, being already joined, taps Start (which
/// stages the LIVE game). Two distinct texts, never one fused action. There is
/// likewise no "Send invite" button (notes 14/16): creating and joining both
/// AUTO-STAGE the reseal, so the human's very next tap is Messages' own Send.
struct LobbyView: View {
    let env: MessageEnvelope
    /// Re-render this view when a setting changes (see FPrefs). Only the
    /// OBSERVATION matters - the strings still come from FStrings.t and the
    /// table surface still comes from FTextures.
    @ObservedObject private var prefs = FPrefs.shared
    let mySeat: Int?
    let onJoin: (String) -> Void
    let onStart: () -> Void
    /// Leave the lobby: reseal it without me and stage that. Round 16.
    let onExit: () -> Void
    /// Re-stage this same WAITING chain so the human can send the invite again
    /// — the recovery path for a lobby whose auto-staged bubble is gone (see
    /// the `mySeat != nil, joins < 2` branch in `body`).
    let onInvite: () -> Void
    /// Change the table's rules: reseal this lobby with the passing checkbox
    /// moved, and stage that. Whoever does it cannot then start the game (see
    /// `LobbyControls.rulesChanged`).
    let onSetPassing: (Bool) -> Void
    /// The passing rule as of the last bubble somebody ELSE put on this chain,
    /// or nil if there has been none. The lobby needs it to tell "the rules are
    /// what the table agreed" from "I have just changed them and not sent it
    /// yet" - see `LobbyControls.rulesChanged`.
    let passingBaseline: Bool?
    /// Testing-only (MessageDebugFlags.soloSeats): seat a puppet player from
    /// this device. nil in every shipping build — see `soloControls`.
    var onAddSoloSeat: (() -> Void)?
    /// A BEAT'S STILL (1.1(56)): this lobby is not the live one, it is one beat
    /// of an arriving stream held on screen while the human reads it, so it
    /// shows the rule THAT BEAT carries rather than the chain's. Everything else
    /// is drawn exactly as the live lobby draws it — a beat is a picture of a
    /// state the game really was in, and half a lobby is not. What keeps it from
    /// being acted on is `allowsHitTesting(false)` on the overlay, one layer up.
    /// nil is the ordinary lobby.
    var stillPassing: Bool?
    /// WHICH LOBBY THE CONTROLS ANSWER TO, when that is not this one. A beat
    /// whose stream is about to dissolve the lobby draws the roster it carries
    /// but the buttons the human already had - see `SurfacePlan.Controls` and
    /// anim_plan.h. nil everywhere else, including in the live lobby.
    var controlsEnv: MessageEnvelope?
    /// Somebody ELSE moved the checkbox and the plan says to show it turning.
    /// Threaded straight through to `FCheckbox.Turn`, which has the reasoning.
    var rulesTurn: FCheckbox.Turn?

    /// The joiner's editable name (B3): compact can't host a field, so this is the
    /// place a joiner names themselves before claiming a seat. Seeded from the
    /// stored nickname, blank if it's the neutral default.
    @State private var nickname: String

    /// The join field's keyboard, and the hand-off that must outlive it. Same
    /// pair, same reason, as `NewGameSetup` - its `nameFocused` carries the
    /// whole story.
    @FocusState private var nameFocused: Bool
    @State private var handingOff = false

    /// Drop our keyboard, THEN hand off. See `NewGameSetup.handOff`.
    private func handOff(_ act: @escaping () -> Void) {
        guard !handingOff else { return }
        handingOff = true
        nameFocused = false
        DispatchQueue.main.async { act(); self.handingOff = false }
    }

    /// WHERE I JUST PUT THE TICK, ahead of the chain agreeing with me.
    ///
    /// Round 21, the owner: "in lobby, Passing checkbox is not very responsive,
    /// seems to wait for stage before it updates. Make the checkbox UI update
    /// FIRST, THEN stage the message." The box was drawn straight from
    /// `env.passingAllowed`, which is a fact about the newest BUBBLE - so the
    /// tick could not move until `setLobbyPassing` had re-decoded the lobby,
    /// re-sealed it, decoded that, and handed a new `Lobby` back. Every one of
    /// those is correct and none of them belongs between a finger and a tick.
    ///
    /// nil means "nothing of mine is outstanding - draw what the chain says",
    /// which is the state the box is in almost all the time. It is cleared the
    /// moment `env.passingAllowed` moves for ANY reason: my own reseal landing,
    /// or somebody else's bubble arriving with the other rule on it. So the
    /// wish can never outlive the truth, and a lost or rejected change heals by
    /// itself on the next paint rather than leaving the box lying.
    @State private var passingWish: Bool?

    init(env: MessageEnvelope, mySeat: Int?, nickname: String,
         onJoin: @escaping (String) -> Void,
         onStart: @escaping () -> Void,
         onExit: @escaping () -> Void = {},
         onInvite: @escaping () -> Void,
         onSetPassing: @escaping (Bool) -> Void = { _ in },
         passingBaseline: Bool? = nil,
         onAddSoloSeat: (() -> Void)? = nil,
         stillPassing: Bool? = nil,
         controlsEnv: MessageEnvelope? = nil,
         rulesTurn: FCheckbox.Turn? = nil) {
        self.env = env; self.mySeat = mySeat
        self.onJoin = onJoin; self.onStart = onStart; self.onExit = onExit
        self.onInvite = onInvite
        self.onSetPassing = onSetPassing
        self.passingBaseline = passingBaseline
        self.onAddSoloSeat = onAddSoloSeat
        self.stillPassing = stillPassing
        self.controlsEnv = controlsEnv
        self.rulesTurn = rulesTurn
        // Already normalised by MessageGameStore.nicknamePrefill.
        _nickname = State(initialValue: nickname)
    }

    /// What the box should be DRAWN as: my outstanding tap if there is one, the
    /// chain's answer otherwise.
    private var passingShown: Bool { stillPassing ?? passingWish ?? env.passingAllowed }

    /// Have I moved the checkbox on the bubble now at the head of this chain?
    ///
    /// A wish outstanding counts, and has to: this gate is what stops whoever
    /// changed the rules from also starting the game before anyone has seen the
    /// change, and round 21's optimistic tick opens a window where the box has
    /// moved but the reseal carrying it has not landed yet. Withholding Start
    /// for those few milliseconds is free; offering it is the exact thing the
    /// gate exists to prevent.
    private var iChangedTheRules: Bool {
        if passingWish != nil { return true }
        return LobbyControls.rulesChanged(baseline: passingBaseline,
                                          current: env.passingAllowed,
                                          mine: env.lastActorSeat == mySeat)
    }

    /// The lobby SCROLLS when it does not fit, and is centred when it does.
    ///
    /// It is shown in whatever height the drawer happens to have, and the tall
    /// case is real: a rematch at three or more players carries a roster, the
    /// fool's penalty in two lines and the rules checkbox, which together do not
    /// fit the COMPACT drawer - and the extension opens compact. Left to lay out
    /// unbounded it ran through the settings squares in the corner; simply
    /// clipping it instead truncated the penalty sentence to "Ann1 was the fool,
    /// so Ann1 gets attacked first -…", which is the half that matters. Both
    /// found on the simulator, 1.0(17).
    ///
    /// `minHeight: geo.size.height` is what keeps the SHORT lobby exactly where
    /// it was: the content is centred in a frame at least as tall as the drawer,
    /// so nothing moves until there is genuinely more content than room.
    var body: some View {
        GeometryReader { geo in
            ScrollView {
                content
                    .frame(maxWidth: .infinity, minHeight: geo.size.height)
            }
            .modifier(BounceOnlyWhenTooTall())
        }
    }

    private var content: some View {
        VStack(spacing: 12) {
            // Round-6 #17: `onTableText` (Tokens.swift).
            Text(FStrings.t("ios.lobby")).font(.headline).onTableText()
            // Joined players only — never env.nPlayers rows: an open lobby has
            // no "open seat" placeholders, because there is no fixed seat count
            // to fill (note 19/25's whole point, unchanged by v3).
            VStack(spacing: 6) {
                ForEach(env.joins.sorted { $0.seat < $1.seat }, id: \.seat) { j in
                    HStack {
                        // Round-5 M10: full-opacity ink + a light shadow, not
                        // 55% black - the wool weave has no fixed-opacity
                        // foreground that survives it. Round-6
                        // #17 thickened both columns, not just the seat number.
                        Text("\(j.seat + 1).").onTableText().monospacedDigit()
                        Text(j.name + (j.seat == mySeat ? " (\(FStrings.t("ios.you")))" : ""))
                            .onTableText()
                        Spacer()
                    }
                }
            }
            .padding(.horizontal)

            // Testing-only solo controls REPLACE the normal ones when they are
            // live, rather than sitting alongside them: the shipping lobby can
            // legitimately be offering "waiting" at the same moment solo play
            // wants to offer Start, and two contradictory controls on one
            // screen is worse than either. See `soloControls`.
            // A STILL DRAWS THE CONTROLS TOO (1.1(56), owner: "why does 'start
            // playing' become disabled for Alex temporarily?").
            //
            // The first cut of the still rendered no controls at all, reasoning
            // that a beat is not a thing anyone may act on. That is true and it
            // is handled a layer up - the whole overlay is `allowsHitTesting(false)`
            // - but drawing HALF a lobby is not a picture of any state the game
            // was ever in. Vera joining makes the table startable; a beat that
            // shows her name arriving while Start vanishes says the opposite of
            // what just happened, and it read exactly as the owner described it,
            // as the button going disabled and coming back.
            //
            // So the still is the whole lobby, as it stood at that beat, and the
            // controls it shows are the ones that state really offers - Start
            // included, because two people really are seated by then.
            if let onAddSoloSeat, soloSeatsEnabled {
                soloControls(onAddSoloSeat)
            } else {
                standardControls
            }

            // THE TABLE'S RULES, chosen here because here is the only place
            // they CAN be chosen: the game is dealt at Start, and after that
            // the rules are a term of a chain everyone is already playing.
            //
            // BELOW the controls (owner, 1.0(17)). It is not a step on the way
            // to starting - it is a standing fact about the table that anyone
            // may change while the lobby is open, so it sits under the buttons
            // rather than between the roster and them.
            //
            // A spectator sees the box but cannot move it - the rules are as
            // much a part of "what game is this" as the player list, and
            // hiding them from the person deciding whether to join would be
            // the wrong half to keep. Moving it takes a seat, because a reseal
            // has to be sent by somebody who is at the table.
            // The tick moves NOW and the bubble is resealed behind it (round
            // 21 - see `passingWish`). Writing the wish here rather than inside
            // `onSetPassing` keeps the staging closure exactly what it was, and
            // puts the whole of the optimism in the one view that draws the box.
            //
            // THE NOTE BELONGS TO THIS ROW, so it is stacked WITH it at zero
            // rather than a twelfth step further down the lobby. Owner, 1.1(68):
            // "too much padding between the bottom of the passing button and the
            // top of 'waiting for the others'". The checkbox draws a ~22pt box
            // inside a 44pt touch target, so its own row already hangs ~11pt of
            // clear space under the word - the stack's 12 was landing on top of
            // that and reading as double. Taking the 12 rather than trimming the
            // target keeps the finger's 44pt, and keeps the number out of it:
            // what is left below the checkbox is the target's own slack, at
            // whatever size the type happens to be.
            VStack(spacing: 0) {
                FCheckbox(FStrings.t("ios.lobby.passing"),
                          isOn: passingShown,
                          // …and for the same reason the box is not dimmed in a
                          // still: it is drawn as the state really had it. Nothing
                          // in the overlay can be touched anyway.
                          enabled: LobbyControls.canSetRules(mySeat: mySeat),
                          turn: rulesTurn,
                          action: { on in
                              passingWish = on
                              onSetPassing(on)
                          })
                    .padding(.horizontal)
                waitingNote
            }
        }
        .frame(maxWidth: .infinity)
        .padding()
        // THE WISH IS SPENT when the chain agrees with it, or when somebody
        // ELSE moves the rule out from under it.
        //
        // Not simply "on any change", which is the obvious version and flashes:
        // two taps inside one round trip stage two bubbles, so the box would
        // snap to the first rule for a frame on its way to the second, even
        // though the finger only ever asked for the second. My own intermediate
        // bubble is therefore not an answer to my wish - it is a step on the way
        // to it - and only a bubble that is not mine can overrule it.
        .onChange(of: env.passingAllowed) { now in
            if now == passingWish || env.lastActorSeat != (mySeat ?? -1) {
                passingWish = nil
            }
        }
    }

    /// Testing-only (SOLO_TESTING / DEBUG): "Add player" until the lobby has
    /// enough seats, then Start. Deliberately bypasses `LobbyControls.offered`
    /// — specifically its round-5 M9 authorship gate, which withholds Start
    /// from whoever sent the newest bubble so one human cannot lock the others
    /// out of a lobby that still has room. Seating a puppet from this device
    /// makes me the newest sender every time, so that gate would make solo play
    /// impossible; and there is by definition nobody to lock out.
    @ViewBuilder
    private func soloControls(_ addSeat: @escaping () -> Void) -> some View {
        if env.joins.count < env.nPlayers {
            FButton("Add player (testing)", kind: .wood, action: addSeat)
        }
        if env.joins.count >= 2 {
            // The SAME row the shipping lobby renders, not a lookalike: this is
            // the path a seeded simulator run actually films, and a dev control
            // that drifted from the real one would verify the wrong pixels.
            startExitRow(canExit: LobbyControls.canExit(mySeat: mySeat,
                                                        joined: env.joins.count))
        }
    }

    /// Start, and Exit beside it when both are possible.
    ///
    /// The pair spans exactly the width Start spans on its own (owner: "the
    /// distance between the left edge of the left one and the right edge of the
    /// right one should be the same as the current width"). That falls out
    /// rather than being computed: a non-compact FButton is `maxWidth
    /// .infinity`, so two of them share the padded row and `FSpace.m` of
    /// daylight sits between them - true on every device, in every locale, at
    /// every accessibility size, with no arithmetic to drift.
    @ViewBuilder
    private func startExitRow(canExit: Bool) -> some View {
        if canExit {
            HStack(spacing: FSpace.m) {
                FButton(FStrings.t("ios.msg.startgame"), kind: .wood, action: onStart)
                FButton(FStrings.t("ios.msg.exitgame"), kind: .wood, action: onExit)
            }
        } else {
            FButton(FStrings.t("ios.msg.startgame"), kind: .wood, action: onStart)
        }
    }

    /// "Waiting for the others", AT THE FOOT OF THE LOBBY.
    ///
    /// It used to sit in the control slot, above the checkbox - and moving
    /// between `.start` (one button row) and `.waiting` (a line of text PLUS a
    /// button row) changed that slot's height, so everything below it jumped.
    /// The owner hit it on his own rules toggle, which is exactly the move that
    /// crosses between those two states: "on my own screen, when i hit
    /// passing/non passing, it seems to completely rearrange the layout. the
    /// checkbox itself moves down and up! Not ideal. Put 'waiting for others' at
    /// the bottom so it stops fucking with the layout".
    ///
    /// Down here it costs nothing above it: the control slot is one button row
    /// in every state that has a button, so the roster, the buttons and the
    /// checkbox hold still and only the foot of the card grows.
    ///
    /// It answers the same question `standardControls` does, off the same
    /// facts, rather than being handed a flag - one derivation, so the line and
    /// the buttons can never disagree about which state this lobby is in.
    @ViewBuilder
    private var waitingNote: some View {
        let c = controlsEnv ?? env
        let offered = LobbyControls.offered(mySeat: mySeat, joined: c.joins.count,
                                            capacity: c.nPlayers,
                                            iSentTheInvite: c.lastActorSeat == mySeat,
                                            iChangedTheRules: iChangedTheRules)
        if offered == .waiting || offered == .invite {
            Text(FStrings.t("ios.msg.waiting"))
                .font(.footnote).onTableText()
                .multilineTextAlignment(.center)
                .padding(.horizontal)
        }
    }

    @ViewBuilder
    private var standardControls: some View {
            // note 16: no "Waiting for players — N joined" line here any more —
            // the joined list above already says exactly that, and the owner's
            // read was "the lobby is too tight" for a second line saying the
            // same thing.
            // `controls`, not `env`: a beat that is on its way to the board
            // draws the buttons the human already had. Everywhere else the two
            // are the same object.
            let c = controlsEnv ?? env
            let canExit = LobbyControls.canExit(mySeat: mySeat, joined: c.joins.count)
            switch LobbyControls.offered(mySeat: mySeat, joined: c.joins.count,
                                         capacity: c.nPlayers,
                                         iSentTheInvite: c.lastActorSeat == mySeat,
                                         iChangedTheRules: iChangedTheRules) {
            case .start:
                startExitRow(canExit: canExit)
            case .waiting:
                // Round-4 note 1 / round-5 M9: the newest thing on this chain
                // is mine — either my own invite (nobody else has joined yet)
                // or my own join/re-staged invite in a lobby that still has
                // room (M9) — so there is nothing to send, and no Start,
                // that isn't already mine to wait out. Round-5 M10:
                // full-opacity ink + a light shadow, not 55% black. Round-6
                // #17: `onTableText` (Tokens.swift).
                //
                // Round 16: waiting is no longer a DEAD END. This is exactly
                // the owner's "start game is not possible for the last player
                // that joined, thus they can only exit" - the M9 gate withholds
                // Start from whoever sent the newest bubble, and until now that
                // left them with no action at all. Exit alone, full width:
                // there is no second button to share the row with.
                //
                // ONE line, whichever gate is holding Start back (owner,
                // 1.0(17)): a rules change said so in its own words for a
                // moment, and it read as an error message about something the
                // player had just chosen on purpose.
                // THE LINE ITSELF IS NOT HERE ANY MORE - see `waitingNote`, at
                // the foot of the lobby. Only the button stays in the control
                // slot, so this branch is the same HEIGHT as `.start`'s (one
                // button row either way) and moving between them no longer
                // reflows everything under it.
                if canExit {
                    FButton(FStrings.t("ios.msg.exitgame"), kind: .wood, action: onExit)
                }
            case .invite:
                    // I'm in, nobody else is yet. This branch used to render
                    // NOTHING — no Start (needs 2), no Join (I'm joined), no
                    // invite (notes 14/16 dropped that button as redundant with
                    // the auto-stage). Which is a dead end the moment the
                    // auto-staged invite is gone: sent already, or deleted from
                    // the input field, or the extension reopened later. The
                    // owner hit exactly that — a lobby listing one player and
                    // not a single control on it.
                    //
                    // The invite button is only redundant while the auto-staged
                    // bubble is still sitting in the compose field, so it comes
                    // back HERE and only here: re-stage the same WAITING chain
                    // so there is always a way to ask someone to join.
                    FButton(FStrings.t("ios.msg.invite"), kind: .wood, action: onInvite)
            case .join:
                // Same width as the buttons below (note 29) — both rely on the
                // outer .padding() alone, no extra inset on the field. Round-5
                // B1: same three-state nickname gate as NewGameSetup (see
                // `nameVerdict`) — "Join as {name}" only appears once the
                // field holds a valid, trimmed name.
                TextField(FStrings.t("ios.msg.nickname_ph"), text: $nickname)
                    .textFieldStyle(.roundedBorder).focused($nameFocused)
                    // Owner, round 46: a device that owes us a name lands able to
                    // type, with no second tap. See NameFieldAutofocus.
                    .modifier(NameFieldAutofocus(
                        active: nickname.isEmpty, focused: $nameFocused))
                switch nameVerdict {
                case .ok(let name):
                    // Names are the only identity the payload carries (§6), so
                    // each chain's names must stay distinct — the ghost-seat
                    // guard, the §6.3 picker and the "(you)" tag all key on
                    // them (NicknameGate.isTaken's doc has the full story).
                    if NicknameGate.isTaken(name, in: env.joins) {
                        FButton(FStrings.t("ios.msg.nametaken"), kind: .wood, enabled: false) {}
                    } else {
                        // `handOff`, never `onJoin` directly - see
                        // `NewGameSetup.handOff`.
                        FButton(FStrings.t("ios.msg.joinas", ["name": name]),
                                kind: .wood) { handOff { onJoin(name) } }
                    }
                case .empty:
                    FButton(FStrings.t("ios.msg.entername"), kind: .wood, enabled: false) {}
                case .tooLong:
                    FButton(FStrings.t("ios.msg.nametoolong"), kind: .wood, enabled: false) {}
                }
            case .full:
                // Round-5 M10: full-opacity ink + a light shadow, not 55%
                // black. Round-6 #17: `onTableText` (Tokens.swift).
                Text(FStrings.t("ios.msg.lobbyfull")).font(.footnote).onTableText()
            }
    }

    /// Is solo seating compiled in AND switched on? False in every shipping
    /// build — the flag type itself does not exist there, so this is the one
    /// place the condition is spelled and the call site stays readable.
    private var soloSeatsEnabled: Bool {
        #if DEBUG || SOLO_TESTING
        return MessageDebugFlags.soloSeats
        #else
        return false
        #endif
    }

    /// Round-5 B1: the three-state verdict on the CURRENT field text (see
    /// NicknameGate). Replaces the old `displayName`, which only ever
    /// substituted the "You" placeholder for a blank field — there is no
    /// substitute name any more, a name that fails either cap is rejected
    /// outright, not replaced.
    private var nameVerdict: NicknameGate.Verdict { NicknameGate.check(nickname) }
}

/// §B3 one-time name entry for a player being seated without a stored name.
/// Since lobby v3 every player names themselves on the way in (setup or the
/// lobby's Join field), so the one REACHABLE road here is §6.2 cache-loss
/// recovery — a reinstall or second device resolves the seat from an exact
/// signal while the stored nickname is gone with the cache — at any player
/// count (m8's "not redundant with the other two name screens" survives as
/// exactly this: recovery has no setup or Join field to pass through). Shown
/// once (until a name is stored), prefilled with the current nickname if it
/// is not the neutral default.
///
/// Round-5 B1: Continue is no longer always enabled. It gates on the SAME
/// NicknameGate verdict as NewGameSetup and LobbyView's join — blank or
/// over-cap dims the button and swaps its label for the reason. There is no
/// "call me the default" fallback any more: a name is REQUIRED, never
/// substituted, and `.onSubmit` (the keyboard's own Return key) respects the
/// same gate so it cannot hand a rejected name onward either.
struct NameGateView: View {
    @State private var name: String
    let onContinue: (String) -> Void

    init(prefill: String, onContinue: @escaping (String) -> Void) {
        // Already normalised by MessageGameStore.nicknamePrefill.
        _name = State(initialValue: prefill)
        self.onContinue = onContinue
    }

    private var nameVerdict: NicknameGate.Verdict { NicknameGate.check(name) }

    /// This screen's keyboard, and the hand-off that must outlive it. Same
    /// pair, same reason, as `NewGameSetup` - its `nameFocused` carries the
    /// whole story. This is the one of the three that lands on a LIVE BOARD
    /// (§6.2 cache-loss recovery, so mid-game), which is the shape the owner
    /// was sent a screenshot of.
    @FocusState private var nameFocused: Bool
    @State private var handingOff = false

    /// Drop our keyboard, THEN hand off. See `NewGameSetup.handOff`.
    private func handOff(_ act: @escaping () -> Void) {
        guard !handingOff else { return }
        handingOff = true
        nameFocused = false
        DispatchQueue.main.async { act(); self.handingOff = false }
    }

    var body: some View {
        VStack(spacing: 16) {
            // Round-6 #17: `onTableText` (Tokens.swift).
            Text(FStrings.t("ios.msg.nameprompt")).font(.headline)
                .onTableText().multilineTextAlignment(.center)
            // No extra .padding(.horizontal) here — the field and the button below
            // both rely solely on the VStack's outer .padding() so they render the
            // same width (note 29; the field used to be inset twice, making it
            // visibly narrower than the full-width Continue button).
            TextField(FStrings.t("ios.msg.nickname_ph"), text: $name)
                .textFieldStyle(.roundedBorder).focused($nameFocused)
                // Owner, round 46: a device that owes us a name lands able to
                // type, with no second tap. See NameFieldAutofocus.
                .modifier(NameFieldAutofocus(
                    active: name.isEmpty, focused: $nameFocused))
                .submitLabel(.done).onSubmit {
                    // The Return key resigns on its own, but it goes through
                    // `handOff` anyway so there is ONE way off this screen.
                    if case .ok(let trimmed) = nameVerdict { handOff { onContinue(trimmed) } }
                }
            switch nameVerdict {
            case .ok(let trimmed):
                // `handOff`, never `onContinue` directly - see
                // `NewGameSetup.handOff`.
                FButton(FStrings.t("ios.msg.continue"), kind: .wood) { handOff { onContinue(trimmed) } }
            case .empty:
                FButton(FStrings.t("ios.msg.entername"), kind: .wood, enabled: false) {}
            case .tooLong:
                FButton(FStrings.t("ios.msg.nametoolong"), kind: .wood, enabled: false) {}
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .padding()
    }
}

/// §6.3 tertiary identity: N≥3, cache lost, not the last actor - ask the human.
/// Offers every seat (named where a join is known, else "Seat N") so it also
/// covers the DEBUG single-sim case, where a 2-player game has only one join.
struct SeatPicker: View {
    let nPlayers: Int
    let joins: [MessageJoin]
    let onPick: (Int) -> Void

    private func label(_ seat: Int) -> String {
        joins.first { $0.seat == seat }?.name ?? "Seat \(seat + 1)"
    }

    var body: some View {
        VStack(spacing: 12) {
            // Round-6 #17: `onTableText` (Tokens.swift).
            Text(FStrings.t("ios.msg.pickseat")).font(.headline).onTableText()
            ForEach(0..<nPlayers, id: \.self) { seat in
                FButton(label(seat), kind: .secondary) { onPick(seat) }
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .padding()
    }
}
