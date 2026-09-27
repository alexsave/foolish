# Ultimate Tic-Tac-Toe - App Store submission

The checklist for taking `cards.uttt.msg` (App Store Connect record 6815039449) from TestFlight to the store: what is done, what is drafted for pasting, and what only the owner can do.
It follows foolish's own iMessage submission (`docs/IMESSAGE_APP_STORE_SUBMISSION.md` at the repo root), which went through the same review for the same kind of app; where a lesson came from there, it says so.
Nothing below was checked against the live App Store Connect record or a running build: this was written from the repo alone.

## 1. Status

| Item | State | Where |
|---|---|---|
| Privacy Policy URL | Done: `https://uttt.live/privacy-msg`, plain HTML, no script, scoped to the iMessage app | `uttt/web/public/privacy-msg.html` |
| Support URL | Done: `https://uttt.live/support-msg`, plain HTML: how to play, contact, privacy | `uttt/web/public/support-msg.html` |
| Marketing URL (optional) | Done: `https://uttt.live/about`, shaped like foolish's About | `uttt/web/app/about/page.tsx` |
| Privacy manifests | Done: container, extension, and UtttKit (`UserDefaults`, reason CA92.1) | `uttt/ios/*/PrivacyInfo.xcprivacy` |
| Export compliance | Done: `ITSAppUsesNonExemptEncryption = NO` in the container and, new, in the extension | both `Info.plist` |
| No Home Screen icon | Done: `LSApplicationLaunchProhibited` on the container, as foolish ships | `UtttMessagesApp/Info.plist` |
| App icons | Done: 1024x1024 container icon and the Messages icon set incl. 1024x768, all RGB with no alpha | `uttt/ios/*/Assets.xcassets` |
| Review notes | Done, with a one-device path (a replay link) | `uttt/docs/APP_REVIEW_NOTES.md` |
| Store listing copy | Drafted below (section 3) | here |
| Privacy label, age rating | Answers drafted below (sections 4, 5) | here |
| **Diagnostics sheet and seat claim** | **BLOCKER, owner call** (section 6) | `UtttDiagnostics.swift` |
| **Screenshots** | **Owner** (section 7) | the rig |
| **Two-phone screen recording** for review | **Owner**, needs two real phones | App Store Connect |
| **Store name** | **Owner**, availability is only known in App Store Connect | section 2 |
| Build with the new manifests | **Owner**: `xcodegen generate`, archive, check section 8 | Mac |

## 2. App Information

| Field | Value | Notes |
|---|---|---|
| Name | `Ultimate Tic-Tac-Toe` (20) | Generic, and likely taken. Fallbacks, in order: `Ultimate Tic-Tac-Toe Napkin` (27), `Napkin Tic-Tac-Toe` (18). The name in the Messages drawer stays `Ultimate` (`CFBundleDisplayName`) whatever the listing is called. |
| Subtitle | `Tic-tac-toe with a twist` (24) | Fallback: `Nine boards. One game.` (22). |
| Primary category | Games | |
| Game sub-categories | Board, Strategy | |
| Bundle ID | `cards.uttt.msg` | Extension `cards.uttt.msg.MessagesExtension`. |
| Primary language | English (U.S.) | The app ships English only (no `CFBundleLocalizations`). |
| Copyright | `2026 <legal name>` | Owner. |
| Privacy Policy URL | `https://uttt.live/privacy-msg` | The iMessage app's own policy, and only the app's. |
| Support URL | `https://uttt.live/support-msg` | The iMessage app's support page: how to play and the contact address. Plain HTML. |
| Marketing URL | `https://uttt.live/about` | Optional. |

## 3. Version information (the listing)

### Description

```
Ultimate Tic-Tac-Toe, played with a friend right in Messages.

Nine little tic-tac-toe boards make one big one. Win a little board the usual way, three in a line, and win the game by taking three little boards in a line. The catch: the square you play in sends your friend to the matching board. Play the top-right square, and they have to play in the top-right board next.

Easy to learn, surprisingly deep, and made for playing one move at a time over a day of texting.

- Every move is a message. Your friend taps it, plays, and sends it back.
- The board is drawn in pen on a paper napkin, one stroke at a time.
- A yellow highlighter shows where your friend has to play next.
- Changed your mind before sending? Tap another square.
- Every finished game has a replay link anyone can watch at uttt.live.

No accounts, no ads, no tracking, no purchases, nothing to sign up for. Open a conversation, tap the apps button next to the text field, and choose Ultimate.
```

### Promotional text (153 of 170)

```
Tic-tac-toe with a twist, one message at a time. The square you play in decides where your friend plays next. Nine boards, one game, no accounts, no ads.
```

### Keywords (98 of 100)

```
tic tac toe,ultimate,super tic tac toe,noughts and crosses,board game,strategy,two player,imessage
```

### What's New

Not asked on a first version.

## 4. App Privacy (the nutrition label): Data Not Collected

Answer **"No, we do not collect data from this app."**
Apple counts data as collected when it leaves the device in a way the developer or a third party can access it. Nothing here does:

- The app has no network code and no server; a move travels inside the iMessage itself, delivered by Apple to the people in the conversation.
- What a message carries: the moves, the time the game started (the drawing seed), and two 9-byte seat tags, each a hash of the anonymous participant identifier Messages hands the app (`uttt/c/src/uttt_msg.h`). The developer never receives any of it.
- On the device: which side is mine in the newest 256 games (`UtttSeats.swift`, `UserDefaults`). It never leaves the device.
- No names, no free text, no contacts, no analytics, no ads, no SDKs, no purchases.
- The policy covers the app only. uttt.live, where a copied replay link opens, is a separate website and keeps nothing of its own.

This matches `uttt.live/privacy-msg` word for word in substance. Keep the two in step: foolish's lesson was that a reviewer found a contradiction between its label and its own policy without playing a game.

## 5. Age rating

Every content question: **None**. Every capability question (unrestricted web access, user-generated content, messaging or chat features, gambling, contests, advertising): **No**.
Unlike foolish, there is no nickname or any other free text a player can type, so the user-generated-content row that cost foolish its 4+ does not arise.
Expected band: **4+**.

## 6. BLOCKER: the diagnostics sheet and the seat claim ship in Release

1.0(9) added a 1.5-second hold on the rulebook that opens a Diagnostics sheet, and on it **Claim O / Claim X / Clear claim** buttons. They compile into Release on purpose, to diagnose a seat bug on TestFlight (`UtttDiagnostics.swift`: "THE CLAIM IS TEMPORARY. Delete the buttons and uti_msg_claim together once 1.0(9) has shown the record and the sender fallback seat the owner by themselves").

For the store this is a problem twice over:

- **Integrity.** Anyone who finds the hold can claim the other player's seat in any game and move for them.
- **Guideline 2.3.1 (hidden features).** A reviewer who finds an undocumented gesture that opens a debug panel can reject for it.

The fix is the one the file already plans: delete the claim buttons and `uti_msg_claim`, and either delete the Diagnostics sheet or put it behind `#if DEBUG`. It was left alone here because it is Swift that could not be built or run from this session, and because it is the owner's call whether 1.0(9) has told us what it was shipped to learn.

## 7. Screenshots

- **Required:** iPhone 6.9" (1320x2868 portrait) or 6.5" (1242x2688). The app is iPhone-only (`TARGETED_DEVICE_FAMILY: 1`), so no iPad set.
- **What to show** (3 to 5): a game in progress in a real conversation; the expanded board with the highlighted block; a won small board with its big mark; the end of a game with the win line; the invitation bubble.
- **How:** the rig reaches every state on one simulator (`rig.sh devgame <plies>` opens an exact game, and the seeded path covers won, lost, drawn and spectator; see `TESTFLIGHT_PLAN.md`). foolish's note on its own set applies: regenerate from the recipe rather than committing tens of MB of PNGs.
- A screen recording of a two-phone game, for App Review (not the listing), is still open from `TESTFLIGHT_PLAN.md`; the review notes now also give a one-device replay link.

## 8. The build

- `MARKETING_VERSION` is `1.0`; `ship.sh` sets a new build number per upload.
- After `xcodegen generate` and an archive, check that each manifest landed in its bundle: `find <archive>.xcarchive -name PrivacyInfo.xcprivacy` should list three: in the container `.app`, the `.appex` and `UtttKit.framework`. xcodegen treats `.xcprivacy` as a resource (foolish's are picked up the same way, unlisted in `project.yml`), but this is the first uttt build that has them.
- Build with the Xcode and iOS SDK Apple currently requires for uploads; App Store Connect refuses older SDKs at upload.

## 9. Pricing and availability

- Free, no in-app purchases.
- **All territories**, with "make available in new territories" on. foolish shipped with South Korea missing from a hand-picked list and every Korean recipient of a bubble hit "app unavailable" at the store page; select all instead.

## 10. App Review information

- Sign-in required: **No**.
- Contact name, phone and email: owner.
- Notes: paste the text between the rules in `uttt/docs/APP_REVIEW_NOTES.md` (2,862 characters, under 4,000).
- Attachment: the two-phone recording, when it exists.

## 11. After approval

- The rules sheet's new wording (the illustrated mockup, `uttt/docs/RULES.html`) is not in the kernel yet; the app shows the kernel's current six lines.
- Keep `uttt.live/privacy-msg` in step with any change to what a message carries.
