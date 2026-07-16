# Branch consolidation plan (v2)

Step-by-step recipe to merge every outstanding branch into one verified
integration branch. **This exact sequence was executed end-to-end in a trial
worktree and finished green** — clean build, cnitro tests 14/14, arena
fingerprint bit-identical, bots.wasm (with semtex/octogen) builds, and the
parity + kernel-fuzz + adversarial suites pass (22/22). Follow it literally;
every conflict you will hit is listed with its resolution. Run all commands
from the repo root unless a `cd` is shown.

> **What changed since v1:** the base is now `claude/adversarial-hardening`
> (it already contains all the c-wasm single-source-of-truth work PLUS the
> security hardening and both plan/security docs). And a new bot branch,
> `claude/novichok-cheat-eval`, **supersedes** `bot-beat-cordite` — it
> contains every semtex/octogen/torpex commit and adds the novichok cheating
> research bot on top. So we merge novichok instead of bot-beat-cordite, and
> bot-beat-cordite becomes deletable.

## Branch inventory

| Branch | State | Action |
|---|---|---|
| `claude/adversarial-hardening` | c-wasm single-source-of-truth (C kernel = rules + bots via WASM, retired TS strategies frozen, racing + resident-state perf) **plus** the WASM-boundary + TS-layer security hardening + `docs/CONSOLIDATION_PLAN.md` + `docs/SECURITY_WASM_BOUNDARY.md`. | **Base of the integration.** |
| `claude/e2e-test-harness-glitches-xwab9q` | 20 commits: server perf (logs inside `commit_game`, ELO overlap), 4 migrations, ServerContext split, animation aim fix, dead-table cleanup. | Merge 2nd. 1 conflict. |
| `claude/project-analysis-roadmap-mkjnka` | Leaderboard + match-history/replay-gallery UI, 2 migrations, screenshots. | Merge 3rd. Clean. |
| `claude/lobby-bot-addition-bug-i4x2vk` | Lobby add-bot race fix + jsdom e2e test (changes `package.json`). | Merge 4th. Clean; needs `npm install`. |
| `claude/monetization-roadmap-algsai` | One docs file. | Merge 5th. Clean. |
| `claude/novichok-cheat-eval` | 44 commits: NEW BOTS — semtex (beats cordite, ships), octogen (ships), torpex (negative-result value net, arena-only), oracles (arena-only), **novichok** (CHEATING bot that reads real hands — arena/eval ONLY, must never ship). Built against pre-wasm main, so its TS-layer changes are obsolete. Superset of `bot-beat-cordite`. | Merge 6th (LAST). 5 conflicts + post-merge wiring. |
| `claude/funny-bohr-vwdo9m` | 2 unmerged commits: fern-texture welcome-title styling (~50 lines). | Optional cherry-pick at the end. |
| `claude/bot-beat-cordite-6xb8d3` | Fully contained in `novichok-cheat-eval`. | **Delete** (superseded). |
| `lobby-pick-bot`, `httpsend-broadcast`, `skip-create-broadcast`, `fold-apis-into-meta`, `fix-double-animation`, `replay-codec-e2e-test`, `supabase-local-stress-test-e6snsa` | Already in main (`git cherry` shows 0 unmerged patches each). | Delete after consolidation. |
| `claude/game-replay-qr-url-lwcb0m` | No merge base with main (disconnected pre-history), superseded. | Delete (tag `archive/game-replay-qr` first if sentimental). |
| `claude/c-wasm-single-source-of-truth` | Fully contained in `adversarial-hardening` (it's the base minus the 2 security commits). | Delete after the integration branch merges to main. |

Merge-order rationale unchanged: the base redefined the bot + rules
architecture, so everything else adapts TO it; the heavy bot branch goes last
because its integration is partly semantic (registering new C bots through
the wasm path).

## Step 0 — setup

```bash
git fetch --all --prune
git checkout -b integration origin/claude/adversarial-hardening
sudo service postgresql start   # e2e needs it (role `stress`, db `foolish` already exist)
```

Baseline check before touching anything (all three must hold; if not, STOP):

```bash
cd c && make clean && make CC=clang -j4 all && make CC=clang tests   # 14 passed, 0 failed
./build/cnitro_eval 2>/dev/null | tail -1        # exactly:  2  1.300  1.500  70.0%  140 60
cd .. && npm install && npm run test:e2e         # all pass (80 on this base)
```

## Step 1 — merge e2e-test-harness-glitches (1 conflict)

```bash
git merge --no-ff origin/claude/e2e-test-harness-glitches-xwab9q
```

**Conflict: `server/api/common/actions/cover.ts`.** Their change
removes a dead `skipBroadcast` param from the OLD TypeScript cover
implementation; our base replaced that whole file with the C-kernel bridge
(`kernelCover`). The kernel version wins:

```bash
git checkout --ours server/api/common/actions/cover.ts
git add -A && git commit --no-edit
```

**Verify (gate A):** `npm run test:e2e` (count grows — their branch adds
tests) and `npm run test:validate` → 35/35. A validation failure here means
a migration clash — inspect `server/impls/supabase/migrations` ordering before continuing.

## Step 2 — merge the UI branch (clean)

```bash
git merge --no-ff --no-edit origin/claude/project-analysis-roadmap-mkjnka
```

**Verify (gate B):** `npm run build` (Next.js build must pass), then
`npm run dev` and eyeball the leaderboard page and match-history/replay
gallery.

## Step 3 — merge the lobby fix (clean, needs npm install)

```bash
git merge --no-ff --no-edit origin/claude/lobby-bot-addition-bug-i4x2vk
npm install    # adds jsdom + an --experimental-test-module-mocks flag to test:e2e
```

**Verify:** run `npm run test:e2e` (via the npm script — the new
`e2e/lobby_add_bot.test.ts` needs the module-mocks flag the script carries;
invoking node directly fails with `mock.module is not a function`, which is
NOT a code bug).

## Step 4 — merge the monetization doc (clean)

```bash
git merge --no-ff --no-edit origin/claude/monetization-roadmap-algsai
```

No verification needed (docs only).

## Step 5 — merge novichok-cheat-eval (5 conflicts + wiring)

```bash
git merge --no-ff origin/claude/novichok-cheat-eval
```

### 5a. The five conflicts (identical pattern to v1, plus novichok's id)

1. **`c/src/strategy.h`** — both sides claim ids 10-14/17. Take OUR
   hunks everywhere (ids 10-17 stay simple_heuristic…distilled), then APPEND
   novichok's strategies RENUMBERED **18-23** in all three places
   (safe — all their code uses the symbolic names, verified no hardcoded ids):
   - defines: `STRAT_SEMTEX 18`, `STRAT_SEMTEX_ORACLE 19`, `STRAT_OCTOGEN
     20`, `STRAT_OCTOGEN_ORACLE 21`, `STRAT_TORPEX 22`, `STRAT_NOVICHOK 23`.
   - prototypes: append `semtex/semtex_oracle/octogen/octogen_oracle/torpex/
     novichok`'s six `*_strategy_choose` after `distilled_strategy_choose`.
   - `parse_strategy`: append six name→id lines
     (`semtex/sx, semtex_oracle/sxo, octogen/og, octogen_oracle/ogo,
     torpex/tx, novichok/nv`) after the `distilled` line.
2. **`c/Makefile`** — union. `CORE_SRC`: keep ours AND append their five
   files (`semtex_strategy.c octogen_strategy.c torpex_strategy.c
   torpex_value.c novichok_strategy.c`, keeping backslash continuations
   valid). Keep BOTH the `build/cnitro_distill` and their `build/cnitro_gen`
   targets (each needs its own `$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)` line).
3. **`c/src/main_eval.c`** — union: keep both dispatch case blocks (ours
   SIMPLE_HEURISTIC…DISTILLED and theirs SEMTEX…NOVICHOK).
4. **`server/api/common/bot_strategy.ts`** — take OURS
   (`git checkout --ours`). Their version imports TS strategy classes that no
   longer exist; the new bots get registered in 5c.
5. **`c/README.md`** — take OURS (`git checkout --ours`); optionally
   hand-merge their semtex/octogen doc paragraph later.

> **`c/src/cordite_sim.c` does NOT conflict** even though both the base
> (security hardening) and novichok touch it — git merges the non-overlapping
> hunks. VERIFIED: it builds clean and the arena fingerprint stays
> bit-identical. Don't hand-edit it.

### 5b. Silent-auto-merge traps (fix before committing — same as v1)

- `offlinefun/localtest/frozen/cordite_core.ts`: rename detection applied
  novichok's semtex change to the FROZEN oracle. Revert:
  `git checkout HEAD -- offlinefun/localtest/frozen/cordite_core.ts`
- `server/api/common/strategies/semtex_strategy.ts`: their new TS
  strategy recreates the retired pattern; the C implementation is canonical.
  `git rm -f server/api/common/strategies/semtex_strategy.ts`
- `server/impls/supabase/seed.sql`: auto-merged, KEEP — seeds `semtex/semtex_max/octogen/
  octogen_max` rows; 5c must register those four keys.

### 5c. Wire the new bots through the wasm path

**Ship only semtex + octogen. Novichok, torpex, and the oracles stay
native-arena-only** — novichok is a CHEATING bot (it reads opponents' real
hands); it must never be reachable as a playable production bot. torpex is a
measured-negative value net; oracles are research budgets.

1. `c/wasm/wasm_bots_api.c`, in `wasm_choose_move`'s switch, after the
   `STRAT_HANDWRITTEN_PROD` case:
   `case STRAT_SEMTEX: fn = semtex_strategy_choose; break;` and
   `case STRAT_OCTOGEN: fn = octogen_strategy_choose; break;`
2. `c/Makefile` `WASM_BOT_SRC` (a SEPARATE list from CORE_SRC — edit BOTH
   or the wasm link fails with `undefined symbol: semtex_strategy_choose`):
   add `src/semtex_strategy.c src/octogen_strategy.c` (NOT novichok/torpex).
3. `sdk/ts/wasm/bots.ts`, `STRAT` map: add
   `semtex: 18,` and `octogen: 20,`.
4. `server/api/common/bot_strategy.ts` registry, after fulminate:
   ```ts
   ['semtex', new WasmBotStrategy('semtex', STRAT.semtex, { logs: true })],
   ['octogen', new WasmBotStrategy('octogen', STRAT.octogen, { logs: true })],
   ['semtex_max', new WasmBotStrategy('semtex_max', STRAT.semtex, { logs: true })],
   ['octogen_max', new WasmBotStrategy('octogen_max', STRAT.octogen, { logs: true })],
   ```
   (self-budgeted C brains — no env knobs; `_max` aliases the base until a
   kernel-side max-budget knob exists — leave a TODO.)
5. Commit the merge, then rebuild everything:
   ```bash
   git add -A && git commit --no-edit
   cd c && make clean && make CC=clang -j4 all && make CC=clang tests
   rm -f build/*.wasm && make CC=clang wasm wasm-bots   # regenerates both embeds
   git add -A && git commit -m "rebuild wasm embeds with semtex/octogen"
   cd ..
   ```

### 5d. Verify (gate C — all must pass)

```bash
cd c
./build/cnitro_eval 2>/dev/null | tail -1          # still exactly: 2  1.300  1.500  70.0%  140 60
./build/cnitro_eval --strategy=semtex   --opp=cordite --players=2 --games=20 2>/dev/null | tail -1
./build/cnitro_eval --strategy=novichok --opp=cordite --players=2 --games=20 2>/dev/null | tail -1
                                                    # novichok (cheater) should dominate (~75%)
make CC=clang build/sim_difftest && ./build/sim_difftest 4 300    # 0 real divergences
cd .. && npm run test:e2e && npm run test:validate  # all pass
# and confirm the cheater is NOT reachable in production:
grep -R "novichok" sdk/ts/wasm/bots.ts server/api/common/bot_strategy.ts   # expect NO hits
```

## Step 6 — optional: funny-bohr styling

```bash
git cherry-pick 81ae0fa 9298947   # fern welcome-title texture (Welcome.tsx, typography.css)
```

If `Welcome.tsx` conflicts, prefer the cherry-picked side for the title
markup and keep integration's side for everything else. Verify in `npm run dev`.

## Step 7 — local run for owner review

```bash
sudo service postgresql start
npm install && npm run dev     # UI: lobby (Add Bot before roster loads), leaderboard, replay gallery
npm run test:e2e && npm run test:validate
```

Bots to eyeball live: add `semtex` and `octogen` bots (in the seeded roster)
and confirm they play at normal speed. A "bot instantly plays first legal
move" symptom means the wasm dispatch case from 5c-1 is missing. Confirm no
`novichok`/`torpex` appears in the lobby bot list (they're arena-only).

## Step 8 — cleanup (after the integration branch merges to main)

```bash
for b in bot-beat-cordite-6xb8d3 lobby-pick-bot httpsend-broadcast \
         skip-create-broadcast fold-apis-into-meta fix-double-animation \
         replay-codec-e2e-test supabase-local-stress-test-e6snsa \
         c-wasm-single-source-of-truth adversarial-hardening; do
  git push origin --delete claude/$b; done
git tag archive/game-replay-qr origin/claude/game-replay-qr-url-lwcb0m && git push origin archive/game-replay-qr
git push origin --delete claude/game-replay-qr-url-lwcb0m
# plus the source branches of this consolidation once they're in main.
```

## Rollback and rules of engagement

- Every step is a separate merge commit: `git reset --hard <last-good-merge>`
  undoes exactly one step. Never `--force` push over `integration`.
- If a verification gate fails, STOP at that step and report; do not proceed
  or fix-forward past a red gate.
- Three sacred invariants that must survive every step:
  1. arena fingerprint `2  1.300  1.500  70.0%  140 60` (deterministic; any
     change means bot behavior drifted),
  2. `e2e/bot_parity.test.ts`, `e2e/wasm_kernel_fuzz.test.ts`, and
     `e2e/adversarial_ts_layer.test.ts` all passing (kernel == frozen TS
     oracles; kernel + lobby survive malformed/hostile input),
  3. `offlinefun/localtest/frozen/**` byte-identical to the base
     (`git diff origin/claude/adversarial-hardening -- offlinefun/localtest/frozen/` empty).
- Novichok safety: it is a CHEATING bot (reads real hands). It exists for
  eval curiosity only. It must remain unreachable from `bots.wasm` and the
  TS registry — the grep in gate C enforces this.
