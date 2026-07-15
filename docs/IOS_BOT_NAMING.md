# iOS bot naming — explosives → Russia, with localization

*Design + decision record, July 2026. Scope: the **iOS app (and its iMessage /
watch surfaces) only**. The website, the database, the wire, replays, and the
C engine keep the explosive codenames unchanged. This is a pure client-side
display mapping. Nothing in this doc requires a server or schema change, and
nothing here may leak back into stored names — `%Octogen 1` stays `%Octogen 1`
everywhere except pixels on Apple screens.*

Companions: mockups showing the names in situ —
`docs/ios-phone-layout.html` §6 (offline picker, en + ko). Verified research
on exonyms/CLDR/sensitivities is summarized in §5–§7 below.

---

## 1. Why

1. **Age rating.** Apple's July-2025 rating overhaul (4+/9+/13+/16+/18+, new
   "violent themes" questionnaire, in force since Jan 31 2026) invites broader
   readings than the old depiction-based descriptors. A card game whose bots
   are *named* after military explosives would *probably* still pass 4+/9+ —
   but a reviewer reading "Semtex"/"Novichok" as real-world-violence references
   is a nonzero risk, and renaming is the zero-risk path. (Novichok — a nerve
   agent tied to real assassinations — is research-only and never player-facing,
   but the theme guilt-by-associates.)
2. **Theme fit.** Durak is *the* Russian card game. A ladder of Russian cities
   culminating in Moscow is warmer, funnier, and more on-brand than munitions —
   and it gives the strength ladder a legible geography: **you start against
   the diaspora and fight your way to the Kremlin.**

## 2. The production roster (what actually needs mapping)

Only strategies that are (a) seeded in `bots` and (b) dispatched by
`bots.wasm` are production (`supabase/seed.sql:912-986`,
`cnitro/wasm/wasm_bots_api.c:216-252`). Weakest → strongest, with instance
counts as seeded:

| Rung | strategy_key | Seeded nicknames (after `%` prefix) |
|---|---|---|
| 1 | `random` | Random 1–7 |
| 2 | `simple_heuristic` | Simple Heuristic 1–3 |
| 3 | `handwritten` | Handwritten 1, **0x00C0FFEE**, Handwritten 3–4 |
| 4 | `firecracker` | Firecracker 1–3 |
| 5 | `blackpowder` | Blackpowder 1–3 |
| 6 | `cordite` / `cordite_max` | Cordite 1–3 / Cordite Max 1–3 |
| 7 | `octogen` / `octogen_max` | Octogen 1–3 / Octogen Max 1–3 |

Key wire fact (verified): bot identity reaches clients **only as the raw
`name` string + `is_ai`** — there is no strategy enum on the wire
(`_shared/wire/view.ts:29-30`; `strategy_key` is server-only). So the iOS
mapping keys off the *name*, and because the roster is a fixed seeded set, an
**exact-match table of the ~26 nicknames** is the robust approach (regex
parsing of "Family N" is the fallback for future bots; unknown names render
raw). Offline is cleaner still: roster names come from the C strategy table
(`fio_strategy_name` → `EngineC.roster()`), so offline maps by strategy id
directly with no string parsing at all.

## 3. The mapping (recommended: cities — "the road to Moscow")

Strength = closeness to the capital. Diaspora → Far East → Volga → the two
capitals. The user-fixed anchor: **octogen = Moscow, top dog.**

| strategy_key | Explosive (web) | **iOS display (en)** | Why this rung |
|---|---|---|---|
| `random` | Random | **Brighton Beach** | The boardwalk hustler; learned Durak on the Coney Island boardwalk; chaotic, lovable, weakest. (7 instances — the crowd you meet first.) |
| `simple_heuristic` | Simple Heuristic | **Miami** | Sunny Isles "Little Moscow": all bling, simple fundamentals. |
| `handwritten` | Handwritten | **Khabarovsk** | Far-east workhorse on the 5,000-ruble note: solid, provincial, rule-book player. |
| `firecracker` | Firecracker | **Vladivostok** | End of the Trans-Siberian, Pacific-fleet fortress — the "Medium" rung where the mainland starts fighting back. |
| `blackpowder` | Blackpowder | **Samara** | Volga workhorse; WWII *reserve capital*; builds Soyuz rockets. The "Hard" rung guarding the road to the capitals. |
| `cordite` | Cordite | **St Petersburg** | The imperial second capital: cultured, exacting, beats almost everyone. |
| `cordite_max` | Cordite Max | **St Petersburg Max** | Same brain, bigger budget (see §4 on "Max"). |
| `octogen` | Octogen | **Moscow** | Top dog. ★ |
| `octogen_max` | Octogen Max | **Moscow Max** | The final boss budget. |

Instance numbers carry over verbatim: `%Octogen 2` → displays **⚙ Moscow 2**
(ko: **⚙ 모스크바 2**). The easter egg `%0x00C0FFEE` is left untouched on all
platforms — hex is culture-neutral and beloved.

Notes on the choices (from the verified research, §7):
- "St Petersburg" is written without the period (`St Petersburg`) on chips —
  it is the longest name in every locale (ko 상트페테르부르크 = 8 syllables);
  seat chips truncate with an ellipsis, and the picker/roster show it in full.
  If truncation grates in practice, the sanctioned short form is the Russian
  colloquial **Piter** (Питер) — hold in reserve, don't ship two names at once.
- **Brighton Beach framing:** its "Little Odessa" identity is historically
  *Ukrainian-Jewish* diaspora. In any copy, call it a *Russian-speaking* /
  Soviet-émigré enclave, never "a Russian city." As a bot name with zero
  captioning it's charming and safe.
- Deliberately avoided: Ukrainian cities, anything in Crimea, Magadan/Norilsk
  (gulag), Kaliningrad (militarized-exclave headlines), and non-Russian
  Russophone cities (Riga, Almaty, Tel Aviv) — claiming those as "Russia" is
  the one way this theme can offend.
- zh-Hans note for a future locale: Chinese uses **海参崴** (Haishenwai) for
  Vladivostok — the politically loaded native name, and also what CLDR/iOS
  produce. Ship what CLDR says; do not editorialize.

### 3b. The "Max" suffix

Keep it as a suffix word, localized once: en `Max`, ru `Макс`, ko `맥스`.
It survives every locale, keeps the instance-number parser trivial, and reads
gamer-natural. (Considered and rejected: "Moscow-City" for octogen_max — cute
skyscraper joke, but it breaks the `<city> <Max?> <n>` grammar and needs its
own translations.)

## 4. Alternative mapping (Russian first names — "the table of regulars")

Kept as a fully-designed alternative in case the city ladder tests poorly.
Ladder logic: the folkloric card table, weakest = **Vanya** (*Ivan-durak*,
the fool of Russian folklore who sometimes wins anyway — the perfect
weakest-bot name in a game literally called Fool).

| strategy_key | Name | Flavor |
|---|---|---|
| `random` | **Vanya** | Ivan-durak, the holy fool. |
| `simple_heuristic` | **Petya** | The kid at the table (diminutives read junior). |
| `handwritten` | **Nikolai** | Old-regime solidity, plays by the book. |
| `firecracker` | **Boris** | Spassky — aggressive, uneven. |
| `blackpowder` | **Sergei** | Korolyov — the engineer. |
| `cordite` / `_max` | **Dmitri** / Dmitri Max | Mendeleev/Shostakovich — the eternal, formidable #2. |
| `octogen` / `_max` | **Vladimir** / Vladimir Max | Top dog, per the owner's anchor. |

Two honest flags from research: (a) in 2026, "Vladimir, the strongest" will be
read as a Putin reference by nearly everyone — that's either the joke or a
liability; decide with eyes open (neutral-prestige alternative top:
**Alexander**). (b) Personal names have **no CLDR data at all** — every CJK
locale needs hand transliteration (블라디미르 / ウラジーミル / 弗拉基米尔 …),
so names are ~30 hand strings vs the cities' partially-CLDR-backed table.
**Recommendation stands: cities.** More flavor, better geography-as-difficulty
metaphor, no head-of-state pun to defend in review.

## 5. Localization design ("I want ⚙ 모스크바 1, not ⚙ Moscow 1")

**Decision: hardcode a per-locale display table in FoolishKit, generated into
`Localizable.xcstrings` keys, seeded from the verified exonym table below.**
Rationale, in order of what was investigated:

1. **The "free" CLDR ride is real but partial.** Moscow, Samara, Vladivostok
   (and Novosibirsk/Omsk/Yekaterinburg/Kirov) are IANA tz zones, and ICU's
   `VVV` date pattern emits the localized *exemplar city* for them in every
   locale — `DateFormatter` with `dateFormat="VVV"`, `timeZone=Europe/Moscow`,
   `locale=ko_KR` → "모스크바". **But**: Khabarovsk is *not* a tz zone
   (it lives inside Asia/Vladivostok — the task's premise was wrong), and
   neither are St Petersburg, Brighton Beach, or Miami-as-Miami. A mechanism
   that covers 3 of 7 names is not a mechanism; it's a trap. Use CLDR as the
   *build-time data source*, not a runtime API.
2. **Only three locales ship today** (en/ru/ko, identical on web and iOS —
   `Localizable.xcstrings`, `FStrings.swift`). 7 city names + "Max" × 3
   locales ≈ 24 strings. This is not a scale problem; it's an afternoon.
3. **Future locales are pre-solved:** the 12-language exonym table (§6) is
   verified against Wikipedia/CLDR and lives here; when a locale ships, copy
   its column.

### Mechanics (for the implementer)

- New file `ios/FoolishKit/Net/BotName.swift` (mirror of the web's
  `src/common/botName.ts`, which iOS currently lacks — today iOS shows the raw
  `%`-prefixed name on the online table, a bug this work fixes in passing):
  - `isBotName(_:)` / `botDisplayName(_:)` — port of the web pair
    (strip `%`, gate on prefix).
  - `botLocalizedName(_ raw: String) -> String` — exact-match the stripped
    name against the 26 seeded nicknames → `(familyKey, isMax, instance)`;
    fallback: regex `^(.+?)( Max)? (\d+)$` against the family list; fallback:
    raw. Then compose
    `FStrings.t("bot.city.\(familyKey)") + (isMax ? " " + t("bot.max") : "") + " \(instance)"`.
  - Unknown names (future bots, `0x00C0FFEE`) render raw minus `%` — graceful
    by construction.
- Apply at exactly **two** choke points: `PackedGame.swift`'s roster merge
  (covers every online seat, lobby, win screen) and `EngineC.roster()`'s
  display path (offline picker; offline maps by strategy id → familyKey with
  no parsing). The ⚙ bot icon accompanies the name everywhere (it is the
  is-bot signal once `%` is stripped).
- String keys (all three locales in the same commit — repo rule):
  `bot.city.random`, `bot.city.simple_heuristic`, `bot.city.handwritten`,
  `bot.city.firecracker`, `bot.city.blackpowder`, `bot.city.cordite`,
  `bot.city.octogen`, `bot.max`.
- **Never reverse-map.** Display names must never be typed back into any
  server-bound field. Replay share codes, `add_bot` flows, chat mentions —
  all operate on raw names. The mapping is render-only, one-directional.
- Cross-platform note (accepted, deliberate): the same finished game shows
  "Cordite 1" in a web replay and "St Petersburg 1" in the iOS replay viewer.
  The name in the *stored* replay is the raw one; consistency-of-record wins.

### Shipped-locale values (copy-paste ready)

| key | en | ru | ko |
|---|---|---|---|
| bot.city.random | Brighton Beach | Брайтон-Бич | 브라이턴비치 |
| bot.city.simple_heuristic | Miami | Майами | 마이애미 |
| bot.city.handwritten | Khabarovsk | Хабаровск | 하바롭스크 |
| bot.city.firecracker | Vladivostok | Владивосток | 블라디보스토크 |
| bot.city.blackpowder | Samara | Самара | 사마라 |
| bot.city.cordite | St Petersburg | Санкт-Петербург | 상트페테르부르크 |
| bot.city.octogen | Moscow | Москва | 모스크바 |
| bot.max | Max | Макс | 맥스 |

(ko forms verified against Korean Wikipedia titles; 브라이턴비치 is written
without a space, per the ko.wikipedia article.)

## 6. Future-locale exonym reference (verified)

For when locales beyond en/ru/ko ship. Sources: CLDR 47 exemplar-city data
and live Wikipedia article titles (spot-verified on the ambiguous cells).

| en | ja | zh-Hans | es | fr | de | pt-BR | it | pl | tr | uk |
|---|---|---|---|---|---|---|---|---|---|---|
| Moscow | モスクワ | 莫斯科 | Moscú | Moscou | Moskau | Moscou | Mosca | Moskwa | Moskova | Москва |
| St. Petersburg | サンクトペテルブルク | 圣彼得堡 | San Petersburgo | Saint-Pétersbourg | Sankt Petersburg | São Petersburgo | San Pietroburgo | Petersburg | Sankt-Peterburg | Санкт-Петербург |
| Samara | サマーラ | 萨马拉 | Samara | Samara | Samara | Samara | Samara | Samara | Samara | Самара |
| Vladivostok | ウラジオストク | 海参崴* | Vladivostok | Vladivostok | Wladiwostok | Vladivostok | Vladivostok | Władywostok | Vladivostok | Владивосток |
| Khabarovsk | ハバロフスク | 哈巴罗夫斯克 | Jabárovsk | Khabarovsk | Chabarowsk | Khabarovsk | Chabarovsk | Chabarowsk | Habarovsk | Хабаровськ |
| Brighton Beach | ブライトン・ビーチ | 布莱顿海滩 | Brighton Beach | Brighton Beach | Brighton Beach | Brighton Beach | Brighton Beach | Brighton Beach | Brighton Beach | Брайтон-Біч |
| Miami | マイアミ | 迈阿密 | Miami | Miami | Miami | Miami | Miami | Miami | Miami | Маямі |

\* zh: CLDR/iOS emit the historic Chinese name 海参崴 for Vladivostok, not the
transliteration 符拉迪沃斯托克; ship what CLDR says (see §3 note).

## 7. Research provenance & sensitivities (summary)

- Apple age-rating overhaul: developer.apple.com/news `ks775ehf` (Jul 24 2025);
  new tiers in force since Jan 31 2026. Renaming = guaranteed-clean
  questionnaire; current names are *probably* fine but nonzero reviewer risk.
- tz/CLDR facts: IANA `zone1970.tab` (Khabarovsk is NOT a zone), CLDR 47
  `timeZoneNames` exemplar cities, UTS #35 `VVV` pattern. Korean CLDR coverage
  verified (419 exemplar cities, all Hangul).
- Cultural flags: Brighton Beach = Ukrainian-Jewish-rooted "Little Odessa"
  (frame as Russian-*speaking*); avoid Ukrainian/Crimean cities, gulag towns,
  Kaliningrad; zh dual naming of Vladivostok is politically loaded both ways.
- 2026 context: the war continues; city names are geography, not endorsement,
  and Durak is pan-ex-Soviet — but the framing rules above are load-bearing.

## 8. What is explicitly NOT changing

- `supabase/seed.sql`, `bots` table, migrations — untouched.
- Wire formats, replay codec, `game_snapshots.extras` names — untouched.
- Web client rendering — untouched (still explosives; still has its own
  pre-existing quirk of showing the raw `%` on the live board, tracked
  separately in the consolidation report).
- The `%` reservation system (`botName.ts`, DB trigger) — untouched and
  still the source of truth for is-a-bot.
