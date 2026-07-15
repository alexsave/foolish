# iOS bot names — the Russia map (explosives → cities)

*Design doc + implementation spec (NOT yet implemented — this doc is the
work order). The iOS app renames the bot roster from explosives to Russian
cities and Russian-diaspora hubs, so the App Store age-rating questionnaire
stays boring (no weapon/explosive references anywhere in the product
surface). **The website keeps the explosive names.** Strategy KEYS never
change anywhere — this is a render-time display map, implemented entirely
client-side. Designed 2026-07-15.*

## 1. The mapping (primary scheme: cities)

**The gimmick: a bot's strength is its closeness to Moscow.** The ladder is a
journey home — the weakest bot suns itself in Miami; the strongest IS Moscow.
Air distance to the Kremlin decreases strictly monotonically as ELO climbs,
and the bot picker surfaces it as a flavor line ("1,420 km from Moscow" /
Moscow: "The Kremlin itself"), which makes the strength order *legible*
without a single number of ELO shown.

| # | Strategy key | en | ru | ko | ~km | Why this city |
|---|---|---|---|---|---|---|
| 1 | `random` | Miami | Майами | 마이애미 | 9,100 | Sunny Isles Beach = "Little Moscow", the farthest diaspora outpost; beach chaos for the chaos bot |
| 2 | `simple_heuristic` | Brighton Beach | Брайтон-Бич | 브라이턴 비치 | 7,520 | Brooklyn's "Little Odessa" — the biggest Russian-speaking hub in the West; plays the old-school basics |
| 3 | `handwritten` | Vladivostok | Владивосток | 블라디보스토크 | 6,430 | Where the long road home starts — end of the Trans-Siberian |
| 4 | `espresso` | Khabarovsk | Хабаровск | 하바롭스크 | 6,140 | One stop closer, Far East |
| 5 | `robusta` | Novosibirsk | Новосибирск | 노보시비르스크 | 2,810 | Siberia's science capital (Akademgorodok) — fitting for the first Monte-Carlo bot |
| 6 | `firecracker` | Yekaterinburg | Екатеринбург | 예카테린부르크 | 1,420 | The Urals — the Europe/Asia border; the ladder's "Medium" rung |
| 7 | `gunpowder` | Samara | Самара | 사마라 | 860 | Volga aerospace city (owner-requested) |
| 8 | `blackpowder` | Kazan | Казань | 카잔 | 720 | Volga powerhouse; the ladder's "Hard" rung |
| 9 | `cordite` | St. Petersburg | Петербург | 상트페테르부르크 | 635 | The second capital — the previous champion |
| 10 | `octogen` | Moscow | Москва | 모스크바 | 0 | Top dog. Non-negotiable. |

Max tiers compose: the site's `Cordite Max` / `Octogen Max` render as
`St. Petersburg Max` / `Москва Макс` / `모스크바 맥스` (localized "Max"
suffix, key `ios.bot.max`).

Notes on the research (2026-07-15):

- Brighton Beach, Brooklyn — "Little Odessa" — formed by the 1970s Soviet
  Jewish emigration wave; with Sheepshead Bay it is one of the largest
  Russian-speaking communities in the Western world (~100–200k speakers).
  We use the *place's own name* (Brighton Beach), **not** "Little Odessa" —
  Odessa is a Ukrainian city and we deliberately keep Ukrainian place names
  out of a Russia-themed roster.
- Sunny Isles Beach (metro Miami) — "Little Moscow" — the largest
  Russian-born population in Florida. "Miami" is the display name because
  it's globally recognizable; the doc records the real referent.
- All Russian-Federation picks are large, neutral, recognizable cities with
  standard exonyms in every locale we ship. No disputed territories, no
  Ukrainian/Belarusian/Kazakh cities, no politician-adjacent place names.
- ru uses the colloquial-official short form «Петербург» (15-char
  «Санкт-Петербург» wrecks seat badges; «Питер» is too slangy for UI).

## 2. Localization: names are keys, translated at render time

The user-visible requirement: in the Korean locale a bot must read
`% 모스크바 1`, not `% Moscow 1`. The trick is that bot names reach the UI
from three different sources:

| Source | Example | Localization point |
|---|---|---|
| Offline roster picker (client picks the bot) | `octogen` | trivially — the client owns the string |
| Online nicknames (DB rows, later milestone) | `% Octogen Max 2` | render-time transform of the server string |
| Replay blobs / history (names embedded at encode time) | `% Cordite 1` | render-time transform at playback |

**Rule: strategy-derived bot names are treated as KEYS and localized at
render time — never at storage time.** Replay codes stay byte-identical
across locales, DB rows never carry localized strings, and switching the app
language re-renders every bot name instantly.

### Implementation spec (one new file + five small touches)

**New file `ios/FoolishKit/DesignSystem/BotNames.swift`** with exactly three
public entry points (this is the whole API — every current and future
surface calls these, never re-derives):

```swift
public enum BotNames {
    /// Roster strategy key (EngineC.roster() name, e.g. "octogen") →
    /// localized display name. Unknown keys degrade to .capitalized.
    public static func display(strategy key: String) -> String {
        let lookup = "ios.bot.\(key)"
        let s = FStrings.t(lookup)
        return s == lookup ? key.capitalized : s
    }

    /// Localized picker flavor line ("1,420 km from Moscow" / Moscow:
    /// "The Kremlin itself"), from a KM table keyed by strategy
    /// (see §1's km column; format the number with NumberFormatter).
    public static func flavorLine(strategy key: String) -> String?

    /// Server / replay nicknames: "% <Base> [Max] [<n>]" → localized,
    /// preserving the % prefix and index ("% Octogen Max 2" →
    /// ru "% Москва Макс 2"). Parse: strip "%", peel trailing integer,
    /// peel " Max", look Base up in a website-base→strategy-key table
    /// (Random/Simple Heuristic/Handwritten/Espresso/Robusta/Firecracker/
    /// Gunpowder/Blackpowder/Cordite/Octogen). Unknown bases (humans,
    /// easter eggs like "% 0x00C0FFEE") return unchanged.
    public static func displayNickname(_ raw: String) -> String
}
```

**Strings**: add the `ios.bot.*` keys to `FStrings.swift` (en/ru/ko in the
same commit, per the §16.E4 identical-key-sets rule): the ten city names
from §1's table, plus `ios.bot.max` ("Max"/"Макс"/"맥스"),
`ios.bot.km` ("{km} km from Moscow"/"{km} км от Москвы"/"모스크바에서 {km} km"),
`ios.bot.km0` ("The Kremlin itself"/"Сам Кремль"/"크렘린 그 자체").

**Wiring** (all display-side, no engine changes):

1. `HomeView` bot picker: `Text(currentBot.name.capitalized)` →
   `Text(BotNames.display(strategy: currentBot.name))`, plus the
   `flavorLine` as a dim caption under the name (with `lineLimit(1)` +
   `minimumScaleFactor` — Yekaterinburg is long).
2. Offline seat names: the kernel's offline view carries no names, so
   choose them at setup — `AppCoordinator.startOffline` builds
   `[seat: localizedName]` (numbered "Moscow 1/2/3"-style when several
   copies sit down) and passes it to a new `LocalGame.seatNames` property,
   exposed on the `GameSession` protocol with a `[:]` default so
   `OnlineGame` is untouched.
3. `TableView` seat badges + `WinView` fool line: read
   `session.seatNames[seat]` first, else run the view's own name through
   `BotNames.displayNickname` (this is what makes a later online game show
   `% 모스크바 1` in the ko locale), else the existing `P<seat>` fallback.

The same calls are the contract for every future surface: watchOS roster
rows, iMessage lobby labels, native leaderboard. (iMessage v1 ships no
bots, so nothing to do there yet.)

## 3. The alternative considered: Russian first names

Owner-suggested alternative, kept here so it can be swapped in by editing
one table (the `ios.bot.*` values) if the cities don't land:

| Strategy | Name (en / ru / ko) | Logic |
|---|---|---|
| `random` | Alyosha / Алёша / 알료샤 | diminutives at the bottom… |
| `simple_heuristic` | Borya / Боря / 보랴 | |
| `handwritten` | Olya / Оля / 올랴 | |
| `espresso` | Zhenya / Женя / 제냐 | |
| `robusta` | Katya / Катя / 카탸 | |
| `firecracker` | Misha / Миша / 미샤 | |
| `gunpowder` | Nadya / Надя / 나댜 | |
| `blackpowder` | Sergei / Сергей / 세르게이 | …formal names at the top |
| `cordite` | Tatiana / Татьяна / 타티아나 | |
| `octogen` | Vladimir / Владимир / 블라디미르 | top dog, per the owner |

Why cities won: (a) personal names collide with real players' names in mixed
online lobbies and in iMessage threads, where opponents are *actual people
with first names* — a bot called "Sergei" reads as a human, and that
ambiguity is a support burden; (b) cities scale to Max tiers less awkwardly
("Vladimir Max" reads like a nightclub bouncer); (c) the km-to-Moscow ladder
gives the roster order a legible story that first names can't; (d) diminutive
gender/register choices invite endless bikeshedding. Bonus bridge: Vladimir
*is also a Golden-Ring city* 180 km from Moscow — if the roster ever gains an
11th rung, `Vladimir` slots between Kazan and St. Petersburg and both schemes
meet in the middle.

## 4. What does NOT change

- Website: explosive names stay (`supabase/seed.sql` nicknames, leaderboard,
  web replays). The explosive ladder is the research lab's identity.
- Strategy keys (`octogen`, `cordite`, …) in C, TS, DB, wire formats, replay
  blobs: untouched everywhere.
- The `%` bot-name prefix and numbering: untouched (load-bearing for the
  name-only replay codec's bot-vs-human recovery).

## 5. Age-rating context (why this exists)

Apple's July-2025 rating system questionnaire covers violence/weapon themes;
a card game whose opponents are literally named after military explosives
(octogen = HMX, cordite, gunpowder…) invites at minimum a conversation. A
card game whose opponents are named Moscow and Brighton Beach does not. The
Russia theming also *fits the product* — Durak is the Russian national card
game, the ru locale already ships a Soviet visual theme, and the diaspora
nods (Brighton Beach, Miami) are affectionate, not political.
