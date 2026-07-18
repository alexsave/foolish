# iMessage App Store submission package (2026-07-18)

*Everything needed to fill out App Store Connect for the standalone iMessage
app, drafted so the Mac session is a copy-paste job, not a writing job. This
is Track A of the 36-hour plan: paperwork and questionnaires only — no
UI/screenshot work (the game UI isn't ready to photograph yet; that's Track
B). Every recommendation below needs your final wording pass before
submission — nothing here should be pasted into App Store Connect unread.*

> **Revision 2, same session.** Revision 1 of this doc was written against a
> stale assumption — that Apple forces a bundled host-app+extension record and
> the iMessage game has to ride the host app's Chain-A blockers. **That's no
> longer true on this tree.** A commit I hadn't fully read when I wrote
> revision 1 (`e9b9120`, right before my own work started) reversed that
> decision: the iMessage game now ships as its **own, genuinely standalone**
> App Store record — `cards.foolish.msg`, a codeless
> `LSApplicationLaunchProhibited` container with no Home Screen icon,
> discoverable only through the Messages app drawer, linking `FoolishKit`
> only (no `FoolishNet`, no Supabase, no accounts, no backend, no camera —
> nothing). This is a **much simpler** submission than revision 1 described:
> flat "collects nothing" privacy answers, no account-deletion requirement,
> no demo-account question, and no "container app" secondary review path
> because there *is* no container app UI. Every section below is corrected.
> If you already looked at revision 1 (bundle id `cards.foolish.app.*`,
> conditional "if you create an account" privacy answers) — discard that;
> this revision supersedes it entirely.

**Companion docs:** [`IMESSAGE_SHIP_BLOCKERS.md`](IMESSAGE_SHIP_BLOCKERS.md)
(the engineering dependency chain — also flagged as stale on this same point,
correction banner at its top), [`IMESSAGE_MAC_RUNBOOK.md`](IMESSAGE_MAC_RUNBOOK.md)
(the hands-on Mac session), [`../ios/Compliance.md`](../ios/Compliance.md)
(short-form mirror, now split into Part 1 = this app, Part 2 = the deferred
host app).

---

## 0. What "standalone" actually means here, precisely

`ios/project.yml` (read its own extensive comments at the `Foolish`,
`FoolishMessagesApp`, `FoolishMessages`, and `FoolishNet` target definitions —
they're the ground truth this doc is drafted from) now defines two
independent products:

| | `Foolish` (host app) | `FoolishMessagesApp` (**this submission**) |
| --- | --- | --- |
| Bundle id | `cards.foolish.app` | `cards.foolish.msg` |
| Links | `FoolishKit` + `FoolishNet` (Supabase, auth, online play) | `FoolishKit` **only** |
| Home Screen icon | Yes | **No** — `LSApplicationLaunchProhibited = true`; discoverable only via the Messages app drawer |
| Accounts / sign-in | Yes | **None exist in this binary** |
| Network calls | Yes (online multiplayer, replays sync) | **None** — an iMessage game is entirely peer-to-peer through the message chain itself, replayed and validated locally on each device |
| App Group | `group.cards.foolish` | `group.cards.foolish.msg` (separate, deliberately not shared) |
| Guideline 4.2 stub risk | N/A (real app) | **N/A here too** — Apple's Minimum Functionality guideline doesn't apply the same way to a Messages-only app, which is expected to have no standalone UI by design; this is a well-established, still-supported app type, not an edge case |
| Guideline 5.1.1(v) account deletion | Applies (real accounts) | **Does not apply** — no account creation is possible |

You asked for **only this — the iMessage app, not v1, not the host app.**
Given the architecture above, that's now a clean, literal scope: this
submission is entirely self-contained and has zero dependency on the host
app's online/account/staging work. The two products can go to the store on
completely independent timelines.

---

## 1. App Information (App Store Connect → App Information)

| Field | Value | Notes |
| --- | --- | --- |
| **Name** | `Foolish — Durak` | 17 characters. Apple dropped global app-name uniqueness enforcement in 2021 (unique within your own developer account only); a search found ~10 existing Durak apps, none named "Foolish" — low risk. |
| **Name fallbacks** | `Foolish: Durak Card Game`, `Foolish Cards — Durak`, `Play Durak — Foolish` | In that order, only if flagged. |
| **Subtitle** (30 chars) | `Durak card game for iMessage` | 29 chars. |
| **Subtitle fallback** | `Play Durak right in Messages` | 29 chars. |
| **Primary category** | Games | Required. |
| **Primary sub-category** | Card | Matches how every competing Durak app is categorized. |
| **Secondary category** | Optional — leave blank or Entertainment | Your call. |
| **Bundle ID** | `cards.foolish.msg` | **Not** `cards.foolish.app.*` — that's the deferred host app. Extension is `cards.foolish.msg.MessagesExtension`. |
| **SKU** | `foolish-imessage-durak` (suggested) | Any unique string. |
| **Primary language** | English (U.S.) | See §9 for the store-listing localization call. |
| **Copyright** | `© 2026 <your name/entity>` | Fill in. |
| **Privacy Policy URL** | `https://foolish.cards/privacy` | Added this session (`src/app/privacy/page.tsx`) — was missing entirely, a hard blocker for either app record. |
| **Support URL** | `https://foolish.cards/support` | Added this session. |
| **Marketing URL** (optional) | `https://foolish.cards/about` | Pre-existing, unedited. |

### 1a. Open question I can't resolve without an App Store Connect account

`FoolishMessagesApp` has **no `.xcassets` and no app-icon asset at all**
(verified: `find ios/FoolishMessagesApp -iname "*.xcassets"` → nothing) — it
never needs one on-device since it has no Home Screen presence. What's
unverified is whether App Store Connect's **store listing page** still wants
a manually-uploaded 1024×1024 icon for a Messages-only app, or derives one
from the extension's icon set (`ios/FoolishMessages/Assets.xcassets/iMessage
App Icon.stickersiconset`, already committed, jester-Д cropped to 4:3). This
is a two-minute check the first time you're in App Store Connect — if it asks
for one, the jester-Д source art (used for the host app's `AppIcon.png`) can
be re-exported square; flag it and I'll produce that PNG.

---

## 2. App Privacy ("Privacy Nutrition Label") — this app collects nothing

Because `FoolishMessagesApp` links `FoolishKit` alone — no `FoolishNet`, no
Supabase SDK, no account system exists in this binary — there is no
conditional "if you create an account" branch to answer, unlike a typical
app. The honest, complete answer is flat across every category Apple's
questionnaire lists:

| Data type | Collected? |
| --- | --- |
| Contact Info | No |
| Health & Fitness | No |
| Financial Info | No |
| Location | No |
| Sensitive Info | No |
| Contacts | No |
| User Content | No — a finished game's replay is generated by the *web* route (`foolish.cards/<code>`) from data that already lived in the message chain itself; the extension never uploads anything anywhere |
| Browsing History | No |
| Search History | No |
| Identifiers | No — no account, no device identifier collected, no analytics |
| Purchases | No — no IAP in this product |
| Usage Data | No — no analytics SDK anywhere in the app |
| Diagnostics | Apple-collected crash data only, if the user has opted in at the OS level — we never receive it |
| Other Data | No |

**Top-level summary answers:**
- *Does this app collect data?* → **No.**
- *Data Used to Track You* → **None.**
- *Data Linked to You* → **None.**
- *Data Not Linked to You* → **None** (Apple's own opt-in crash diagnostics
  don't count as app-collected).

**Camera**: not used at all by this app (verified: zero
`NSCameraUsageDescription`/`AVCaptureDevice` references anywhere under
`FoolishKit`, `FoolishMessages`, or `FoolishMessagesApp` — QR-scanning a
replay is a host-app-only feature in `FoolishApp/ReplaysView.swift`, entirely
absent from this binary). No permission of any kind is requested by this app.

---

## 3. Age Rating

Recommended answers for Apple's current simplified (post-2024) questionnaire:

| Question | Answer | Reasoning |
| --- | --- | --- |
| Cartoon or Fantasy Violence | None | No violence. |
| Realistic Violence | None | — |
| Sexual Content or Nudity | None | — |
| Profanity or Crude Humor | None | No text input or chat feature exists anywhere in this app. |
| Alcohol, Tobacco, or Drug Use | None | — |
| Mature/Suggestive Themes | None | — |
| Horror/Fear Themes | None | — |
| Medical/Treatment Information | None | — |
| Gambling and Contests | None / "No" | Durak has no betting, wagering, chips, or stakes mechanic — a pure trick-avoidance card game. Double-check this question's exact live wording (it sometimes also asks about simulated-gambling *mechanics*, e.g. slot/roulette visuals) — still "No" either way. |
| Unrestricted Web Access | None | No in-app browser; a tapped `MSMessage` URL opens the *system* browser, standard OS behavior for any app. |
| User-Generated Content | No | No communication features, no free-text input, no user-authored content of any kind in this app. |

**Expected band: 4+.** Nothing above triggers a bump under the current
questionnaire.

---

## 4. Pricing & Availability

| Field | Recommendation |
| --- | --- |
| Price | Free |
| Availability | All territories — no gambling/age-restricted content, nothing region-sensitive |
| In-App Purchases | None |
| Pre-orders | N/A |

---

## 5. App Review Information

| Field | Value |
| --- | --- |
| **First name / Last name** | *(yours — fill in)* |
| **Phone** | *(yours — fill in; Apple requires a reachable number)* |
| **Email** | `support@foolish.cards` (or your personal email) |
| **Sign-in required?** | **No** — there is no sign-in screen anywhere in this app, in any mode. Mark "No" and leave the demo-account fields blank; there's genuinely nothing to fill in, not just "not needed." |
| **Notes** | §5a below. |

### 5a. App Review notes — full text, ready to paste

```
Foolish is a Durak (Russian "Fool") card game that ships ENTIRELY inside
iMessage — there is no separate app to launch; this app record has no Home
Screen icon by design (a standard "Messages-only" app configuration) and is
discoverable only through the Messages app drawer. No account or sign-in
exists anywhere in this app.

TO REVIEW:
1. From the Home Screen, open Messages.
2. Start or open any conversation.
3. Tap the App Store icon (the "+" icon) in the message compose bar.
4. Find "Foolish — Durak" in the app drawer and tap it.
5. Tap "New game" — this deals a fresh 2-player game and opens the table.
6. Play a card (tap a card in your hand, then tap "Send move" — this stages
   a message bubble; press the blue send arrow to actually send it, exactly
   like any other iMessage).
7. The sent bubble's image shows only the PUBLIC table state — no hands are
   ever revealed in the bubble itself, by design; each player's own hand is
   only visible when THEY open the game on their own device.
8. A finished game's final bubble links to a shareable replay page
   (foolish.cards/<code>) showing the complete game, e.g.:
   https://foolish.cards/BOLQXHD5XTJTD7UJOMDTR3ZC53XNYKUQMBCS4PISGG63NKUZHTVE3GKUFQEEY4SA3QLLU4THDGCQ

This app collects no data of any kind (see the App Privacy section) and
requests no permissions.
```

---

## 6. Version Information (the store listing copy)

### 6a. Description (up to 4,000 characters)

```
Play Durak — the classic Russian card game — right inside iMessage.

Foolish brings дурак (Durak) to Messages: start a game with a tap, play your
card, and send it like any other message. No app-switching, no separate
lobby, no account, and nothing to download outside of Messages itself.

HOW IT WORKS
• Tap the Foolish icon in the Messages app drawer to start a new game.
• Play a card — it stages as a message bubble showing the table.
• Send it like you would any text. The other player taps it to see their
  hand and play their turn back.
• Works for 2 players in a direct message, or invite a group chat to a
  lobby for bigger games (up to 8 players).

DURAK, DONE RIGHT
• The real rules — attack, cover, throw in, or pick up — enforced by the
  same rules engine on every device, so there's never a disagreement about
  what's legal.
• A finished game links to a full, shareable replay you can watch again or
  send to anyone, even people without the app.
• No hands are ever visible to anyone but their owner — not even in the
  message bubble image itself.

No ads. No tracking. No account. No data collected — nothing to sign up for,
nothing to configure. Just open Messages and play.
```

*(The previous revision's "play offline too" paragraph is removed — that's a
different app record (the host app), not this one. Don't describe features
this specific binary doesn't have.)*

### 6b. Promotional text (170 chars)

```
Durak, the classic card game, playable right in iMessage — start a game
with a tap, play your card, send it like a text. No app-switching.
```
(169 characters.)

### 6c. Keywords (100 chars total, comma-separated)

```
durak,card game,fool,russian cards,imessage game,multiplayer,card,дурак,подкидной
```
Cyrillic keywords (`дурак`, `подкидной`) target Russian-speaking searchers —
this game's natural audience — and Apple's keyword field accepts non-Latin
characters. Trim if the live field's char-count differs from this estimate.

### 6d. What's New (first submission)

```
Welcome to Foolish! Play Durak with friends right inside iMessage — start a
game, play your card, and send it like a text.
```

---

## 7. The App Review demo replay code

Generated and round-trip verified through the production replay codec
(`c/src/replay.c`) — not a placeholder:

```
Code:  BOLQXHD5XTJTD7UJOMDTR3ZC53XNYKUQMBCS4PISGG63NKUZHTVE3GKUFQEEY4SA3QLLU4THDGCQ
Link:  https://foolish.cards/BOLQXHD5XTJTD7UJOMDTR3ZC53XNYKUQMBCS4PISGG63NKUZHTVE3GKUFQEEY4SA3QLLU4THDGCQ
Fool:  seat 1 (of 4)
```

Produced with a fixed, documented 40-byte seed string
(`FOOLISH-APP-REVIEW-DEMO-SEED-2026-0001!`): dealt a 4-player game, assigned
every seat a bot strategy, drove it to completion via `fio_bot_drive_packed`,
then `fio_replay_encode_b32` / `fio_replay_decode_packed` confirmed the
round-trip. This is a throwaway CLI harness against the shipping engine
(`c/ios/ios_api.h`'s public entry points), not a committed script — happy to
regenerate with a different seed on request.

---

## 8. Privacy Policy & Support pages — added to the site

Two static pages were added (previously missing entirely — a hard submission
blocker for *either* app record, since Apple requires a live Privacy Policy
URL):

- **`src/app/privacy/page.tsx`** + **`src/components/Privacy.tsx`** →
  `foolish.cards/privacy`. Already leads with the correct, unconditional
  claim for this app: *"Playing a game in iMessage sends no data to us at
  all."* It also covers the host app's separate, optional account system
  (accurate for that product, not applicable to this one) — one shared
  policy page across both app records is normal practice and doesn't need
  splitting.
- **`src/app/support/page.tsx`** + **`src/components/Support.tsx`** →
  `foolish.cards/support`. Generic support contact + links; no changes
  needed for this revision.

Both reuse the `/about` page's exact layout classes and `ErrorBoundary`
wrapper. **Not localized** (English only) — flag if you want ru/ko versions.
**Verified with a real production build** (`npm run build`, dependencies
actually installed): both routes statically prerendered clean alongside every
existing route, plus a clean `tsc --noEmit` pass.

---

## 9. Localization scope — a decision for you

The app itself ships en/ru/ko (`FStrings.swift`). The store *listing* is
drafted English-only above.
- **Ship English-only listing** (recommended given the clock) — addable
  anytime later as a metadata-only change, no new binary needed.
- **Add a Russian listing now** — Durak's natural audience skews
  Russian-speaking. Say the word and I'll draft a full `ru` listing
  (name/subtitle/description/keywords) — pure text, no engineering.

---

## 10. Export compliance / encryption declaration

Both `FoolishMessagesApp/Info.plist` and `FoolishMessages/Info.plist` already
set `ITSAppUsesNonExemptEncryption = NO` (the `.appex` needs its own
declaration — doesn't inherit the container's). If App Store Connect asks
interactively at upload: "Does your app use encryption?" → **Yes** (standard
HTTPS/TLS) → "Does it qualify for an exemption (e.g. HTTPS only)?" → **Yes,
exempt** → no further documentation needed. In practice this app makes **no
network calls at all**, so even the "Yes, standard HTTPS" answer is generous;
either answer is defensible.

---

## 11. What's *not* paperwork — real infrastructure gaps this doc surfaces

- **`support@foolish.cards` / `privacy@foolish.cards`** — referenced in the
  new pages and the App Review notes. These need to actually receive mail
  (a forwarding rule is enough) before submission. Account/DNS setup, not
  something draftable from here.
- **The Privacy/Support pages need to actually deploy** to
  `foolish.cards/privacy` and `/support` before you put those URLs in App
  Store Connect — they're committed to this branch, not yet live.

---

## 12. What's still open after this doc (Track B / Track C — not paperwork)

- Screenshots (deferred per your instruction — UI isn't ready). Per Apple's
  own current rules, a Messages-only app typically still needs at least one
  screenshot showing the extension in Messages for the store listing — worth
  confirming the exact requirement once you're in App Store Connect.
- Creating the actual App Store Connect app record for `cards.foolish.msg`
  and pasting everything above in.
- Registering the `cards.foolish.msg` / `cards.foolish.msg.MessagesExtension`
  App IDs and the `group.cards.foolish.msg` App Group in the Apple Developer
  portal (Automatic signing may do this on first build — worth confirming).
- Your legal entity name (Copyright, §1) and contact info (§5).
- Standing up mail for the two addresses above (§11).
- The §1a icon-for-listing question and the §9 localization-scope call.
- A first Mac build/run of the `FoolishMessagesApp` scheme specifically
  (`IMESSAGE_MAC_RUNBOOK.md` targets the extension generally; confirm the
  runbook's steps still match the new standalone target names — flag if you
  want me to re-check the runbook against `e9b9120`'s target rename next).
- The actual Submit button.
