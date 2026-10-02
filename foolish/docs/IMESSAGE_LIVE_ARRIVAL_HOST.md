# What Messages does when a message arrives for an open iMessage extension

**Status: PHASE 2 (static + live on iOS 26.3).**
Phase 1 read the simulator runtime's binaries.
Phase 2 (2026-10-02) observed the real Messages app on the iPhone 17e simulator (`FC7586CF`, iOS 26.3.1, build 23D8133) with a host trace in the extension: every callback, its thread, both clocks, the payload, the session and the selection beside it.
The trace is compiled only into `RIG_ARRIVE` builds (`FOOLISH_ARRIVE=1 rig.sh build`, `MessagesViewController.hostTrace`) and lands in the flight log and the unified log (subsystem `cards.foolish`, category `host`).
Excerpts are in `docs/IMESSAGE_LIVE_ARRIVAL_HOST_TRACE.md`, cited below as T-R1 to T-R7.
Every claim carries a stable label (E, M, F, L, S, N, U) so code and tests can cite it, and a confidence.
Phase-1 claims now say **live: confirmed**, **live: refuted** or **live: not observed** where a run spoke to them.
Anything still needing evidence is in "Unknown" and must not be cited as fact.

## Binaries read

- B1 `System/Library/Frameworks/Messages.framework/Messages`, the extension-side half (runs inside our process).
- B2 `System/Library/Messages/iMessageBalloons/MSMessageExtensionBalloonPlugin.bundle/MSMessageExtensionBalloonPlugin`, the HOST half (runs inside Messages).
- B3 `System/Library/PrivateFrameworks/ChatKit.framework/ChatKit`, the Messages app's UI layer.
- B4 `System/Library/PrivateFrameworks/IMCore.framework/IMCore`, only searched for the selectors below.
- B5 `System/Library/PrivateFrameworks/iMessageApps.framework/iMessageApps`, only searched for the selectors below.
- B6 The same B2 from the iOS 27.0 runtime.
  Messages.framework, ChatKit and IMCore are not on disk there (they live in that runtime's dyld shared cache) and were not extracted.
- SDK headers: `iPhoneOS27.0.sdk/System/Library/Frameworks/Messages.framework/Headers/MSMessagesAppViewController.h` and `MSConversation.h`.

Paths, versions and hashes are in "Provenance".
Addresses below are file addresses in the iOS 26.3.1 binaries.
B2 is stripped of local symbols, so its method names come from the objc metadata (`xcrun dyld_info -objc`), not from otool labels.

## Extension side (B1): what runs inside the extension process

Every XPC entry point below first hops to the main queue.

- E1 `-[_MSMessageAppContext _didReceiveMessage:conversationState:]` (0xb9e0) does `dispatch_async(_dispatch_main_q, block)`.
  The block (0xba8c) calls `updatedConversationForConversationState:`, then `[viewController didReceiveMessage:conversation:]` if the VC responds to it.
  Confidence high.
- E2 The same dispatch-to-main shape is used by `_conversationDidChangeWithConversationState:` (0xb430), `_didUpdateMessage:conversationState:` (0xb8a0), `_presentationWillChangeToPresentationState:` (0xb66c), `_presentationDidChangeToPresentationState:` (0xb77c) and `_resignActive` (0xb0a8).
  Confidence high.
- E3 `_becomeActiveWithConversationState:presentationState:` (0xac54) dispatches to main only when `becomeActiveShouldDispatchAsyncMainQueue` is set, and otherwise runs inline.
  Confidence high on the code; which value the host sets is unknown.
- E4 There is no presentation-style, session or selection check in E1.
  The only filters on delivery are in the host.
  Confidence high.
- E5 `updatedConversationForConversationState:` (0x9b4c) keeps a dictionary of `MSConversation` keyed by `conversationIdentifier`.
  The same `MSConversation` object is reused and mutated by `-[MSConversation _updateWithState:]` (0x1940c).
  A new one is built by `_initWithState:context:` (0x19044), which copies `activeMessage` into `selectedMessage` WITHOUT calling the delegate.
  Confidence high.
- E6 `_updateWithState:` (0x1940c): if the incoming `state.activeMessage` is not `isEqual:` to the current `selectedMessage` (or the current one is nil), it calls, in this order, `delegate _conversation:willSelectMessage:`, `setSelectedMessage:`, `delegate _conversation:didSelectMessage:`.
  Confidence high.
- E7 Consequence of E1, E5 and E6: when the host's state carries a NEW `activeMessage` and the host then calls `_didReceiveMessage`, the extension sees `willSelectMessage`, then `selectedMessage` already updated, then `didSelectMessage`, then `didReceiveMessage` (whose argument is the received message).
  All four run in one main-queue block, in that order.
  When the state's `activeMessage` equals the existing selection, only `didReceiveMessage` fires and `selectedMessage` is unchanged.
  Confidence high on the code; the host decides which case applies (M4, M3).
  **Live: confirmed** for the new-message case: willSelect, didSelect (selection already the arrival), didReceive, all on the main thread within 1.5 ms of `CACurrentMediaTime`, same bytes in all three (T-R2, T-R3, L3).
- E8 `-[MSMessagesAppViewController _conversation:willSelectMessage:]` (0x23dc) and `_conversation:didSelectMessage:` (0x23f8) forward to the public callbacks ONLY IF both arguments are non-nil.
  A nil `activeMessage` never produces willSelect or didSelect.
  Confidence high.
- E9 First activation order: the `_becomeActive...` block (0xad94) sets presentation style and context, calls `updatedConversationForConversationState:` (which on a cached conversation can itself fire willSelect and didSelect BEFORE willBecomeActive), then `willBecomeActive`, `setActiveConversation:`, then `didBecomeActive`.
  Confidence high.
  This is why the VC's `freshlyActive` window exists.
  **Live:** willBecomeActive, didBecomeActive, then willTransition/didTransition, observed on every activation; a willSelect/didSelect before willBecomeActive was **not observed** in any run (a tapped bubble arrives already selected in willBecomeActive).
  Each activation is handed a NEW `MSConversation` object (its identity changes per activation, stable within one), so E5's reuse holds within an activation only.
- E10 The `_resignActive` block (0xb134) calls `willResignActive`, sets `activeConversation` to nil, then `didResignActive`.
  The conversation cache is not cleared.
  Confidence high.
  **Live: confirmed**: closing the drawer gives willResignActive, didResignActive and `activeConversation` nil 30 ms later (T-R6).
  The process survives a resign, and the next activation can be a FRESH controller instance in the same process (T-R6, L7).
- E11 `_presentationWillChange...` calls `willTransitionToPresentationStyle:`, and `_presentationDidChange...` sets `presentationStyle` THEN calls `didTransitionToPresentationStyle:`.
  Confidence high.
- E12 The SDK header says `didReceiveMessage`, `willSelectMessage`, `didSelectMessage` and `didStartSending` "will not be called when presentationStyle is Transcript or presentationContext is Media" (`MSMessagesAppViewController.h` lines 143-176).
  B1 has no such check, so it is enforced on the host or not at all.
  Confidence medium.
- E13 The SDK header says `selectedMessage` is set only when "the extension has been invoked in response to the user interacting with a message in the transcript" (`MSConversation.h` lines 36-44).
  E6 and E7 show it also moves on host-pushed state.
  Confidence high.

## Host side (B2, B3): what decides to send a message to the extension

Two host paths send `_didReceiveMessage:conversationState:`.
They are its only two call sites in the runtime: a scan of every binary for the selector string found it only in B1 (the receiver) and B2, and `callers.sh` on B2 attributes the two calls to M3 and M4.

### Path A: a new transcript item for our bundle (B3 then B2)

- M1 `-[CKChatController _handleChatItemDidChange:]` (0xcf528) handles `IMChatItemsDidChange`, collects the INSERTED chat items from `userInfo[IMChatItemsInserted]`, and calls `[inputController notifyBrowserViewControllerOfMatchingNewMessages:items]` (call at 0xcf68c).
  It is a `CKChatController` method, not a `CKChatInputController` one.
  Confidence high.
- M2 `-[CKChatInputController notifyBrowserViewControllerOfMatchingNewMessages:]` (0x3d268c) loops the items.
  For each item that `isKindOfClass:IMTranscriptPluginChatItem` and whose `dataSource.bundleID` equals `self.pluginBundleID`, it calls `[browserSwitcher.currentViewController messageAddedWithDataSource:]` if that VC responds.
  Confidence high.
- M2a There is NO `isFromMe` filter in M2.
  Our own sent bubble is also a matching inserted item, so an own send is a candidate for this path.
  This matches the `isMine` guard in `didReceive`.
  Confidence high on the code; whether it actually reaches us is U1.
  **Live: not observed** on an unbound drawer: an own send from a drawer opened through the + menu (compact) produced didStartSending and nothing else, no didReceive, no didSelect, no selection change, for 15 s (T-R1, L1).
  An own send on a BOUND drawer does reach `didReceive`, but in path B's shape (willSelect and didSelect first), so it is attributed to M4, not to this path (L2).
- M2b `pluginBundleID` (0x3de780) is the handwriting plugin's id if that is visible, else `browserPlugin.identifier`, i.e. the plugin currently in the drawer.
  Confidence high.
- M2c The gate is `browserSwitcher.currentViewController` being our browser.
  `setCurrentViewController:` is sent from four `CKBrowserSwitcherViewController` methods: `_loadBrowserForBalloonPlugin:datasource:` (3 sites), `_updateVisibleBrowserView` (2), `_handleRemoteViewControllerConnectionInterrupted:` (1) and `browserTransitionCoordinator:expandedStateDidChange:withReason:` (2).
  `-[CKBrowserTransitionCoordinator setExpanded:withReason:]`, which reaches the last of these, has no `objc_msgSend` call site in B3.
  So "currentViewController is restored only through setExpanded:withReason:, therefore path A is dead" does NOT follow from the binary: `_loadBrowserForBalloonPlugin:datasource:` sets it too.
  Whether path A delivers in practice is U1.
  Confidence that the static "dead" claim is unsupported: medium (a method can also be reached by a selector stored as data, which a call-site scan does not see).
- M3 `-[MSMessageExtensionBrowserViewController messageAddedWithDataSource:]` (B2 0xb0ac) builds the message with `+[MSMessageExtensionDataSource messagePayloadFromPluginPayload:]` from the new item's payload, builds `currentConversationState`, and sends `remoteProxy _didReceiveMessage:message conversationState:state`, then `_markCurrentMessageAsPlayedIfNeeded`.
  It does NOT touch the browser's stored `message` ivar, so `state.activeMessage` is the browser's PREVIOUSLY BOUND message (or nil when unbound), not the arrival.
  By E7 this path gives `didReceive` only, with `selectedMessage` unchanged and no `didSelect`.
  It does not look at presentation style or at whether a datasource is bound.
  The `message` argument is NOT passed through `_configureMessage:messageSenderAddress:withConversation:`, so its `senderParticipantIdentifier` is probably unset (U6).
  Confidence high on the code, medium on the consequences.

### Path B: the bound datasource's payload changed (B2 only)

- M4 `-[MSMessageExtensionBrowserViewController datasourcePayloadDidChange:updateFlags:]` (0xaf08) first stores `[dataSource message]` into the browser's `message` ivar, so the arrival becomes the browser's current message.
  Only if `(flags & 0x13) != 0` does it build `currentConversationState` (whose `activeMessage` is that NEW message, passed through `_configureMessage:messageSenderAddress:withConversation:`, where `senderParticipantIdentifier` is computed from the recipients list, skipped when `isFromMe`) and call `remoteProxy _didReceiveMessage:[message _sanitizedCopy] conversationState:state`.
  Confidence high.
- M4a By E7 this path gives willSelect, `selectedMessage` updated, didSelect, then didReceive, with the same message in didSelect and didReceive.
  This is the "an arrival moves the selection" behaviour observed on device in 1.1(60) and 1.1(61) (the `reload-is-arrival` branch in `GameSurface.swift`).
  It does not call `_markCurrentMessageAsPlayedIfNeeded`.
  Confidence high on the code.
  **Live: confirmed** for a message sent into the bound bubble's own `MSSession` (T-R2, T-R3, L2).
  A message in a NEW session over the same bound drawer is **not** delivered (T-R7, L6), which is what M4 predicts: only the bound datasource's session replaces its payload.
- M4b The datasource delegate is set only by `setBalloonPluginDataSource:` (0x2f40), which also stores the datasource's message, sends `_conversationDidChangeWithConversationState:` (call at 0x3000) and calls `_markCurrentMessageAsPlayedIfNeeded`.
  A browser created with a nil datasource (a drawer launch) registers no delegate, so path B cannot fire for it.
  Confidence high.
- M4c `-[MSMessageExtensionDataSource pluginPayloadDidChange:]` (B2 0x13c68) clears cached message state and forwards the flags to its delegates.
  What calls it, and what sets bits 0x13, was NOT traced (U2).

### Threading and ordering (host to extension)

- M5 Host calls go through `remoteProxy`, an XPC proxy.
  The host code has no coalescing, no debounce and no queue-and-drop.
  One inserted matching item gives one `_didReceiveMessage`, and several matching items inserted in one `IMChatItemsDidChange` give several calls in index order.
  Confidence high on the code.
  XPC delivery on one connection is FIFO, and every extension entry point hops to the main queue (E1, E2), so host order is preserved onto the main thread: reasoned, not observed, confidence medium.
  **Live:** every callback in every run arrived on the main thread (`thr=main`); two arrivals closer together than one Send press could not be produced through Messages on one simulator (U7).
- M6 The host thread that runs `_handleChatItemDidChange:` is not established statically (U3).
- M7 In the host, `_sendBecomeActiveMessage` is called from `_addRemoteViewControllerAndConfigureExtension` (B2 0x3d50) and `_sendResignActiveMessage` from `forceTearDownRemoteViewOverridingExceptions:` (0x378c).
  What triggers that teardown was not traced (U4).
- M8 The iOS 27.0 B2 (B6) has the same two `_didReceiveMessage:conversationState:` call sites, in the same two methods, with the same `(flags & 0x13)` test in `datasourcePayloadDidChange:updateFlags:`.
  Nothing else about iOS 27 was read (U10).

## Feasibility: a live arrival to an awake extension on one simulator

Phase 1 verdict was "probably possible, not proven".
Phase 2 verdict: possible for a BOUND drawer, through a route other than the one phase 1 proposed.

- F1 The rig README said a message sent in a thread appears in that same thread as INCOMING.
  **Live: refuted** on this simulator on 2026-10-02: the stub threads are iMessage threads now, a send lands OUTGOING (right, "Delivered") in the thread it was sent from, and its incoming twin is a caption-only pill in the OTHER stub thread (L9).
  So there is no same-thread self-echo for path A to see.
- F2 For path B the extension must be bound, i.e. opened by tapping a bubble (M4b).
  **Live: confirmed, and sufficient**: on a bound drawer, pressing Send on a bubble in the bound bubble's own session delivers that bubble back to the drawer as willSelect, didSelect, didReceive (L2), whoever sealed it.
  That is the route the rig uses (L10).
- F3 The product has no way to send ARBITRARY sealed bytes (a move made by a different seat) without registering them in `lastSentPayload` or `pendingStage`, which makes `StagedBubbleRouting.isMine` drop the echo.
  The rig's `RIG_ARRIVE` door seals another seat's move with the shipping kernel and sends it without registering it; `isMine` then routes the delivery as an arrival (`select ... routing as an arrival`, `receive`, T-R2).
- F4 `didStartSending` fires for the door's send too (about 1 s after the delivery, L4); the door returns early there so nothing is registered.
  `didCancelSending` did not fire for a draft the door's bubble replaced (L8).
- F5 Two simulator instances cannot iMessage each other.
  Not tested; nothing in phase 2 needed it.
- F6 The simulator has two participants.
  A 4-seat game there is a seeded board or a lobby filled by the door, which exercises the real host delivery and `selectedMessage` behaviour for 4-seat payloads.
  It does NOT exercise `senderParticipantIdentifier` across three distinct remote participants, group-thread session behaviour, or `recipientIdentifiers` ordering in `MSConversation`.

## Live evidence (phase 2, iOS 26.3 simulator)

Runs, numbered as the phase-1 plan had them; T-Rn is the trace excerpt, frames are under the scratchpad film directory named in the report that accompanied this change (they are not committed).

| Label | Run | What happened | Confidence |
| --- | --- | --- | --- |
| L1 | R1 unbound (+ menu), compact | Own send: didStartSending only; no didReceive, no didSelect, no selection change in 15 s; the drawer kept the stale board while the transcript showed the new bubble. | high (T-R1) |
| L2 | R2/R3 bound (tapped bubble), compact | Send pressed on a bubble in the bound session: willSelect, didSelect (`selectedMessage` already the arrival), didReceive, one main-thread burst within 1.5 ms; same bytes in all three. Worked for lobby joins, Start, and board moves by other seats at 4 seats. | high (T-R2, T-R3) |
| L3 | all | Every callback on the main thread. | high |
| L4 | R2/R3 | didStartSending for the same bubble came 1.03 to 1.05 s AFTER its didReceive, each time. | high |
| L5 | R2/R3 | `senderParticipantIdentifier` on the delivered message was set, stable across four deliveries in one run (`2CD2C682`), and NOT the local id, though the message was sent from this device. On didStartSending it was a different id on every send. | medium: one device, own sends only |
| L6 | R7 new session | A door bubble sent in a NEW `MSSession` over a bound drawer: didStartSending only, no delivery; the old bubble stayed a full bubble instead of collapsing to a caption. | high (T-R7) |
| L7 | R6 drawer closed | Closing the drawer: willResignActive, didResignActive. Send pressed with the drawer closed: the host activated a FRESH controller in the same process (didBecomeActive, style expanded), sent didStartSending to it, and resigned it at once; no didReceive. Re-tapping the bubble was a cold open (willBecomeActive with the arrival already selected) that replayed the move. | high (T-R6) |
| L8 | R5 draft in the field | A second staged bubble (the door's) REPLACED our unsent draft in the input field; no didCancelSending fired for the replaced draft. | high on the replace, medium on the missing cancel |
| L9 | setup | Thread direction: a send lands outgoing in its own thread and as an incoming caption pill in the other stub thread (see F1). | high |
| L10 | all | `conversation.send` from the extension does not send on the simulator: it STAGES the bubble in the input field exactly like `insert` ("Add comment or Send"), and its completion reports no error about 0.2 to 0.3 s later. A human (or the rig) presses Send. | high |
| L11 | R8 timing (film `closing_good_4p_flagON`) | didReceive fires on the Send press: the board reacted (Pickup plank gone) in the same 60 Hz frame the bubble began leaving the input field, about 0.18 s before the bubble settled in the transcript; the board's first flight began about 0.35 s after that. | medium: frame-aligned, not clock-aligned |
| L12 | R4 quick succession | Not producible through Messages on one simulator: each door bubble must be staged and Sent, at least one Send press apart. | n/a |

## What a harness may simulate (cite lines)

A harness, a fixture or a unit test may pose these as host behaviour, citing the label.

- S1 One main-queue hop per host call, in host order: E1, E2, M5, L3.
- S2 On a bound browser, an arrival in the bound session delivers willSelect, `selectedMessage` updated, didSelect, didReceive, in one main-queue burst, with the same message: E7, M4, M4a, L2.
- S3 On an unbound browser nothing is delivered for an own send: L1. (An arrival that leaves the selection unchanged giving didReceive alone is still only the static E7/M3 reading.)
- S4 First activation may fire willSelect and didSelect before willBecomeActive for a cached conversation: E9 (static only; not observed live).
- S5 A nil message produces no willSelect or didSelect: E8.
- S6 The `MSConversation` object is reused and mutated within an activation, and replaced across activations: E5, E6, E9 (live note).
- S7 A message in another `MSSession` is not delivered to a bound drawer: L6.
- S8 A closed drawer gets no delivery; reopening is a cold open of the selected bubble: L7.
- S9 didStartSending for an own send comes about 1 s after that bubble's own delivery on a bound drawer: L4.
- S10 A staged bubble replaces any draft in the input field: L8.

## What a harness must not pretend to cover

- N1 Delivery to an EXPANDED drawer: every live delivery in phase 2 was observed compact, because Send is under an expanded drawer (U1).
- N2 Timing finer than a frame, and timing for a remote arrival (L11 is an own send).
- N3 Two arrivals closer than one Send press (L12, U7), and what an arrival from ANOTHER device does to a draft (L8 is a second staged bubble, U8).
- N4 Multi-participant `senderParticipantIdentifier` behaviour (F6, U6); L5 is one device's own sends.
- N5 The door's `direct` mode and `HarnessModel.arrive` skip the host entirely, so they cannot support any claim in this report.
- N6 The controller seam in `ios/FoolishTests/LiveArrivalFixture.swift` (`arrive`, which calls `MessageTurnController.offerArrival` the way `GameSurface.seatOnBoard` does) starts AFTER the host has delivered and the surface has accepted the bubble.
  It may cite only extension-side callback and order facts (S1 to S10) and claims nothing about host delivery.
  What it pins is the kernel and controller behaviour for a bubble that did arrive.
- N7 The rig's `send` door (`rig.sh arrive`) delivers through real Messages, but the delivered bubble is an OWN send on the host side: it lands on the right of the transcript, `isFromMe` is set, and Messages had a staged bubble in the input field until the Send press. It exercises S2's callback shape and everything downstream of `didReceive`; it is not evidence about remote senders, notifications, or a recipient's input field.

## Unknown (still not established; do not cite as fact)

- U1 Delivery to an EXPANDED drawer, bound or unbound; and whether path A (M1, M2) ever delivers in practice, for any item, on this runtime.
  An own send on an unbound compact drawer is not delivered (L1).
- U2 Which IMCore event calls `pluginPayloadDidChange:` on a bound datasource and what bits 0x13 mean.
  L2 and L6 show a new message in the bound session triggers delivery and one in another session does not; the event itself was not traced.
- U3 The host thread that runs `_handleChatItemDidChange:`.
- U4 Whether a REMOTE arrival while the drawer is closed is queued for the next activation; L7 shows only an own send, which was not.
- U5 Timing of a remote arrival against the transcript (L11 is an own send).
- U6 `senderParticipantIdentifier` for messages from distinct remote people in a group thread (L5 is one device).
- U7 Two arrivals in quick succession through the host (L12).
- U8 What a REMOTE arrival does to a staged, unsent draft (L8 is a second staged bubble replacing it).
- U9 Session replacement on the host side beyond L6 (what `datasourcePayloadDidChange` does for an older bubble).
- U10 iOS 27.0 behaviour of Messages.framework and ChatKit (not extracted, not run).
- U11 The device runtime.
  Nothing here was read from or run on a device.
- U12 Group threads with three or more real participants: nothing in phase 2 had more than two.

## Provenance

Read on 2026-10-01 (phase 1) and run on 2026-10-02 (phase 2) on macOS 27.0 (Darwin 27.0.0) with Xcode 27.0 (27A266a).

iOS 26.3.1 simulator runtime, build 23D8133 (BuildID 03C240C6-1396-11F1-A802-E8D8889322D1), RuntimeRoot:
`/Library/Developer/CoreSimulator/Volumes/iOS_23D8133/Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 26.3.simruntime/Contents/Resources/RuntimeRoot`

| Label | Binary | Size (bytes) | SHA-256 |
| --- | --- | --- | --- |
| B1 | Messages.framework/Messages | 756960 | 91c001651497be012a77580c2102a38f33e5a99f8e888bad25bfbe9564a67e4c |
| B2 | MSMessageExtensionBalloonPlugin | 409680 | 655df36d47a80f880cc55ddab716f556b17a25246226f1321555983d42a19763 |
| B3 | ChatKit.framework/ChatKit | 29217200 | fd9531f8699336a1979c561ca5c7517b4121b2395463dbd588c19fdb4baeb1ae |
| B4 | IMCore.framework/IMCore | 5733392 | 352454e65664b580fbba388d8cc8539ce0e7ab59af7da6954b5f5b8e2461fab7 |
| B5 | iMessageApps.framework/iMessageApps | 175008 | 0ae79fe7a897c659a913795604a00bee7ba8d3a6649fc597e05c13ac4c0c1087 |

iOS 27.0 simulator runtime, build 24A434 (BuildID 0ED37BFC-A77E-11F1-9905-06A7AB4B7BC9), RuntimeRoot:
`/private/var/run/com.apple.security.cryptexd/mnt/com.apple.iPhoneOS.SimulatorRuntime-v24.1.434.0.ZHX4Sg/Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 27.0.simruntime/Contents/Resources/RuntimeRoot`
(the cryptex mount point name changes between boots).

| Label | Binary | Size (bytes) | SHA-256 |
| --- | --- | --- | --- |
| B6 | MSMessageExtensionBalloonPlugin | 408912 | 377eacc670d087c95313c4485fec8e3f968a096c794cf7183aaf1cea0515f2a4 |

All binaries are thin arm64.
The sanctioned test simulator (iPhone 17e, `FC7586CF-78D5-4E61-810E-9449B9AC6C5A`) runs the iOS 26.3 runtime.

### How to re-run on a new runtime

The tools are in `ios/Tools/hostdis/` and never touch a simulator.

1. `ios/Tools/hostdis/dump.sh "<RuntimeRoot>" <out dir> <tag>` compiles `objc_stubs.c`, then for each of B1 to B5 writes `<tag>.<name>.text.txt` (`otool -tV`, with calls through objc stubs named by `symbolize.sh`), `<tag>.<name>.objc.txt` (`xcrun dyld_info -objc`), and records sizes and hashes in `<tag>.binaries.txt` and the runtime's `SystemVersion.plist` in `<tag>.SystemVersion.txt`.
   A binary missing on disk is in the dyld shared cache and is reported and skipped.
   The dumps are large (ChatKit is about 110 MB) and stay out of git.
2. `ios/Tools/hostdis/callers.sh <text dump> <selector> [objc dump]` lists the methods that send a selector.
   Pass the objc dump for B2, which is stripped.
   The two claims this report leans on hardest re-check as:
   - `callers.sh <tag>.plugin.text.txt _didReceiveMessage:conversationState: <tag>.plugin.objc.txt` must print exactly `messageAddedWithDataSource:` and `datasourcePayloadDidChange:updateFlags:` (M3, M4).
   - `callers.sh <tag>.chatkit.text.txt setCurrentViewController:` must still include `_loadBrowserForBalloonPlugin:datasource:` (M2c).
3. Read a method by searching its name in the objc dump for its address, then reading the text dump from that address.
   For B2, `grep -n '<address>' <tag>.plugin.text.txt` finds the start.

`objc_stubs` was checked against an independent decoder (a throwaway Python script) on B2 of iOS 26.3.1: the two produced byte-identical selector tables (811 stubs).
