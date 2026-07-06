# Adversarial review: the C/WASM kernel boundary

An attacker-mindset pass over the newest, memory-unsafe attack surface: the
freestanding C kernel (`cnitro`, compiled to `rules.wasm` + `bots.wasm`) that
is now the single source of truth for rules AND bot play. The kernel has no
libc bounds checking, so anything that reaches it from the TypeScript
marshaling layer must be assumed hostile and handled without crashing,
corrupting memory, or hanging.

## Threat model

The reachable boundary is `marshalGame(Game)` → the kernel's `get_state()`.
In production the `Game` comes from the trusted DB, so these are
**defense-in-depth** hardenings, not a live remote-exploit fix: the TS
validation layer remains the gate that *rejects* illegal moves. But the
kernel is the source of truth; a bug upstream, a corrupt DB row, a future
feature, or bot self-play on a malformed state must never turn into memory
corruption. "The engine defends itself regardless of its caller."

## Findings (all fixed)

| # | Class | Where | Trigger | Impact |
|---|---|---|---|---|
| 1 | **DoS / unbounded hang** | `legal.c` attack + pass enumerators | a hand with many same-value cards (only reachable via a corrupt state — real hands hold no duplicates) | `combinations_attack`/`_pass` explored ~2^n subsets. The `MAX_LEGAL_MOVES` cap only stopped `push_move`, not the recursion, and when defender capacity rejects the combos at the leaf, moves never accumulate to trip even that. Wall-clock hang → the whole edge-function request wedges. |
| 2 | **OOB stack read/write** | `legal.c` `seen[16]` / `table_values[16]` | a card `value` outside 1..13 (e.g. 99) | the value indexed a 16-entry stack array out of bounds. |
| 3 | **Heap/struct buffer overflow** | `wasm_api.c` `get_state` | `num_players`/`hand_count`/`deck_count`/`num_battles`/`num_eliminated` above array capacity | the count was used directly as a loop bound writing into a fixed array — e.g. `hand_count=100` overflows `hand[64]` into the next player's struct. |
| 4 | **Undefined shift** | `cordite_sim.c` `1ull << card_id(card)` (+ `buf[MAX_MOVE_CARDS]` in `calc_pass_moves`) | a card whose `suit`/`value` makes `card_id` outside 0..51 | `1 << id` with `id ≥ 64` or negative is UB; a `k > 40` pass combo overran a stack buffer. |

## Fixes (all off the hot path — measured zero perf impact)

- **`get_state` clamps every count to its array capacity** and **sanitizes
  every card** to (suit 0..3, value 1..ACE_VALUE) via `clamp_card`. Identity
  on all real states (fingerprint bit-identical); runs once per marshal, not
  in the rollout, which clones the already-imported game.
- **The attack/pass enumerators bound `k` by defender/next-player capacity**
  (and `MAX_MOVE_CARDS`), so they never recurse into combos that
  `emit_*` would reject anyway. **Same emitted move set** — the doomed
  recursion is the only thing removed. Plus the `MAX_LEGAL_MOVES` early-exit
  is now mirrored into `combinations_attack`/`_pass` (belt and suspenders for
  a corrupt-but-large defender hand).
- **The enumerator value-array indices are range-guarded** (finding 2).
- The card sanitization at the boundary (finding 4) means every downstream
  `card_id` is a valid 0..51 index, so the bot bitboards are safe with no
  guards added to the rollout inner loop.

## Verification

- `e2e/wasm_kernel_fuzz.test.ts` — permanent regression: handpicked
  overflow/OOB/DoS cases + ~1500 randomized wild games through the rules
  path, plus malformed games through every bot (including cordite/fulminate's
  `1<<card_id` belief build). Asserts: no wasm trap, bounded move count,
  in-range enumerated cards, and no >2s hang.
- Ad-hoc adversarial harness (scratch): 25 handpicked cases + ~14,000
  malformed-game bot decisions — 0 crashes, 0 corruption, 0 hangs after the
  fixes (multiple hard hangs before them).
- Unchanged elsewhere: cnitro tests 14/14, arena fingerprint bit-identical
  (`2 1.300 1.500 70.0% 140 60`), sim/apply/solver difftests clean, bot
  parity 7/7, e2e 72/72, validation 35/35.
- Performance: stash A/B on `bots.wasm`, controlled same-trajectory cordite
  bench — before ~1736 µs/decision median, after ~1735; native all-cordite
  eval 5.49s both. No measurable change.

## Residual notes

- The clamps make a *malformed* game play as a bounded, nonsense-but-safe
  game; correctness of rejection stays with the TS validation layer (that is
  by design — the kernel's job here is memory safety, not policy).
- Raw linear-memory injection (writing `g_io` bytes directly, bypassing
  `marshalGame`) is out of scope: an attacker with arbitrary write access to
  the module's memory has already escaped the sandbox.
