# Adversarial fuzzer — and the card-duplication exploit it found

`e2e/fuzz.test.ts` plays the hacker: it fires malformed and rule-breaking action
requests through the REAL server validation+execution path (the same
`verify_player_in_game` + handler dispatch as `functions/action/index.ts`) under
the REAL CAS commit, and after EVERY attempt asserts the hard safety invariant —
**card conservation holds; no input ever duplicates or loses a card.**

## Exploit found (now fixed): duplicate a card by naming it twice

The "no duplicate cards" guard in `attack` / `pass` / `cover` was:

```ts
if (new Set(cards).size !== cards.length) throw 'duplicates';
```

`cards` is an array of `{suit,value}` **objects**, so `new Set` dedupes by
reference, not value. A client sending the same card twice as two distinct
objects — `cards: [{suit:2,value:7},{suit:2,value:7}]` — passes the check. Then
`executeAttack` removes the card from the hand once (value match) but pushes
**both** onto the table → the card now exists twice:

```
attack cards:[{suit:2,value:7},{suit:2,value:7}] -> total=37/36  DUP[2:7x2]
```

The fuzzer reproduced it deterministically at iteration 69 (seed `0x1234abcd`).
It's a real exploit: a malicious or buggy client can mint duplicate cards (and,
via the same hole in `cover`/`pass`, duplicate covers/pass cards), corrupting the
36-card invariant.

### Fix
Dedupe by value in all four checks (`attack.cards`, `pass.cards`,
`cover.cover_cards`, `cover.attack_cards`):

```ts
if (new Set(cards.map(c => `${c.suit}-${c.value}`)).size !== cards.length) throw 'duplicates';
```

Legal multi-card plays (e.g. attacking with `6♠` + `6♥`) are unaffected — those
are distinct by value-key. Regression test: a same-card-twice move is now rejected
and the durable state is untouched.

## What else the fuzzer throws

Across ~1900 hostile requests per run it fires: duplicate cards, forged cards not
in any hand, out-of-range / wrong-typed cards (string/object/number), wrong-role
moves (defender attacks, attacker covers), non-member `player_id`, empty/null/huge
payloads, mismatched array lengths, and SQL-injection / prototype-pollution
strings in `player_id` and `type`.

Results after the fix: **0 card-conservation violations**, every illegal input
rejected, and the process survives every payload. ~98/run are "crash-class"
errors (a `TypeError` deep in a handler from a wrong-typed payload, e.g. `cards`
as a string) — these are **caught** (in production by `wrap400`'s catch-all → a
400, never a process crash) and never corrupt state or duplicate a card, so
they're a message-quality smell, not an exploit. The SQL-injection strings are
inert: the harness adapter (like PostgREST) uses parameterized queries.

Run: `npm run test:e2e` (or a single seed: `FUZZ_SEED=0xdeadbeef FUZZ_ITERS=5000
TSX_TSCONFIG_PATH=e2e/tsconfig.json node --import tsx --test e2e/fuzz.test.ts`).
