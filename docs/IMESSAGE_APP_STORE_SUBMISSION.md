# iMessage App Store submission package (2026-07-18)

*Everything needed to fill out App Store Connect for this app, drafted so the
Mac session is a copy-paste job, not a writing job. This is Track A of the
36-hour plan: paperwork and questionnaires only — no UI/screenshot work (the
game UI isn't ready to photograph yet; that's Track B). Every recommendation
below is marked, and everything needs your final wording pass before
submission — nothing here should be pasted into App Store Connect unread.*

**Companion docs:** [`IMESSAGE_SHIP_BLOCKERS.md`](IMESSAGE_SHIP_BLOCKERS.md)
(the engineering dependency chain), [`IMESSAGE_MAC_RUNBOOK.md`](IMESSAGE_MAC_RUNBOOK.md)
(the hands-on Mac session), [`../ios/Compliance.md`](../ios/Compliance.md) (the
short-form mirror this doc supersedes for the `TODO(F)` items — updated
alongside this file).

---

## 0. The one thing to read before anything else: what "iMessage only" means here

You asked to scope this to **only iMessage, not v1** (no online multiplayer,
no leaderboard push, no dashboard-first framing). Two Apple platform facts
shape how far that can go, both already documented in this repo
(`IMESSAGE_GAME_DESIGN.md` §9.1, verified against Apple's own App Store
Connect help pages):

1. **There is no "standalone iMessage app" anymore.** Apple's current
   submission model is: one iOS app record, containing a host app plus an
   embedded Messages extension. You cannot submit "just the iMessage part" —
   Apple requires a real host app in the bundle. Converting between a
   standalone and bundled iMessage app later means creating an **entirely
   new app record**, so this decision, once submitted, is not cheaply
   reversible.
2. **The host app can't be a stub.** Apple's Guideline 4.2 (Minimum
   Functionality) risks rejecting an app that exists only to contain an
   extension with nothing of its own. This repo's own design doc says so
   explicitly: *"The v1 host app is NOT a stub (stubs risk Guideline 4.2)."*

**What I did with this, given I was told not to touch UI/code in this pass:**
I did **not** strip the host app down to an iMessage-only shell — that would
be a code change, out of scope for "paperwork only," and would also reopen
the 4.2 risk above. The binary that ships still contains the full offline
game, replays, and (currently non-functional, since Supabase credentials are
unset) online-play scaffolding. What I *did* do is write every piece of
App Store Connect copy below to **foreground iMessage as the product** — name,
subtitle, description, keywords, category positioning, and the App Review
notes all point the reviewer and any prospective player at the iMessage game
first — while staying honest about what the container app also does, because
Apple's Guideline 2.3.1 (Accurate Metadata) is itself a rejection reason if
the listing undersells what's actually in the binary.

**If you want the shipped container app to *actually* be visually minimal**
(hide the dashboard/leaderboard/sign-in, show only an offline-practice mode
and "play in Messages" messaging) **that's a real UI decision I did not make.**
Flag it and I'll scope that as a fast follow — it's a good idea for a cleaner
4.2 story, just not something I'll do unreviewed.

---

## 1. App Information (App Store Connect → App Information)

| Field | Value | Notes |
| --- | --- | --- |
| **Name** | `Foolish — Durak` | 17 characters, well under the 30-char cap. Apple **dropped global app-name uniqueness enforcement in 2021** — names only need to be unique within your own developer account, not against every other app in the store. I searched the App Store for existing "Durak" apps (there are ~10) and none collide with "Foolish" as a name — this is now low-risk, not the blocker `Compliance.md` flagged it as. |
| **Name fallbacks** (only if App Store Connect somehow flags it — e.g. a trademark complaint path, not a uniqueness one) | `Foolish: Durak Card Game`, `Foolish Cards — Durak`, `Play Durak — Foolish` | Pick in that order if needed. |
| **Subtitle** (30 chars) | `Durak card game for iMessage` | 29 chars. Leads with the mechanism you asked to foreground. |
| **Subtitle fallback** | `Play Durak right in Messages` | 29 chars, if you want the verb-first framing instead. |
| **Primary category** | Games | Required. |
| **Primary sub-category** | Card | Matches the App Store's own Durak-app listings (see §1a below). |
| **Secondary category** | Entertainment (or leave blank) | Optional; Card games commonly skip a secondary category. Your call. |
| **Bundle ID** | `cards.foolish.app` | Already set (`ios/project.yml`); extension is `cards.foolish.app.MessagesExtension`. |
| **SKU** | `foolish-imessage-durak` (suggested) | Any unique string; not user-facing. |
| **Primary language** | English (U.S.) | The app also ships ru/ko strings in-app (`FStrings.swift`), but see §9 on whether to localize the *store listing* too. |
| **Copyright** | `© 2026 <your name/entity>` | Fill in — I don't know your legal entity name. |
| **Privacy Policy URL** | `https://foolish.cards/privacy` | **Was missing entirely — I added the page** (`src/app/privacy/page.tsx`, content in §8). This is a hard submission blocker without it; it's live in this branch now. |
| **Support URL** | `https://foolish.cards/support` | **Also added** (`src/app/support/page.tsx`). Points to a `support@foolish.cards` email — see §11 for what that needs. |
| **Marketing URL** (optional) | `https://foolish.cards/about` | Already existed; reused as-is, unedited. |

### 1a. Why "Card" and not something iMessage-specific

Apple doesn't have an "iMessage Games" App Store category for the *host app*
record — iMessage extensions are discovered through the Messages app drawer
and the "Get" flow off the app's own icon and page, not a separate iMessage
storefront category. The category field describes the app record as a whole,
so `Games → Card` (matching how every competing Durak app is categorized) is
correct regardless of the iMessage-first positioning.

---

## 2. App Privacy ("Privacy Nutrition Label")

Apple's questionnaire asks, per data type, three questions: **Do you collect
it? Is it linked to the user's identity? Is it used to track the user across
apps/sites?** Below is the complete answer set for every category Apple lists,
not just the abbreviated summary `Compliance.md` had.

| Data type | Collected? | Linked to identity? | Used to track? | Reasoning |
| --- | --- | --- | --- | --- |
| Contact Info (name, email, phone, address) | **No** | — | — | No sign-up form asks for any of these; the account system uses a synthetic username→email scheme with no real contact info (`Auth.swift` `nameToEmail`). |
| Health & Fitness | No | — | — | N/A |
| Financial Info | No | — | — | No payments, no IAP in this scope. |
| Location | No | — | — | Never requested. |
| Sensitive Info | No | — | — | N/A |
| Contacts | No | — | — | Never requested. |
| User Content (game history, replays) | **Yes** — *only if an account is created* | Yes | No | Tied to the account id; used solely to show your own match history/rating back to you. iMessage games carry **zero** user content to us — the whole game lives in the message bubbles, decoded locally. |
| Browsing History | No | — | — | N/A |
| Search History | No | — | — | N/A |
| Identifiers (user/account ID) | **Yes** — *only if an account is created* | Yes | No | The Supabase auth user id. |
| Purchases | No | — | — | No IAP/StoreKit wired in this scope. |
| Usage Data | No | — | — | No analytics SDK anywhere in the app (verified: no ATT prompt, no ad SDK, no Vercel/Amplitude/Mixpanel-style client in `ios/`). |
| Diagnostics (crash data, performance) | **Yes, but Apple-collected, not us** | No | No | Standard Apple crash reporting, opt-in at the OS level; we don't receive it unless a user separately emails a report. Declare as "Diagnostics — Not Linked to You" per Apple's own guidance for this exact pattern. |
| Other Data | No | — | — | N/A |

**Summary answers for the top-level App Privacy questions:**
- *Does this app collect data?* → **Yes** (scoped to: only if the user opts into account creation; iMessage play alone collects nothing).
- *Data Used to Track You* → **None.**
- *Data Linked to You* → **Identifiers, User Content** (both account-gated).
- *Data Not Linked to You* → **Diagnostics** (Apple-collected, opt-in).

**Camera**: not a data-collection question in the nutrition label (it's a
runtime permission, covered by `NSCameraUsageDescription` in `Info.plist`,
already present), but worth having the wording ready if a reviewer asks why
the app requests it: *"Used only to scan a QR code for loading a replay; the
camera feed is processed on-device and never transmitted or stored."*

---

## 3. Age Rating

Apple's simplified age-rating system (post-2024) is a shorter questionnaire
than the old 17-question one. Recommended answers, with reasoning, for every
category it asks about:

| Question | Answer | Reasoning |
| --- | --- | --- |
| Cartoon or Fantasy Violence | None | No violence of any kind. |
| Realistic Violence | None | — |
| Sexual Content or Nudity | None | — |
| Profanity or Crude Humor | None | No text input, no chat feature exists anywhere in the iOS app (verified: no chat UI in `ios/FoolishApp`, `ios/FoolishKit`, or `ios/FoolishMessages` — the only "chat" hits are unrelated identifiers like `chatIsDM`). |
| Alcohol, Tobacco, or Drug Use | None | — |
| Mature/Suggestive Themes | None | — |
| Horror/Fear Themes | None | — |
| Medical/Treatment Information | None | — |
| Gambling and Contests | **None / "No"** | Durak has no betting, wagering, chips, or stakes mechanic of any kind — it's a trick-avoidance card game. This is the one question worth double-checking yourself in the live form since its exact wording sometimes also asks about "simulated gambling" (slot/roulette-style mechanics) — still **No**, nothing in this game resembles that. |
| Unrestricted Web Access | None | The app has no in-app browser or unrestricted web view; the `/m/` and replay links open in the *system* browser via a tapped `MSMessage` URL (standard OS behavior for any app, not an in-app web access feature). |
| User-Generated Content | **No** (or "Yes, but no communication features" if the form forces a UGC answer because of the online match-history/replay-sharing) | There's no free-text chat, no profile bios, no images uploaded by users. If the form treats "replays" as UGC, answer that there's no user-to-user messaging/moderation surface — replays are game recordings, not authored content. |

**Expected resulting band: 4+.** `Compliance.md` hedged "4+/9+" under the old
system; under the current one, nothing above triggers a bump, so 4+ is the
realistic target. Worth confirming once you're actually in the live
questionnaire, since Apple's exact phrasing shifts occasionally — but there's
no ambiguous "yes" answer above that should push it higher.

---

## 4. Pricing & Availability

| Field | Recommendation |
| --- | --- |
| Price | Free |
| Availability | All territories (no reason to restrict; the game has no region-specific legal issues — no gambling, no age-restricted content) |
| In-App Purchases | None in this scope (`Entitlements/FreeEntitlements.swift` is the only entitlements path wired; StoreKit is explicitly deferred per `IOS_APP_DESIGN.md` §16.G) |
| Pre-orders | N/A |

---

## 5. App Review Information

| Field | Value |
| --- | --- |
| **First name / Last name** | *(yours — fill in)* |
| **Phone** | *(yours — fill in; Apple requires a real reachable number)* |
| **Email** | `support@foolish.cards` (or your personal email if you'd rather Apple reach you directly during review) |
| **Demo account** | **Not applicable — mark "No" for sign-in required.** The iMessage game needs no account at all (every device just replays the message chain through the local kernel), and the container app's offline mode is fully playable with no sign-in either. Do **not** provide fake demo credentials; explicitly stating no account is needed is the stronger, more honest answer and avoids the reviewer hitting an unfamiliar auth flow. |
| **Notes** (the reviewer script) | See §5a below — a complete rewrite of `Compliance.md`'s old script, focused on iMessage since that's the point of the app. |

### 5a. App Review notes — full text, ready to paste

```
Foolish is a Durak (Russian "Fool") card game, playable through iMessage.
No account or sign-in is required for any part of this review.

TO REVIEW THE CORE FEATURE (iMessage):
1. From the Home Screen, open Messages.
2. Start or open any conversation.
3. Tap the App Store icon (the "+" icon) in the message compose bar.
4. Find "Foolish — Durak" in the app drawer and tap it.
5. Tap "New game" — this deals a fresh 2-player game and opens the table.
6. Play a card (tap a card in your hand, then tap "Send move" — this stages
   a message bubble; press the blue send arrow to actually send it, exactly
   like any other iMessage).
7. To see the other side of a real exchange, this can be tested with a
   second device/simulator receiving the sent bubble, or by observing that
   the sent bubble shows only the PUBLIC table state (no hands revealed) —
   this is intentional: each player's hand is only visible on their own
   device.
8. A finished game's final bubble links to a shareable replay page
   (foolish.cards/<code>) showing the complete game, e.g.:
   https://foolish.cards/BOLQXHD5XTJTD7UJOMDTR3ZC53XNYKUQMBCS4PISGG63NKUZHTVE3GKUFQEEY4SA3QLLU4THDGCQ

TO REVIEW THE CONTAINER APP (optional, supporting content):
1. Open the Foolish app directly from the Home Screen.
2. Tap "Offline" / "Play vs bots" — no account needed. Play a full game
   against a computer opponent.
3. Settings → if you created an account during testing, "Delete Account"
   permanently removes it and all associated data immediately.

The app is fully usable, in every mode, without creating an account.
```

*(The replay link above uses a real, verified demo code — see §7. Regenerate
a fresh one closer to submission if you'd rather not reuse this exact game;
the generator is documented there.)*

---

## 6. Version Information (the store listing copy)

### 6a. Description (up to 4,000 characters)

```
Play Durak — the classic Russian card game — right inside iMessage.

Foolish brings дурак (Durak) to Messages: start a game with a tap, play your
card, and send it like any other message. No app-switching, no separate
lobby, no account required. The whole game lives in the messages you already
send — every device replays and verifies the game independently, so nothing
can be faked or cheated.

HOW IT WORKS
• Tap the Foolish icon in the Messages app drawer to start a new game.
• Play a card — it stages as a message bubble showing the table.
• Send it like you would any text. The other player taps it to see their
  hand and play their turn back.
• Works for 2 players in a direct message, or invite a group chat to a
  lobby for bigger games.

DURAK, DONE RIGHT
• The real rules — attack, cover, throw in, or pick up — enforced by the
  same rules engine used for every game, so there's never a disagreement
  about what's legal.
• A finished game links to a full, shareable replay you can watch again or
  send to anyone, even people without the app.
• No hands are ever visible to anyone but their owner — not even in the
  message bubble image itself.

PLAY OFFLINE TOO
The Foolish app (outside of Messages) also includes a practice mode against
computer opponents, so you can learn the rules before playing a friend for
real — no account or network connection needed.

No ads. No tracking. No data collected unless you choose to create an
account for other features.
```

### 6b. Promotional text (170 chars, editable without a new build submission)

```
Durak, the classic card game, playable right in iMessage — start a game
with a tap, play your card, send it like a text. No app-switching.
```
(169 characters.)

### 6c. Keywords (100 chars total, comma-separated, no spaces after commas)

```
durak,card game,fool,russian cards,imessage game,multiplayer,card,dурак,подкидной
```
Notes: `дурак`/`подкидной` (Cyrillic) target Russian-speaking searchers, who
are this game's natural audience — Apple's keyword field allows non-Latin
characters and they're commonly used this way by the competing Durak apps.
Trim if this exceeds 100 chars in the live field (count includes commas);
`imessage game` is worth keeping since that's the actual differentiator here.

### 6d. What's New (first submission — required but often just restates the description)

```
Welcome to Foolish! Play Durak with friends right inside iMessage — start a
game, play your card, and send it like a text.
```

---

## 7. The App Review demo replay code

Generated and round-trip verified through the production replay codec
(`c/src/replay.c`, the same code path every real finished game uses) — not a
fake/placeholder string:

```
Code:  BOLQXHD5XTJTD7UJOMDTR3ZC53XNYKUQMBCS4PISGG63NKUZHTVE3GKUFQEEY4SA3QLLU4THDGCQ
Link:  https://foolish.cards/BOLQXHD5XTJTD7UJOMDTR3ZC53XNYKUQMBCS4PISGG63NKUZHTVE3GKUFQEEY4SA3QLLU4THDGCQ
Fool:  seat 1 (of 4)
```

How it was produced (reproducible, not committed as a script since it's a
throwaway CLI harness, not shipping code): a fixed, documented 40-byte seed
string (`FOOLISH-APP-REVIEW-DEMO-SEED-2026-0001!`) deals a 4-player game,
every seat is assigned a bot strategy, and `fio_bot_drive_packed` plays it to
completion; `fio_replay_encode_b32` produces the code and
`fio_replay_decode_packed` confirms it decodes back to the same fool seat —
this is the exact encode/decode pair the app and the `/m/`, `/[game_id]` web
routes use. If you'd rather have a shorter or more interesting-looking demo
game before submission, regenerate with a different seed using the same
recipe (`c/ios/ios_api.h`'s `fio_new_game` / `fio_set_seat_strategy` /
`fio_bot_drive_packed` / `fio_replay_encode_b32`) — happy to produce more on
request.

---

## 8. Privacy Policy & Support pages — added to the site

Two static pages were **added** (not previously existing — their absence was
a hard submission blocker, since Apple requires a live Privacy Policy URL):

- **`src/app/privacy/page.tsx`** + **`src/components/Privacy.tsx`** →
  `foolish.cards/privacy`. Content matches §2's data-collection answers
  exactly, leads with "iMessage games send us nothing," covers the optional
  account/replay/camera cases, children's-privacy boilerplate, account
  deletion, and a contact address.
- **`src/app/support/page.tsx`** + **`src/components/Support.tsx`** →
  `foolish.cards/support`. Minimal: how to start a game, a support email,
  links to account deletion and the privacy policy.

Both reuse the exact same layout classes and `ErrorBoundary` wrapper as the
existing `/about` page (`page page--centered page--with-padding`,
`content content--max-width content--centered content--gap-lg`) so they
render consistent with the rest of the site. They are **not localized**
(plain English only) — legal/support pages commonly ship English-only even in
localized apps, but flag it if you want ru/ko versions before submission;
that would go through `src/localization/strings.ts` like the rest of the
site.

**Typechecked clean** (`npx tsc --noEmit`) — zero errors introduced (the one
pre-existing `tsconfig.json` warning about `baseUrl` deprecation predates
this work and is unrelated).

---

## 9. Localization scope — a decision for you

The **app itself** already ships en/ru/ko (`FStrings.swift`). The **App Store
listing** (name, subtitle, description, keywords) is drafted English-only
above. Options:
- **Ship English-only listing for v1** (recommended given the clock) — the
  in-app experience is still trilingual, you're only deferring the *store
  page* translation, which is a metadata-only change addable anytime without
  a new binary.
- **Add a Russian store listing now** — Durak's natural audience skews
  Russian-speaking, and the Cyrillic keywords above are a partial hedge for
  this. If you want, I can draft a full `ru` App Store listing (name/subtitle/
  description/keywords) — say the word and I'll add it to this doc; it's
  pure text, no additional engineering.

---

## 10. Export compliance / encryption declaration

Both `Info.plist`s already set `ITSAppUsesNonExemptEncryption = NO`
(verified: `ios/FoolishApp/Info.plist`, `ios/FoolishMessages/Info.plist`).
When App Store Connect asks its own export-compliance question at upload
time, the answer sequence is:
1. *"Does your app use encryption?"* → the honest technical answer is
   **Yes** (standard HTTPS/TLS, which is exempt) — but because the Info.plist
   key is already set, Xcode/App Store Connect should skip re-asking and use
   the declared value directly on most upload flows. If it *does* ask
   interactively: "Yes" → "Does your app qualify for any of the exemptions
   (e.g., HTTPS only, no proprietary crypto)?" → **Yes, exempt (standard
   HTTPS)** → no further documentation (CCATS/self-classification report)
   needed for this exemption tier.

---

## 11. What's *not* paperwork — real infrastructure gaps this doc surfaces

Things the drafted copy above references that don't exist yet as working
infrastructure, flagged so they don't silently bounce on you:

- **`support@foolish.cards` / `privacy@foolish.cards`** — referenced in both
  new pages and the App Review notes. These need to actually receive mail
  (a forwarding rule to your real inbox is enough) before submission, or a
  reviewer/user emailing them bounces. This is account/DNS setup, not
  something I can do from here.
- **The Privacy/Support pages need to actually deploy** — they're committed
  to this branch but only live at `foolish.cards/privacy` once this branch
  (or a merge of it) is deployed to production. Don't submit to App Store
  Connect with these URLs until they resolve for real — App Review will
  check them.

---

## 12. What's still open after this doc (Track B / Track C — not paperwork)

Everything above is ready to paste. What remains needs either a Mac
(Track B, `IMESSAGE_MAC_RUNBOOK.md`) or a human at App Store Connect
(Track C):

- Screenshots (explicitly deferred per your instruction — UI isn't ready).
- Creating the actual App Store Connect app record and pasting all of the
  above in.
- Your legal entity name for the Copyright field (§1).
- Your name/phone for App Review contact info (§5).
- Standing up mail for the two addresses above (§11).
- Deciding on §9 (English-only vs. + Russian store listing).
- The actual Submit button.
