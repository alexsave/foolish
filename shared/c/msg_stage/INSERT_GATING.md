# Why an auto-insert on open never reaches the input field

The evidence behind `msg_stage.h` beside this file: `ms_drawer_up`, `ms_insert_silence` and the silent-insert retry budget.
The device log that found the window-sized first appearance is summarised in the header.

Reverse-engineered on 2026-09-23 from Apple binaries on the build Mac, no simulator booted.

## Sources

Device: `~/Library/Developer/Xcode/iOS DeviceSupport/iPhone16,2 26.5.2 (23F84)/Symbols` (arm64e, extracted from the device shared cache, stub names resolve).
The device copy has `Messages.framework` and `ChatKit.framework` but not the Messages balloon plugin bundle.
Simulator: the iOS 26.3 runtime (`iOS_23D8133`), which has everything including `System/Library/Messages/iMessageBalloons/MSMessageExtensionBalloonPlugin.bundle`.
Tools: `otool -tV`, `xcrun llvm-objdump`, `xcrun dyld_info -objc`, and a small script that resolves `__objc_stubs` to selector names.
The owner's phone may run a different 26.x build than 23F84; the gate below is identical in 26.3 (sim) and 26.5.2 (device), so it is unlikely to differ.

## The path of `MSConversation.insert(_:completionHandler:)`

1. Extension, `Messages.framework`: `-[MSConversation insertMessage:completionHandler:]` tail-calls `_insertMessage:skipShelf:NO completionHandler:`, which calls `[self.context stageAppItem:skipShelf:completionHandler:]`.
2. `-[_MSMessageAppExtensionContext stageAppItem:skipShelf:completionHandler:]` sends `_stageAppItem:skipShelf:completionHandler:` to `remoteProxy` over XPC.
   There is no check at all on the extension side: no activity, touch, or presentation test.
3. Host, `Messages.framework`: `-[_MSMessageAppExtensionHostContext _stageAppItem:...]` loads its weak delegate and forwards only if it `respondsToSelector:`; otherwise the call is dropped and the completion is never called.
4. Host, `MSMessageExtensionBalloonPlugin`: `-[MSMessageExtensionBrowserViewController _stageAppItem:skipShelf:completionHandler:]` (sim 0x7a40) hops to the main queue and:
   - fails with `MSMessagesErrorDomain` code 11 if `+[_MSPresentationState isRunningInCameraContext]`;
   - fails with the validation error if `isValidMessagePayload:` returns one (log: "MSMessageExtensionBrowserViewController not valid message item with error %@");
   - substitutes participant names into the layout captions;
   - calls `checkForTouchInRemoteProcessIfNecessaryWithCompletion:`, and in its block (sim 0x7e7c) computes `skipShelf && (allowAllPayloadCommits || hadTouch)`.
   If that is true it sends immediately (`commitPayload:forPlugin:allowAllCommits:completionHandler:`).
   Otherwise it stages with `[sendDelegate startEditingPayload:dismiss:NO forPlugin:<id> completion:<the extension's completion>]`.
5. Host, `ChatKit`: `-[CKChatInputController startEditingPayload:dismiss:forPlugin:completion:]` (device 0x190e8e140) is where the gate lives.

## The touch check is not what blocks insert

The touch tracker (`MSTouchTracker`, `ReportTouchOperation`, `touchUpOccuredForIdentifier:detached:context:pid:`) only matters when `skipShelf` is YES, which is `send(_:)`.
`insert` passes `skipShelf:NO`, so the touch result is computed and ignored: insert is always routed to staging.
This is the documented "send without a recent touch falls back to staging" rule, not an insert rule.

## The gate: `startEditingPayload:dismiss:forPlugin:completion:`

If Digital Touch or handwriting is up, it stages straight away.
Otherwise it asks `_pluginCanMessageAPI:(pluginID)`, which only matches a few fixed Apple plugin IDs (string compares against constants), so for a third-party app it is NO.
Then it asks `switcherPluginCanMessageAPIOnBehalfOfPlugin:(pluginID)` (device 0x190e86d4c):
- if the plugin ID is not equal to `self.browserPlugin.identifier` (and it is not the combined stickers app), it logs `"Denying action for plugin %@ (the current plugin is %@)"` (category `CKChatInputController`, gated by `IMOSLoggingEnabled`) and returns NO;
- otherwise it returns `_switcherPluginCanMessageAPI`.

`_switcherPluginCanMessageAPI` (device 0x190e8706c), with app cards on (`CKIsAppCardsEnabled` is true in MobileSMS on both sim and device; it keys off `IMCurrentlyRunningMessagesClient`, not the platform):
- YES if `appModalIsDisplayed` (the browser transition coordinator `isPresentingFullScreenModal`);
- else YES if `appCardPresentationOverseer.isPresentingCard`;
- else YES if there is a `stickerReactionSession`;
- else the overseer's `presentationBegan` flag.
The pre-app-cards branch instead needed `browserSwitcher.isBrowserReadyForUserInteraction` or `transitionCoordinator.currentConsumer == 2`.

WHEN THE GATE SAYS NO, THE PAYLOAD IS DROPPED AND THE COMPLETION HANDLER IS NEVER CALLED.
The NO branch jumps straight to `updateInteractionTimeForPlugin:` and returns; nothing calls the block, with or without an error.
No `MSMessageErrorCode` is produced for this case.

When the gate says YES, the completion is called at once with `nil` (or, if `_shouldDeferCallbackForInsertingPayload`, which is `_isAppBrowserFullScreen`, it is parked in `setInsertPayloadCompletionHandler:` until later).
The actual staging then runs 100 ms later (`dispatch_after(0x5f5e100 ns)`) through `startEditingPayload:` -> `_startEditingPayload:` -> `compositionWithShelfPluginPayload:completionHandler:`.
That downstream path can still refuse (Send Later unsupported plugin, replace-composition alert), but those are unrelated to opening.

## What this means for a product

The one condition an extension can violate by acting on its own is timing: an insert that arrives before the host counts the app card as presenting (`isPresentingCard`, or `presentationBegan`) or before `browserPlugin` is our plugin is silently discarded.
ChatKit also has "Remote app card controller %s timed out during delayed presentation", i.e. the card waits for the remote view before presenting, so the extension's `willBecomeActive`, `didBecomeActive` and even `viewDidAppear` can run before the card counts as presenting.
Simulator and device run the same code; the difference is only how long that window lasts, which is why the simulator stages and the phone does not.
A product whose insert follows a tap on a drawer that is already on screen never hits it, because by then `isPresentingCard` is true.
An insert issued on open, before any tap, does.
There is no user-gesture requirement for insert, no rate limit, and no device-only branch (no `TARGET_OS_SIMULATOR` or internal-install test on this path).

An `insert` that only retries on an error never retries this refusal, because it never produces one.
Prediction for the device log: `insert attempt 1` followed by neither `inserted` nor `insert ... failed`.
If `Denying action for plugin ... (the current plugin is ...)` also appears, it is the plugin-identity branch; if not, it is the presentation-state branch.

## Confidence

High that insert has no touch gate and that a denied insert never calls its completion: read directly from the device 26.5.2 ChatKit and the 26.3 plugin, and identical in the sim ChatKit.
Medium on which of the two checks refuses an insert on open, and on exactly when `presentationBegan` flips, which is a Swift stored property set through vtable dispatch that was not traced.

## Recommended pattern

Treat "no completion" as a refusal.
Insert, then arm a watchdog of about 500 ms; if the completion has not fired, insert the same message again, up to roughly 8 to 10 tries across about 5 s, and stop at the first completion.
The first accepted try calls back with `nil` at once in compact mode, so a refused try leaves nothing behind and a retry cannot double-stage.
Only retry like this in compact mode, because in expanded mode an accepted completion is deferred and a watchdog would mistake it for a refusal (collapse before inserting).
Keep the generation check so a newer stage cancels the loop.
If every try goes unanswered, fall back to a one-tap door (a "Send invitation" button); a tap is not required by the gate, but by the time the user can tap, the card is presented and the gate passes, which is why tap-driven inserts work.
Log each watchdog firing so a device log shows how many tries the host needed.
