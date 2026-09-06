# Deploying `foolish_server` — platform, TLS, email, and the stateful-scaling plan

This is a deployment recommendation, not an implementation — no code in this
repo changed for it. It's decision support for putting `foolish_server.c`
(see [`README.md`](README.md), [`SERVER_SCALING.md`](SERVER_SCALING.md),
[`DURABILITY.md`](DURABILITY.md), [`TLS.md`](TLS.md)) somewhere real people
can reach over the internet.

## What we're placing (the constraints, not repeated elsewhere)

- **One long-lived C process holds every in-progress game as a `Game` struct
  in RAM** — not a request-per-invocation model. Connections are
  **persistent WebSockets** (`/ws`). Anything that recycles the process
  between requests, or kills an idle connection/process to reclaim
  capacity, destroys live games.
- Internally sharded by `hash(game_id) % N_GAME_WORKERS`, one **epoll**
  event loop per shard (Stage 6) for plaintext; `--tls` falls back to
  thread-per-connection (non-blocking TLS wasn't attempted — see
  `TLS.md`'s "TLS-over-epoll status"). Measured cost: ~51 MB RSS / 46
  threads at 160 WS connections, ~109 MB / 105 threads at 400 (game-workers
  default of 4), from `SERVER_SCALING.md`'s Stage 6 Deliverable 2 table —
  call it low hundreds of KB per connection once past a small fixed floor.
- **Durability is local SQLite (WAL, write-behind, `synchronous=NORMAL`)** —
  one file, one dedicated persistence thread, per **instance**
  (`DURABILITY.md`). It is not a shared/network database. A `kill -9` and
  restart against the *same* `--db` file recovers everything up to the
  last ~75ms write-behind interval — a different machine's copy of the
  binary pointed at an *empty* `--db` recovers nothing.
- TLS is built into the binary (`--tls --cert=... --key=...`, OpenSSL,
  TLS 1.2/1.3 only) but costs real epoll design: `--tls` reverts to
  thread-per-connection because the epoll rewrite only covers the
  plaintext path. Running the process plaintext and terminating TLS in
  front of it keeps the Stage 6 win.
- **Horizontal scale-out was explicitly not attempted** — README's
  "Production readiness" table lists it plainly: "one process, one
  machine — the per-game lock design doesn't extend across processes."
  That's the crux of this doc (section 3 below).
- Single native binary, no runtime beyond `libsqlite3`/`libssl`/`libcrypto`
  — trivially containerizable, no interpreter/VM to schlep along.

## 1. Recommendation

**Primary: Fly.io Machines.** **Alternative: Hetzner Cloud VMs + Hetzner
Load Balancer** (self-managed, cheaper raw compute, more ops ownership).

Both are argued for below against Cloud Run, ECS/Fargate, Railway, and
Render, which are covered honestly and mostly ruled out or demoted for
stated reasons — this isn't a feature dump, it's why those two won.

### Platform comparison

| Platform | Long-lived WS? | Persistent process? | Runs a raw binary/container? | Verdict |
|---|---|---|---|---|
| **AWS Lambda** (and request-scoped serverless generally) | No — request-scoped, no persistent socket across invocations | No | N/A | **Ruled out.** Wrong execution model entirely; not analyzed further per the brief. |
| **Google Cloud Run** | Yes, *per-connection* — but request timeout caps a WS session at 60 min max (5 min default), and instance autoscale/recycle is stateless by design | No durable local disk across instance replacement; session affinity for routing repeat connections to the *same* instance is documented as **best-effort**, not guaranteed | Yes, any container | **Ruled out as primary**, honestly assessed: Cloud Run's own docs say WebSockets are GA and request timeouts up to 60 minutes are GA ([docs](https://docs.cloud.google.com/run/docs/configuring/request-timeout)), and WS-is-inherently-sticky-for-that-one-connection is real. But a *scaled-in* instance loses every RAM-held game it was holding with no local-disk continuity to fall back on, and "best-effort" affinity for the *next* connection is incompatible with "this game's state lives on exactly one machine." Making it work would mean externalizing state first (§3b) — at that point Cloud Run is a fine *stateless* target, but that's a different, bigger rewrite than a deployment choice. |
| **AWS ECS/Fargate** | Yes — ALB-fronted tasks, and a WS upgrade is inherently sticky to whichever target answered the 101 ([websocket.org guide](https://websocket.org/guides/infrastructure/aws/alb/)) | Yes, tasks are long-running | Yes, container only (no bare binary without a container) | **Viable, not the pick.** EFS can back persistent storage across task replacement, similar in spirit to a Fly Volume. But the glue is heavier: ALB/NLB + target groups + a NAT Gateway for private-subnet egress add roughly **$90/month of fixed AWS infrastructure cost before a single task runs**, per current breakdowns ([fortem.dev](https://fortem.dev/blog/aws-fargate-pricing-real-costs/)), plus VPC/subnet/security-group setup. Good "you're already an AWS shop" alternative; more ops surface than Fly for the same outcome at our scale. |
| **Railway** | Yes — "WebSockets and workers that run all day belong on Railway"; container-based, no cold starts ([Railway blog via search](https://www.13labs.au/compare/railway-vs-vercel)) | Yes | Yes, Docker | **Viable single-instance alternative, weak on multi-shard routing.** Railway's raw **TCP Proxy** — what you'd need for a custom game_id-aware routing layer in front of multiple instances — currently **does not support attaching custom domains or TLS certificates** ([Railway Central Station feature request](https://station.railway.com/feedback/custom-domain-ssl-support-for-tcp-prox-08a88924)); only its HTTP-fronted "web service" path gets automatic TLS. Fine for one instance behind Railway's own domain; once you need N shards with your own routing, you're building that layer yourself with less native support than Fly gives you. |
| **Render** | Yes — Render's own docs: "VM-based compute is a better fit for WebSocket servers or anything that benefits from a warm, persistent process" ([render.com/docs/websocket](https://render.com/docs/websocket)) | Yes | Yes, Docker or native runtime | **Viable single-instance alternative**, same shape as Railway: automatic TLS via Let's Encrypt/Google Trust Services on custom domains ([render.com/docs/tls](https://render.com/docs/tls)), simple fixed per-service pricing (background workers **from $7/mo**), but no scriptable edge/routing primitive for the game_id-sharding crux — you'd bring your own router the same as Railway. |
| **Bare IaaS — Hetzner** | Yes, it's just a VM | Yes | Yes, anything | **Alternative pick** — see §5. Cheapest raw compute, full control, you own TLS/routing/patching/deploys. |
| **Bare IaaS — DigitalOcean/AWS EC2/GCP Compute** | Yes | Yes | Yes | Same shape as Hetzner, pricier per unit compute (e.g. AWS `t4g.small`, 2 vCPU/2GB, ~$12.26/mo — [cloudprice.net](https://cloudprice.net/aws/ec2/instances/t4g.small) — vs. Hetzner CX23 at €5.49/mo — [betterstack.com](https://betterstack.com/community/guides/web-servers/hetzner-cloud-review/)); pick if you're already paying for the ecosystem (RDS, IAM, etc.) elsewhere. Not analyzed separately below; the Hetzner tradeoffs generalize. |
| **Fly.io Machines** | Yes — purpose-built; Fly's own material repeatedly frames Machines as the target for "a stateful API, a WebSocket server, a queue worker" that "does not fit the serverless model" | Yes — Machines are VM-like (Firecracker), start/stop, but stay resident while started | Yes — a Dockerfile or a raw binary in a minimal image | **Primary pick** — see §5. |

**libcurl footnote on the "no other runtime" property**: whichever
platform is chosen, the deploy artifact stays a single static-ish Linux
binary (or a tiny container wrapping it) — none of the above require a
language runtime beyond what's already linked (OpenSSL, SQLite, and after
§4, libcurl). That rules nothing in or out by itself, but it does mean the
container image is small and the cold-build story on any of these
platforms is just "compile with `make`, `COPY` the binary."

## 2. Managed TLS / WSS at the edge

The design goal (`TLS.md`, `README.md`) is: terminate TLS where it's
cheap to rotate certs and where non-blocking I/O is already solved, and
let `foolish_server` run **plaintext**, so it keeps the Stage 6 epoll
path instead of falling back to thread-per-connection. Concretely, that
means **never pass `--tls` to the deployed process** — the platform's
edge/LB holds the certificate and speaks TLS 1.2/1.3 to the internet;
the hop from edge to the Machine/container/VM is either plaintext over a
private/trusted network (Fly's Anycast edge to its own Machines, an ALB
to its Fargate tasks, Render's LB to its service) or, if paranoid about
that hop, TLS-passthrough to the app's own `--tls` listener — but that
gives up the epoll design specifically for the WS hot path, so it's not
recommended here.

- **Fly.io**: Fly Proxy terminates TLS by default on 443 for every app,
  Anycast-routed to the nearest edge; certificates are automatic Let's
  Encrypt via `fly certs add <domain>`, min protocol TLS 1.2/1.3 ([Fly
  TLS termination docs](https://fly.io/docs/security/tls-termination/),
  [Fly TLS support docs](https://fly.io/docs/networking/tls/)). No cert
  rotation to own — exactly the gap `TLS.md` names as "still needed"
  (`--tls` loads a cert once at startup, no reload on rotation) becomes
  moot because the app never holds the cert.
- **Hetzner**: no built-in edge TLS the way Fly has, but Hetzner Cloud
  Load Balancers do TLS termination as a first-class feature (upload a
  cert or, in practice, run Caddy/Traefik+ACME on the LB path or a small
  edge box) at **€4.90/mo flat + 20 TB included traffic**, and support
  sticky sessions at the LB layer ([Hetzner LB
  product page](https://www.hetzner.com/cloud/load-balancer/), [Pulumi
  TLS-termination tutorial](https://community.hetzner.com/tutorials/pulumi-load-balancer-tls-termination-hetzner/)).
  This is the piece you own on bare IaaS: either the LB's TLS termination
  feature, or a Caddy instance in front doing automatic ACME — both are
  well-trodden, not novel engineering.
- **Cloudflare in front of either**: viable as an additional layer (DDoS
  absorption, geo-routing) for plain HTTPS/WSS on 80/443 with no extra
  config. Cloudflare **Spectrum** (arbitrary TCP/UDP port proxying with
  Cloudflare's own TLS termination) is an **Enterprise-plan add-on**, not
  something available on standard plans — not worth pulling in here since
  our server only ever needs 80/443 (see [Spectrum
  docs](https://developers.cloudflare.com/spectrum/) and [settings-by-plan](https://developers.cloudflare.com/spectrum/reference/settings-by-plan/)).
  Recommendation: skip Spectrum, use Cloudflare (if at all) in plain
  proxy/CDN mode in front of Fly or the Hetzner LB, not as the TLS
  terminator of record.

## 3. The crux — scaling a stateful, sharded, in-memory server horizontally

Today: one process, `hash(game_id) % N_GAME_WORKERS` picks an **epoll
shard** *inside that process*, each shard's games live only in that
process's RAM, and the SQLite file backing them is local to that one
process's host. There is no cross-process concept of "which machine owns
this game" — because there's only ever one machine. Scaling to multiple
machines means inventing that concept. Three shapes, evaluated against
what's already here:

### (a) Sticky / consistent-hash routing (keep RAM + local SQLite, add a routing layer)

Extend the **exact same idea** `foolish_server.c` already uses internally
(`hash(game_id) % N`) up one level: instead of `N_GAME_WORKERS` threads in
one process, `N` **machines**, each running an unmodified
`foolish_server` with its own volume and its own `--db`. A routing layer
in front picks the machine for a `game_id` and keeps sending that game's
traffic there.

- **What makes this easy on Fly**: Fly Proxy has two built-in primitives
  for exactly this — `fly-replay` (an app response header telling the
  edge to replay the request to a different Machine/region/app) and
  `fly-force-instance-id` / preferred-instance routing (pin a request to
  a specific Machine, with fallback) — see [Dynamic Request Routing with
  fly-replay](https://fly.io/docs/networking/dynamic-request-routing/),
  [Session Affinity / Sticky Sessions
  blueprint](https://fly.io/docs/blueprints/sticky-sessions/), and a
  community thread specifically about **routing a game server to the
  correct Machine instance** via these headers ([fly-force-instance-id
  discussion](https://community.fly.io/t/problem-routing-to-correct-instance-via-fly-force-instance-id/22941)).
  A request lands on *some* Machine (Fly's default balancing), that
  Machine computes `hash(game_id)` (or looks it up — see the hybrid
  below) and, if it isn't the owner, responds `fly-replay:
  instance=<machine-id>` to bounce the request to the right one — this
  happens before the WS upgrade completes, so it works for `/ws` too.
- **Rebalancing / instance-loss story**: this is the real weakness of
  naive `hash(game_id) % N`. Add or remove a shard machine and the modulus
  changes for nearly every game, silently routing in-flight games to a
  machine that's never heard of them (their state is on the *old* owner,
  which local-SQLite durability doesn't fix — recovery only replays a
  crash on the *same* machine/volume, per `DURABILITY.md`; it doesn't
  migrate a game to a *different* machine). **Minimal fix, still no
  kernel/architecture change**: don't recompute the mapping from a live
  machine count — pin it once. At `/create`, write one row (`game_id ->
  machine_id`) to a tiny directory (this is the "(c) hybrid" below); every
  router decision becomes a lookup, not a hash recompute, so adding
  capacity only affects *new* games, and draining a machine means "stop
  giving it new games, wait for its existing ones to finish, then take it
  down" — a graceful drain, not a silent remap. If a Machine dies outright,
  Fly's `restart.policy = "always"` restarts it **in place, same Machine
  ID, same attached Volume** — which is precisely the `crash_test.sh`
  scenario (`kill -9`, restart against the same `--db`) `DURABILITY.md`
  already proves recovers correctly. A full **host**-level failure (not
  just the process) is the one gap Fly Volumes don't cover on their own —
  Fly's own docs are explicit that a volume "exists on one server in a
  single region" and isn't automatically replicated
  ([Volumes overview](https://fly.io/docs/volumes/overview/)) — mitigated
  by adding **Litestream** (continuous SQLite WAL streaming to
  S3-compatible object storage, a Fly-authored tool, integrates with Fly's
  Tigris object storage) purely as an out-of-band backup sidecar — no
  application change, since it reads the same WAL file `persist.c` already
  writes ([Litestream: Revamped](https://fly.io/blog/litestream-revamped/),
  [LiteFS backup docs](https://fly.io/docs/litefs/backup/)).
- **On Hetzner/bare IaaS**: the LB's own sticky-session cookie is
  IP/cookie-based, not `game_id`-aware, so the same routing layer has to
  be hand-built — a small Envoy/Nginx+Lua/Go proxy doing the identical
  `hash(game_id)`-or-directory lookup, then proxying the TCP connection
  (or HTTP request, pre-WS-upgrade) to the right backend VM. More to
  build than "call `fly-replay`," but the same idea, and well-understood
  (this is exactly how self-hosted game-server fleets have solved this
  for years).
- **Minimal code changes this path needs** (none of it touches the
  kernel `c/src/*`, matching every prior stage's constraint): (1) a tiny
  directory service (could be as small as a SQLite/Postgres table or a
  Redis hash, `game_id -> machine_id`, written once at `/create`); (2) a
  routing shim — either a `fly-replay`-emitting check added to
  `foolish_server.c`'s existing request dispatch (it already parses
  `game_id` off every request, so this reuses that, not new plumbing) or
  a standalone reverse proxy in front on bare IaaS; (3) nothing in
  `persist.c`, `ws.c`, or the kernel changes at all — every instance keeps
  being exactly today's single-process design.

### (b) Externalize state to a shared store (Redis/Postgres) — stateless instances

This is the rewrite that lets any instance answer any request (true
Cloud-Run-style elastic autoscaling, no pinning, no drain-on-deploy
story). It directly contradicts the stated design goal quoted in
`README.md` from `ARCHITECTURE_AS_A_PATTERN.md`: "the server is
in-memory authoritative state... no marshal in/out, no DB read on the hot
path." Moving the `Game` struct's authority into Redis/Postgres means
every `/action`, `/ws` frame, and bot tick now pays a network round trip
to the shared store where today it's a `memcpy` under a per-game mutex —
a materially different latency/throughput profile, not a deployment
tweak.

**When it's worth it**: only when a single machine's RAM genuinely can't
hold the working set, or when the ops requirement is "any instance can
die anytime with zero drain and zero pinned routing." Given the measured
footprint (low hundreds of KB per WS connection, Stage 6 numbers above),
even the top of the stated range — 10k concurrent games, optimistically
~20k live WS connections at ~2 humans/game — extrapolates to roughly
**5-8 GB RAM total**, comfortably shardable across a handful of
Fly-Machine-sized boxes without ever touching this path. **Not
recommended now**; flag it as the option if concurrent-game counts grow
an order of magnitude or more beyond this, or if elastic (not
pre-provisioned) autoscaling becomes a hard requirement.

### (c) Hybrid — what's actually recommended

Almost entirely (a): keep RAM + local SQLite exactly as today, on every
shard machine, unmodified. The **only** new shared state is the thin
`game_id -> machine_id` directory described above — not game state, not
even a cache of it, just an assignment written once per game at creation
and read on every subsequent routing decision. This is what turns naive
"(a) with mod-N hashing" (fragile on resize) into something that
rebalances safely: new shard capacity only ever receives *new* games;
existing games keep their pinned machine until they end, and a machine
can then be drained and retired without breaking anything in flight. It
is the smallest change that fixes (a)'s real weakness without paying for
(b)'s full rewrite — and it's the plan this doc recommends pairing with
Fly.io Machines + `fly-replay`.

## 4. Transactional email (email verification)

A C server's simplest integration with any of these is **one HTTP POST
via libcurl** (`curl_easy_setopt(CURLOPT_URL/CURLOPT_POSTFIELDS/
CURLOPT_HTTPHEADER)`, `curl_easy_perform`) against the provider's JSON
API, run off the request-handling hot path — the same "don't block the
socket-handling thread on a slow external call" discipline `persist.c`
already applies to disk I/O, e.g. a dedicated `email_thread` fed by a
small queue, mirroring `persist_thread`'s shape. **libcurl also speaks
SMTP directly** (`curl/lib/smtp.c`,
[smtp-tls.c example](https://curl.se/libcurl/c/smtp-tls.html)) if a
provider is SMTP-only, but for a provider with an HTTP API, the HTTP path
is less code: no MIME envelope construction, no SMTP AUTH/STARTTLS
handshake to reason about, just a JSON body and a Bearer/API-key header —
and it gives a structured JSON response (message ID, rejection reason)
instead of an SMTP response code to parse.

| Provider | Free tier | Paid entry | API | Deliverability posture |
|---|---|---|---|---|
| **AWS SES** | 3,000 msgs/mo for 12 months (new accounts); **starts in sandbox — can only send to verified addresses until you request production access**, a real gotcha for a verification-email feature whose whole point is emailing *unverified* addresses | $0.10/1,000 emails — cheapest at scale by a wide margin | HTTP API (SendEmail/SendRawEmail) or SMTP interface | Good once warmed up and out of sandbox; you own reputation/warm-up, no built-in inbox-placement guardrails |
| **SendGrid** | 60-day trial at 100/day only — **not a permanent free tier anymore** | $19.95/mo for 50k | HTTP API + SMTP | Solid, high-volume-oriented |
| **Postmark** | 100/mo, permanent, no expiry | Restructured 2026: Basic $15/mo / Pro $16.50/mo / Platform $18/mo, all include 10,000 emails/mo | HTTP API + SMTP | **Best-in-class for transactional specifically** — Postmark refuses bulk/marketing traffic by policy, keeping its sending IP pool's reputation clean; this is the one differentiator that matters most for a verification email, which is worthless if it lands in spam |
| **Resend** | 3,000/mo free | Pro $20/mo for 50k | Modern HTTP API + SMTP relay | Good, newer platform, strong developer experience |
| **Mailgun** | 100/day free | Basic $15/mo for 10k | HTTP API + SMTP, EU region option | Flexible, less differentiated on transactional-specific reputation |

**Recommendation: Postmark.** For a feature whose entire job is "make
sure this one email reaches the inbox so the user can click a link,"
Postmark's transactional-only policy and reputation management is the
right trade for a small amount of money (email-verification volume is
one email per signup, not per game — at the top of the stated 1-10k
concurrent-games range this is nowhere near 10k emails/month unless
signups are also in the thousands/month, so the $15/mo Basic tier likely
covers it with room to spare, and the free 100/mo tier covers
development/staging entirely). **Alternative: Resend**, if the extra free
volume (3,000/mo, no card required) or its more modern API matters more
than Postmark's deliverability specialization at this stage — reasonable,
just a different risk trade. AWS SES is the move *if and when* volume
grows enough that $0.10/1,000 matters, but budget for the sandbox→
production-access request (and the associated wait/review) as a real
setup step, not a checkbox.

## 5. Cost + ops sketch, top 2 picks

Both sized for the stated **1k-10k concurrent games** range. Using the
measured Stage 6 footprint (~51 MB/160 conns, ~109 MB/400 conns — call it
roughly 250-300 KB/connection past a small fixed floor) and assuming
~2 human WS connections/game on average: ~1,000 concurrent games ≈ 2,000
connections ≈ **~1 GB** RAM; ~10,000 concurrent games ≈ 20,000 connections
≈ **~5-8 GB** RAM. This is an extrapolation from `SERVER_SCALING.md`'s
numbers, not a new benchmark — flagged as such.

### Fly.io Machines (primary)

- **1k games**: one `shared-cpu-2x`/2GB Machine (~$13.94/mo — [Fly
  pricing via search summary](https://fly.io/docs/about/pricing/)) + a
  5-10GB Volume for the SQLite file ($0.15/GB/mo → ~$0.75-1.50/mo) +
  negligible bandwidth (packed binary state frames are small; inbound is
  free, outbound ~$0.02/GB in NA/EU). **Roughly $15-20/month.**
- **10k games**: split across ~4-6 shard Machines (2GB each, per §3) for
  headroom and blast-radius reasons, not just raw RAM: ~4-6 × $14/mo ≈
  **$56-84/mo** compute + ~$6-9/mo volumes + modest bandwidth. **Roughly
  $70-120/month** — cheap for the stated scale, and each shard's
  blast radius on failure is 1/N of the games instead of all of them.
- **Autoscaling**: Fly Machines start/stop in ~sub-second, but for a
  *stateful* shard "autoscaling" mostly means "provision more shards
  ahead of the game-count curve," not react-to-load elasticity — a
  scaled-in shard would strand its in-memory games, so treat shard count
  as a capacity-planning knob, not an autoscaler input, consistent with
  §3's drain-don't-recycle model.
- **Zero-downtime deploys**: Fly's rolling/canary/bluegreen strategies
  hold traffic on the old Machine until a health-checked replacement is
  up ([Zero Downtime Deploys](https://fly.io/zero-downtime-deploys), [Seamless
  Deployments blueprint](https://fly.io/docs/blueprints/seamless-deployments/))
  — but that's liveness, not "no active games." For this sharded design,
  a real zero-*impact* deploy still needs the app-level drain from §3(c)
  (stop assigning new games to a shard, wait for its existing ones to
  finish, then roll it) — Fly gives the deploy mechanics, not the
  game-aware drain, which is on us either way.
- **Observability**: Fly ships managed, no-extra-charge Prometheus
  (VictoriaMetrics-backed) + Grafana with built-in per-Machine CPU/mem/
  network metrics, and will scrape a custom `/metrics` endpoint if the
  app exposes one ([Metrics on Fly.io](https://fly.io/docs/monitoring/metrics/),
  [Observability for User Apps](https://fly.io/docs/blueprints/observability-for-user-apps/)).
  The server already has `/stats` (bot/octogen decision counters,
  Stage 4) — wiring that (and per-shard connection/game counts) into a
  Prometheus-format endpoint is a small addition, not a platform problem;
  README already names "structured observability" as the one thing not
  attempted yet.
- **TLS + email**: TLS is free/automatic (§2); Postmark's HTTP API is a
  libcurl call from an off-hot-path thread, platform-agnostic — no
  Fly-specific integration needed.

### Hetzner Cloud VMs + Load Balancer (alternative)

- **1k games**: one CX-series VM sized similarly (a few GB RAM, a couple
  vCPUs) at roughly **€10-20/month** (entry CX23 is €5.49/mo but under-
  provisioned for headroom; a step up is closer to this range — see
  [Hetzner review/pricing roundup](https://betterstack.com/community/guides/web-servers/hetzner-cloud-review/))
  + a Load Balancer for TLS termination at **€4.90/mo flat** (20TB
  included traffic) ([Hetzner LB
  pricing](https://www.hetzner.com/cloud/load-balancer/)). **Roughly
  $15-25/month equivalent** — competitive with Fly, sometimes cheaper,
  at the cost of owning more of the stack yourself.
- **10k games**: several VMs behind either the Hetzner LB (cookie/IP
  sticky, not game_id-aware — fine only if you also run the hand-built
  routing shim from §3 in front) or a hand-built proxy VM doing the
  `hash(game_id)`-or-directory routing itself. Compute cost scales
  similarly to Fly's per-shard math; the LB is a flat €4.90/mo regardless
  of shard count, arguably cheaper than N Fly Machines' worth of bandwidth
  at large N, but you're now also the one patching OS packages, rotating
  SSH access, and building the deploy pipeline.
- **Autoscaling**: none built in — provision ahead of the curve, same
  capacity-planning posture as Fly, but manually (or via your own
  Terraform/Ansible).
- **Zero-downtime deploys**: build it yourself — typically blue-green
  (stand up new VMs, health-check, swap the LB's target list, drain and
  terminate old ones) or a systemd-managed binary swap with the same
  game-aware drain from §3(c) layered on top, since Hetzner gives no
  platform-level equivalent of Fly's rolling-deploy health-check gating.
- **Observability**: nothing built-in — stand up your own (Prometheus +
  Grafana on a small VM, or a hosted SaaS) scraping the same `/stats`-
  style endpoint recommended above. More setup than Fly's managed
  offering, full control in exchange.
- **TLS + email**: TLS via the Hetzner LB's termination feature (or
  Caddy/ACME on a fronting box) — same "app runs plaintext, edge holds
  the cert" shape as §2. Email integration is identical to the Fly case
  (libcurl → Postmark), fully platform-independent.

## Summary

**Deploy on Fly.io Machines**, one per game-shard, each running today's
unmodified `foolish_server` plaintext (no `--tls`) with a Fly Volume for
its local SQLite file. Route by `game_id` using Fly's `fly-replay`/
instance-routing primitives backed by a **small `game_id -> machine_id`
directory** written once at `/create` (the §3(c) hybrid) — this keeps the
current in-memory-RAM-plus-local-SQLite architecture completely intact
(no kernel change, no `persist.c` change) while fixing plain
`hash(game_id) % N`'s fragility when shards are added or drained. Add
Litestream as an out-of-band backup sidecar against true host loss (Fly
Volumes are single-copy per Fly's own docs). Terminate TLS at Fly's
Anycast edge (automatic Let's Encrypt, TLS 1.2/1.3) rather than the
server's own `--tls`, preserving the Stage 6 epoll path. Send
verification email via **Postmark**'s HTTP API over a libcurl POST from
a dedicated, off-hot-path thread (mirroring `persist_thread`'s shape),
chosen for its transactional-only deliverability posture; Resend is the
reasonable alternative if its larger free tier matters more at this
stage. **Alternative platform: Hetzner Cloud VMs + Hetzner Load
Balancer** — same architecture, cheaper raw compute, materially more
self-owned ops (TLS cert lifecycle, deploy orchestration, observability
stack, routing shim), a good fit if the team already runs Linux boxes and
wants to avoid Fly's opinions. Cloud Run, ECS/Fargate, Railway, and
Render were each considered and demoted or ruled out for stated,
specific reasons in §1 — not omitted, just not the best fit for a
stateful, sharded, in-memory C server at this scale.
