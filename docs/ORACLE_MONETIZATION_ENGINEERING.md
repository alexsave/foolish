# Selling the Infinite Oracle — Engineering & Business Doc

*How to take the shipped, client-side Infinite Oracle (`docs/INFINITE_ORACLE_DESIGN.md`)
to paid, across Web/Stripe, iOS, Google Play, and Telegram — with auth, entitlements,
metering, threading, pricing, fees, and tax. All external facts verified July 13, 2026;
sources inline. Supersedes the Phase-1 monetization mix in `docs/MONETIZATION_ROADMAP.md`
(no ads, no cosmetics-first — the Oracle subscription is the product).*

---

## 0. Executive summary

- **Verdict: yes, it's worth paying for — as a conversion product, not a business by
  itself.** It is the only move-analysis product in the entire Durak market (verified:
  no competing app offers anything like it), it monetizes the market's #1 documented
  grievance ("the game is rigged / why did I lose"), it has ~zero marginal cost
  (user's CPU does the work), and the exact business model is proven at scale by
  chess.com (~2M paying subscribers, ~$150M/yr, 88% from subscriptions, built on a
  "1 free Game Review per day" cap). But conversion for this genre is ~1–2% of
  actives — the Oracle monetizes an audience, it does not create one. Ship it lean
  (§15: ~3 weeks to first revenue), and keep spending the rest of the effort on
  distribution.
- **Recommended order: Web+Stripe → Telegram → iOS → Play.** Web is 1–3 weeks of
  work, keeps ~90–95% of every dollar, and validates willingness-to-pay before any
  store bureaucracy. Telegram is the *only* payment rail that reaches Russia (Apple
  halted all RU payments Apr 1, 2026; Google Play billing died there Dec 2024). iOS
  is the highest-ARPU storefront and the place where the native-C Oracle shines —
  and it can carry a bundled **iMessage extension** (§7.5), a GamePigeon-style
  turn-based Durak in chat threads that our replay codec is unusually suited to.
  Play last (smallest marginal audience, now fee-equal to Apple).
- **Guarding:** on iOS, the paywall can genuinely live in the client (StoreKit 2
  entitlement + natively compiled engine inside a signed binary). On web/TMA the
  wasm is a public static asset (`public/oracle.wasm.gz`), so the paywall must be
  **server-issued analysis tokens** — meter the *service*, accept that the *bits*
  are free, exactly like chess.com charging for review on top of free Stockfish.
- **Auth must be rebuilt first.** Today: username → SHA-256 → fake
  `<hash>@foolish.cards` email + password (`src/contexts/AuthContext.tsx`),
  `enable_confirmations = false`, no reset flow, and Supabase's built-in mailer
  sends **2 emails/hour** (dev-only). Nobody can attach a subscription to that.
  §4 is the migration plan that keeps existing accounts.
- **Pricing (western storefronts):** free = 1 Oracle analysis/day per account;
  **$4.99/mo**, **$34.99/yr** (launch intro $29.99), **$1.99/wk** on mobile stores
  only, **$79 one-time "Founder" lifetime** (capped), **day passes only as Telegram
  Stars / IAP consumables**, never as a daily subscription. Telegram/CIS at
  0.3–0.5×: ~150 Stars/month, 25-Star single deep review.
- **Supabase free tier: fine until the first paying customer, then upgrade to Pro
  ($25/mo) the same week** — free projects pause after 7 days of inactivity (a paused
  project silently drops Stripe/Apple/Google webhooks), have no backups (entitlement
  data!), and cap Realtime at 200 concurrent connections.

---

## 1. What we have (repo facts the plan builds on)

| Fact | Where | Consequence |
| --- | --- | --- |
| Oracle is fully client-side: worker fleet ≤ 8 × ~12 MB, each its own `oracle.wasm` instance (TT20 = 8 MiB solver table) | `src/oracle/`, `docs/INFINITE_ORACLE_DESIGN.md` §8 | Zero COGS per analysis; but web client can't be trusted to meter itself |
| `oracle.wasm.gz` is a **public static asset** (60 KB) | `public/oracle.wasm.gz` | Anyone can download the engine; web guarding = metering, not secrecy |
| Mode B (shared-memory threads) fully specced, unbuilt | design doc §8b | Optional premium polish; separate build + branch (§10) |
| The same C engine already runs **on the server** (`bots.wasm` on Deno edge functions) | `supabase/functions/_shared/wasm/` | Server-side verdicts are possible — but edge functions cap at **2s CPU** per invocation, so the server can issue tokens & spot-checks, not the "infinite" convergence |
| Replay page decodes entirely client-side, **no auth** | `src/app/[game_id]/page.tsx` | Free replays stay the viral loop; Oracle button becomes the login/paywall funnel |
| Auth is a username hash masquerading as email; no verification, no reset | `src/contexts/AuthContext.tsx:12-33`, `supabase/config.toml` (`enable_confirmations = false`) | Rebuild required before payments (§4) |
| `cnitro` is freestanding C (clang, `-nostdlib` for wasm; plain C11 natively), thread-safety proven under the native OMP model (`_Thread_local` scratch/RNG/TT) | `cnitro/Makefile`, design doc §3 | Compiles natively for iOS arm64 with pthreads — no wasm needed on iOS (§7) |
| Backend is Supabase **free tier** | — | §14: upgrade trigger and cost model |

---

## 2. Is this worth paying for? (the honest case)

**Evidence for:**

- **Whitespace.** No Durak app offers move analysis, coaching, or review of any
  kind — store listings and searches turn up rules variants, bots, chat and gifts
  only (checked July 2026 across Google Play / App Store leaders). We would be the
  only "why did I lose" product in a game with tens of millions of players
  (Rstgames' Durak Online alone: ~69M downloads,
  [AppMagic](https://appmagic.rocks/google-play/durak-online/com.rstgames.durak)).
- **It monetizes the market's loudest complaint.** The incumbent's rating is a
  brutal 2.45/5 with endless "rigged deals / bots see my cards / payers get better
  cards" reviews ([AppBrain](https://www.appbrain.com/app/durak-online/com.rstgames.durak),
  [AppGrooves reviews](https://appgrooves.com/android/com.rstgames.durak/durak-online/r-soft-llc/negative)).
  An objective, provably-non-cheating analyzer ("here is the exact probability
  math behind your loss") is the antidote product, and our no-peeking engine
  contract is the marketing.
- **The model is proven against a free substitute.** Lichess gives unlimited free
  Stockfish analysis, yet chess.com converts ~2M of 250M members (~1%) into
  ~$132M/yr of subscriptions, with the free tier capped at **one Game Review per
  day** — the exact cap we can copy
  ([TechCrunch Apr 2025](https://techcrunch.com/2025/04/24/chess-com-reaches-200-million-members/),
  [Sherwood](https://sherwood.news/culture/how-the-chess-com-empire-makes-more-than-usd100m-a-year/),
  [chess.com pricing](https://support.chess.com/en/articles/8562418-what-does-each-level-of-premium-membership-get-me)).
  For us there isn't even a free substitute.
- **~Zero marginal cost.** The analysis runs on the buyer's own CPU. Our COGS is
  an edge-function token check. Gross margin is the payment processor's fee.

**Evidence against (plan around these, don't argue with them):**

- **Conversion is ~1–2% of actives, not 9%.** chess.com ≈ 1% of registered;
  RevenueCat's cross-app median download→paid is ~2.2% for freemium
  ([RevenueCat SOSA 2026](https://www.revenuecat.com/state-of-subscription-apps)).
  Napkin: 50k WAU × 1.5% × $30/yr ≈ **$22k/yr**. The Oracle turns audience into
  revenue at a good rate; it does not replace the distribution problem. Millionaire
  money still routes through six-figure MAU (see `MONETIZATION_ROADMAP.md` §2).
- **Niche-game WTP is lower than chess.** Gaming is the lowest-priced subscription
  category (median $4.99/mo, $24.99/yr —
  [RevenueCat 2026 benchmarks](https://www.airbridge.io/en/blog/subscription-app-pricing-by-category-2026-benchmark)),
  and the core Durak audience skews to low-WTP regions (§11).
- **Therefore:** build the paid Oracle in its lean form (metering + billing on top
  of what's already shipped — §15 estimates ~3 weeks to first web revenue), price
  per §11, and treat every additional engineering week spent gold-plating it as a
  week stolen from distribution.

---

## 3. The guarding model (client vs server, per platform)

One principle: **we are metering a service, not hiding bits.** The engine already
ships to every browser as a public asset; if the repo is public it's doubly free.
That is fine — it's chess.com selling reviews on top of open-source Stockfish. The
subscription buys *unlimited, integrated, in-flow analysis attached to your account
and history*, not secret math.

| Platform | Enforcement point | Strength | Mechanism |
| --- | --- | --- | --- |
| iOS | **Client** (allowed, as you suspected) | Strong | StoreKit 2 entitlement check in native code inside a signed, DRM'd binary; oracle compiled natively (no separable wasm asset). Jailbreak bypass exists and is ignorable. Entitlement also mirrored server-side (§5) so the account carries it everywhere. |
| Web / Stripe | **Server** (mandatory) | Real metering, soft secrecy | Server-issued **analysis tokens** (below). Optionally move `oracle.wasm.gz` behind an authenticated, short-lived signed URL to raise the casual bar — but never rely on that. |
| Telegram Mini App | **Server** (same as web) | Same | Same token flow; auth comes from validated `initData` (§9). |
| Play / TWA | **Server** (it's the web app) | Same | Same tokens; Play Billing feeds the same entitlement table. |

**The analysis-token flow** (the one new server surface, ~an edge function + two
tables):

```
client (Oracle button pressed)
  → POST /functions/v1/oracle-token   (Supabase auth JWT)
      edge fn: read entitlements row for user
               ├─ premium active  → sign token {sub, exp: +2h, unlimited: true}
               └─ free            → check oracle_uses (user_id, day) counter
                                     ├─ under quota → increment, sign token {…, quota: 1}
                                     └─ over quota  → 402 + paywall payload (prices, trial)
  → OracleController.start(job, token)   // controller refuses to start without a token
```

- Token = HMAC-signed JSON (secret held by edge functions), verified client-side
  for expiry only; the *server-side truth* is the usage counter. This is soft
  enforcement on web by design — the design doc says it plainly
  (`INFINITE_ORACLE_DESIGN.md` §14) — and it is the correct trade: a user skilled
  enough to strip the gate and run the fleet by hand was never a customer, and the
  1/day free tier makes casual circumvention pointless.
- **Do not** build the "hard" server-compute oracle now. Edge functions allow 2s
  CPU/invocation ([Supabase limits](https://supabase.com/docs/guides/functions/limits))
  — enough for a *single-position server verdict* (anti-tamper spot checks, or a
  future "verified accuracy badge" on shared replays), not for streaming
  convergence. Client compute is a feature: infinite depth, zero COGS.
- Free-tier UX: replays stay public and auth-free (they're the growth loop). The
  Oracle button for an anonymous visitor opens "create a free account — 1 free
  deep analysis every day." That makes the Oracle the **account-creation engine**,
  which is worth more than a second free analysis.

---

## 4. Auth rebuild (prerequisite for everything)

**Target state:** real email + password with verification and reset; existing
username-hash accounts keep working and can upgrade in place; account deletion
(required by both stores); optional Sign in with Apple later (NOT required —
Guideline 4.8 only triggers if we add third-party/social login; our own
email/password system is explicitly exempt,
[App Review Guidelines](https://developer.apple.com/app-store/review/guidelines/)).

Work items:

1. **Custom SMTP now.** Supabase's built-in mailer is 2 emails/hour and only
   delivers to team addresses — it is not a production service
   ([Supabase SMTP docs](https://supabase.com/docs/guides/auth/auth-smtp)).
   Wire Resend/Postmark/SES (Resend free tier: 3k/mo) into `[auth.email.smtp]`,
   set `site_url = https://foolish.cards`, add production redirect URLs, brand
   the confirmation/reset templates. Note new custom-SMTP projects start
   rate-limited to 30/hr — raise it in dashboard settings.
2. **Flip verification on:** `enable_confirmations = true` for *new* email
   signups, `secure_password_change = true`.
3. **Keep username login, add real email underneath.** The hash-email hack stays
   as the *guest/legacy* tier (it is genuinely good UX for a card game). New flow:
   - Sign up = username + password (unchanged, instant, unverified — can play).
   - "Add email" prompt appears when the user tries to do anything money-adjacent
     (subscribe, restore purchase, use Oracle beyond day 1):
     `supabase.auth.updateUser({ email })` → Supabase sends verification to the
     new address (double-confirm is already on: `double_confirm_changes = true`)
     → account's fake `<hash>@foolish.cards` is replaced by the real address;
     username survives in `user_metadata.username`.
   - Password reset: `resetPasswordForEmail` + `/reset` route — only meaningful
     once a real email is attached, which is exactly the gate that matters
     (paying users all have one by construction).
   - **Rule: no purchase without a verified email.** It's the receipt address,
     the recovery path, and the cross-platform account key.
4. **Account deletion**: in-app button + a web page that works without the app
   (Play requires the standalone web path,
   [Play policy](https://support.google.com/googleplay/android-developer/answer/13327111);
   Apple requires in-app, guideline 5.1.1(v)). Edge function: cancel/flag active
   subscriptions, delete auth user, scrub PII from game history (replays keep the
   codec's player-name blob only if anonymized).
5. **Rate limiting / abuse:** Supabase auth rate limits + CAPTCHA (Turnstile) on
   signup, because 1-free-analysis-per-day invites throwaway accounts. Cheap extra:
   free Oracle quota keys on verified email, not raw account.

Estimated effort: **1–2 weeks**, mostly UI states and email templates.

---

## 5. Entitlements: one table, four payment sources

The account is the product's spine: **buy anywhere, entitled everywhere.**

```sql
create table entitlements (
  user_id     uuid primary key references auth.users(id) on delete cascade,
  product     text not null default 'oracle_premium',
  source      text not null check (source in ('stripe','apple','google','telegram','promo')),
  status      text not null check (status in ('active','trialing','grace','canceled','expired')),
  expires_at  timestamptz,          -- null = lifetime
  external_id text,                 -- stripe sub id | apple originalTransactionId | play purchaseToken | tg charge id
  updated_at  timestamptz not null default now()
);
-- RLS: owner may SELECT; only service_role writes.

create table oracle_uses (
  user_id uuid references auth.users(id) on delete cascade,
  day     date not null,
  count   int  not null default 0,
  primary key (user_id, day)
);
```

Writers (all edge functions, all idempotent on `external_id + event id`):

| Source | Webhook → edge function |
| --- | --- |
| Stripe | `checkout.session.completed`, `customer.subscription.updated/deleted` |
| Apple | App Store **Server Notifications V2** (JWS-signed JSON: SUBSCRIBED, DID_RENEW, EXPIRED, GRACE…) |
| Google | Real-time Developer Notifications via Cloud Pub/Sub push → HTTPS endpoint |
| Telegram | bot webhook `successful_payment` (+ recurring flag on Stars subscription renewals) |

**The cross-platform question, answered ("subscribed on web, signs into the iOS
app"): honor it, silently and fully.** Mechanically it's automatic — the iOS app
reads the same entitlements row. Policy-wise:

- Apple Guideline **3.1.3(b) Multiplatform Services** allows users to access
  "content, subscriptions, or features they have acquired … on other platforms or
  your web site, provided those items are also available as in-app purchases within
  the app" ([guidelines](https://developer.apple.com/app-store/review/guidelines/)).
  So the iOS app **must also sell the subscription via IAP** — which we want
  anyway. With IAP offered, honoring the web sub is explicitly legal. (How strictly
  the "parity" proviso is enforced for US apps is currently muddy post-Epic; we
  comply with the strict reading, so it doesn't matter.)
- **Never show the paywall to an entitled user.** Not selling to someone who
  already has the thing is fine (Spotify pattern); double-charging is the only
  real failure mode. If both an IAP and a Stripe sub somehow exist, keep the
  entitlement keyed on the later `expires_at` and surface both in account
  settings ("manage on the App Store" / "manage on the web") — each store's sub
  can only be canceled in its own store, so deep-link accordingly.
- Since May 2025, **US-storefront apps may link out to web checkout with 0% Apple
  commission** (post-contempt guidelines; Ninth Circuit affirmed the contempt
  Dec 11, 2025 while allowing Apple a future *cost-based* fee; SCOTUS granted cert
  ~Jun 30, 2026 — decision expected by mid-2027;
  [Apple developer news May 2025](https://developer.apple.com/news/?id=9txfddzf),
  [9th Cir. opinion](https://cdn.ca9.uscourts.gov/datastore/opinions/2025/12/11/25-2935.pdf),
  [IPWatchdog](https://ipwatchdog.com/2026/06/30/high-court-grants-cert-in-apples-challenge-to-ninth-circuit-contempt-ruling-in-app-store-dispute/)).
  Practical play: US iOS build shows IAP *and* a quiet "save 20% on the web"
  link (web price minus Stripe fees still beats IAP minus 15–30%). Keep the
  link behind a remote flag so it can be pulled the day the legal weather turns.

---

## 6. Platform: Web + Stripe (ship first)

**Stack:** Stripe Checkout (hosted) + Customer Portal + one webhook edge function.
No card data touches us; SCA/3DS handled by Stripe.

Setup checklist:

1. Stripe account (LLC recommended first — §13), Products: `oracle_monthly $4.99`,
   `oracle_yearly $34.99` (intro $29.99), `oracle_lifetime $79` one-time.
2. Edge functions: `checkout-session` (auth user → Stripe Checkout session with
   `client_reference_id = user_id`, `customer_email` = verified email),
   `stripe-webhook` (signature-verified; writes `entitlements`), `portal-link`.
3. Paywall UI on the Oracle overlay (the 402 payload from `oracle-token` carries
   prices); `/account` page with subscription state + portal link.
4. 7-day free trial on the yearly plan only (RevenueCat: 17–32-day trials convert
   42.5% vs 25.5% for ≤4-day; long trials favor annual —
   [RC 2026](https://www.revenuecat.com/blog/growth/subscription-app-trends-benchmarks-2026/)).

**Economics per $4.99/month:** 2.9% + $0.30 + 0.7% Billing + 0.5% Tax Basic ≈
$0.50 → **~90% net**. Per $34.99/year ≈ $1.75 → **~95% net** — the web annual is
the best rail we have anywhere; push traffic toward it.
([stripe.com/pricing](https://stripe.com/pricing)). Chargebacks cost $15 + $15
counter-fee ([Stripe dispute FAQ](https://support.stripe.com/questions/dispute-fees-faq))
— another reason to prefer annual (fewer, larger, less disputed charges).

**Stripe alternative — merchant of record (real option, seriously consider):**
Paddle or similar (5% + $0.50) becomes the legal seller and owns *all* global
sales tax/VAT registration, filing, and chargebacks
([paddle.com/pricing](https://www.paddle.com/pricing)). At our scale the math:
Paddle nets ~85% — identical to Apple's Small Business rate — with zero tax
paperwork forever. Stripe direct nets ~90–95% but leaves us holding §13's
compliance burden. **Recommendation:** start Stripe-direct (US + simple states
only is genuinely fine at launch volume), move to Paddle *or* enable Stripe Tax +
registrations when EU/UK volume appears. Other MoRs: Creem (3.9% + $0.40, young),
Polar, FastSpring (enterprise-ish), Xsolla (games-specific); Lemon Squeezy is in
wind-down-by-migration into Stripe Managed Payments (pricing for which is still
in flux — get terms in writing before betting on it).

**Crypto (yes, we can):** two sane options, both one-time payments —
crypto rails cannot pull auto-renewals, so sell **yearly and lifetime only**:

- **Stripe "Pay with Crypto"**: USDC (Ethereum/Solana/Polygon/Base), flat 1.5%,
  settles to USD inside the same Stripe account/webhooks — one checkbox in
  Checkout ([docs](https://docs.stripe.com/payments/stablecoin-payments)). Do this one.
- Coinbase Commerce (1%, BTC/ETH/USDC…) if we want a non-Stripe fallback
  ([faq](https://www.coinbase.com/commerce/faq)).
- A US merchant accepting crypto for its own product is a "user," not a money
  transmitter (no MSB/KYC obligations — FinCEN FIN-2013-G001); tax treatment in §13.
- TON specifically matters inside Telegram (§9), not on the web.

---

## 7. Platform: iOS

### 7.1 Port architecture — yes, we reuse the C, and it gets *better*

Three options, pick #2:

1. **Pure wrapper (Capacitor + wasm oracle in WKWebView).** Works — WKWebView
   runs Web Workers and JIT'd wasm fine (WebKit has supported wasm since iOS 11;
   Apple's 2.5.2 "no JIT" rule applies to *your* process, not WebKit's) — but it
   is the weakest 4.2 story and wastes the platform.
2. **Hybrid (recommended): Capacitor shell + existing web client + NATIVE oracle.**
   Compile `cnitro` directly for arm64 (it's plain C11; the wasm build's
   freestanding constraints don't even apply natively) into a small Capacitor
   plugin: `OraclePlugin.start(jobJSON) → stream of batch events`. The JS
   controller/overlay stay identical; only `oracleBridge` gets a second transport
   (native events instead of worker `postMessage`). Threading: **pthreads, one
   deliberation per thread** — this is exactly the already-proven native OMP model
   (`_Thread_local` RNG/scratch/TT), i.e. *Mode B semantics with zero browser
   constraints*. On a modern iPhone (6 performance cores, real SIMD, no wasm
   overhead) this is the fastest Oracle on any platform — "your phone analyzes
   millions of worlds per second" is an App Store screenshot, not just eng pride.
   Native-compiled C engines are unambiguously allowed (every Unity game is one).
3. Full SwiftUI rewrite — months; not justified at this stage.

Also in the app: StoreKit 2 subscriptions, offline bot play (the PWA layer
already proves the offline engine — on iOS it's a first-class feature and a
4.2 argument), haptics on card plays, share-sheet for replay QRs.

### 7.2 Guarding on iOS (your instinct is right)

Native paywall: `Transaction.currentEntitlements` (StoreKit 2) checked in the
plugin before starting analysis; server webhook (App Store Server Notifications
V2) mirrors the sub into `entitlements` so the same purchase works on web/TMA.
Client-side enforcement is appropriate here: the binary is signed and encrypted,
there's no public wasm to lift, and the bypass population (jailbroken devices
running patched binaries) rounds to zero revenue. Still issue oracle tokens on
iOS too — for usage metering and the free-tier counter, not enforcement.

### 7.3 App Review: requirements & the "vibe-coded app" climate

- **Costs:** $99/yr developer program; commission 30% standard, **15% via the
  Small Business Program** (auto-qualifies under $1M/yr — enroll immediately), and
  subscriptions drop to 15% after 12 continuous months per subscriber even outside
  SBP ([Apple SBP](https://developer.apple.com/app-store/small-business-program/)).
  Apple is merchant of record for IAP: they remit sales tax/VAT in 70+ countries;
  we book net proceeds ([App Store Connect tax help](https://developer.apple.com/help/app-store-connect/making-payments-to-apple/understanding-taxes/)).
- **4.2 Minimum Functionality / 4.3 Spam:** Apple tightened again at WWDC26
  (Jun 8, 2026) — they now reserve the right to remove low-value, unmaintained
  apps, and in Mar 2026 they blocked "vibe coding" platform apps under 2.5.2 and
  minimum-functionality grounds
  ([MacRumors](https://www.macrumors.com/2026/06/09/app-store-guidelines-low-quality-apps/),
  [MacRumors Mar 2026](https://www.macrumors.com/2026/03/18/apple-blocks-updates-for-vibe-coding-apps/)).
  There is no "AI-generated apps" rule per se — enforcement runs through quality.
  **Our defense is genuine substance:** real-time multiplayer, a native C engine
  doing visible heavy compute, offline play, three localizations, procedural
  rendering. A reviewer poking it for five minutes must hit native-feeling
  interactions, not a scrolling website. Budget one polish week for: launch
  screen, no browser chrome anywhere, native transitions on the shell, App Store
  screenshots that lead with the Oracle.
- **Compliance list:** 3.1.2 subscription rules (restore purchases, price+term
  before buy, ToS/EULA + privacy policy links in app and metadata); 5.1.1(v)
  in-app account deletion (§4); privacy nutrition labels (we collect: account
  info, gameplay — no tracking, so **no ATT prompt**); age rating under the
  Jul 2025 overhaul (4+/9+ band — Durak has no wagering or simulated gambling;
  answer the expanded questionnaire accordingly).
- **Epic v. Apple strategy** is in §5: sell IAP, honor web subs, US-only web
  link behind a remote flag, watch the SCOTUS docket (No. 25-1311) before
  building anything that *depends* on 0% link-outs.

Effort: **4–8 weeks** including review round-trips; the native oracle plugin is
~1–2 of those weeks (the C compiles in an afternoon; the bridge, thread pool,
and battery/thermal throttling are the real work — cap at
`activeProcessorCount - 2` threads and pause batches when
`thermalState >= .serious`).

### 7.5 iMessage extension — the bundled distribution channel

An iMessage app is an **extension target inside the main iOS app** (a
`MSMessagesAppViewController` in the same App Store submission — no separate
listing fees or review track), surfaced in the Messages app drawer. The
channel is alive but niche: the Messages framework is still maintained
([docs updated Jan 2026](https://developer.apple.com/documentation/Messages)),
while the iMessage App Store itself has near-zero browse discoverability —
distribution is **person-to-person**: a recipient without the app taps the
game bubble and gets an install prompt. That mechanic built **GamePigeon**: a
solo developer's 20+ minigame pack, #1 Top Free in the iMessage store within
six months of launch, monetized on IAP cosmetics/ad-removal, culturally sticky
with US teens for a decade ([Wikipedia](https://en.wikipedia.org/wiki/GamePigeon)).
Notably, GamePigeon's catalog is checkers/8-ball/sea-battle-tier — there is
**no serious card game with a real engine in the channel**, and the
Russian-diaspora demographic (US/DE/IL iPhone teens and their parents) is
exactly who texts on iMessage.

**Why our stack is unusually suited to it — the codec is the killer fit:**

- Turn-based iMessage games work via `MSSession`: each turn sends an
  `MSMessage` whose **URL payload carries the game state**, and Messages
  replaces the previous bubble in the same session. Most games have to invent
  a compact state encoding for this; **we already ship one** — the rANS packed
  state/replay codec was built to fit a whole game in a QR-alphanumeric URL.
  A Durak turn bubble is literally `WWW.FOOLISH.CARDS/<code>` plus a rendered
  snapshot of the table. The same URL opens on web for green-bubble friends —
  the iMessage game and the web game are one artifact.
- Async "correspondence Durak" (play a turn when you like) is a *new mode*, not
  a port: the rules engine is authoritative on both ends (guards.wasm / native
  C), so each turn is validated locally, no server in the loop for 1v1 casual
  play — which also means it works with **zero infra cost**.

**Constraints to respect (they shape the design, not kill it):**

- Extensions run under a **strict, undocumented memory ceiling** (tens of MB;
  jetsam kills over-budget extensions —
  [Apple forums](https://developer.apple.com/forums/thread/60706)). The Oracle
  worker fleet does NOT run in-extension. Pattern: play turns in the bubble;
  the "Analyze with the Oracle" button deep-links into the **main app** (full
  native oracle, full paywall) — the extension becomes an Oracle *funnel*, not
  an Oracle host. Keep the extension UI native-lightweight (the procedural
  renderer's 2D-canvas fallback, or plain UIKit cards), not the full Next.js
  client in a WKWebView.
- IAP works from extensions (GamePigeon sells inside Messages), but keep the
  paywall in the main app — one purchase surface, fewer review variables. The
  extension and app share the entitlement via an App Group + the same account.
- Expectations: this is a **distribution experiment, not a revenue pillar** —
  the iMessage store's discoverability is search-only and the channel is
  US/diaspora-shaped. Success metric is installs-per-shared-game and the
  extension→main-app→Oracle funnel, measured before investing past v1.

Effort: **2–3 weeks** on top of the finished iOS app (extension target, MSSession
turn plumbing over the existing codec, bubble snapshot rendering, deep links).
Do not build it before the main app exists — it can't ship alone.

---

## 8. Platform: Google Play

- **Wrapper tech:** **TWA (Bubblewrap)** — the app runs in real Chrome, which
  means Workers, wasm, and even SharedArrayBuffer/Mode B work (§10). Play Billing
  integrates into a TWA via the **Digital Goods API** + Payment Request API, so we
  keep one web codebase. Play allows PWA/TWA wrappers but enforces a quality bar
  (Play's catalog shrank ~47% in the 2024 quality purge — same defense as §7.3:
  we're a real game, not webview spam).
- **Fees — new regime as of June 30, 2026 (US/EEA/UK):** service fee **10%**
  (first $1M and all auto-renewing subscriptions) **+ 5% billing fee** if using
  Play Billing → **15% effective**, same as before; using external
  billing/web-link drops the 5% but the 10% service fee stays
  ([Android Developers Blog Jun 2026](https://android-developers.googleblog.com/2026/06/play-expanded-billing.html)).
  With Stripe costing ~3.7%+, external billing saves ~1.3 points — **not worth
  the UX/tax burden; just use Play Billing.** Google is merchant of record for
  Play-billing sales (remits sales tax/VAT) — another reason.
- **Epic v. Google:** the injunction is in force (Ninth Circuit affirmed Jul 31,
  2025; SCOTUS stay denied Oct 6, 2025); link-outs via the "external content
  links" program currently carry **0% Google fee** (enrollment required since
  Jan 28, 2026; Google has published intent to charge later), third-party store
  catalog access opens Jul 22, 2026, and a revised Epic settlement (9%/20% tiers)
  awaits a summer-2026 evidentiary hearing
  ([policy pages](https://support.google.com/googleplay/android-developer/answer/15582165),
  [stash.gg Apr 2026](https://www.stash.gg/blog/blog-epic-v-google-settlement-update-april-2026)).
  Same posture as iOS: IAP by default, flag-gated web link, don't architect
  around a moving target.
- **Solo-dev requirements:** $25 one-time; personal accounts created after
  Nov 2023 need **12 testers opted in for 14 continuous days** of closed testing
  before production access
  ([testing requirements](https://support.google.com/googleplay/android-developer/answer/14151465))
  — start this clock early or register an organization account (D-U-N-S needed).
  Data safety form; account deletion incl. **web link** (§4 covers it); target
  API 35 now / API 36 for new submissions after Aug 31, 2026.
- **Russia note:** Play billing is dead in RU (since Dec 2024). For RU Android
  users the answer is the TMA (§9) and later possibly **RuStore** distribution
  of the same TWA — treat that as an experiment, not a launch requirement.

Effort: **1–2 weeks** on top of the finished web product (Bubblewrap + Digital
Goods API + RTDN webhook), plus the 14-day testing clock.

---

## 9. Platform: Telegram Mini App

**Why it's strategic, restated with 2026 facts:** Apple stopped processing all
payments in Russia on Apr 1, 2026 ([MacRumors](https://www.macrumors.com/2026/04/02/apple-turns-off-payments-in-russia/));
Google Play billing there died Dec 2024. For the game's heartland audience,
**Telegram Stars is effectively the only compliant, low-friction payment rail.**
The tap-to-earn bubble has deflated (Hamster Kombat −86% DAU), which is good:
distribution is cheaper, and we're a real game, not a points farm. Telegram is
1B+ MAU.

### 9.1 Architecture

```
Telegram client (iOS/Android/Desktop/Web)
  └─ WebView → https://foolish.cards/?tma=1        (same Next.js app, TMA shell mode)
       │  Telegram.WebApp.initData  ──────────────┐
       ▼                                          ▼
  Next.js client                     edge fn: telegram-auth
   • hides site chrome, uses          • HMAC-SHA256 validate initData
     Telegram theme params              (secret = HMAC(bot_token,"WebAppData"),
   • MainButton = primary action        reject stale auth_date)
   • Mode A oracle (workers) ✓        • upsert user tg_<id> (Supabase admin API)
   • Mode B ✗ (no SAB in TG webviews) • mint Supabase session → client
       │
       ▼ payments
  Bot Payments API, currency "XTR" (Stars)
   • one-offs: sendInvoice / createInvoiceLink        (day pass, single deep review)
   • subscription: createInvoiceLink{subscription_period: 2592000}  ← 30 days is the
     ONLY allowed period, auto-debits Stars monthly ≤10,000 Stars
   • bot webhook successful_payment → entitlements (source='telegram')
```

- The game itself already fits the TMA envelope: tiny bundle, zero image assets,
  offline-tolerant, realtime over plain websockets. Vercel keeps serving it;
  Supabase Realtime works from Telegram webviews (ordinary WSS).
- **The Foolish bot** doubles as the retention channel: challenge links, daily
  free-analysis reset pings, replay shares that unfurl into "re-fight this vs
  the Oracle."

### 9.2 Rules and economics (know before building)

- **Stars are mandatory** for digital goods sold *inside* Telegram, "regardless
  of any other web portals … you may have set up outside the Telegram ecosystem"
  ([core.telegram.org/bots/payments-stars](https://core.telegram.org/bots/payments-stars)).
  So: **no Stripe links inside the TMA.** The same account, subscribed via web
  Stripe elsewhere, is still entitled inside Telegram (that direction is ours to
  honor and Telegram doesn't care) — but the TMA's own paywall sells Stars only.
- **Payout haircut ≈ 35% + volatility + delay:** users pay ~$0.02/Star in-app;
  developers withdraw via Fragment at ~$0.013/Star as **TON**, per-payment
  **21-day hold**, 1,000-Star minimum
  ([Stars docs](https://core.telegram.org/bots/payments-stars),
  [InviteMember guide](https://blog.invitemember.com/telegram-stars-to-ton-how-to-withdraw-and-convert-2025/)).
  Net-net Stars ≈ app-store economics with worse liquidity — price accordingly
  (§11) and treat TON receipt as taxable income at receipt (§13).
- **Subscriptions:** 30-day period only; no annual. Sell monthly (150 Stars) and
  push whales toward one-off "season passes" (e.g., 1,200 Stars ≈ 12 months) if
  demand appears.
- Affiliate program (Stars commissions for referrers) is native — free
  distribution lever once the paywall exists.

Effort: **2–4 weeks** (TMA shell + telegram-auth + Stars invoices/webhook +
paywall variants), reusing the entire game and oracle unchanged.

---

## 10. Oracle threading: where Mode A / Mode B / native threads run

Mode A (shipped): N independent single-thread wasm instances in Web Workers,
merge in TS. Mode B (specced in `INFINITE_ORACLE_DESIGN.md` §8b, **build on a
separate branch as `wasm-oracle-mt`**): one shared `WebAssembly.Memory`, real
TLS, C atomics accumulation, main thread polls `wasm_mt_snapshot()`. Native
(iOS/desktop builds): pthreads over the same `_Thread_local` model — Mode B
semantics without a browser.

| Environment | Mode A | Mode B (SAB + crossOriginIsolated) | Native threads | Notes |
| --- | --- | --- | --- | --- |
| Desktop Chrome/Firefox/Edge | ✅ shipped | ✅ with COOP/COEP headers | — | The Mode B showcase target |
| Desktop/iOS Safari (real browser) | ✅ | ✅ Safari 15.2+ honors COI headers | — | Fewer workers on iPhone Safari (memory) |
| Android **TWA** (Play app) | ✅ | ✅ it's real Chrome | — | Only *app* wrapper where Mode B works |
| Android System WebView (Capacitor) | ✅ | ❌ SAB unsupported in WebView ([caniwebview](https://caniwebview.com/features/mdn-javascript-builtin-sharedarraybuffer/)) | — | Reason to prefer TWA on Android |
| iOS WKWebView (Capacitor shell) | ✅ | ⚠️ unreliable/undocumented — do not plan on it | ✅ via native plugin | Native plugin replaces both modes (§7.1) |
| Telegram webviews (iOS/Android/Desktop) | ✅ | ❌ assume absent | — | Cap workers at 4 on low-end Androids; honor `deviceMemory` |
| iMessage extension | ❌ don't try | ❌ | ❌ | Tens-of-MB jetsam ceiling (§7.5); "Analyze" deep-links to the main app's native oracle |
| Server (Deno edge) | single instance, 2s CPU | ❌ | — | Tokens & spot verdicts only |

**Mode B rollout plan (separate branch, exactly as §8b prescribes):**

1. Branch `oracle-mode-b`; new Makefile target `wasm-oracle-mt` per §8b.3
   (`-matomics`, real TLS restored, `--shared-memory --max-memory=128MiB`,
   drop `CD_WASM_OVERLAY`/`OG_EXPLAIN_BUILD`, add `FOOLISH_ORACLE_MT`); commit
   `public/oracle-mt.wasm.gz` **alongside** the Mode A artifact.
2. Next.js `headers()`: `Cross-Origin-Opener-Policy: same-origin`,
   `Cross-Origin-Embedder-Policy: credentialless` — **audit first**: the headers
   land on the shared `/[game_id]` route (live games too); our asset story is
   unusually clean (no CDN fonts, procedural textures, Supabase = CORS
   fetches/WSS which COEP permits), but any future OAuth *popup* flow breaks
   under COOP — use redirect flows (§4 uses email links; fine).
3. Loader: `crossOriginIsolated && SharedArrayBuffer ? oracle-mt : oracle` —
   Mode A remains the permanent fallback; rollback = delete headers.
4. Ship it as a premium-tier polish ("Turbo convergence") on desktop web only.
   It is **not** on the revenue-critical path — do it after the money loop works.

---

## 11. Pricing (direct answers)

Anchors from the comps: chess.com Gold/Platinum/Diamond ≈ $50/$80/$150-yr with
review paywalled at 1/day free; gaming-category medians $4.99-mo / $24.99-yr;
weekly plans = 55% of mobile subscription revenue (≈82% in gaming); annual
renews at 83%; lifetime prices at ~3.7× annual; XG (backgammon's gold-standard
analyzer) has sold for a decade at $59.95 one-time
([sources in §2](#2-is-this-worth-paying-for-the-honest-case),
[RevenueCat/Adapty benchmarks](https://adapty.io/app-subscription-pricing-index/)).

| Question | Answer | Rationale |
| --- | --- | --- |
| **Daily?** | **No daily subscription.** Sell a **day pass as a consumable**: 25 Stars (TMA) / $0.99 IAP (stores). Skip on web — Stripe's $0.30 fixed fee eats 33% of a dollar. Stores can't even bill sub-weekly subs. | Impulse tier without the churn mess |
| **Weekly?** | **$1.99/wk, mobile stores + TMA only** (125 Stars). Not on web. | Gaming revenue is ~82% weekly on stores; on web it just cannibalizes monthly |
| **Monthly?** | **$4.99/mo** everywhere; TMA **150 Stars** (~$3 — CIS-adjusted; Stars subs are 30-day-only anyway) | Category median; safely under chess.com Platinum |
| **Yearly?** | **$34.99/yr** (launch intro $29.99), 7-day trial, web-first push | RC median $34.80; ~40% off monthly; 83% renewal cohort; ~95% net on Stripe |
| **One-time?** | **$79 "Founder" lifetime**, capped (say first 500), first year only | ~2.3× annual = deliberately cheap early cash + evangelists; XG proves niche analyzers sell one-time. Retire it once LTV data exists (lifetime at scale caps your best users' LTV) |
| Single deep review | 25 Stars one-off (TMA only) | Stars favor micropayments; also the natural gift/tip unit |

Regional: rely on Apple/Google regional price tiers (auto-lowered in low-WTP
countries); Stars pricing *is* the CIS price. Diaspora (US/DE/IL) pays full
western prices — do not underprice web to match Telegram.

Free tier forever: **1 analysis/day** — chess.com has A/B-tested that number
against 250M users for a decade; steal it and A/B later, not before launch.

---

## 12. Unit economics per rail (at $4.99/mo, $34.99/yr)

| Rail | Fees | We keep (monthly) | We keep (annual) | Tax handled for us? |
| --- | --- | --- | --- | --- |
| Web Stripe (card) | 2.9%+$0.30 +0.7% Billing +0.5% Tax calc | ~$4.49 (90%) | ~$33.2 (95%) | ❌ calc only — we register/remit (§13) |
| Web MoR (Paddle-class) | 5% + $0.50 | ~$4.24 (85%) | ~$32.7 (93%) | ✅ fully |
| Web crypto (Stripe USDC) | 1.5% flat | n/a (no auto-renew — annual/lifetime only) | ~$34.5 (98.5%) | ❌ (sales-tax gray; income tax on us) |
| Apple IAP (SBP) | 15% | $4.24 | $29.74 | ✅ Apple is MoR |
| Apple IAP (standard, yr-1 subs >$1M) | 30% | $3.49 | $24.49 | ✅ |
| Google Play Billing (new regime) | 10%+5% = 15% | $4.24 | $29.74 | ✅ Google is MoR |
| US iOS/Play web-link-out | 0% store fee **today** + Stripe fees | ~$4.49 | ~$33.2 | ❌ ours |
| Telegram Stars (150/mo) | user pays ~$3.00; payout ~150×$0.013 | ~$1.95 (≈65%) + 21-day hold + TON volatility | n/a (30-day subs only) | ❌ income tax on TON at receipt |

Read: **web annual is king; store IAP is fine at 15%; Stars is the price of
reaching Russia, not a margin play.**

---

## 13. Taxes (US solo dev; not legal advice — one hour with a CPA before launch)

- **Sales tax (US):** 30+ states tax digital goods/subscriptions, but economic
  nexus thresholds (~$100k/state; many states dropping the 200-transaction test)
  mean a small seller registers **home state only** at first. Apple/Google IAP
  sales are marketplace-facilitated — they remit; those sales are off your
  sales-tax plate entirely. Stripe Tax *calculates* but you must register and
  remit where registered (Tax Basic 0.5%; automated filing = Tax Complete from
  ~$90/mo) — or a MoR makes it all vanish (§6).
- **EU/UK VAT:** due from the **first** B2C sale by a non-EU seller — no
  threshold. Compliance = non-Union OSS (one quarterly EU filing) + separate UK
  registration… or a MoR. Practical launch posture: Stripe-direct, US-only tax
  registration, and flip EU/UK checkout to a MoR (or register OSS) once that
  revenue is real. Do not silently ignore it forever.
- **Income tax:** all of it is ordinary income (net proceeds from Apple/Google/
  Stripe/Paddle alike). Sole prop/LLC → Schedule C + **15.3% self-employment tax**;
  S-corp election worth evaluating above ~$50–80k consistent profit; the 20% QBI
  deduction is now permanent (OBBBA, Jul 2025). Pay **quarterly estimates** from
  the first profitable quarter (safe harbor: 100/110% of prior year).
- **Crypto/TON specifics:** Stars→TON withdrawals are **ordinary (SE) income at
  USD fair market value on the day received**, then capital gain/loss on the
  later TON→fiat conversion — two taxable events, and Telegram issues no US tax
  forms, so export Fragment history religiously
  ([IRS digital assets](https://www.irs.gov/filing/digital-assets)). Same
  two-event structure for any Stripe-USDC you hold instead of auto-converting.
- **1099-K:** back to >$20k AND >200 transactions for TPSOs (OBBBA), but Stripe
  card volume is reported with no minimum — irrelevant either way: report all
  income.
- **Entity:** a single-member LLC (cheap, quick) before the Stripe account is
  worth it for cleanliness; S-corp later if profits justify payroll overhead.

---

## 14. Supabase free tier — is it fine?

**Fine today, not fine the day money moves.** Current free limits
([supabase.com/pricing](https://supabase.com/pricing)): 500 MB database, 50k MAU
auth, 500k edge invocations/mo, 200 concurrent Realtime connections, 2M Realtime
messages/mo, 5 GB egress, **no backups**, and **projects pause after 7 days of
inactivity**.

Why the paid flip is non-negotiable at first revenue:

1. **A paused project drops webhooks.** Stripe retries for ~3 days, Apple/Google
   notifications are best-effort — a quiet week (or a growing one that trips a
   quota) must never be able to corrupt entitlement state. (Mitigation either
   way: webhook handlers are idempotent and we reconcile entitlements against
   Stripe/Apple/Google truth nightly.)
2. **No backups on free.** `entitlements` is billing data; Pro has daily backups
   (+ PITR add-on later).
3. **200 concurrent Realtime connections is a ~200-CCU cap on live games** — the
   first marketing spike hits it.
4. Custom SMTP (§4) is needed regardless of tier; the built-in 2/hr mailer is
   never acceptable in production.

**Budget:** Pro $25/mo + Resend free tier + Vercel (current plan) + $99/yr Apple
+ $25 Google. Total fixed burn to run all four platforms ≈ **$35–45/mo** — the
first ~10 subscribers cover it.

---

## 15. Build order & effort (solo dev, honest estimates)

| # | Work | Effort | Unlocks |
| --- | --- | --- | --- |
| 1 | Auth rebuild (§4): SMTP, verification, reset, add-email upgrade, deletion | 1–2 wk | Everything |
| 2 | Entitlements + oracle tokens + free-tier metering (§3, §5) | ~1 wk | The paywall exists |
| 3 | Stripe checkout/portal/webhook + paywall UI (§6) + Supabase Pro | ~1 wk | **First revenue (~week 3–4)** |
| 4 | Telegram Mini App + Stars (§9) | 2–4 wk | RU/CIS revenue, viral channel |
| 5 | iOS: Capacitor shell + native oracle plugin + StoreKit 2 (§7) | 4–8 wk | Highest-ARPU store |
| 6 | Play: TWA + Digital Goods API + RTDN (§8; start the 12-tester clock at step 4) | 1–2 wk | Android store |
| 7 | iMessage extension: MSSession turns over the replay codec + deep-link Oracle funnel (§7.5) | 2–3 wk | Person-to-person iOS distribution — optional, requires step 5 |
| 8 | Mode B `wasm-oracle-mt` on `oracle-mode-b` branch (§10) | 1–2 wk | Desktop "Turbo" polish — optional, last |

Cross-cutting rules: every webhook idempotent; nightly entitlement
reconciliation job; remote flags on all store-steering links; price experiments
only after 60 days of baseline data.

## 16. What's still in legal flux (re-check before each launch)

1. **Apple US link-out 0%** — SCOTUS granted cert (~Jun 2026, No. 25-1311);
   Ninth Circuit already authorized a future cost-based fee. Keep IAP primary.
2. **Google settlement** (9%/20% tiers) awaits a summer-2026 hearing; external
   content links are 0% *for now* with published intent to charge 10%/20%.
3. **3.1.3(b) parity enforcement** for US apps leaning on web purchases is
   inconsistent — we comply with the strict reading, so no exposure.
4. **Stars→TON terms** (Fragment rate, 21-day hold) are Telegram policy, not
   contract — the ~35% effective haircut can move.
5. California taxes electronically delivered software from Jan 1, 2027 —
   re-check state registrations then.
