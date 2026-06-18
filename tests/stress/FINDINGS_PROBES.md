# Five subsystem probes — results

Deliberately aimed at subsystems the broadcast/client work didn't touch. Each
question was turned into an experiment (`probe.ts`, plus the deterministic
`cover_repro.ts`). **4 of 5 came back clean; 1 is a real bug** — and the probe
that found it was looking elsewhere, so it's a bonus on top of the 5.

Run: `npx tsx tests/stress/probe.ts` and `npx tsx tests/stress/cover_repro.ts`.

| # | subsystem | question | verdict |
| --- | --- | --- | --- |
| Q1 | bot lease | exactly-one driver, TTL recovery, stale-token fencing? | ✅ no bug |
| Q2 | CAS liveness | legit moves dropped by retry exhaustion? | ✅ none at realistic contention (note below) |
| Q3 | endgame | elimination_order / rankings ever malformed? | ✅ no bug |
| Q4 | `pass` integrity | rules-legal pass hitting an impossible-state throw? | ✅ pass is clean — **but found a `cover` bug** |
| Q5 | leakage | public payloads expose hidden hand cards? | ✅ no bug |

## Q4 → BUG: `cover` validation/execution matching mismatch (`SEVERE` 500)

`validateCover` checks each named attack card is on the table **by value only**:

```ts
// cover.ts:32
if (!game.table_battles.some(b => b.attack.value === card.value && b.defense === null)) throw ...
```

but `executeCover` then locates it **by exact suit+value**:

```ts
// cover.ts:72
const i = game.table_battles.findIndex(b => card_comp(b.attack, attack_card) && b.defense === null);
if (i === -1) throw new Error('SEVERE: Card not found on table');
```

When two **same-rank** attacks are on the table (e.g. 7♠ and 7♥ — normal Durak
throw-ins) and the *specific* card the defender names is already covered,
validation passes (the *other* 7 satisfies the value check) and execution can't
find the exact card → the uncaught `SEVERE: Card not found on table`, which
surfaces to the user as a 500-class error / failed move.

**Reachable in production** without any harness: the defender double-taps "cover"
on one of two same-rank attacks. The first tap covers 7♠; the second reloads a
state where 7♠ is covered but 7♥ is still uncovered → mismatch → `SEVERE`. The
stress run hit it **271 times** across 50 contended games, and `cover_repro.ts`
reproduces it **deterministically with no concurrency**:

```
after first cover, table: 0:6/def 0:7  1:6
Q4 repro: threw -> "SEVERE: Card not found on table"
CONFIRMED BUG
```

This is distinct from the broadcast findings — it's a server-side
validate/execute contract bug, not a delivery issue, and it's *not* protected by
the CAS (the throw happens inside the operation, before commit).

### Fix
Make the two agree. Either validate by exact card —

```ts
if (!game.table_battles.some(b => card_comp(b.attack, card) && b.defense === null)) throw ...
```

— or have `executeCover` treat a not-found card as a graceful rejection
(re-throw a normal "card is not on the table" 400) instead of `SEVERE`. The first
is preferable: it makes the validator actually validate what the executor does.

## Q1 → no bug: bot lease is sound

- 30 concurrent `try_acquire_bot_lease` → **exactly 1** winner (mutual exclusion).
- A dead driver (no renew) is recovered after the TTL expires; a live lease
  blocks acquisition meanwhile.
- `renew_bot_lease` correctly **fences a stale token** (returns false once another
  driver has taken over), so an old isolate can't extend a lease it no longer
  holds. No double-driver, no permanent wedge.

## Q2 → no bug at realistic contention (one note)

Across 50 full 3-player games (~20k committed moves, ~7k benign stale rejections),
**0** moves were dropped by the 5-attempt CAS exhaustion. The earlier `stress.ts`
runs showed it *can* happen (≈1 in tens of thousands) only at artificially high
injected compute delay. So it isn't a bug today, but the failure mode is real: an
exhausted retry surfaces as a spurious 400 on a legitimate move. If contention
ever rises (more attackers, slower compute), a small exponential backoff between
attempts or a higher attempt cap would remove the user-visible failure. Logged as
a watch-item, not a bug.

## Q3 → no bug: endgame accounting is well-formed

Over 50 games played to completion: `elimination_order` had **no duplicates** and
length exactly `players − 1`, exactly **one** fool each game, and
`calculateGameRankings` returned a full, duplicate-free ranking every time. (The
defensive de-dup in `calculateGameRankings` never had to fire — good to know it's
belt-and-suspenders, not load-bearing.) ELO inputs are sound.

## Q5 → no bug: no hidden-hand leakage

14k events run through the real `convertToPublicAnimationEvents` + `gameToPublicGame`:
the public `game_state` never carries a hand array (players are reduced to
`hand_length` via `other_player`), and the only events that move genuinely hidden
cards — `DEAL` and `REFILL` (cards drawn into a hand) — are reduced to card-backs.
Everything else (`attack_pass`, `cover`, `pickup`, `discard`) reveals cards that
are physically face-up on the table, which is correct, not a leak.

> Honesty note: the first draft of the Q5 detector flagged ~7.5k "leaks" — all of
> them `pickup` cards. That was a false positive: those cards were public on the
> table the instant before pickup. The corrected detector checks the two real
> leak surfaces (unsanitized DEAL/REFILL, and hand arrays in the public state) and
> reports zero.
