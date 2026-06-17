# Local-emulated server stress harness

Hammers a single game with rapid, overlapping calls against a **real Postgres**
instance running the **real `commit_game` CAS RPC** + the **real move handlers** +
a faithful replica of the **fire-and-forget broadcast path**, then audits durable
card state and per-client animation-sequence ordering.

See `FINDINGS.md` for results (TL;DR: the CAS keeps card state perfectly correct;
the live animation stream reorders under realistic Realtime latency).

## Setup (one-time, per fresh container)

```bash
# Postgres 16 (client + server) — installable from the Ubuntu mirror
apt-get install -y postgresql postgresql-contrib
service postgresql start

# role + db the harness expects
sudo -u postgres psql -c "CREATE ROLE stress WITH LOGIN SUPERUSER PASSWORD 'stress';"
sudo -u postgres psql -c "CREATE DATABASE foolish OWNER stress;"

# schema + the real CAS / lease RPCs
PGPASSWORD=stress psql -h 127.0.0.1 -U stress -d foolish -f tests/stress/schema.sql

npm install pg --no-save
```

> Note: a full `supabase start` is the "proper" emulation, but it pulls its image
> stack from CloudFront/ghcr, which this environment's network policy blocks
> (403 on the layer CDN). A local Postgres + the verbatim CAS plpgsql tests the
> exact concurrency primitive that matters, without the unreachable images.

## Run

```bash
npm run test:stress                              # defaults: 8 games, 2H+1B
npx tsx tests/stress/stress.ts 10 --humans=3 --bots=1 --delay=8 --blatency=120
```

Flags:

| flag | meaning | default |
| --- | --- | --- |
| `<n>` (positional) | number of games | 8 |
| `--humans=N` | human players | 2 |
| `--bots=N` | bot players (driven by a concurrent lease loop) | 1 |
| `--delay=ms` | injected load→commit delay (widens the CAS race window) | 6 |
| `--blatency=ms` | modelled Realtime broadcast delivery latency, uniform 0..N | 120 |
| `--steps=N` | max human bursts per game | 4000 |

## What it asserts

- **Card conservation**: `deck + flipped + hands + table + discard == 36` (≤4
  players) and no duplicate live card — checked after every step against the
  durably-committed DB state.
- **Broadcast ordering**: per client, the committed version of delivered
  sequences must be monotonically non-decreasing in arrival order. Regressions =
  visible rubber-banding.
