# Branch consolidation plan

Step-by-step recipe to merge every outstanding branch into one verified
integration branch. **This exact sequence was executed end-to-end in a trial
on 2026-07-04 and finished green** (cnitro tests 14/14, arena fingerprint
bit-identical, e2e 74/74, validation 35/35, bots.wasm builds), so follow it
literally; every conflict you will hit is listed with its resolution. Run
all commands from the repo root unless a `cd` is shown.

## Branch inventory (what exists and why)

| Branch | State | Action |
|---|---|---|
| `claude/c-wasm-single-source-of-truth` | C kernel is the rules AND bot engine (wasm), TS strategies retired to `offlinefun/localtest/frozen/`, racing + resident-state perf work. 11 commits, biggest change. | **Base of the integration.** |
| `claude/e2e-test-harness-glitches-xwab9q` | 20 commits: server perf (logs committed inside `commit_game`, ELO overlap), 4 migrations, ServerContext split, animation aim fix, dead-table cleanup. | Merge 2nd. 1 conflict. |
| `claude/project-analysis-roadmap-mkjnka` | Leaderboard + match-history/replay-gallery UI, 2 migrations, roadmap doc, screenshots. | Merge 3rd. Clean. |
| `claude/lobby-bot-addition-bug-i4x2vk` | Lobby add-bot race fix + jsdom e2e test (changes `package.json`). | Merge 4th. Clean; needs `npm install`. |
| `claude/monetization-roadmap-algsai` | One docs file. | Merge 5th. Clean. |
| `claude/bot-beat-cordite-6xb8d3` | 36 commits: NEW BOTS — semtex (beats cordite), octogen, torpex (negative-result value net), oracles, docs. Built against pre-wasm main, so its TS-layer changes are obsolete. | Merge 6th (LAST). 5 conflicts + mandatory post-merge fixes. |
| `claude/funny-bohr-vwdo9m` | 2 unmerged commits: fern-texture welcome title styling (~50 lines). | Optional cherry-pick at the end. |
| `claude/lobby-pick-bot`, `httpsend-broadcast`, `skip-create-broadcast`, `fold-apis-into-meta`, `fix-double-animation`, `replay-codec-e2e-test`, `supabase-local-stress-test-e6snsa` | Content already in main (verified with `git cherry`: 0 unmerged patches each). | Delete after consolidation. |
| `claude/game-replay-qr-url-lwcb0m` | **No merge base with main** (disconnected pre-history). Unmergeable, superseded. | Delete (tag `archive/game-replay-qr` first if sentimental). |

Merge-order rationale: the c-wasm branch redefined the bot architecture, so
it must be the base everything adapts TO. The bots branch goes last because
its integration is partly semantic (registering new C bots through the wasm
path), which is easier once everything else is settled.

## Step 0 — setup

```bash
git fetch --all --prune
git checkout -b integration origin/claude/c-wasm-single-source-of-truth
sudo service postgresql start   # e2e needs it (role `stress`, db `foolish` already exist)
```

Baseline check before touching anything (both must hold; if not, STOP):

```bash
cd cnitro && make clean && make CC=clang -j4 all && make CC=clang tests   # expect: 14 passed, 0 failed
./build/cnitro_eval 2>/dev/null | tail -1    # expect exactly:    2        1.300     1.500     70.0%   140 60
cd ..
```

## Step 1 — merge e2e-test-harness-glitches (1 conflict)

```bash
git merge --no-ff origin/claude/e2e-test-harness-glitches-xwab9q
```

**Conflict: `supabase/functions/_shared/actions/cover.ts`.** Their change
removes a dead `skipBroadcast` parameter from the OLD TypeScript cover
implementation; our branch replaced that whole implementation with the C
kernel bridge (`kernelCover`). The kernel version wins:

```bash
git checkout --ours supabase/functions/_shared/actions/cover.ts
git add -A && git commit --no-edit
```

**Verify** (gate A): `npm run test:e2e` → all pass (their branch adds tests,
count grows); `npm run test:validate` → 35/35. Their migrations
(`supabase/migrations/2026*`) apply via the e2e schema automatically — a
validation failure here means a migration clash; inspect
`supabase/migrations` ordering before proceeding.

## Step 2 — merge the UI branch (clean)

```bash
git merge --no-ff --no-edit origin/claude/project-analysis-roadmap-mkjnka
```

No conflicts (README merges textually). **Verify** (gate B): `npm run build`
(Next.js build must succeed — this branch is UI + 2 migrations), then run
locally so the owner can see it: `npm run dev` → check the leaderboard page
and match-history/replay gallery render.

## Step 3 — merge the lobby fix (clean, needs npm install)

```bash
git merge --no-ff --no-edit origin/claude/lobby-bot-addition-bug-i4x2vk
npm install    # it adds jsdom + an --experimental-test-module-mocks flag to test:e2e
```

**Verify**: `npm run test:e2e` — must be run via the npm script (the new
`e2e/lobby_add_bot.test.ts` needs the module-mocks flag the script now
carries; invoking node directly will fail with
`mock.module is not a function`, which is NOT a code bug).

## Step 4 — merge the monetization doc (clean)

```bash
git merge --no-ff --no-edit origin/claude/monetization-roadmap-algsai
```

No verification needed (docs only).

## Step 5 — merge bot-beat-cordite (5 conflicts + mandatory post-merge fixes)

```bash
git merge --no-ff origin/claude/bot-beat-cordite-6xb8d3
```

### 5a. The five conflicts

1. **`cnitro/src/strategy.h`** — both sides claimed STRAT ids 10-14. Take
   OUR hunks everywhere (ids 10-17 stay simple_heuristic…distilled), then
   APPEND their strategies RENUMBERED **18-22**, in all three places:
   - defines: `STRAT_SEMTEX 18`, `STRAT_SEMTEX_ORACLE 19`, `STRAT_OCTOGEN
     20`, `STRAT_OCTOGEN_ORACLE 21`, `STRAT_TORPEX 22` (keep their
     comments). Renumbering is safe: all their code uses the symbolic names
     (verified — zero hardcoded ids).
   - prototypes: append their five `*_strategy_choose` prototypes after
     `distilled_strategy_choose`.
   - `parse_strategy`: append five lines mapping
     `semtex/sx, semtex_oracle/sxo, octogen/og, octogen_oracle/ogo,
     torpex/tx` to the new ids, after the `distilled` line.
2. **`cnitro/Makefile`** — union. `CORE_SRC`: keep ours AND append their
   four files (`semtex_strategy.c octogen_strategy.c torpex_strategy.c
   torpex_value.c`, keeping the backslash line-continuations valid). Keep
   BOTH the `build/cnitro_distill` and their `build/cnitro_gen` targets
   (each needs its own `$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)` recipe line).
3. **`cnitro/src/main_eval.c`** — union: keep both dispatch case blocks
   (ours STRAT_SIMPLE_HEURISTIC…DISTILLED and theirs SEMTEX…TORPEX).
4. **`supabase/functions/_shared/bot_strategy.ts`**
   — take OURS wholesale (`git checkout --ours`). Their version imports TS
   strategy classes that no longer exist. Their bots get registered in 5c.
5. **`cnitro/README.md`** — take OURS (`git checkout --ours`); optionally
   hand-merge their semtex/octogen doc paragraph into the bots list later.

### 5b. Silent-auto-merge traps (MUST fix before committing)

- `offlinefun/localtest/frozen/cordite_core.ts`: git's rename detection
  applied their 130-line semtex change TO THE FROZEN ORACLE. Revert it:
  `git checkout HEAD -- offlinefun/localtest/frozen/cordite_core.ts`
- `supabase/functions/_shared/strategies/semtex_strategy.ts`: their new TS
  strategy recreates the retired pattern; the C implementation is
  canonical. `git rm -f supabase/functions/_shared/strategies/semtex_strategy.ts`
- `supabase/seed.sql`: auto-merged, KEEP — it seeds `semtex`, `semtex_max`,
  `octogen`, `octogen_max` bot rows; 5c must register all four keys or those
  bots silently fall back to random.

### 5c. Wire the new bots through the wasm path (this is the real port)

1. `cnitro/wasm/wasm_bots_api.c`, in `wasm_choose_move`'s switch, after the
   `STRAT_HANDWRITTEN_PROD` case:
   `case STRAT_SEMTEX: fn = semtex_strategy_choose; break;` and
   `case STRAT_OCTOGEN: fn = octogen_strategy_choose; break;`
   (torpex/oracles stay native-arena-only, like `distilled`).
2. `cnitro/Makefile` `WASM_BOT_SRC`: add `src/semtex_strategy.c` and
   `src/octogen_strategy.c` (NOT torpex/torpex_value).
3. `supabase/functions/_shared/wasm/bots.ts`, `STRAT` map: add
   `semtex: 18,` and `octogen: 20,`.
4. `supabase/functions/_shared/bot_strategy.ts` registry, after fulminate:
   ```ts
   ['semtex', new WasmBotStrategy('semtex', STRAT.semtex, { logs: true })],
   ['octogen', new WasmBotStrategy('octogen', STRAT.octogen, { logs: true })],
   ['semtex_max', new WasmBotStrategy('semtex_max', STRAT.semtex, { logs: true })],
   ['octogen_max', new WasmBotStrategy('octogen_max', STRAT.octogen, { logs: true })],
   ```
   (their C brains are self-budgeted — no env knobs; `_max` aliases the base
   until a kernel-side max-budget knob exists — leave a TODO comment.)
5. Commit the merge, then rebuild everything:
   ```bash
   git add -A && git commit --no-edit
   cd cnitro && make clean && make CC=clang -j4 all && make CC=clang tests
   rm -f build/*.wasm && make CC=clang wasm wasm-bots   # regenerates both embeds
   git add -A && git commit -m "rebuild wasm embeds with semtex/octogen"
   cd ..
   ```

### 5d. Verify (gate C — all must pass)

```bash
cd cnitro
./build/cnitro_eval 2>/dev/null | tail -1          # still exactly: 2  1.300  1.500  70.0%  140 60
./build/cnitro_eval --strategy=semtex --opp=cordite --players=2 --games=30 2>/dev/null | tail -1
                                                    # sanity: semtex ≥ ~50% (trial measured 66.7%)
make CC=clang build/sim_difftest && ./build/sim_difftest 4 300   # 0 real divergences
cd .. && npm run test:e2e && npm run test:validate  # trial result: 74/74 and 35/35
```

## Step 6 — optional: funny-bohr styling

```bash
git cherry-pick 81ae0fa 9298947   # fern welcome-title texture; conflicts unlikely (Welcome.tsx, typography.css)
```

If Welcome.tsx conflicts, prefer the cherry-picked side for the title markup
and keep integration's side for anything else. Verify visually in `npm run dev`.

## Step 7 — local run for owner review

```bash
sudo service postgresql start
npm install && npm run dev     # UI: check lobby (Add Bot before roster loads), leaderboard, replay gallery
npm run test:e2e && npm run test:validate
```

Bots to eyeball in a live game: add `semtex` and `octogen` bots (they're in
the seeded roster) and confirm they play at normal speed (they run in
bots.wasm like cordite; a "bot instantly plays first legal move" symptom
means the wasm dispatch case from 5c-1 is missing).

## Step 8 — cleanup

After the integration branch is reviewed and merged to main:

```bash
for b in lobby-pick-bot httpsend-broadcast skip-create-broadcast fold-apis-into-meta \
         fix-double-animation replay-codec-e2e-test supabase-local-stress-test-e6snsa; do
  git push origin --delete claude/$b; done
git tag archive/game-replay-qr origin/claude/game-replay-qr-url-lwcb0m && git push origin archive/game-replay-qr
git push origin --delete claude/game-replay-qr-url-lwcb0m
# and, once merged: the six source branches of this consolidation.
```

## Rollback and rules of engagement

- Every step is a separate merge commit: `git reset --hard <last-good-merge>`
  undoes exactly one step. Never `--force` push over `integration`.
- If a verification gate fails, STOP at that step and report; do not proceed
  and do not "fix forward" past a red gate.
- The three sacred invariants that must survive every step:
  1. arena fingerprint `2  1.300  1.500  70.0%  140 60` (deterministic;
     any change means bot behavior drifted),
  2. `e2e/bot_parity.test.ts` passing (kernel bots == frozen TS oracles),
  3. `offlinefun/localtest/frozen/**` byte-identical to the pre-merge state
     (`git diff origin/claude/c-wasm-single-source-of-truth -- offlinefun/localtest/frozen/` empty).
