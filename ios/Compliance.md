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

### Device capabilities

- **`UIRequiredDeviceCapabilities` is not declared** in `FoolishMessagesApp/Info.plist`.
  Round-5 B4 (`docs/APP_REVIEW_NOTES.md`): the key used to carry `[armv7]`, the
  32-bit capability, on an arm64-only binary — a metadata contradiction App
  Store Connect rejects at upload, not something a UI pass would ever catch
  (the container is `LSApplicationLaunchProhibited` and never launches, so the
  wrong value never bit at runtime). Owner's call: delete the key rather than
  correct it to `arm64` — it buys this codeless container nothing either way,
  since App Store Connect derives real device capabilities from the arm64
  slice. `ios/project.yml` was checked and does not re-inject this key (or any
  `UIRequiredDeviceCapabilities` value) at `xcodegen generate` time for any
  target, so the deletion holds across regeneration.

### Localization declaration

- **`CFBundleLocalizations = [en, ru, ko]`** now set in both
  `FoolishMessagesApp/Info.plist` and `FoolishMessages/Info.plist`. Round-5 Q1
  (`docs/APP_REVIEW_NOTES.md`): visible strings were already fully localized
  in en/ru/ko at runtime (`FStrings.swift`, switched on
  `Locale.preferredLanguages`) but nothing declared this to the store, so the
  listing would have advertised English-only while the app silently presented
  Russian or Korean. The owner's decision was to ship ru/ko declared in 1.0.
  The in-code `FStrings` table remains the backing store — there is still no
  `.lproj` bundle — and Milestone E4's String Catalog work supersedes it
  later; this key is the store-facing declaration and does not depend on
  which mechanism holds the strings. `ios/project.yml` does not inject or
  override `CFBundleLocalizations` for either target, so this is not
  clobbered at generate time.

  (`FStrings.override`'s App Group scoping bug, noted alongside Q1, is a
  runtime/Swift fix, not a plist or compliance-doc matter, and is out of
  scope for this pass.)

### Encryption

- **`ITSAppUsesNonExemptEncryption = NO`** in both `FoolishMessagesApp/Info.plist`
  and `FoolishMessages/Info.plist` (the `.appex` is a separate binary and needs
  its own declaration — doesn't inherit the container's). Round-5 Q2
  (`docs/APP_REVIEW_NOTES.md`): both files' comments were extended to spell out
  *why* `false` is still correct rather than an inherited template value — the
  FMSG payload path (chain ordering / parent linkage) runs SHA-256
  (`c/src/sha256.c`; swift-crypto is linked) as a **digest**, not a cipher.
  Hashing for ordering/identity is not encryption, so this answer stands, but
  it is explicitly flagged in both plists to be revisited the day the FMSG
  path ever encrypts payload bytes instead of just digesting them.

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

### Pricing & Availability

- Price: Free. No IAP, no pre-orders (submission doc §4).
- Availability: **all territories** — and this is a live fix, not a default:
  **1.0 (3) shipped with South Korea missing.** Verified 2026-08-10 from a
  KR-storefront device — the store page dies on Apple's "not available in
  your country or region" alert. That check lives only in App Store Connect
  (no plist/binary involvement, no new build or review), so the fix is the
  availability edit in the app record: select all territories, confirm South
  Korea, keep new-territories auto-add on. Full symptom/diagnosis/steps:
  submission doc §4a.
- KR needs nothing beyond the checkbox — 4+ card game, gambling questions all
  "None", so the platform questionnaire self-classifies it and no GRAC filing
  applies. Excluding KR was never intended; the app even declares `ko`
  localization (Q1 above).

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
