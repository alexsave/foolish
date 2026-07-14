# iOS redesign — handoff notice

*Written July 2026 for the next agent picking up UI tuning. Branch:
**`claude/ios-redesign`** (pushed). Everything below builds and the test suite is
green; this is a working checkpoint, not a finished design. The brief was: make
the native app feel like the **website** — woven **wool** background, **wooden**
buttons, the real **fern** card back — a "smoother, cleaner" take, with
**drag-to-play** working. Textures are unique per install (a persisted random
seed), like the web.*

## TL;DR state

| Area | State |
|---|---|
| Build | ✅ `xcodebuild -scheme Foolish -destination 'platform=iOS Simulator,name=iPhone 16' build` |
| Tests | ✅ 20 unit/snapshot + a drag-to-play UI test all green |
| Textures (wool/wood/fern) | ✅ ported to Swift, per-install seed, disk-cached |
| Palette + type | ✅ warm wool/wood/fern tokens, condensed titles |
| Wooden buttons | ✅ `FButton` + `FActionBar` |
| Fern card back | ✅ gold/red/amber on black (`FernBack`) |
| Table layout | ✅ opponents arced across top (8 seats fit on iPhone SE), deck corner, compact action plaques |
| Drag-to-play | ✅ drag up = attack, drag onto a battle = cover, drag as defender = pass; tap-then-button still works |
| App icon | ✅ transformed from the web icon (`public/android-chrome-512x512.png`) → 1024² opaque |
| Online vs staging | ⛔ **blocked** — no staging creds; local Supabase blocked by migration drift (see below) |
| Milestone F (signing/TestFlight) | ⛔ **blocked** — needs the Apple Developer account/team |

## What changed (files)

New in `FoolishKit/DesignSystem/`:
- `ProceduralTextures.swift` — wool/wood/fern generators (CoreGraphics, ported
  from the web `WoolBackground.tsx` / `WoodTexture.tsx` / `utils/fernFractal.tsx`).
  Validated visually before integration. Heather flecks toned from the web's
  hot-pink to warm russet (the "cleaner" call).
- `TextureStore.swift` — `@MainActor` singleton, generates once off-main, caches
  PNGs to `Caches/FoolishTextures/<kind>-<seed>.png`, publishes `wool/wood/fern`.
- `Textures.swift` — `WoolBackground`, `WoodSurface`/`.woodSurface(seed:)`, `FernBack`.

Changed: `Tokens.swift` (warm palette + condensed type + `FRadius.button`),
`FButton.swift` (wood + press style), `FActionBar.swift` (compact plaques),
`FCard.swift` (fern back, trump star badge removed), `FHandFan.swift` (drag
gestures), `FBattleGrid.swift` (frame reporting + drop highlight), `TableView.swift`
(arc layout + full drag-to-play wiring), `Models.swift` (public `BattleView` init),
`AppCoordinator.swift`/`RootView.swift` (`-offlinePlayers N` debug launch),
`FoolishApp.swift` (inject + warm `TextureStore`).

Also: `ComponentSnapshotTests.swift` (record-mode type fix + re-recorded refs),
`project.yml` (`GENERATE_INFOPLIST_FILE`, new `FoolishUITests` target),
`cnitro/ios/ios_goldens.c` + `Fixtures/goldens.json` (buffer fix so all 20 golden
games run to completion — see git log), `Tools/IconGen` left as-is (unused; icon
now comes from the web asset).

## How to run / iterate

- **Any table config without tapping**: launch arg `-offlinePlayers N` (2–8) drops
  straight into an N-player offline game. e.g.
  `xcrun simctl launch <udid> cards.foolish.app --args -offlinePlayers 8`.
- **Smallest phone test**: an iPhone SE (3rd gen) sim was used to verify 8-seat
  layout. Create with `xcrun simctl create ... iPhone-SE-3rd-generation ...`.
- **First-launch textures take a few seconds** (wool is ~3M iterations). Until
  ready, fallback colors + vignette show. Cached after; to regenerate, delete the
  app (new install → new random seed → new textures) or clear the cache dir.
- **After any DesignSystem visual change, re-record snapshots**: set
  `record = true` in `ComponentSnapshotTests`, run
  `-only-testing:FoolishTests/ComponentSnapshotTests`, set it back, commit the
  `__Snapshots__` PNGs. (They currently render with textures nil = fallback, since
  `TextureStore` isn't warmed in tests — that's fine and deterministic.)

## Known rough edges / suggested tuning (the "further tuning" the human wants)

1. **Wool reads a bit loud behind the live table.** Options: lower the heather
   contrast in `ProceduralTextures.wool` (the `heather * {26,74,86}` terms), and/or
   strengthen the table scrim (`WoolBackground` vignette, or a table-only darken).
   Bump a cache-version suffix in `TextureStore` keys when you change the generator
   so old cached PNGs are invalidated.
2. **Action plaques float a little above the hand** on tall screens — consider
   anchoring them to a bottom corner (the web puts them bottom-right) or tightening
   the `handZone` height in `TableView`.
3. **Screens not deeply reskinned**: Home + Table are done; `WinView`,
   `SettingsView`, `TutorialView`, `GalleryView`, `ReplaysView`, `AuthView` inherit
   the wool background + wood buttons but haven't had a layout pass.
4. **Card faces are still the rectangular web/iOS style.** The human sketched a
   minimalist "suit glyph + white value" token for **watchOS** (see
   `docs/WATCHOS_LAYOUT.md`) — that's a *separate* effort, but if they want the same
   token direction on iOS, `FCard.face` is the place.
5. **Drag is single-card.** Multi-card attack/cover is only via tap-select today.
   `TableView.dropAction`/`execute` + `FHandFan` would need selection-aware drag.
6. **Dead code**: `FernCardBack.swift` is unused now (superseded by `FernBack`);
   safe to delete.
7. **Perf/size**: wool PNG is ~7 MB on disk (1400×2400). Fine, but if first-launch
   time matters, drop the resolution or add a brief "preparing" state.

## Blocked items + runbooks

### Online testing vs staging — BLOCKED
Two independent blockers:
- **No staging Supabase project/credentials.** The DoD (§16.D) forbids pointing
  tests at prod, so we need a *staging* project's URL + anon key.
- **Local Supabase won't initialize.** `supabase start` (Docker) fails: the
  baseline migration `20250628051540_remote_baseline.sql` is an 8-line repair stub
  that never `CREATE`s the `games` table, so later `ALTER TABLE games` migrations
  error (`relation "games" does not exist`). This is the known migration drift
  (`project_supabase_deploy`) — the real schema lives on the hosted project, not in
  a from-scratch migration. Reconstructing a full local baseline is out of scope
  and risky.

**The Net/ layer is already de-risked**: every supabase-swift call shape (auth
signIn/signUp/session/signOut, `functions.invoke` decode+void overloads,
`FunctionInvokeOptions`, `realtimeV2.channel`/`postgresChange`/
`RealtimePostgresFilter.eq`/`subscribe`/`decodeRecord`, PostgREST
`from/select/eq/execute.value`) was verified against the resolved SDK **2.51.0**
source and compiles in the shipped build.

**Runbook once a staging project exists** (5 min):
1. Put the staging values in an **untracked** `ios/Config/Local.xcconfig` that
   `#include`s `Base.xcconfig`, or set them in `Config/Debug.xcconfig` locally
   (never commit real keys):
   `SUPABASE_URL = https://<staging-ref>.supabase.co` /
   `SUPABASE_KEY = <staging-anon-key>`.
2. `xcodegen generate && xcodebuild ... build`, run in the sim.
3. Walk `docs/PROTOCOL.md §9`: quick-match vs a bot, a 2-device game, spectate,
   resync (foreground/reconnect).

### Milestone F (signing, icon, screenshots, TestFlight) — PARTIAL
- ✅ **Icon** rendered (1024² opaque, from the web asset) and wired into the asset
  catalog.
- ⛔ **Signing / TestFlight** need the Apple Developer **team id** — set
  `DEVELOPMENT_TEAM` in `project.yml` and flip `CODE_SIGN_STYLE` to `Automatic`
  (currently Manual/none for simulator-only dev, §16.0). Then screenshots (capture
  from the sim per device size), privacy labels (`ios/Compliance.md`), and the
  TestFlight upload. All gated on the account.

## Pointers
- Web design source of truth: `src/components/{WoolBackground,WoodTexture,
  TexturedSurface}.tsx`, `src/utils/fernFractal.tsx`, `src/styles/variables.css`,
  `src/components/GameBoard.tsx` + `GameDisplay/*`.
- watchOS design study (separate effort): `docs/WATCHOS_LAYOUT.md`.
- Original iOS spec: `docs/IOS_APP_DESIGN.md`; online protocol: `docs/PROTOCOL.md`.
