# Foolish → $1M: A Monetization Roadmap

*A realistic, staged plan for turning foolish.cards into a seven-figure business,
written against the actual state of this repo (July 2026).*

---

## TL;DR

- **The market is proven.** Durak is played by hundreds of millions of people in
  the Russian-speaking world and its diaspora. The incumbent ("Дурак Онлайн" by
  Rstgames) has tens of millions of installs and monetizes well on coins and
  cosmetics. You are not inventing demand; you are competing for a slice of a
  demonstrated market.
- **The bottleneck is distribution, not technology.** This repo is dramatically
  over-invested in engine quality (a good problem) and at roughly zero on
  accounts, retention, analytics, and payments. Every phase below is about
  converting the four under-the-hood projects into *distribution and
  monetization weapons* rather than building more engine.
- **The single highest-leverage move is a Telegram Mini App.** The core Durak
  audience lives on Telegram; Mini Apps give you frictionless distribution,
  viral invites, and — critically — **Telegram Stars/TON payments, which
  sidestep the card-processing problem in RU/CIS markets** that would otherwise
  kill monetization there.
- **Your defensible moat is Cordite.** No competing Durak app has a
  provably-fair, superhuman-strength engine that can *analyze* games. The
  chess.com playbook (free play, paid **game review / coach subscription**) is
  directly portable and is the most credible path to recurring revenue.
- **$1M is a 30–80k-DAU problem** at normal casual-card ARPDAU, or a
  ~2,800-subscriber problem at $30/yr — or, most likely, a blend. Neither
  happens without 6–12 months of deliberate distribution work.

---

## 1. What you actually have (asset inventory)

Read this as an investor would, not as an engineer would:

| Asset | Engineering framing | Business framing |
| --- | --- | --- |
| Cordite (C MC engine, ELO #1, no-cheat contract) | Bot research lab | **Chess.com-style paid game analysis; trust marketing ("our bots provably don't peek"); single-player content at zero liquidity** |
| rANS replay codec (game in a QR / short URL) | Information-theory flex | **Built-in viral loop: every finished game is a shareable artifact** |
| Procedural rendering (zero texture files, seeded fractals) | Rendering novelty | **Infinite cosmetics at zero art cost: every card back is a seed → limited editions, collectibles, gifting** |
| Offline PWA + tiny bundle | Nice engineering | **Perfect fit for Telegram Mini Apps and low-end Android in CIS markets** |
| Shared-ruleset engine (TS + C, e2e-fuzzed) | Single source of truth | **Cheap to add sibling games (Podkidnoy variants, Perevodnoy, 1000, Preferans) on the same infra** |
| Bots as real players | Architecture choice | **Solves the multiplayer cold-start problem — the game is fun at 0 CCU, so you can grow without a liquidity death spiral** |
| ru/ko/en localization + Soviet theme | i18n | You already speak the market's language, literally and aesthetically |

The strategic insight: **every "over-engineered" piece maps to a monetization or
distribution primitive.** The roadmap below is mostly *packaging*, not new
research.

---

## 2. The honest math (what "millionaire money" requires)

Comparable economics for casual card games (RU/CIS-heavy audience):

- Blended ARPDAU (ads + IAP), competently monetized: **$0.02–0.06**.
- $1M/year at $0.035 ARPDAU ⇒ **~78k DAU**. At the high end (~$0.06) ⇒ ~46k DAU.
- Subscription alternative: **$30/yr "Cordite Coach"** ⇒ ~2,800 payers per $1M
  cumulative... realistically 33k payers for $1M/yr. Chess.com converts roughly
  1–2% of MAU to paid; at 1.5% you'd need ~2.2M MAU for $1M/yr from subs alone.
- Blend scenario (the plan): 40k DAU × $0.03 ads/IAP ≈ $440k/yr, plus 8k
  subscribers × $30 ≈ $240k/yr, plus tournaments/events ≈ $150–300k/yr
  ⇒ **$0.8–1.0M/yr around month 18–24**, with an exit/acquisition option worth
  a low-seven-figure multiple on top.

None of these numbers are exotic — the incumbent proves the audience exists at
10–100× this scale. But be clear-eyed: **the whole game is getting to tens of
thousands of DAU.** Everything below is sequenced to make that climb cheap.

---

## 3. Phase 0 — Make the game measurable and sticky (weeks 0–6)

*Revenue: $0. Purpose: don't pour water into a leaky bucket.*

1. **Analytics or death.** Instrument funnel events (visit → first game →
   finished game → second session → D1/D7 return). Vercel Analytics is not
   enough; add a product analytics tool (PostHog self-hosted keeps costs ~$0).
   Every later phase has a KPI gate; without this you can't read the gates.
2. **Real accounts + persistent identity.** Supabase Auth (anonymous → upgrade
   to email/Telegram/Apple). Persistent ELO, stats, game history. Retention in
   card games is driven almost entirely by *rating and streaks*.
3. **Ship the retention floor:** daily "beat the ladder" bot challenge (free —
   Cordite ladder is already built), ELO leaderboard, streaks. This costs days,
   not weeks, because the bot roster exists.
4. **Replay virality v1:** after every finished game, a share card — QR + short
   URL + auto-generated result image ("I beat Cordite as a 2-card underdog").
   The codec is done; this is pure UI. Track share→visit→signup conversion.

**Gate to Phase 1:** D1 retention ≥ 25% and D7 ≥ 8% for new players (casual
card game norms). If you're below, fix onboarding/tutorial first — paid or
viral traffic into a leaky product is wasted.

---

## 4. Phase 1 — Distribution beachhead: Telegram Mini App (months 1–4)

*Revenue: first dollars. Purpose: put the game where the audience already is.*

This is the out-of-the-box-but-completely-real centerpiece:

1. **Port the client to a Telegram Mini App.** You're uniquely positioned:
   tiny bundle (procedural rendering, zero image assets), offline-tolerant,
   realtime already works over the open web. Most game studios have to rebuild
   for TMA; you mostly have to reskin the shell and wire Telegram auth.
2. **Telegram-native growth loops:**
   - Challenge links: "Play me" deep links in any chat; group-chat tables
     (a Durak table *is* a group chat activity — this is the product's most
     natural home).
   - Replay QRs/links unfurl into playable "re-fight this game vs Cordite"
     challenges — a viral artifact no competitor can copy without your codec.
   - Bot-as-contact: the Foolish bot DMs you when a friend challenges you or
     your daily ladder resets (retention channel with ~free CPMs).
3. **Payments via Telegram Stars (+ TON for withdraw-ables).** This is the
   quiet killer feature of the channel: **you can charge Russian-speaking users
   without touching Visa/Mastercard-in-RU sanctions mess.** Stars handles IAP
   inside Telegram on both app stores' terms.
4. **First monetization: procedural cosmetics.**
   - Every card back is a seed. Sell **limited-edition seed drops** (e.g.,
     "Novy God 2027" fern, 10,000 mints), giftable, tradeable later.
   - Table themes (the Soviet theme proves the pipeline), deck faces, win
     animations. Zero art budget — parameters, not PNGs.
   - Price like the market: $0.99–$4.99, plus a soft currency earned by play.
5. **Also ship thin mobile wrappers** (Capacitor/TWA) to App Store/Google Play
   for the diaspora audience (Germany, Israel, US/Brighton Beach, Baltics) that
   isn't Telegram-first. Same codebase, adds store discoverability for
   "durak" searches — a real, high-intent keyword.

**Gate to Phase 2:** 3–5k DAU organic/viral, share loop K-factor measured
(target ≥ 0.2), first $2–5k/mo from cosmetics. If cosmetics don't sell at all
at 3k DAU, the audience is telling you monetization must lean subscription/
tournament instead — adjust weights, don't stall.

---

## 5. Phase 2 — The moat becomes the money: Cordite Coach (months 4–9)

*Revenue: recurring. Purpose: the subscription no competitor can clone.*

Chess.com's most important lesson: **free play, paid understanding.**

1. **Post-game review ("What would Cordite do?").** After any game, a
   move-by-move review: accuracy score, blunder markers, "best move" with the
   belief-model's reasoning ("by move 12, Cordite knew your opponent held no
   spades — here's why"). The C engine already evaluates positions at scale;
   this is an API + UI project, not research. One free review/day, unlimited
   with **Foolish Premium (~$4–6/mo or $30–40/yr)**.
   *(The client-side seed of this shipped first: the Infinite Oracle
   (`docs/INFINITE_ORACLE_DESIGN.md`) runs octogen's judgment of every move in
   the browser on the replay screen. It is a growth/wow feature — being
   client-side, any paywall on it is UI-enforced only; a server-metered tier is
   the billable path this section describes.)*
2. **Premium bundle:** unlimited review, exclusive seed-drop cosmetics,
   tournament entries, ad-free, extended stats (win rate by trump suit,
   pickup-rate deltas — you have full replay history in a few bytes per game).
3. **Rewarded ads for free users** (mobile/TMA): watch-to-earn soft currency,
   interstitial-free by policy (protects retention). Expect ads alone ≈
   $0.01–0.02 ARPDAU — meaningful at scale, never the headline.
4. **"Provably fair" as marketing.** Publish the no-LLM/no-peeking contract and
   the open engine. Cheating paranoia is *the* recurring complaint on every
   incumbent Durak app's reviews. "Our bots mathematically cannot see your
   hand — here's the source" is a positioning weapon; make it a landing page,
   a talk, and an App Store screenshot.
5. **Content flywheel (near-free UA):** the four projects are elite dev-content.
   "A transformer from scratch in TypeScript," "an entire card game in one QR
   code," "how our Monte-Carlo bot deduces your hand without cheating" — each is
   a Hacker News / conference-talk candidate that funnels a technical audience
   (and hiring/acquisition interest) to the game. Budget one post/month; this
   is your zero-CAC channel while cash is thin.

**Gate to Phase 3:** ≥ 2% of weekly-active users converting to Premium, LTV
estimable, churn < 8%/mo. Blended revenue run-rate ≥ $15–25k/mo.

---

## 6. Phase 3 — Scale and compounding (months 9–24)

*Revenue: the seven-figure climb. Purpose: pour fuel only on measured fire.*

1. **Paid UA, but only where LTV > 2× CPI.** CIS-region CPIs for card games are
   low ($0.10–0.40 via VK Ads/Yandex, myTarget; Telegram Ads for the TMA). With
   measured LTV from Phase 2 you can buy growth profitably instead of praying.
   Add **VK Games and Yandex Games** platform builds — both accept web builds
   (yours is tiny) and have native RU payments and discovery. Also ship a
   **Steam build** (Electron wrapper over the same web client, ~1 week — see
   `docs/ORACLE_MONETIZATION_ENGINEERING.md` §8b): Durak has a real PC audience,
   Steam is one of the few Western storefronts still transacting with Russian
   users (Steam Wallet), and no Durak on Steam offers analysis — the same
   whitespace as everywhere. Treat it as a credible secondary storefront and a
   second RU payment rail, not a growth engine.
2. **Tournaments and seasons.**
   - Free sponsored weekly tournaments first (prizes = cosmetics/premium) to
     build the habit; then paid-entry tournaments with prize pools where legal.
   - **Get real legal counsel before any real-money play.** Durak has a
     genuine skill claim (you have the engine to *prove* skill-rating validity
     statistically — a novel and useful legal artifact), but skill-gaming law
     is jurisdiction-by-jurisdiction. Treat real-money as an option, not a plan.
3. **Clubs/leagues:** Telegram-group-bound clubs with inter-club leagues.
   Card games in this culture are social furniture; clubs are the retention
   endgame and cost little on your existing channel model.
4. **Second game on the same rails.** The shared-ruleset architecture makes
   Perevodnoy/variant Durak nearly free, and Burkozel/1000/Preferans cheap.
   Each sibling game reuses accounts, cosmetics, Premium, and bots —
   **the pitch quietly becomes "the card table of the Russian-speaking
   internet,"** which is an acquirable platform story, not an app.
5. **The exit option.** At 50k+ DAU with proven ARPDAU, this is a textbook
   acquisition target for Rstgames-class studios, Playrix-orbit casual
   publishers, or Telegram-gaming rollups — typically 2–4× revenue for
   growing casual titles. "Millionaire money" may arrive as an exit rather
   than as dividends; run the company so both doors stay open (clean metrics,
   clean cap table, documented engine).

---

## 7. Side quests (real but smaller — don't confuse them with the main path)

| Idea | Honest ceiling | Verdict |
| --- | --- | --- |
| License the rANS replay codec / "game in a QR" tech | $10–50k consulting-shaped deals | Do it if inbound comes from the content flywheel; don't sell outbound |
| Sell Cordite-style bot ladders to other studios | Similar | Same — inbound only |
| Sponsorships/brand tournaments (vodka/telecom brands love card culture) | $20–100k/yr once you have audience | Pursue at 20k+ DAU |
| Merch (QR-replay-of-famous-games shirts) | Beer money | Fun marketing, not revenue |
| NFT-ifying seed cosmetics on TON | Volatile; reputational risk | Only as *optional* tradability for seed drops, never the pitch |

---

## 8. Risks and pre-committed answers

- **Incumbent competition (Rstgames et al.):** don't fight them on their turf
  (mobile app store UA in RU). Win on channels they're weak in (TMA, web,
  diaspora stores) and features they can't copy (analysis, provable fairness,
  replay artifacts).
- **Payments/sanctions complexity in RU:** lead with Telegram Stars/TON and
  VK/Yandex platform payments; serve card payments only to diaspora storefronts.
- **Solo-dev bandwidth:** every phase above is deliberately packaging-over-
  research. The engine is *done*. If a task list item is "improve the bot,"
  strike it — Cordite is already past the bar where users can tell.
- **Multiplayer liquidity:** already solved — bots are real players. Never
  ship a mode that requires human liquidity to be fun until DAU says you can.
- **The gates are the plan.** Each phase has a numeric gate. Skipping a gate
  because the next phase is more fun is how this becomes a fifth beautiful
  engine and zeroth business.

---

## 9. The one-line version

**Put the game where Durak players already live (Telegram), let every finished
game advertise itself (QR replays), sell infinitely-mintable beauty (procedural
seeds) and understanding (Cordite Coach) — and let the engines you already
built do the two jobs no competitor can: prove the game is fair and teach
people to win.**
