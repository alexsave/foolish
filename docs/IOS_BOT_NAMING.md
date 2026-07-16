# iOS bot names — the Russia map (explosives → cities), with localization

*Design doc + implementation spec (NOT yet implemented — this is the work
order). Merged 2026-07-15 from two independent design passes; where they
clashed, the better idea won (clash log in §8). The iOS app renames the bot
roster from explosives to Russian cities and Russian-diaspora hubs so the
App Store age-rating questionnaire stays boring. **The website keeps the
explosive names.** Strategy KEYS never change anywhere — this is a
render-time display map, implemented entirely client-side; nothing here may
leak into stored names, replay blobs, or the wire.*

*Mockups showing the names in situ: `docs/ios-phone-layout.html` §6
(offline picker + Korean-locale frames).*

---

## 1. The mapping (primary scheme: cities — "the road to Moscow")

**The gimmick: a bot's strength is its closeness to Moscow.** The ladder is
a journey home — the weakest bot suns itself in Miami; the strongest IS
Moscow. Air distance to the Kremlin decreases **strictly monotonically** as
strength climbs, and the bot picker surfaces it as a flavor line
("1,420 km from Moscow" / Moscow: "The Kremlin itself"), which makes the
strength order legible without showing a number of ELO.

The table covers the full **offline strategy roster** (10 rungs — the C
table in `c/ios/ios_api.c:37-48` exposes `random … octogen`, which is
wider than the website's *seeded* roster; see §2).

| # | Strategy key | en | ru | ko | ~km | Why this city |
|---|---|---|---|---|---|---|
| 1 | `random` | Miami | Майами | 마이애미 | 9,100 | Sunny Isles Beach = "Little Moscow", the farthest diaspora outpost; beach chaos for the chaos bot |
| 2 | `simple_heuristic` | Brighton Beach | Брайтон-Бич | 브라이턴비치 | 7,520 | Brooklyn's Russian-speaking hub; plays the old-school basics |
| 3 | `handwritten` | Vladivostok | Владивосток | 블라디보스토크 | 6,430 | Where the long road home starts — end of the Trans-Siberian |
| 4 | `espresso` | Khabarovsk | Хабаровск | 하바롭스크 | 6,140 | One stop closer, Far East, on the 5,000-ruble note |
| 5 | `robusta` | Novosibirsk | Новосибирск | 노보시비르스크 | 2,810 | Siberia's science capital (Akademgorodok) — fitting for the first Monte-Carlo bot |
| 6 | `firecracker` | Yekaterinburg | Екатеринбург | 예카테린부르크 | 1,420 | The Urals — the Europe/Asia border; the ladder's "Medium" rung |
| 7 | `gunpowder` | Samara | Самара | 사마라 | 860 | Volga aerospace city; WWII reserve capital (owner-requested city) |
| 8 | `blackpowder` | Kazan | Казань | 카잔 | 720 | Volga powerhouse, "third capital"; the ladder's "Hard" rung |
| 9 | `cordite` | St. Petersburg | Петербург | 상트페테르부르크 | 635 | The second capital — the previous champion |
| 10 | `octogen` | Moscow | Москва | 모스크바 | 0 | Top dog. Non-negotiable. |

Max tiers compose: `Cordite Max` / `Octogen Max` render as
`St. Petersburg Max` / `Москва Макс` / `모스크바 맥스` (localized "Max"
suffix, key `ios.bot.max`). **No live bot carries a Max key any more** — the
tiers were retired in July 2026 (migration `20260715120000_drop_max_bot_tiers`;
`octogen_max` was a plain alias of `octogen`, and `cordite_max`'s flat world
budget was *weaker* than plain cordite at 6-8 players — see
`docs/C_CORE_CONSOLIDATION.md` §4.1). The suffix stays in the parser purely for
**historical replay blobs**, which embed nicknames at encode time — the same
reason the base table keeps Espresso/Robusta/Gunpowder. Instance numbers carry
over verbatim:
`%Octogen 2` → **⚙ Moscow 2** (ko: **⚙ 모스크바 2**). The easter egg
`%0x00C0FFEE` renders unchanged everywhere — hex is culture-neutral and
beloved.

Notes on the picks (verified research, §6–§7):

- **Brighton Beach** — "Little Odessa," formed by the 1970s Soviet Jewish
  emigration wave; one of the largest Russian-*speaking* communities in the
  West. We use the place's own name, never "Little Odessa" (Odesa is a
  Ukrainian city), and in any copy we say Russian-*speaking* / Soviet-émigré,
  never "a Russian city."
- **Miami** — Sunny Isles Beach is the real "Little Moscow" referent;
  "Miami" is the display name because it's globally recognizable.
- All Russian-Federation picks are large, neutral, recognizable cities.
  Deliberately avoided: Ukrainian/Belarusian/Kazakh cities, anything in
  Crimea, gulag-connoted towns (Magadan, Norilsk, Vorkuta), Kaliningrad
  (militarized-exclave headlines), politician-adjacent names.
- **ru uses the colloquial-official short form «Петербург»** — the 15-char
  «Санкт-Петербург» wrecks seat badges; «Питер» is too slangy for UI. The
  ko 상트페테르부르크 has no comparable sanctioned short form: chips truncate
  with `minimumScaleFactor` + ellipsis, picker/roster show it in full.
- ko 브라이턴비치 is written **without a space** (ko.wikipedia title).
- zh note for a future locale: CLDR/iOS emit the historic Chinese name
  **海参崴** for Vladivostok, not the transliteration 符拉迪沃斯托克 — a
  politically loaded dual naming. Ship what CLDR says; don't editorialize.

## 2. Which roster is which (offline keys vs online nicknames)

Bot identity reaches the UI from three sources, and two different rosters
are in play — the mapping must serve both:

| Source | Shape | Roster |
|---|---|---|
| Offline picker / offline seats | strategy key from `EngineC.roster()` (`fio_strategy_name`) | all 10 rungs above |
| Online nicknames (DB rows) | raw string `"% <Base> [Max] <n>"` in `players[].name` — there is NO strategy enum on the wire (`sdk/ts/wire/view.ts:29-30`; `strategy_key` is server-only) | the *seeded* subset = the C roster's `seeded` column (`c/src/bot_roster.c`): Random ×7, Simple Heuristic ×3, Handwritten ×4 (incl. `0x00C0FFEE`), Firecracker ×3, Blackpowder ×3, Cordite ×3, Octogen ×3 (`server/impls/supabase/seed.sql`). The `[Max]` slot no longer occurs on live rows — only in old replay blobs. |
| Replay blobs / history | names embedded at encode time | anything ever seeded — **including dropped families** (`Espresso` rows existed before migration `20260711130000_drop_non_wasm_bots`), so the nickname parser keeps Espresso/Robusta/Gunpowder in its base table for historical replays |

**Rule: strategy-derived bot names are treated as KEYS and localized at
render time — never at storage time.** Replay codes stay byte-identical
across locales, DB rows never carry localized strings, and switching the app
language re-renders every bot name instantly. Never reverse-map: display
names must never be typed back into any server-bound field.

## 3. Implementation spec (one new file + five small touches)

**New file `ios/FoolishKit/DesignSystem/BotNames.swift`** with exactly three
public entry points (every current and future surface calls these, never
re-derives):

```swift
public enum BotNames {
    /// Roster strategy key (EngineC.roster() name, e.g. "octogen") →
    /// localized display name. Unknown keys degrade to .capitalized.
    public static func display(strategy key: String) -> String {
        let lookup = "ios.bot.\(key)"
        let s = FStrings.t(lookup)
        return s == lookup ? key.capitalized : s
    }

    /// Localized picker flavor line ("1,420 km from Moscow"; Moscow:
    /// "The Kremlin itself"), from the §1 km table keyed by strategy.
    public static func flavorLine(strategy key: String) -> String?

    /// Server / replay nicknames: "% <Base> [Max] [<n>]" → localized,
    /// preserving the % prefix and index ("% Octogen Max 2" →
    /// ru "% Москва Макс 2"). Parse: strip "%", peel trailing integer,
    /// peel " Max", look Base up in a website-base→strategy-key table
    /// (Random / Simple Heuristic / Handwritten / Espresso / Robusta /
    /// Firecracker / Gunpowder / Blackpowder / Cordite / Octogen — the
    /// dropped families stay for historical replays, §2). Unknown bases
    /// (humans, "% 0x00C0FFEE") return unchanged.
    public static func displayNickname(_ raw: String) -> String
}
```

**Strings**: add the `ios.bot.*` keys to FStrings/xcstrings (en/ru/ko in the
same commit, per the §16.E4 identical-key-sets rule): the ten city names
from §1, plus `ios.bot.max` ("Max"/"Макс"/"맥스"),
`ios.bot.km` ("{km} km from Moscow"/"{km} км от Москвы"/"모스크바에서 {km} km"),
`ios.bot.km0` ("The Kremlin itself"/"Сам Кремль"/"크렘린 그 자체").

**Wiring** (all display-side, no engine changes):

1. `HomeView` bot picker: `Text(currentBot.name.capitalized)` →
   `Text(BotNames.display(strategy: currentBot.name))`, plus `flavorLine`
   as a dim caption (`lineLimit(1)` + `minimumScaleFactor` — Yekaterinburg
   and 상트페테르부르크 are long).
2. Offline seat names: the kernel's offline view carries no names —
   `AppCoordinator.startOffline` builds `[seat: localizedName]` (numbered
   "Moscow 1/2/3"-style when several copies sit down) and passes it to a new
   `LocalGame.seatNames` property, exposed on the `GameSession` protocol
   with a `[:]` default so `OnlineGame` is untouched.
3. `TableView` seat badges + `WinView` fool line: read
   `session.seatNames[seat]` first, else run the view's name through
   `BotNames.displayNickname` — **this also fixes a live bug: iOS currently
   renders the raw `%`-prefixed nickname on online tables** (no
   `botDisplayName` port exists; `PackedGame.swift` roster merge →
   `FSeatBadge` shows `%Octogen 1` today) — else the existing `P<seat>`
   fallback. The ⚙ bot icon accompanies the name everywhere (it is the
   is-bot signal once `%` is stripped).

The same calls are the contract for every future surface: watchOS roster
rows, iMessage lobby labels, native leaderboard. (iMessage v1 ships no bots.)

## 4. The alternative considered: Russian first names

Owner-suggested alternative (Vladimir top dog), fully designed so it can be
swapped in by editing one table (the `ios.bot.*` values):

| Strategy | Name (en / ru / ko) | Logic |
|---|---|---|
| `random` | Vanya / Ваня / 바냐 | *Ivan-durak*, the folkloric fool who sometimes wins anyway — the perfect weakest bot in a game literally called Fool |
| `simple_heuristic` | Alyosha / Алёша / 알료샤 | diminutives read junior… |
| `handwritten` | Olya / Оля / 올랴 | |
| `espresso` | Zhenya / Женя / 제냐 | |
| `robusta` | Katya / Катя / 카탸 | |
| `firecracker` | Misha / Миша / 미샤 | Tal — the swashbuckler |
| `gunpowder` | Nadya / Надя / 나댜 | |
| `blackpowder` | Sergei / Сергей / 세르게이 | …formal names at the top |
| `cordite` | Tatiana / Татьяна / 타티아나 | |
| `octogen` | Vladimir / Владимир / 블라디미르 | top dog, per the owner |

**Why cities won** (merged reasoning from both passes):

- (a) personal names collide with real players' names in mixed online
  lobbies and iMessage threads, where opponents are actual people with first
  names — a bot called "Sergei" reads as a human; that ambiguity is a
  support burden;
- (b) cities scale to Max tiers less awkwardly ("Vladimir Max" reads like a
  nightclub bouncer);
- (c) the km-to-Moscow ladder gives the roster order a legible story;
- (d) in 2026, "Vladimir, the strongest" will be read as a Putin reference
  by nearly everyone — that's either the joke or a liability;
- (e) personal names have **zero CLDR support** — every CJK locale needs
  hand transliteration, vs the cities' partially-CLDR-backed table (§6);
- (f) diminutive gender/register choices invite endless bikeshedding.

Bonus bridge: Vladimir *is also a Golden-Ring city* 180 km from Moscow — if
the roster ever gains an 11th rung, `Vladimir` slots between Kazan and
St. Petersburg and both schemes meet in the middle.

## 5. Age-rating context (why this exists)

Apple's July-2025 rating overhaul (tiers 4+/9+/13+/16+/18+, new
"violent themes" questionnaire questions, in force since Jan 31 2026)
invites broader readings than the old depiction-based descriptors. A card
game whose opponents are literally named after military explosives
(octogen = HMX, cordite, gunpowder…) invites at minimum a conversation —
and "Novichok" (research-only, never player-facing, but one grep away) is a
nerve agent tied to real assassinations. A card game whose opponents are
named Moscow and Brighton Beach invites nothing. Renaming is the zero-risk
path to the 4+/9+ band. The Russia theming also *fits the product* — Durak
is the Russian national card game, the ru locale already ships a Soviet
visual theme, and the diaspora nods are affectionate, not political.

## 6. Localization mechanics & the CLDR investigation

**Decision: hardcode the per-locale table (en/ru/ko today; §7 pre-solves
future locales). The "free CLDR ride" was investigated and rejected as a
runtime mechanism:**

- Moscow, Samara, Vladivostok, Novosibirsk, Omsk, Yekaterinburg, Kirov ARE
  IANA tz zones, and ICU's `VVV` date pattern emits the localized exemplar
  city (`DateFormatter`, `dateFormat="VVV"`, `timeZone=Europe/Moscow`,
  `locale=ko_KR` → "모스크바").
- **But Khabarovsk is NOT a tz zone** (it lives inside Asia/Vladivostok —
  `Asia/Khabarovsk` has never existed), and neither are St. Petersburg,
  Kazan, Brighton Beach, or Miami-as-Miami. A mechanism covering ~6 of 10
  names is a trap. Use CLDR as a build-time data source and cross-check,
  not an API.
- Scale check: 10 cities + 3 flavor keys × 3 shipped locales ≈ 40 strings —
  an afternoon, not a system.

## 7. Future-locale exonym reference (verified)

For when locales beyond en/ru/ko ship. Sources: CLDR 47 exemplar-city data
+ live Wikipedia article titles (ambiguous cells spot-verified; the
Yekaterinburg row is standard encyclopedic forms — re-verify when shipping).

| en | ja | zh-Hans | es | fr | de | pt-BR | it | pl | tr | uk |
|---|---|---|---|---|---|---|---|---|---|---|
| Moscow | モスクワ | 莫斯科 | Moscú | Moscou | Moskau | Moscou | Mosca | Moskwa | Moskova | Москва |
| St. Petersburg | サンクトペテルブルク | 圣彼得堡 | San Petersburgo | Saint-Pétersbourg | Sankt Petersburg | São Petersburgo | San Pietroburgo | Petersburg | Sankt-Peterburg | Санкт-Петербург |
| Kazan | カザン | 喀山 | Kazán | Kazan | Kasan | Cazã | Kazan' | Kazań | Kazan | Казань |
| Samara | サマーラ | 萨马拉 | Samara | Samara | Samara | Samara | Samara | Samara | Samara | Самара |
| Yekaterinburg | エカテリンブルク | 叶卡捷琳堡 | Ekaterimburgo | Iekaterinbourg | Jekaterinburg | Ecaterimburgo | Ekaterinburg | Jekaterynburg | Yekaterinburg | Єкатеринбург |
| Novosibirsk | ノヴォシビルスク | 新西伯利亚 | Novosibirsk | Novossibirsk | Nowosibirsk | Novosibirsk | Novosibirsk | Nowosybirsk | Novosibirsk | Новосибірськ |
| Khabarovsk | ハバロフスク | 哈巴罗夫斯克 | Jabárovsk | Khabarovsk | Chabarowsk | Khabarovsk | Chabarovsk | Chabarowsk | Habarovsk | Хабаровськ |
| Vladivostok | ウラジオストク | 海参崴* | Vladivostok | Vladivostok | Wladiwostok | Vladivostok | Vladivostok | Władywostok | Vladivostok | Владивосток |
| Brighton Beach | ブライトン・ビーチ | 布莱顿海滩 | Brighton Beach | Brighton Beach | Brighton Beach | Brighton Beach | Brighton Beach | Brighton Beach | Brighton Beach | Брайтон-Біч |
| Miami | マイアミ | 迈阿密 | Miami | Miami | Miami | Miami | Miami | Miami | Miami | Маямі |

\* zh: CLDR/iOS emit 海参崴 (see §1 note). pt-PT differs from pt-BR on
Moscow (Moscovo). Latin-script locales DO respell Russian cities (kh→j in
Spanish, kh→ch in German/Polish, v→w in German/Polish) — "Latin script
needs no localization" is false for this list; only Brighton Beach and
Miami are verbatim everywhere.

## 8. Clash log (what the two passes disagreed on, and what won)

| Question | Pass A (naming branch) | Pass B (this branch pre-merge) | Kept |
|---|---|---|---|
| Roster width | 10 strategy keys (offline roster) | 7 seeded families (online roster) | **A's table, B's §2 distinction** — both rosters are real; the mapping keys on strategy, the parser on nickname base |
| Weakest city | Miami = random | Brighton Beach = random | **A** — required for strict km monotonicity; flavor works both ways |
| Ladder story | explicit km-to-Moscow flavor line | implicit geography | **A** — the flavor line makes strength legible; adopted into mockups |
| ru Petersburg | «Петербург» short form | full + "Piter in reserve" | **A** — solves the badge today |
| ko Brighton Beach | 브라이턴 비치 | 브라이턴비치 (wiki-verified) | **B** |
| API home | `DesignSystem/BotNames.swift`, 3 entry points, `seatNames` on `GameSession` | `Net/BotName.swift`, exact-match table | **A's shape** (display is a DesignSystem concern; seatNames is the right offline seam) + **B's robustness note**: prefer exact-match of known nicknames before the peel-parser when both are cheap |
| Names alternative | diminutive ladder, collision argument | Vanya=Ivan-durak, Putin-read flag, CJK cost | **merged** (§4) |
| Future locales | none | verified exonym table + CLDR/tz findings | **B** (§6–§7) |

## 9. What does NOT change

- Website: explosive names stay (`server/impls/supabase/seed.sql` nicknames, leaderboard,
  web replays). The explosive ladder is the research lab's identity.
  (Separate web tidy, tracked in `docs/C_CORE_CONSOLIDATION.md`: the live
  board currently shows the raw `%` prefix — `PlayerRing.tsx:184`.)
- Strategy keys in C, TS, DB, wire formats, replay blobs: untouched.
- The `%` bot-name prefix and numbering: untouched (load-bearing for the
  name-only replay codec's bot-vs-human recovery; enforced by the
  `enforce_username_not_bot` trigger).
