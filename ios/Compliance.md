# App Store compliance (committed mirror)

*Milestone F (§11, §16.F3). The authoritative record is App Store Connect; this
file mirrors it so the answers are reviewable in git and don't drift.*

**2026-07-18: the full, copy-paste-ready submission package (App Privacy
questionnaire in full, age-rating question-by-question answers, description/
keywords/promotional text, App Review notes, a verified demo replay code,
and the reasoning behind every answer) now lives in
[`docs/IMESSAGE_APP_STORE_SUBMISSION.md`](../docs/IMESSAGE_APP_STORE_SUBMISSION.md).
This file is kept as the short-form mirror; the items below are updated to
match but the other doc is authoritative for exact wording.**

## Encryption

- **`ITSAppUsesNonExemptEncryption = NO`** — standard HTTPS only, no custom or
  non-exempt cryptography. Set in `FoolishApp/Info.plist`.

## Privacy labels (§11, §16.F3)

| Category | Answer |
| --- | --- |
| Data Used to Track You | **None** — no ATT prompt, no ad-attribution SDK, no Vercel-style analytics in the app. |
| Data Linked to You | Identifiers (the account user id); User Content (game history / replays tied to the account). |
| Data Not Linked to You | None beyond crash diagnostics (Apple-provided, opt-in). |

If product analytics are ever added, use a first-party events table — **not** an
ad-attribution SDK (§11).

## Account deletion (Guideline 5.1.1(v)) — mandatory

- In-app path: **Settings → Delete Account** (§16.E3), which calls the deletion
  edge function. **Confirmed on `main`**: `server/impls/supabase/functions/delete-account/`
  exists and is wired to `AccountService.swift`. Still worth one live-DB check
  before relying on it in review (`IOS_APP_DESIGN.md` §17.6 step 6).
- Account deletion URL (App Store metadata field): **`https://foolish.cards/delete-account`**
  (already existed as `src/app/delete-account`) — also linked from the new
  `/privacy` and `/support` pages (see the submission doc §8).

## Age rating (current simplified system)

- Card game, **no wagering / no gambling / no simulated-gambling mechanic**
  (Durak has no betting/chips/stakes at all) → answer every violence/mature/
  gambling/UGC question **"None"/"No"**. Full question-by-question answer key:
  submission doc §3. Expected band: **4+**.
- No chat feature exists anywhere in the iOS app (verified by grep across
  `ios/FoolishApp`, `ios/FoolishKit`, `ios/FoolishMessages` — no chat UI) — the
  UGC/communication questions are a clean "No", no v1 chat-scope decision
  needed.

## App Review notes (reviewer script)

Full text (iMessage-first, matching the "iMessage only" positioning):
submission doc §5a. Summary: no demo account needed anywhere (mark
"No" for sign-in required); the primary path sends the reviewer straight into
Messages → New game; the container app's offline mode is the secondary/
optional path. Verified demo replay link:
`https://foolish.cards/BOLQXHD5XTJTD7UJOMDTR3ZC53XNYKUQMBCS4PISGG63NKUZHTVE3GKUFQEEY4SA3QLLU4THDGCQ`
(generated and round-trip decoded through the production replay codec —
recipe in the submission doc §7 if you want a fresh one).

- Demo account credentials: **N/A by design** — no account is required to
  review any part of the app (iMessage or offline).

## Bundle / identifiers (§16.F1)

- Bundle id: `cards.foolish.app` (extension: `cards.foolish.app.MessagesExtension`, Milestone G).
- App Group: `group.cards.foolish`.
- App name: "Foolish — Durak" — **availability risk is now low**: Apple dropped
  global app-name uniqueness enforcement in 2021 (names only need to be
  unique within your own developer account), and a search turned up no
  colliding "Foolish" app among the ~10 existing Durak apps. Fallback names
  if still flagged: submission doc §1.
- Category: Games / Card. Primary language: en (submission doc §9 has the
  localization-scope call for the store *listing* — the app itself is already
  en/ru/ko).
- Privacy Policy URL: `https://foolish.cards/privacy` (new page, added
  2026-07-18 — was missing entirely before, a hard submission blocker).
- Support URL: `https://foolish.cards/support` (new page, added 2026-07-18).
