# App Store compliance (committed mirror)

*Milestone F (§11, §16.F3). The authoritative record is App Store Connect; this
file mirrors it so the answers are reviewable in git and don't drift.*

**2026-07-18: `e9b9120` split this into TWO separate App Store products** — the
host app (`cards.foolish.app`, full card-game platform) no longer embeds the
iMessage extension; it ships as its own standalone record
(`cards.foolish.msg`, a codeless `LSApplicationLaunchProhibited` container).
This file now covers both, in that order. **The current submission focus is
iMessage-only** (`cards.foolish.msg`) — the host app's compliance section below
is kept for whenever that one is picked back up, but is **not** part of the
active 36-hour push.

The full, copy-paste-ready submission package for the **standalone iMessage
app** (App Privacy questionnaire in full, age-rating question-by-question
answers, description/keywords/promotional text, App Review notes, a verified
demo replay code, and the reasoning behind every answer) lives in
[`docs/IMESSAGE_APP_STORE_SUBMISSION.md`](../docs/IMESSAGE_APP_STORE_SUBMISSION.md) —
that doc is authoritative for exact wording; this file is the short-form
mirror.

---

## Part 1 — `cards.foolish.msg`, the standalone iMessage app (current focus)

### Bundle / identifiers

- Bundle id: `cards.foolish.msg` (extension: `cards.foolish.msg.MessagesExtension`).
  **Not** the older `cards.foolish.app.MessagesExtension` scheme — that id
  stays registered in the portal for if/when the host app re-embeds a copy,
  but the standalone submission uses the `.msg` ids (`ios/project.yml:98-133`).
- App Group: `group.cards.foolish.msg` — its own group, deliberately not
  shared with the host app's `group.cards.foolish` (`ios/project.yml:101-112`).
- App name: "Foolish — Durak" — availability risk is low: Apple dropped global
  app-name uniqueness enforcement in 2021 (unique within your own developer
  account only), and a search turned up no colliding "Foolish" app among the
  ~10 existing Durak apps. Fallbacks: submission doc §1.
- Category: Games / Card. Primary language: en (submission doc §9 has the
  localization-scope call for the store *listing*).
- Privacy Policy URL: `https://foolish.cards/privacy` (new page, added
  2026-07-18 — was missing entirely before, a hard submission blocker for
  *either* app record).
- Support URL: `https://foolish.cards/support` (new page, added 2026-07-18).
- **No Home Screen icon** (`LSApplicationLaunchProhibited = true`,
  `FoolishMessagesApp/Info.plist`) — discoverable only via the Messages app
  drawer. Open question for App Store Connect (can't verify without an
  account): whether the *store listing page itself* still needs a manually
  uploaded 1024×1024 icon separate from the extension's `.stickersiconset`,
  or whether Apple derives one. Flagged in the submission doc §1 as a
  Track B/C item.

### Encryption

- **`ITSAppUsesNonExemptEncryption = NO`** in both `FoolishMessagesApp/Info.plist`
  and `FoolishMessages/Info.plist` (the `.appex` is a separate binary and needs
  its own declaration — doesn't inherit the container's).

### Privacy labels — this app collects literally nothing

Unlike the host app, `cards.foolish.msg` links **`FoolishKit` only** — no
`FoolishNet`, no Supabase SDK, no account system exists in this binary at all.
There is no "if you create an account" branch to answer:

| Category | Answer |
| --- | --- |
| Data Used to Track You | **None.** |
| Data Linked to You | **None.** |
| Data Not Linked to You | **None.** |

Full per-data-type table (every category Apple's questionnaire lists,
individually): submission doc §2. Every one is "Not Collected" — no camera use
either (QR-scan is a host-app-only feature in `FoolishApp/ReplaysView.swift`,
absent from this binary — verified: zero `NSCameraUsageDescription`/
`AVCaptureDevice` hits anywhere under `FoolishKit`/`FoolishMessages`/
`FoolishMessagesApp`).

### Account deletion (Guideline 5.1.1(v)) — does not apply

No account creation is possible in this app record at all (no auth, no
backend, no `FoolishNet`). Guideline 5.1.1(v) is triggered only when an app
offers account creation — mark this **N/A** in App Store Connect rather than
providing a URL/mechanism that doesn't apply to this specific binary. (The
host app's real `delete-account` function + `/delete-account` page remain
correct and necessary for *that* app record, whenever it's submitted — see
Part 2.)

### Age rating (current simplified system)

Card game, **no wagering / no gambling / no simulated-gambling mechanic**
(Durak has no betting/chips/stakes at all), no chat/UGC feature, no accounts,
no web access → every question in the current questionnaire answers
"None"/"No". Full question-by-question key: submission doc §3.
Expected band: **4+**.

### App Review notes (reviewer script)

Full text: submission doc §5a. Summary: **there is no demo account concept to
even decline** — the app has no sign-in screen of any kind, in any mode, and
no way to launch it outside Messages (`LSApplicationLaunchProhibited`). The
entire review surface is: open Messages → app drawer → New game → play a
move → send. Verified demo replay link (works regardless of which app record
reviews it, since replay decoding is a public web route):
`https://foolish.cards/BOLQXHD5XTJTD7UJOMDTR3ZC53XNYKUQMBCS4PISGG63NKUZHTVE3GKUFQEEY4SA3QLLU4THDGCQ`

---

## Part 2 — `cards.foolish.app`, the host app (deferred — not this push)

Kept for when this is picked back up; not active work right now.

- Bundle id: `cards.foolish.app` (no longer embeds the extension — see the
  2026-07-18 note above). App Group: `group.cards.foolish`.
- In-app account deletion path: **Settings → Delete Account**, calling
  `server/impls/supabase/functions/delete-account/` (confirmed on `main`,
  wired to `AccountService.swift`). Deletion URL for App Store metadata:
  `https://foolish.cards/delete-account`. Still worth one live-DB check
  before relying on it in review (`IOS_APP_DESIGN.md` §17.6 step 6).
- Privacy labels: Data Used to Track You — None. Data Linked to You —
  Identifiers (account user id), User Content (game history/replays tied to
  the account). Data Not Linked to You — crash diagnostics (Apple-collected,
  opt-in). If product analytics are ever added, use a first-party events
  table, not an ad-attribution SDK.
- Age rating: same 4+ expectation; no chat feature exists in the iOS app at
  all (verified by grep — no v1 chat-scope decision needed).
- Reviewer script (draft, needs a refresh once this is picked back up):
  Play → Offline → beat a bot; Replays → paste the demo code above; Settings →
  Delete account works. Fully usable without an account.
- Demo account credentials: not needed — the app is usable without one.
- Blocked on: staging Supabase credentials (online runtime never verified
  against a live backend) — see `docs/IMESSAGE_SHIP_BLOCKERS.md` A2.
