# App Store compliance (committed mirror)

*Milestone F (§11, §16.F3). The authoritative record is App Store Connect; this
file mirrors it so the answers are reviewable in git and don't drift. Fill the
`TODO(F)` items when the app record is created.*

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
  edge function. `TODO(F)`: confirm the endpoint exists (Oracle doc §4 item 4) —
  **it BLOCKS submission** if not. Do not fake it with a mailto.
- Account deletion URL (for the App Store metadata field): `TODO(F)`.

## Age rating (July-2025 system)

- Card game, **no wagering / no gambling** → answer the gambling-themes
  questionnaire **"no"**. Expected band 4+/9+.
- User-generated content: chat, if shipped, should be **fixed-emoji only** in v1
  to keep the questionnaire clean (free-text chat changes the moderation
  answers). `TODO(F)`: confirm what chat, if any, ships in v1.

## App Review notes (reviewer script, §16.F5)

> 1. Play → **Offline** → beat **Espresso**.
> 2. **Replays** → paste code `<committed demo code, TODO(F)>`.
> 3. **Settings** → **Delete account** works.
> The app is fully usable **without an account**.

- Demo account credentials: `TODO(F)`.
- Note for the reviewer: offline bots and replays are fully functional without a
  network or account (the 4.2 substance argument, §11).

## Bundle / identifiers (§16.F1)

- Bundle id: `cards.foolish.app` (extension: `cards.foolish.app.MessagesExtension`, Milestone G).
- App Group: `group.cards.foolish`.
- App name: "Foolish — Durak" (`TODO(F)`: confirm availability; fallbacks in review notes).
- Category: Games / Card. Primary language: en.
