# Foolish-side cutover map: c_base becomes the submodule cbase/

Pinned sources.
Foolish monorepo: origin/main at 48af777d32c1de2ad6e4bf00be092fbdaadda00f.
cbase repo: main at e4ccc7b20114e426b5bf8b1deef599aa9a5ee9eb.
Read only from those commits, never from either working tree.
Per the owner's correction, the submodule mounts at `cbase/` in the foolish repo root and the GitHub repo is alexsave/cbase.
So there is no "keep as-is" category: every `c_base/` path outside c_base/ is repointed to `cbase/`.
Live names do not change: stacks (cbase, cbase-vm, cbase-web, cbase-github), table `cbase`, SSM `/cbase-vm/release/*`, code bucket `cbase-code-<account>-<region>`, binaries `cbase`, `cbase-serve-*`, `cbase-bench`, `cbase-import`, `cbase-export`, env vars `CBASE_*`.
Only the DIRECTORY name `c_base` changes, and the include prefix `"cbase/app.h"` / `"cbase/provider.h"` already says cbase and stays (it resolves through `-I cbase/core/include`).

## 0. Summary table

Counts are files (not lines) from `git grep` on origin/main, excluding c_base/ itself.
"build/code" means at least one non-comment, non-doc line.
`shared/tools/name_gate/allow.txt` (220 lines naming c_base paths) is counted separately in section 5.

| area | files naming c_base/cbase: build/code | comment | doc | files including/building moved shared/c helpers: code | doc |
|---|---|---|---|---|---|
| .github/scripts | 1 | 0 | 0 | 0 | 0 |
| .github/workflows | 3 | 4 | 0 | 1 | 0 |
| chuiniu | 0 | 0 | 0 | 18 | 1 |
| docs | 0 | 0 | 1 | 0 | 5 |
| foolish/(other) | 0 | 2 | 2 | 0 | 0 |
| foolish/c | 1 | 17 | 3 | 51 | 0 |
| foolish/docs | 0 | 0 | 14 | 0 | 0 |
| foolish/e2e | 2 | 14 | 1 | 5 | 0 |
| foolish/foolyard | 0 | 0 | 1 | 1 | 0 |
| foolish/ios | 0 | 9 | 1 | 0 | 0 |
| foolish/rust | 0 | 0 | 1 | 2 | 0 |
| foolish/scripts | 1 | 3 | 0 | 0 | 0 |
| foolish/sdk | 0 | 1 | 1 | 0 | 0 |
| foolish/server | 24 | 34 | 1 | 23 | 0 |
| foolish/src | 0 | 6 | 0 | 0 | 0 |
| foolish/tools | 0 | 2 | 0 | 1 | 0 |
| pickemup | 0 | 0 | 0 | 9 | 3 |
| root docs | 0 | 0 | 4 | 0 | 1 |
| shared/c | 1 | 4 | 1 | 1 | 0 |
| shared/fuzz | 0 | 1 | 0 | 0 | 0 |
| shared/tools | 0 | 0 | 1 | 3 | 0 |
| tallybones | 0 | 0 | 0 | 4 | 0 |
| uttt | 0 | 3 | 1 | 27 | 1 |
| werewolf | 0 | 0 | 4 | 1 | 0 |
| TOTAL | 33 | 100 | 37 | 147 | 11 |


Action counts.
Repoint `c_base/` -> `cbase/`: every path-bearing line in the build/code and workflow rows (36 non-markdown files name a `c_base/` path, the rest of the 171 files are prose or live names).
Repoint `shared/c/X` -> `cbase/lib/X` (and `$(SHARED)/X` in Makefiles): 16 moved helper families, listed in section 2: 192 include sites in 131 files, plus 9 Makefiles that name `$(SHARED)/` or `shared/c/` sources (chuiniu, foolish/c, foolyard, pickemup, tallybones, uttt, werewolf, shared/fuzz, foolish/rust).
Repoint `shared/tools/{structgen,sgcommon}` -> `cbase/tools/...`: about 12 files with code (section 2b).
Build change: foolish/server gets its own Makefile on `cbase.mk` (section 6); `foolish/c/Makefile` COV_FLAGS; `shared/tools/{gen_tools,llvm}.mk`; datagen Makefile.
Workflow changes: 17 workflow files exist; 3 carry real c_base paths (c_base.yml, c_base_fuzz_nightly.yml, web_aws.yml), 14 carry shared/c, shared/tools, shared/scripts or structgen references (section 4).
Doc-only: about 45 files (comments and markdown), batch with a sed.

## Top 5 risks (details in section 7)

1. A `cbase/` submodule makes every `paths: c_base/**` filter match only the gitlink and `git diff`-based selectors (fuzz_select.sh) see no files inside it.
2. Relative includes `../../../shared/c/X.h` in about 150 sites across all five products plus foolyard and rust break when X moves, and `shared_headers_reachable_validation.test.ts` and `one_rng_validation.test.ts` hard-code the old homes.
3. cbase HEAD is not yet clean of product code: Games/Bots console panels, `games` shape in core_json, CBASE_BOT_WORKERS/SPEEDUP/RATED_MIN_AGE, `rows_foolish.h` includes in fuzz/seeds.c and aws bench/fuzz, plus a stale second copy of datagen and lib/wasm/libc.
4. Two diverged copies of the make fragments exist (shared/tools/c_rules.mk, gen_tools.mk, llvm.mk, wasm_cc.mk vs cbase tools/*): C_ROOT default and `$(CB)` semantics differ, and datagen's `../sgcommon` / `../llvm.mk` relative paths break once sgcommon leaves shared/tools.
5. Deploy lane: the stack/OIDC/web templates (`github_oidc.yaml`, `site/web.yaml`, `web_deploy.sh`, `build_bootstrap.sh`) are already gone from cbase HEAD and must land in foolish/server BEFORE the cutover commit, or c_base.yml and web_aws.yml point at nothing.


## 1. Files outside c_base/ that name c_base (generated, grouped by area)

Rules that apply to every row below.
A path of the form `c_base/...` becomes `cbase/...`.
`#include "cbase/app.h"` and `#include "cbase/provider.h"` stay unchanged (include prefix, not a directory path).
Strings such as `cbase-export`, `cbase-bench`, table `cbase`, `CBASE_TABLE`, `CBASE_STORE` are live names and must NOT be touched by a blanket sed of `c_base`.
Lines marked "comment" or "doc" are prose; rewriting them is optional but should be one mechanical sed over `c_base/` -> `cbase/`, never over bare `c_base`.
The `foolish/server/**` rows inherit the move of those 11 tests (listed in section 6) and the new Makefile.

### .github/scripts (1 files)
- `.github/scripts/fuzz_select.sh`: code/build lines 93,99,144,146,151,220,242,246 -> repoint c_base/ to cbase/ where it is a path; leave live names (cbase-*, table cbase, CBASE_*); comment lines 10,14,15,16,17,20,60,69 -> wording/path only

### .github/workflows (7 files)
- `.github/workflows/c_base.yml`: code/build lines 19,25,30,38,49,61,76,78,82,95,97,106,111,127,129,161,188,190,214,215,221,264,268,270,271,286,288,291,302,305,306,308,310,312,314,316,318,320,322,324,326,335,343,346,347,348,350,353,355,356,359,364,367,370,389,396,397,403 -> repoint c_base/ to cbase/ where it is a path; leave live names (cbase-*, table cbase, CBASE_*); comment lines 1,2,3,4,6,12,13,15,17,85,113,115,132,133,134,177,220,252,284,328,332,366 -> wording/path only
- `.github/workflows/c_base_fuzz_nightly.yml`: code/build lines 7,27,43,45,51 -> repoint c_base/ to cbase/ where it is a path; leave live names (cbase-*, table cbase, CBASE_*); comment lines 1,2 -> wording/path only
- `.github/workflows/coverage.yml`: comment lines 8 -> wording/path only
- `.github/workflows/memory.yml`: comment lines 10 -> wording/path only
- `.github/workflows/metrics.yml`: comment lines 6,7 -> wording/path only
- `.github/workflows/product_fuzz_nightly.yml`: comment lines 4,9 -> wording/path only
- `.github/workflows/web_aws.yml`: code/build lines 42,43,51,52,195,198 -> repoint c_base/ to cbase/ where it is a path; leave live names (cbase-*, table cbase, CBASE_*); comment lines 1,3,4,11,19,22,27,28 -> wording/path only

### docs (1 files)
- `docs/ARCHITECTURE_AS_A_PATTERN.md`: doc lines 43,79,104,242,256,305,314,490 -> doc-only repoint

### foolish/(other) (4 files)
- `foolish/.env.production`: comment lines 4 -> wording/path only
- `foolish/README.md`: doc lines 20,55,113,155,158,161,172,241,252,260,262,263,278,279 -> doc-only repoint
- `foolish/ROADMAP.md`: doc lines 4,7 -> doc-only repoint
- `foolish/next.config.mjs`: comment lines 27,41,44,67 -> wording/path only

### foolish/c (21 files)
- `foolish/c/FUSION.md`: doc lines 197,315 -> doc-only repoint
- `foolish/c/Makefile`: code/build lines 1902 -> repoint c_base/ to cbase/ where it is a path; leave live names (cbase-*, table cbase, CBASE_*); comment lines 86,114,120,1098,1141,1208,1510,1523,1654,1874,1993,1994,2079 -> wording/path only
- `foolish/c/README.md`: doc lines 7,21 -> doc-only repoint
- `foolish/c/ios/include/ios_api.h`: comment lines 1450 -> wording/path only
- `foolish/c/src/bot_drive.h`: comment lines 85 -> wording/path only
- `foolish/c/src/fm_exp.h`: comment lines 15 -> wording/path only
- `foolish/c/src/game.c`: comment lines 806 -> wording/path only
- `foolish/c/src/hand_layout.c`: comment lines 15 -> wording/path only
- `foolish/c/src/local_table.h`: comment lines 4 -> wording/path only
- `foolish/c/src/netclient.c`: comment lines 385 -> wording/path only
- `foolish/c/src/netclient.h`: comment lines 220 -> wording/path only
- `foolish/c/src/netwire.h`: comment lines 2,6,9 -> wording/path only
- `foolish/c/src/replay.h`: comment lines 439 -> wording/path only
- `foolish/c/src/snap_room.h`: comment lines 14 -> wording/path only
- `foolish/c/src/table.c`: comment lines 931 -> wording/path only
- `foolish/c/src/thread_heap.h`: comment lines 8 -> wording/path only
- `foolish/c/tests/bench_engine.c`: comment lines 16 -> wording/path only
- `foolish/c/tests/table_host.c`: comment lines 7 -> wording/path only
- `foolish/c/tests/tests.c`: comment lines 1662,14808,21006,22541 -> wording/path only
- `foolish/c/tools/wasm_anatomy/README.md`: doc lines 16 -> doc-only repoint
- `foolish/c/wasm/wasm_api.c`: comment lines 697 -> wording/path only

### foolish/docs (14 files)
- `foolish/docs/ARCHITECTURE.html`: doc lines 144,146,633,721,736 -> doc-only repoint
- `foolish/docs/C_GAME_SHAPE_MIGRATION.md`: doc lines 11 -> doc-only repoint
- `foolish/docs/INFINITE_ORACLE_MODE_B.md`: doc lines 156,159 -> doc-only repoint
- `foolish/docs/IOS_APP_DESIGN.md`: doc lines 11 -> doc-only repoint
- `foolish/docs/NATIVE_SERVER.md`: doc lines 3 -> doc-only repoint
- `foolish/docs/ORACLE_MONETIZATION_ENGINEERING.md`: doc lines 11 -> doc-only repoint
- `foolish/docs/PACKED_WIRE_CUTOVER.md`: doc lines 13 -> doc-only repoint
- `foolish/docs/PLAYER_VIEWS.md`: doc lines 4,6,7 -> doc-only repoint
- `foolish/docs/PODKIDNOY.md`: doc lines 204 -> doc-only repoint
- `foolish/docs/PROTOCOL.md`: doc lines 4,6,7 -> doc-only repoint
- `foolish/docs/SERVER_LIFECYCLE_CONSOLIDATION.md`: doc lines 8 -> doc-only repoint
- `foolish/docs/SUPABASE_SECURITY_AUDIT.md`: doc lines 4,6,7 -> doc-only repoint
- `foolish/docs/WATCHOS_APP_PLAN.md`: doc lines 18 -> doc-only repoint
- `foolish/docs/e2e-findings/README.md`: doc lines 6 -> doc-only repoint

### foolish/e2e (17 files)
- `foolish/e2e/README.md`: doc lines 13,16,17,111 -> doc-only repoint
- `foolish/e2e/ask_wire.test.ts`: comment lines 11 -> wording/path only
- `foolish/e2e/helpers/detid.ts`: comment lines 6 -> wording/path only
- `foolish/e2e/helpers/fake_backend.ts`: comment lines 2 -> wording/path only
- `foolish/e2e/helpers/rng.ts`: comment lines 5 -> wording/path only
- `foolish/e2e/helpers/server_table.ts`: comment lines 3 -> wording/path only
- `foolish/e2e/helpers/table_host.ts`: comment lines 6 -> wording/path only
- `foolish/e2e/lobby_rules_box.test.ts`: comment lines 12 -> wording/path only
- `foolish/e2e/mem/wasm_memory.test.ts`: comment lines 5,175,183 -> wording/path only
- `foolish/e2e/table_no_game_object.test.ts`: comment lines 17 -> wording/path only
- `foolish/e2e/ui_animation_trace.test.ts`: comment lines 175 -> wording/path only
- `foolish/e2e/validation/kernel_gate_validation.test.ts`: comment lines 105,106 -> wording/path only
- `foolish/e2e/validation/layering_validation.test.ts`: comment lines 15 -> wording/path only
- `foolish/e2e/validation/no_json_validation.test.ts`: code/build lines 70,71 -> repoint c_base/ to cbase/ where it is a path; leave live names (cbase-*, table cbase, CBASE_*)
- `foolish/e2e/validation/one_rng_validation.test.ts`: code/build lines 43 -> repoint c_base/ to cbase/ where it is a path; leave live names (cbase-*, table cbase, CBASE_*)
- `foolish/e2e/validation/shared_is_shared_validation.test.ts`: comment lines 58 -> wording/path only
- `foolish/e2e/wasm_web_link.test.ts`: comment lines 9 -> wording/path only

### foolish/foolyard (1 files)
- `foolish/foolyard/SCALE_PROJECTION.md`: doc lines 4,7 -> doc-only repoint

### foolish/ios (10 files)
- `foolish/ios/.gitignore`: comment lines 9 -> wording/path only
- `foolish/ios/Compliance.md`: doc lines 209,210 -> doc-only repoint
- `foolish/ios/Config/Base.xcconfig`: comment lines 3 -> wording/path only
- `foolish/ios/Config/Debug.xcconfig`: comment lines 3 -> wording/path only
- `foolish/ios/FoolishApp/Info.plist`: comment lines 69 -> wording/path only
- `foolish/ios/FoolishApp/PhoneOnly/BotNames.swift`: comment lines 64 -> wording/path only
- `foolish/ios/FoolishApp/SettingsView.swift`: comment lines 82 -> wording/path only
- `foolish/ios/FoolishNet/Backend.swift`: comment lines 1 -> wording/path only
- `foolish/ios/FoolishNet/Stream.swift`: comment lines 1 -> wording/path only
- `foolish/ios/FoolishTests/HostileLinkTests.swift`: comment lines 2 -> wording/path only

### foolish/rust (1 files)
- `foolish/rust/README.md`: doc lines 66 -> doc-only repoint

### foolish/scripts (4 files)
- `foolish/scripts/check_determinism.mjs`: code/build lines 242 -> repoint c_base/ to cbase/ where it is a path; leave live names (cbase-*, table cbase, CBASE_*); comment lines 7 -> wording/path only
- `foolish/scripts/collect_metrics.mjs`: comment lines 20 -> wording/path only
- `foolish/scripts/run_e2e.mjs`: comment lines 8,9 -> wording/path only
- `foolish/scripts/wasm_build.sh`: comment lines 18 -> wording/path only

### foolish/sdk (2 files)
- `foolish/sdk/README.md`: doc lines 45 -> doc-only repoint
- `foolish/sdk/ts/wasm/wasm_asset.ts`: comment lines 15 -> wording/path only

### foolish/server (59 files)
- `foolish/server/bench/cpu.c`: code/build lines 215,312 -> repoint c_base/ to cbase/ where it is a path; leave live names (cbase-*, table cbase, CBASE_*); comment lines 1 -> wording/path only
- `foolish/server/bench/mqtt.c`: code/build lines 222 -> repoint c_base/ to cbase/ where it is a path; leave live names (cbase-*, table cbase, CBASE_*); comment lines 1 -> wording/path only
- `foolish/server/bench/roundtrip.c`: comment lines 1 -> wording/path only
- `foolish/server/bench/spaced.c`: comment lines 1 -> wording/path only
- `foolish/server/fuzz/fz_import.c`: code/build lines 28 -> repoint c_base/ to cbase/ where it is a path; leave live names (cbase-*, table cbase, CBASE_*)
- `foolish/server/fuzz/fz_netwire.c`: comment lines 5 -> wording/path only
- `foolish/server/fuzz/fz_sequence.c`: comment lines 39 -> wording/path only
- `foolish/server/migrate/README.md`: doc lines 1,4,5,7,43,44,45,60,66,68 -> doc-only repoint
- `foolish/server/migrate/export.c`: code/build lines 157,189,190,218,231,237,240,252,259,269,274,283,300,317,343,354 -> repoint c_base/ to cbase/ where it is a path; leave live names (cbase-*, table cbase, CBASE_*); comment lines 1,2,4,5,26 -> wording/path only
- `foolish/server/migrate/export.sh`: comment lines 3 -> wording/path only
- `foolish/server/migrate/import.c`: code/build lines 47,53,54,56,63,73,84,93 -> repoint c_base/ to cbase/ where it is a path; leave live names (cbase-*, table cbase, CBASE_*); comment lines 1,5,6,7,8,32 -> wording/path only
- `foolish/server/sql/schema.sql`: comment lines 1,2 -> wording/path only
- `foolish/server/src/app/admin_panels.c`: comment lines 1 -> wording/path only
- `foolish/server/src/app/admin_panels.h`: comment lines 5 -> wording/path only
- `foolish/server/src/app/bots.h`: comment lines 4,82 -> wording/path only
- `foolish/server/src/app/foolish_app.c`: comment lines 1,77,174 -> wording/path only
- `foolish/server/src/app/foolish_app.h`: comment lines 1,6,12 -> wording/path only
- `foolish/server/src/app/functions_http.h`: comment lines 28 -> wording/path only
- `foolish/server/src/app/pushes.h`: comment lines 26 -> wording/path only
- `foolish/server/src/app/table_ops.c`: code/build lines 45 -> repoint c_base/ to cbase/ where it is a path; leave live names (cbase-*, table cbase, CBASE_*); comment lines 18 -> wording/path only
- `foolish/server/src/app/unfurl_http.h`: comment lines 28 -> wording/path only
- `foolish/server/src/app/v1_http.c`: code/build lines 39 -> repoint c_base/ to cbase/ where it is a path; leave live names (cbase-*, table cbase, CBASE_*); comment lines 35 -> wording/path only
- `foolish/server/src/app/v1_http.h`: comment lines 14,15,23 -> wording/path only
- `foolish/server/src/store/bot_seed.c`: comment lines 15 -> wording/path only
- `foolish/server/src/store/game_id.h`: comment lines 22 -> wording/path only
- `foolish/server/src/store/journal.h`: comment lines 1,2 -> wording/path only
- `foolish/server/src/store/migrate_merge.c`: comment lines 1 -> wording/path only
- `foolish/server/src/store/migrate_merge.h`: comment lines 1 -> wording/path only
- `foolish/server/src/store/migrate_rows.c`: code/build lines 118,385,466 -> repoint c_base/ to cbase/ where it is a path; leave live names (cbase-*, table cbase, CBASE_*)
- `foolish/server/src/store/migrate_rows.h`: comment lines 1 -> wording/path only
- `foolish/server/src/store/rows_foolish.c`: code/build lines 1746 -> repoint c_base/ to cbase/ where it is a path; leave live names (cbase-*, table cbase, CBASE_*)
- `foolish/server/src/store/rows_foolish.h`: comment lines 317,420,421 -> wording/path only
- `foolish/server/src/store/rows_sql.c`: comment lines 1 -> wording/path only
- `foolish/server/src/store/store_import.c`: code/build lines 234,271,299,376,386,403,694 -> repoint c_base/ to cbase/ where it is a path; leave live names (cbase-*, table cbase, CBASE_*); comment lines 2 -> wording/path only
- `foolish/server/src/store/store_import.h`: comment lines 2 -> wording/path only
- `foolish/server/test/gate/product_includes_aws.c`: comment lines 2 -> wording/path only
- `foolish/server/test/lib_store_backend.c`: code/build lines 42,43 -> repoint c_base/ to cbase/ where it is a path; leave live names (cbase-*, table cbase, CBASE_*)
- `foolish/server/test/lib_store_backend.h`: comment lines 5,28 -> wording/path only
- `foolish/server/test/smoke.c`: code/build lines 78,102,1112,1168 -> repoint c_base/ to cbase/ where it is a path; leave live names (cbase-*, table cbase, CBASE_*); comment lines 1,3 -> wording/path only
- `foolish/server/test/smoke_authz.c`: code/build lines 57,518 -> repoint c_base/ to cbase/ where it is a path; leave live names (cbase-*, table cbase, CBASE_*); comment lines 2,4,6,331 -> wording/path only
- `foolish/server/test/smoke_matrix.c`: code/build lines 1380 -> repoint c_base/ to cbase/ where it is a path; leave live names (cbase-*, table cbase, CBASE_*); comment lines 1,3 -> wording/path only
- `foolish/server/test/unit_admin.c`: code/build lines 42,43,113,321,329,339,364 -> repoint c_base/ to cbase/ where it is a path; leave live names (cbase-*, table cbase, CBASE_*); comment lines 5,6 -> wording/path only
- `foolish/server/test/unit_after.c`: comment lines 9 -> wording/path only
- `foolish/server/test/unit_app.c`: comment lines 12,13,1802 -> wording/path only
- `foolish/server/test/unit_bots_kick.c`: comment lines 16 -> wording/path only
- `foolish/server/test/unit_export.c`: code/build lines 146,206,297 -> repoint c_base/ to cbase/ where it is a path; leave live names (cbase-*, table cbase, CBASE_*); comment lines 1,6,12 -> wording/path only
- `foolish/server/test/unit_import.c`: code/build lines 222,290 -> repoint c_base/ to cbase/ where it is a path; leave live names (cbase-*, table cbase, CBASE_*)
- `foolish/server/test/unit_inline_moves.c`: code/build lines 155 -> repoint c_base/ to cbase/ where it is a path; leave live names (cbase-*, table cbase, CBASE_*); comment lines 2,12 -> wording/path only
- `foolish/server/test/unit_journal.c`: code/build lines 1042,1050,1059,1061 -> repoint c_base/ to cbase/ where it is a path; leave live names (cbase-*, table cbase, CBASE_*); comment lines 10,2975 -> wording/path only
- `foolish/server/test/unit_leftovers.c`: code/build lines 47,54,134,140,156,174 -> repoint c_base/ to cbase/ where it is a path; leave live names (cbase-*, table cbase, CBASE_*); comment lines 6 -> wording/path only
- `foolish/server/test/unit_limits.c`: comment lines 5 -> wording/path only
- `foolish/server/test/unit_migrate.c`: comment lines 1 -> wording/path only
- `foolish/server/test/unit_oom.c`: code/build lines 58,415,506 -> repoint c_base/ to cbase/ where it is a path; leave live names (cbase-*, table cbase, CBASE_*); comment lines 17,413 -> wording/path only
- `foolish/server/test/unit_snap_window.c`: comment lines 5,29 -> wording/path only
- `foolish/server/test/unit_store.c`: code/build lines 1895,2629 -> repoint c_base/ to cbase/ where it is a path; leave live names (cbase-*, table cbase, CBASE_*); comment lines 6,244,338,2022 -> wording/path only
- `foolish/server/test/unit_store_faults.c`: code/build lines 44 -> repoint c_base/ to cbase/ where it is a path; leave live names (cbase-*, table cbase, CBASE_*); comment lines 12 -> wording/path only
- `foolish/server/test/unit_unfurl.c`: comment lines 5 -> wording/path only
- `foolish/server/test/unit_v1.c`: code/build lines 77,130 -> repoint c_base/ to cbase/ where it is a path; leave live names (cbase-*, table cbase, CBASE_*); comment lines 8 -> wording/path only
- `foolish/server/tools/unfurl_payloads.c`: comment lines 15 -> wording/path only

### foolish/src (6 files)
- `foolish/src/app/delete-account/page.tsx`: comment lines 7 -> wording/path only
- `foolish/src/common/backend.ts`: comment lines 1,2,182 -> wording/path only
- `foolish/src/common/botName.ts`: comment lines 7 -> wording/path only
- `foolish/src/common/localBackend.ts`: comment lines 3 -> wording/path only
- `foolish/src/env.d.ts`: comment lines 3 -> wording/path only
- `foolish/src/utils/gameShell.ts`: comment lines 2 -> wording/path only

### foolish/tools (2 files)
- `foolish/tools/structgen/gen.sh`: comment lines 302 -> wording/path only
- `foolish/tools/structgen/specs/msg_layout.args`: comment lines 16 -> wording/path only

### root docs (4 files)
- `CBASE_SPEEDUPS.md`: doc lines 1,27,33 -> doc-only repoint
- `README.md`: doc lines 9 -> doc-only repoint
- `VM_FIXES.md`: doc lines 1,8,15,17,18,48,49,80,91 -> doc-only repoint
- `VM_FIXES_HANDOFF.md`: doc lines 3,17,54,71,83,162,185,227,242,249,254,270,271,291,292,293,294,307,308,309,322,328,329,389,393 -> doc-only repoint

### shared/c (6 files)
- `shared/c/MUTATION_CHECKS.md`: doc lines 90 -> doc-only repoint
- `shared/c/hex/hex_test.c`: code/build lines 35 -> repoint c_base/ to cbase/ where it is a path; leave live names (cbase-*, table cbase, CBASE_*)
- `shared/c/num_parse.h`: comment lines 23,24,32 -> wording/path only
- `shared/c/stats/hist.h`: comment lines 16,17 -> wording/path only
- `shared/c/stream_frame.h`: comment lines 129 -> wording/path only
- `shared/c/test/check.h`: comment lines 67 -> wording/path only

### shared/fuzz (1 files)
- `shared/fuzz/Makefile`: comment lines 3,4 -> wording/path only

### shared/tools (1 files)
- `shared/tools/name_gate/words.txt`: doc lines 6 -> doc-only repoint

### uttt (4 files)
- `uttt/web/.gitignore`: comment lines 13 -> wording/path only
- `uttt/web/README.md`: doc lines 27,45,47 -> doc-only repoint
- `uttt/web/lib/shell.ts`: comment lines 2 -> wording/path only
- `uttt/web/next.config.mjs`: comment lines 3,7 -> wording/path only

### werewolf (4 files)
- `werewolf/docs/CODEGEN_ALTERNATIVES.md`: doc lines 4,5 -> doc-only repoint
- `werewolf/docs/IMESSAGE_GAME_DESIGN.md`: doc lines 4,5 -> doc-only repoint
- `werewolf/docs/PLAYER_VIEWS.md`: doc lines 4,5 -> doc-only repoint
- `werewolf/docs/SECURITY_WASM_BOUNDARY.md`: doc lines 4,5 -> doc-only repoint

## 2. Files outside c_base/ that include or build the moving helpers (generated)

Moved helpers present in cbase lib/ at e4ccc7b: b64/b64.{c,h}, be_bytes.h, clock.h, fnv.h, hex/hex.{c,h}, le_bytes.h, mqtt/mqtt.{c,h}, num_parse.h, proc.{c,h}, rng.h, sha256.{c,h}, stats/hist.{c,h}, stats/percentile.h, stream_frame.h, test/check.h, text_util/utf8.h, plus their *_test.c, plus wasm/libc.c and wasm/include/{math,stdio,stdlib,string}.h.
Every one of those files is byte-identical to foolish shared/c at the pinned commits (checked with sha), so there is no drift today.
Repoint rule: `shared/c/X` -> `cbase/lib/X`; relative form `../../../shared/c/X` -> `../../../cbase/lib/X` (same depth, so only the middle segments change); `$(SHARED)/X` -> a new `$(CBLIB)/X` variable (`SHARED := ../../shared/c` is defined once per product Makefile, so each needs a second variable, not a redefinition, because deal_rng, b32, mixrad, crc32c, stats.c, text_util.c, wire_check stay in shared/c).
Sites by header (files): rng 67, fnv 29, sha256 26, check.h 21, le_bytes 20, clock 19, stream_frame 11, hex 7, num_parse 7, be_bytes 4, b64 3, percentile 2, mqtt 1, utf8 1, wasm/libc 5, wasm/include 8.
The wasm/libc and wasm/include rows are NOT repointed to cbase: the decision is that libc returns to foolish next to bump_alloc.c, so those stay at shared/c/wasm (and `lib/wasm` in cbase HEAD is to be deleted by cbase).
`shared/c/test/check.h` has 21 users including uttt, tallybones, pickemup and chuiniu test code, so those products now need the submodule to build their C tests.
Files in shared/c itself (stats/hist_test.c, test/check.h, stream_frame.h, num_parse.h, MUTATION_CHECKS.md, hex_test.c) are the copies being deleted; their rows are informational.
Products that do not move any helper but need the submodule anyway because of shared/c deletions: none, as long as only the 16 families move.

### .github/workflows (1 files)
- `.github/workflows/shared_openssl.yml` [build/include]: 12:sha256, 14:rng, 21:sha256, 23:rng

### chuiniu (19 files)
- `chuiniu/c/README.md` [doc]: 50:test/check
- `chuiniu/c/bot/cn_bot.h` [build/include]: 33:rng
- `chuiniu/c/ios/cn_api_smoke.c` [build/include]: 16:fnv, 18:test/check
- `chuiniu/c/src/cn.c` [build/include]: 6:fnv
- `chuiniu/c/src/cn_dice.c` [build/include]: 24:sha256
- `chuiniu/c/src/cn_roll.c` [build/include]: 22:rng
- `chuiniu/c/src/cn_stage.c` [build/include]: 7:fnv
- `chuiniu/c/src/cn_tex.c` [build/include]: 8:fnv, 9:le_bytes
- `chuiniu/c/tests/cn_check.h` [build/include]: 1:test/check, 8:test/check, 14:rng
- `chuiniu/c/tests/cn_dice_test.c` [build/include]: 18:sha256
- `chuiniu/c/tests/cn_msg_test.c` [build/include]: 19:sha256
- `chuiniu/c/tests/cn_roll_test.c` [build/include]: 23:fnv
- `chuiniu/c/tests/cn_scene_bench.c` [build/include]: 28:clock, 29:stats/percentile
- `chuiniu/c/tests/cn_scene_frame.h` [build/include]: 20:rng, 21:fnv
- `chuiniu/c/tests/cn_stage_test.c` [build/include]: 14:fnv
- `chuiniu/c/tests/cn_tex_test.c` [build/include]: 9:test/check, 10:le_bytes
- `chuiniu/c/tools/cn_icon.c` [build/include]: 35:be_bytes
- `chuiniu/c/tools/cn_names_shot.c` [build/include]: 18:clock
- `chuiniu/c/tools/cn_texgen.c` [build/include]: 42:le_bytes, 43:be_bytes

### docs (5 files)
- `docs/ARCHITECTURE_AS_A_PATTERN.md` [doc]: 672:sha256
- `docs/shared-consolidation/C_PRIMITIVES.md` [doc]: 3:rng, 7:sha256, 8:include, 14:sha256, 26:libc, 27:libc, 52:sha256, 55:sha256, 115:sha256, 133:libc, 134:libc, 142:libc, 145:include, 147:libc, 188:sha256
- `docs/shared-consolidation/SECURITY_REVIEW_LIFTED.md` [doc]: 109:libc
- `docs/shared-consolidation/SECURITY_REVIEW_SHARED.md` [doc]: 14:libc, 58:sha256, 110:libc, 255:libc, 264:libc, 271:libc
- `docs/shared-consolidation/SUMMARY.md` [doc]: 4:rng

### foolish/c (51 files)
- `foolish/c/ios/ios_api_msg.c` [build/include]: 22:sha256
- `foolish/c/ios/ios_goldens.c` [build/include]: 21:fnv
- `foolish/c/src/analyse.c` [build/include]: 16:le_bytes, 31:clock, 33:rng
- `foolish/c/src/ask.c` [build/include]: 3:le_bytes
- `foolish/c/src/astrolite_strategy.c` [build/include]: 60:rng
- `foolish/c/src/blackpowder_strategy.c` [build/include]: 43:rng
- `foolish/c/src/bot_drive.c` [build/include]: 9:rng, 10:fnv
- `foolish/c/src/bot_play.c` [build/include]: 6:rng
- `foolish/c/src/client_table.c` [build/include]: 8:le_bytes
- `foolish/c/src/cordite_strategy.c` [build/include]: 42:rng
- `foolish/c/src/distilled_strategy.c` [build/include]: 47:clock
- `foolish/c/src/evwire.c` [build/include]: 4:le_bytes
- `foolish/c/src/game.c` [build/include]: 15:rng, 16:fnv
- `foolish/c/src/legal.c` [build/include]: 6:le_bytes
- `foolish/c/src/main_distill.c` [build/include]: 29:clock
- `foolish/c/src/main_elo.c` [build/include]: 34:rng, 35:clock
- `foolish/c/src/main_eval.c` [build/include]: 23:fnv, 32:clock, 33:clock
- `foolish/c/src/msg_wire.c` [build/include]: 4:le_bytes, 10:fnv, 12:le_bytes, 16:include
- `foolish/c/src/msg_wire.h` [build/include]: 110:sha256
- `foolish/c/src/netclient.c` [build/include]: 7:stream_frame, 8:fnv, 1177:stream_frame
- `foolish/c/src/netclient.h` [build/include]: 220:stream_frame
- `foolish/c/src/netwire.c` [build/include]: 3:le_bytes
- `foolish/c/src/netwire.h` [build/include]: 47:stream_frame, 82:stream_frame
- `foolish/c/src/octogen_strategy.c` [build/include]: 53:rng
- `foolish/c/src/replay.c` [build/include]: 42:le_bytes
- `foolish/c/src/replay_extras.c` [build/include]: 20:le_bytes
- `foolish/c/src/replay_steps.c` [build/include]: 5:le_bytes
- `foolish/c/src/robusta_strategy.c` [build/include]: 16:rng
- `foolish/c/src/roster.c` [build/include]: 8:text_util/utf8, 25:text_util/utf8
- `foolish/c/src/table.c` [build/include]: 13:le_bytes, 590:le_bytes
- `foolish/c/src/table_wire.c` [build/include]: 7:le_bytes
- `foolish/c/src/view.c` [build/include]: 3:le_bytes, 4:fnv, 277:le_bytes
- `foolish/c/src/winprob.c` [build/include]: 12:le_bytes, 25:clock
- `foolish/c/tests/analyse_test.c` [build/include]: 70:clock
- `foolish/c/tests/bench_engine.c` [build/include]: 27:clock
- `foolish/c/tests/fm_exp_digest.h` [build/include]: 16:rng, 17:fnv
- `foolish/c/tests/hand_contract_test.c` [build/include]: 23:fnv, 24:test/check
- `foolish/c/tests/move_list_test.c` [build/include]: 28:le_bytes, 29:rng
- `foolish/c/tests/msg_flow_sim.c` [build/include]: 47:rng
- `foolish/c/tests/msg_wire_test.c` [build/include]: 33:sha256, 42:rng
- `foolish/c/tests/oracle_rank_test.c` [build/include]: 45:test/check
- `foolish/c/tests/replay_log_oracle.h` [build/include]: 13:le_bytes
- `foolish/c/tests/replay_overlay_test.c` [build/include]: 39:fnv
- `foolish/c/tests/replay_rebuild_difftest.c` [build/include]: 34:rng
- `foolish/c/tests/replay_v6_test.c` [build/include]: 26:rng
- `foolish/c/tests/tests.c` [build/include]: 48:rng, 49:fnv, 10897:rng
- `foolish/c/tools/endgame_retro/retro_seed.h` [build/include]: 12:rng
- `foolish/c/tools/endgame_retro/validate.c` [build/include]: 11:rng
- `foolish/c/tools/leafbook/verify_book.c` [build/include]: 21:rng
- `foolish/c/wasm/include/stdlib.h` [build/include]: 2:include
- `foolish/c/wasm/wasm_api.c` [build/include]: 16:libc

### foolish/e2e (5 files)
- `foolish/e2e/helpers/rng.ts` [build/include]: 25:rng
- `foolish/e2e/oracle_replay.test.ts` [build/include]: 79:rng
- `foolish/e2e/validation/one_rng_validation.test.ts` [build/include]: 1:rng, 38:rng, 80:rng, 117:rng
- `foolish/e2e/validation/shared_headers_reachable_validation.test.ts` [build/include]: 92:le_bytes, 96:num_parse, 131:sha256, 168:sha256, 221:le_bytes
- `foolish/e2e/validation/shared_is_shared_validation.test.ts` [build/include]: 13:sha256, 100:sha256

### foolish/foolyard (1 files)
- `foolish/foolyard/include/rng.h` [build/include]: 5:rng

### foolish/rust (2 files)
- `foolish/rust/Makefile` [build/include]: 26:sha256
- `foolish/rust/bench_common.h` [build/include]: 12:fnv, 13:clock

### foolish/server (23 files)
- `foolish/server/bench/cpu.c` [build/include]: 39:b64/b64, 40:hex/hex, 54:clock
- `foolish/server/bench/mqtt.c` [build/include]: 30:mqtt/mqtt
- `foolish/server/bench/roundtrip.c` [build/include]: 5:stream_frame, 45:rng, 46:clock
- `foolish/server/fuzz/fz_netclient.c` [build/include]: 15:stream_frame
- `foolish/server/migrate/export.c` [build/include]: 53:num_parse
- `foolish/server/src/app/admin_panels.c` [build/include]: 21:hex/hex
- `foolish/server/src/app/foolish_app.c` [build/include]: 24:num_parse
- `foolish/server/src/app/pushes.c` [build/include]: 8:stream_frame
- `foolish/server/src/app/table_ops.c` [build/include]: 24:hex/hex, 28:rng, 43:fnv
- `foolish/server/src/app/v1_http.c` [build/include]: 7:num_parse
- `foolish/server/src/store/journal.c` [build/include]: 15:clock, 17:fnv, 18:num_parse
- `foolish/server/src/store/migrate_rows.c` [build/include]: 16:hex/hex, 17:num_parse
- `foolish/server/src/store/store_import.c` [build/include]: 12:hex/hex, 13:num_parse
- `foolish/server/test/smoke.c` [build/include]: 64:rng, 65:clock, 66:fnv, 298:stream_frame
- `foolish/server/test/smoke_authz.c` [build/include]: 32:stream_frame, 242:stream_frame
- `foolish/server/test/smoke_matrix.c` [build/include]: 39:rng, 49:b64/b64, 51:hex/hex, 70:clock, 408:stream_frame
- `foolish/server/test/unit_app.c` [build/include]: 49:rng, 50:clock
- `foolish/server/test/unit_import.c` [build/include]: 13:hex/hex
- `foolish/server/test/unit_inline_moves.c` [build/include]: 29:rng
- `foolish/server/test/unit_journal.c` [build/include]: 32:fnv, 33:rng
- `foolish/server/test/unit_leftovers.c` [build/include]: 14:b64/b64
- `foolish/server/test/unit_legal_cover.c` [build/include]: 17:fnv, 18:rng
- `foolish/server/test/unit_netclient.c` [build/include]: 18:stream_frame

### foolish/tools (1 files)
- `foolish/tools/structgen/gen.sh` [build/include]: 522:include

### pickemup (12 files)
- `pickemup/c/README.md` [doc]: 43:test/check
- `pickemup/c/ios/pk_api.c` [build/include]: 14:fnv
- `pickemup/c/ios/pk_api_smoke.c` [build/include]: 14:test/check
- `pickemup/c/src/pk.c` [build/include]: 9:fnv
- `pickemup/c/src/pk_belief.c` [build/include]: 7:rng
- `pickemup/c/src/pk_bot.c` [build/include]: 7:rng
- `pickemup/c/tests/pk_bot.h` [build/include]: 12:rng
- `pickemup/c/tests/pk_check.h` [build/include]: 1:test/check, 8:test/check
- `pickemup/c/tests/pk_cross.c` [build/include]: 67:fnv
- `pickemup/c/tests/pk_msg_test.c` [build/include]: 14:sha256
- `pickemup/docs/REUSE_AUDIT.md` [doc]: 125:sha256
- `pickemup/docs/RULES_AND_KERNEL.md` [doc]: 658:sha256, 1623:sha256

### root docs (1 files)
- `VM_FIXES_HANDOFF.md` [doc]: 283:stream_frame

### shared/c (1 files)
- `shared/c/stats/hist_test.c` [build/include]: 115:rng

### shared/tools (3 files)
- `shared/tools/c_rules.mk` [build/include]: 23:sha256, 52:include
- `shared/tools/datagen/test/cli.sh` [build/include]: 271:include
- `shared/tools/wasm_cc.mk` [build/include]: 58:include

### tallybones (4 files)
- `tallybones/c/bot/tb_solve.c` [build/include]: 20:clock
- `tallybones/c/ios/tb_api_smoke.c` [build/include]: 11:test/check
- `tallybones/c/src/tb.c` [build/include]: 4:sha256, 7:fnv
- `tallybones/c/tests/tb_check.h` [build/include]: 1:test/check, 8:test/check, 11:sha256, 14:rng

### uttt (28 files)
- `uttt/c/Makefile` [build/include]: 348:libc, 356:include, 391:include, 398:libc
- `uttt/c/src/uttt_analyse.c` [build/include]: 5:rng
- `uttt/c/src/uttt_big_bot.c` [build/include]: 8:rng
- `uttt/c/src/uttt_big_msg.c` [build/include]: 3:sha256, 4:be_bytes
- `uttt/c/src/uttt_bots.c` [build/include]: 5:rng, 6:fnv
- `uttt/c/src/uttt_msg.c` [build/include]: 3:sha256, 6:be_bytes
- `uttt/c/src/uttt_pen.c` [build/include]: 2:rng
- `uttt/c/tests/uttt_analyse_test.c` [build/include]: 24:sha256, 28:rng
- `uttt/c/tests/uttt_anim_test.c` [build/include]: 13:rng, 16:test/check
- `uttt/c/tests/uttt_big_bot_test.c` [build/include]: 21:test/check, 24:rng
- `uttt/c/tests/uttt_big_chain.c` [build/include]: 33:rng
- `uttt/c/tests/uttt_big_diag_imageio.c` [build/include]: 22:test/check, 27:rng
- `uttt/c/tests/uttt_big_diag_test.c` [build/include]: 28:test/check, 32:rng
- `uttt/c/tests/uttt_big_msg_test.c` [build/include]: 74:sha256, 80:rng, 83:test/check
- `uttt/c/tests/uttt_big_rules_sim.c` [build/include]: 16:rng
- `uttt/c/tests/uttt_big_test.c` [build/include]: 94:rng, 97:test/check
- `uttt/c/tests/uttt_bots_test.c` [build/include]: 19:rng
- `uttt/c/tests/uttt_draw_math_test.c` [build/include]: 14:test/check
- `uttt/c/tests/uttt_ink_test.c` [build/include]: 19:rng, 22:test/check
- `uttt/c/tests/uttt_ladder.c` [build/include]: 23:rng, 24:clock
- `uttt/c/tests/uttt_lang_test.c` [build/include]: 28:rng
- `uttt/c/tests/uttt_line.c` [build/include]: 15:rng
- `uttt/c/tests/uttt_mate.c` [build/include]: 26:rng
- `uttt/c/tests/uttt_msg_test.c` [build/include]: 14:sha256, 19:rng, 22:test/check
- `uttt/c/tests/uttt_test.c` [build/include]: 12:rng, 13:stats/percentile
- `uttt/c/tests/uttt_web_test.c` [build/include]: 28:rng, 31:test/check
- `uttt/c/tools/uttt_play.c` [build/include]: 22:rng
- `uttt/docs/TESTFLIGHT_PLAN.md` [doc]: 89:sha256, 104:sha256, 116:sha256

### werewolf (1 files)
- `werewolf/c/src/ww_wire.h` [build/include]: 50:sha256

## 2b. structgen and sgcommon users, and what stays

structgen and sgcommon move to cbase/tools; datagen stays in shared/tools/datagen and must build against cbase's sgcommon.
cbase HEAD has tools/structgen (identical to shared/tools/structgen at the pinned commits, test/ included), tools/sgcommon (identical), and also a tools/datagen copy (identical) that must be deleted by cbase since datagen stays in foolish.
Users that must repoint (path `shared/tools/structgen` -> `cbase/tools/structgen`, `shared/tools/sgcommon` -> `cbase/tools/sgcommon`):
- `shared/tools/gen_tools.mk:13,21,25` STRUCTGEN_DIR and the `$(STRUCTGEN_DIR)/../sgcommon` and `$(DATAGEN_DIR)/../sgcommon` wildcards and `$(DATAGEN_DIR)/../llvm.mk`: datagen's sgcommon and llvm.mk are no longer siblings, so the second and third must point into cbase explicitly. Build change.
- `shared/tools/llvm.mk:60-61,83` `SGC_SRC := ../sgcommon/sgc.c ...` and `-I../sgcommon`: relative to the including Makefile's directory. Correct for tools/structgen (sibling of sgcommon) but wrong for shared/tools/datagen. datagen/Makefile must override SGC_SRC, SGC_HDR and the -I. Build change.
- `shared/tools/datagen/Makefile:1-6` includes `../llvm.mk` and uses SGC_SRC: repoint to cbase/tools/llvm.mk or keep a foolish llvm.mk (see risk 4).
- `shared/tools/datagen/test/cli.sh` (shared/c/wasm/include at line 271, stays) and `shared/tools/datagen/datagen.c:98,794` comments.
- `foolish/tools/structgen/gen.sh:88`, `foolish/tools/structgen/test/{kotlin,oracle,sizes}.sh` (`sg="$mono/shared/tools/structgen"`): repoint to `$mono/cbase/tools/structgen`. gen.sh:522 also names `$mono/shared/c/wasm/include` (stays).
- `.github/workflows/wasm.yml:191,208-210` (`make -C ../shared/tools/structgen ...`, `bash ../shared/tools/structgen/test/{cli,hash,exports}.sh`): repoint to `../cbase/tools/structgen`. Line 198 and 211 datagen stay.
- `.github/workflows/metrics.yml:91-92` runs `bash pr/foolish/tools/structgen/gen.sh` and the base copy: copies measurement files into a base checkout, so the base checkout also needs `submodules: recursive` and the base's gitlink may predate the move (the base side of a PR that moves the paths will not have cbase/). Flag.
- `.gitignore:88` `shared/tools/structgen/build/` -> `cbase/tools/structgen/build/` (belongs in the submodule's own .gitignore; then drop the line).
- `foolish/e2e/validation/shared_is_shared_validation.test.ts:14-15,106-108` lists `shared/tools/sgcommon/sgc.c`, `shared/tools/structgen/structgen.c`, `.../Makefile` as shared files to guard: the test must drop or retarget them.
- Product Makefiles `foolish/c`, `uttt/c`, `tallybones/c`, `pickemup/c`, `chuiniu/c`, `werewolf/c` reach structgen only through `include ../../shared/tools/gen_tools.mk`, so they need no edit if gen_tools.mk stays and is repointed. If gen_tools.mk is replaced by cbase's, `C_ROOT`/`CB` must be set (cbase's gen_tools.mk takes `CB := ../` style, not `$(C_ROOT)`).
- Comments only: chuiniu/pickemup/tallybones/uttt headers (`cn_api_layout.h`, `pk_api.h`, `tb_api.h`, `uttt_243_web.h`, etc.), `shared/scripts/ci_llvm.sh:50,149`, `shared/c/stream_frame.h:116`, `shared/tools/gen_tools.mk` header.

Files in shared/ that stay in foolish and are NOT in cbase lib/ (do not repoint): b32.{c,h}, collapse/, crc32c/ (stays by decision; used by foolish/server/src/store/journal.c and test/unit_journal.c, and listed in c_base/Makefile SHARED_SRC today so cbase.mk needs it via APP_LIB_SRC), deal_rng.{c,h}, ease.h, i18n/languages.h, mixrad.{c,h}, motion_ruler/, msg_lobby_roster/, msg_seat_tag/, msg_stage/, msg_whole/, sincos.h, stats/stats.{c,h}, stats/seed_hash.h, test/twophone.h, text_util/text_util.{c,h}, util.h, wasm/bump_alloc.c, wasm/libm.c, wire_check/, Makefile, MUTATION_CHECKS.md.
shared/c/wasm/libc.c, wasm/include/*.h and libc_test.c exist in both repos today; by decision they stay in foolish, so cbase's lib/wasm copy is the one to delete.
Foolish users of the libc stay as they are: foolish/c/Makefile:883,1266,1403,1671-1672,1855-1862,1880; chuiniu/c/Makefile:166,172; pickemup/c/Makefile:127,133; `foolish/c/wasm/include/stdlib.h:2`.
But note `c_base/client/Makefile:101,110,121` (moving to cbase/client) builds the client wasm with `$(C_ROOT)/shared/c/wasm/libc.c` and `-isystem .../wasm/include`: with no libc in cbase, the client's wasm build has no libc to link unless cbase keeps its own or the product supplies it. Open question for cbase.

## 3. Reverse dependencies: cbase HEAD still pointing at foolish or product names

Checked with `git grep` at e4ccc7b. Real build or code dependencies first, prose after.

Code and build:
- `fuzz/seeds.c:18` includes `store/rows_foolish.h`; `providers/aws/bench/direct.c:61`, `providers/aws/fuzz/fz_dynamo.c:11`, `providers/aws/fuzz/fz_dynamo_store.c:14` too. rows_foolish.h lives in foolish/server/src/store, so these only build with a foolish APP_INC. They are product code in the platform repo (or the platform's seeds/fuzz/bench need a product hook).
- `providers/local/fuzz/fz_rt_anon.c:18` includes `../../../core/test/lib_client.h`: relative include across trees, fine inside cbase but it is the same pattern the repo gate forbids for products.
- `client/src/client.c:6-7` includes `"stream_frame.h"` and `"rng.h"` with a `// shared/c` comment: resolved by `-I lib`, only the comment is stale.
- `core/src/util/clock.c:4` includes `../../../lib/clock.h` (fine).
- `tools/auth_route_diff.sh:19` and the foolish-side `c_base/tools/hotpath_proof.sh:47` run `git archive "$BASE" c_base shared foolish`: assume the monorepo layout. In cbase this archives paths that do not exist. (hotpath_proof.sh is in cbase tools/ as well.)
- `tools/c_rules_test.sh:10-131` builds a scratch `shared/c` tree: self-contained, but its comments and the `# repo-gate: scratch tree` markers name shared/c.
- `ops/test/deploy_gate_test.sh:38-41,53-66,95-96,150` builds a scratch product repo containing `c_base/`: the fake tree should be `cbase/` now.
- `tools/datagen/` (Makefile, datagen.c, test/*) is in cbase but datagen stays in foolish.
- `lib/wasm/{libc.c,libc_test.c,include/*}` is in cbase but libc returns to foolish.
- `core/src/admin/console.html:281-282,377,552-640`: Games and Bots panels, and "Games needing bots" tile. Product code by decision; not yet removed.
- `core/src/core_json.c:249`, `core_json.h:83,108` (the `games` admin shape) and `core/src/store/store.h:117,189` (`CB_ADMIN_GAMES "games"`): product game shape still in core.
- `core/src/config.c:190,206,208,261,273,276` and `config.h:79,90,92`: CBASE_BOT_WORKERS, CBASE_RATED_MIN_AGE, CBASE_BOT_SPEEDUP still in the platform config; also `core/include/cbase/app.h:42`, `provider.h:28`, `boot.c:66-68`, `ctx.h:72`, `ingress_process.c:14`, and tests `core/test/unit_config.c:115-134`, `unit_edges.c:54-55,154-163`.
- `core/include/cbase/app.h:75` comment `"foolish": the log tag`; `core/src/after.h`, `edge.h`, `ingress_process.c`, `realtime/realtime.h:31`, `realtime/stream_mqtt.h:28` name foolish app.h / netwire.h in comments.
- `providers/aws/src/aws_json.c:1977-1978` and `providers/aws/test/smoke_lambda.c:50`, `core/test/unit_http.c:200`, `unit_net.c:107`, `unit_jwt.c:23` embed foolish.cards hostnames in test strings; `core/test/unit_auth.c` has 51 foolish mentions (strings); `core/src/auth/passwords.c:28` lists "foolish1" etc as common passwords (legit); `auth.c:1` and `auth.h:5` mention foolish.cards in comments.
- `providers/aws/ops/cbase.yaml` (Description/Comment/DisplayName say "c_base", lines 39,120,499,516,619) and `deploy.sh:228` FunctionDescription "c_base (static arm64 bootstrap)": string properties, see risk on CloudFormation below.
- Live-stack material still in cbase HEAD although the decision says it returns to foolish: `providers/local/ops/vm.yaml` (36 hits of cbase-vm/cbase-code/alexsave), `vm_deploy.sh`, `vm_release.sh`, `vm_boot*.sh`, `ops/s3_prune*.sh` (defaults `/cbase-vm/release/serve`, `cbase-code-<account>-<region>`, `CBASE_STACK=cbase`), `providers/aws/ops/{deploy.sh,lambda_deploy.sh,role_covers_release.sh,cbase.yaml}`, `providers/local/ops/VM_RUNBOOK.md`. Already removed from cbase (so already waiting to land in foolish): `providers/aws/ops/github_oidc.yaml`, `site/web.yaml`, `site/web_deploy.sh`, `site/test/web_router.test.mjs`, `ops/test/web_deploy_gate_test.sh`, `providers/aws/ops/build_bootstrap.sh`, `fuzz/test/fuzz_select_test.sh`, `core/test/gate/core_includes_product.c`, `providers/aws/test/golden/dynamo_shapes.txt`, `bench/nano/**` (40 files), and the 9 tests of the 11. Also a tracked stray file `cbase.sqlite.serve-lock` exists in foolish c_base/ and was dropped in cbase.
- Still in cbase though listed to move: `providers/aws/test/unit_conf_store_dynamo.c` and `providers/aws/test/unit_items.c` (2 of the 11 tests).
- `client/swift/Example/project.yml:15,29-31` paths `../../../lib/...` (already repointed, fine); `client/test/ts/socket.test.ts`: fine.
- Docs with the most product references: docs/PLATFORM.md (95), docs/CLIENT_DESIGN.md (85), RUNBOOK.md (75), DESIGN.md (67), docs/VERCEL.md (34): prose, for cbase to triage; `tools/repo_gate_test.sh` already enforces "no foolish/ path" in cbase code.
- `tools/name_gate/allow.txt` differs from foolish's by 460 lines (foolish's names c_base paths); `words.txt` is identical.
- `tools/{c_rules.mk,gen_tools.mk,llvm.mk,wasm_cc.mk,ci_llvm.sh,c_rules_test.sh,lib_tests.mk}` are rewritten forks of foolish's `shared/tools/*` and `shared/scripts/ci_llvm.sh` (differ 4 to 106 lines, see risk 4). They are not identical copies.

## 4. Workflows

All 17 files in .github/workflows plus .github/scripts/fuzz_select.sh.
Checkout: every workflow uses `actions/checkout@v5` with no `submodules:` key.
Any job that (a) builds foolish/server, (b) includes a moved helper (rng.h, fnv.h, sha256.c, check.h, clock.h, le_bytes.h, stream_frame.h, hex, b64, num_parse, utf8, mqtt), or (c) runs structgen needs `submodules: recursive` (or `true`, one level is enough).
Since rng.h, fnv.h, sha256.h and test/check.h are included by every product kernel and test, that is effectively every C-building job in every workflow.
If any deploy job runs in a clone with the default token, a private submodule also needs the checkout `token` to be able to read alexsave/cbase (the default GITHUB_TOKEN only reads the current repo).

### c_base.yml (name c_base): deploy workflow, flagged separately in 4b
- Path filters lines 25-45: `c_base/**` -> `cbase/**` (a submodule bump shows up as a change to path `cbase`, which `cbase/**` does NOT match; add the literal `cbase` entry). `shared/c/**` and `shared/tools/**` stay (they still exist) but are no longer the full input set. `shared/fuzz/**`, `pickemup/c/**` etc. stay.
- Commands: `make -C c_base ...` (13 occurrences) become `make -C foolish/server ...` using the new foolish/server/Makefile (cbase.mk) for build/check/asan/dynamo/coverage; goals `check-asan`, `fuzz-replay`, `check-dynamo`, `test-uring`, `coverage`, `fuzz` must exist in cbase.mk (they do in cbase HEAD cbase.mk).
- `make -C c_base/client wasm` / `ts-test` (line ~140): client is cbase's own build, `make -C cbase/client`; it depends on libc (see 2b) and `bash shared/scripts/ci_llvm.sh` (cbase has `tools/ci_llvm.sh`; foolish keeps its own).
- `fuzz` job `where` step (lines ~190-200): `dir=c_base/client` -> `cbase/client`, `dir=c_base` -> `foolish/server` (or cbase if the target is the platform's own), crash paths `c_base/build/fuzz-crashes` and `c_base/client/build/fuzz-crashes` must follow the build dir. `product:` stays `shared/fuzz`.
- `fuzz_select_test` job runs `bash c_base/fuzz/test/fuzz_select_test.sh`: that test is no longer in cbase (moved out); it belongs with fuzz_select.sh in foolish.
- `coverage` job uploads `c_base/build/cov/report.txt` and `coverage.lcov` and states a floor from `tools/cov_floor.txt`: artifact paths move with the build dir; name `cbase-coverage` is just a label.
- `dynamo` job: `c_base/providers/aws/ops/dynamo_local.sh` and `--stop`, table made from `providers/aws/ops/cbase.yaml`: `cbase/providers/aws/ops/dynamo_local.sh`. The table name in the template is live (`cbase`).
- `ops` job: cfn-lint over `c_base/providers/aws/ops/cbase.yaml`, `.../site/web.yaml`, `.../github_oidc.yaml`, `c_base/providers/local/ops/vm.yaml`; `bash -n` and shellcheck over package.sh, build_bootstrap.sh (gone from cbase), eol_guard.sh, dynamo_local.sh, `site/web_deploy.sh`; test scripts `c_base/ops/test/{web_deploy_gate_test,ops_env_test,s3_prune_test}.sh`, `c_base/providers/local/ops/test/{vm_boot_logs,vm_boot_stub,vm_release_boot}_test.sh`, `c_base/providers/aws/ops/test/{changeset_gate,role_covers_release}_test.sh`, `c_base/fuzz/test/fuzz_mk_test.sh`, `node --test c_base/providers/aws/ops/site/test/web_router.test.mjs`. Each path either becomes `cbase/...` (still shipped by cbase: cbase.yaml, vm.yaml, eol_guard, dynamo_local, ops_env/s3_prune/vm_* tests, changeset_gate, role_covers_release, fuzz_mk_test) or moves into foolish/server (github_oidc.yaml, site/web.yaml, web_deploy.sh, web_router test, web_deploy_gate_test, build_bootstrap). Which side owns vm.yaml, vm_release.sh, deploy.sh is the open question in 3.
- `serve_binary` job: `c_base/providers/aws/ops/build_bootstrap.sh serve` (gone) -> cbase `ops/build_static.sh` with the product root; `cd c_base/dist` artifact paths (`c_base/dist/cbase-serve-linux-arm64`, `.sha256`) follow the new dist dir; `eol_guard.sh` path.
- `deploy` job: `download-artifact path: c_base/dist`, `c_base/providers/local/ops/vm_release.sh c_base/dist/cbase-serve-linux-arm64`. It also contains `git rev-parse --short=9 HEAD` for the release tag (runs in the foolish repo, good; cbase's own vm_release.sh uses `--show-superproject-working-tree`, so a submodule layout resolves to foolish, also good). Secret name `CBASE_AWS_ROLE_ARN` unchanged.
- Checkout needs `submodules: recursive` in all 12 checkout steps; the deploy job's checkout is for vm_release.sh and its main-ancestry gate, which must also have the submodule files.

### c_base_fuzz_nightly.yml
- Lines 27, 43, 45: `make -C c_base print-fuzz-targets|build/fuzz/<t>|fuzz-nightly`; line 51 artifact path `c_base/build/fuzz-crashes/<t>`. Move to the foolish/server Makefile (which lists both sides' fuzz roots: cbase and foolish/server/fuzz). Two checkouts (24, 40) need submodules.

### web_aws.yml: deploy workflow, flagged separately in 4b
- Path filters 42-43 and 51-52: `c_base/providers/aws/ops/site/web_deploy.sh` and `c_base/ops/require_main.sh`. web_deploy.sh moves into foolish (it is gone from cbase); require_main.sh stays in cbase, so the filter becomes `cbase/ops/require_main.sh` plus the literal `cbase`.
- Run lines 195 and 198: `c_base/providers/aws/ops/site/web_deploy.sh site foolish --out foolish/out` and `... site uttt --out uttt/web/out`. The script it calls sources `require_main.sh` and `ops_env.sh` by relative path from the c_base root, which must now be `cbase/ops/`.
- Line 90/129: `bash ../shared/scripts/ci_llvm.sh`, `bash shared/scripts/ci_llvm.sh`, and 93: `bash tools/structgen/gen.sh` (foolish's own gen.sh; it repoints internally to cbase/tools/structgen). Needs `submodules: recursive` on the jobs at lines 78, 119, 177.

### Workflows with shared/c or shared/tools triggers
- shared_openssl.yml lines 12-16, 21-25, 36: filters `shared/c/sha256*`, `shared/c/rng.h`, `shared/c/test/**`, `shared/c/Makefile` and `make -C shared/c openssl-tests`. sha256.c, rng.h, check.h and sha256_openssl_test.c move to cbase/lib, so this workflow's test (shared/c/Makefile rule at lines 31-35 compiles `sha256_openssl_test.c sha256.c rng.h test/check.h`) breaks; cbase already carries `lib/sha256_openssl_test.c`. Move the openssl test lane into cbase (or point the Makefile at `../../cbase/lib`). deal_rng stays.
- uttt-c.yml lines 15, 24: `shared/c/**` stays; add `cbase/lib/**`. Lines 17-18, 26-27, 45: `shared/tools/*.mk`, `shared/tools/c_rules_test.sh`, `bash ../shared/tools/c_rules_test.sh`: stays if foolish keeps c_rules.mk (cbase also has its own tools/c_rules_test.sh).
- werewolf.yml lines 32-33, 39-40: same two filters; line 54 comment about sha256.c from `../../shared/c`.
- chuiniu.yml, pickemup.yml, tallybones.yml: `bash shared/scripts/ci_llvm.sh`, `make -C c structgen datagen`, comments about sha256 from `../../shared/c`; their filters (e.g. pickemup.yml 29-31 lists sha256, b32, mixrad in shared/c, the generators in shared/tools, ci_llvm.sh): add `cbase/lib/**` and `cbase/tools/**`.
- coverage.yml, ios.yml, memory.yml, validate.yml, wasm.yml, foolyard.yml, metrics.yml, product_fuzz_nightly.yml: use `shared/scripts/ci_llvm.sh` (stays), structgen (see 2b), `shared/fuzz` (stays; shared/fuzz/Makefile includes none of the moved files except through fz.h, check), and path filters `shared/c/**` / `shared/tools/**` that need `cbase/**` added so a submodule bump triggers them. foolyard.yml:65 comment notes tools/structgen. wasm.yml:84-87 explains structgen hash portability. All need `submodules: recursive`.
- coverage.yml:8, memory.yml:10, metrics.yml:6-7 are prose about c_base.

### .github/scripts/fuzz_select.sh
- Lines 93, 99, 151, 220, 246: `cb=$root/c_base`, `'c_base/client fuzz client'`, `-v cbrel=c_base`, `make -s -C "$1/c_base" -n -B fuzz`. The selector computes `git diff --name-only base...head`, `git read-tree` of the base into a temp work tree, and nm reachability over fuzz objects; with a submodule it sees only the gitlink path `cbase` in the diff, never `cbase/lib/rng.h` etc. It also reads the base tree via `read-tree -u`, which does not populate submodules. Needs: `git -C cbase diff` of the old and new gitlink SHAs fed in as changed `cbase/<path>` lines, and a recursive checkout for the base tree. Highest-risk script in the cutover.

### 4b. Deploy and live-stack workflows (keep names, zero CloudFormation replacements)
- c_base.yml `deploy` job, `serve_binary` job (creates GitHub releases `cbase-serve-<short sha>`), and web_aws.yml deploy (stack `cbase-web`, OIDC role from stack `cbase-github`, `CBASE_AWS_ROLE_ARN`).
- The OIDC trust is the single repository named by the `GitHubRepo` parameter of github_oidc.yaml (main only, plus an optional `ExtraSubject`). Deploys must keep running in a workflow of the foolish repo (they can), and cbase's own ci.yml/fuzz_nightly.yml must never assume the role. If cbase's repo ever deploys, the trust condition changes and the stack gets an in-place update, not a replacement, but only if the role's logical ID and name are unchanged.
- Template files whose text embeds the old directory name: `providers/aws/ops/cbase.yaml` Description (39), parameter default (120), CloudFront Comment (516), alarm topic DisplayName (619): all in-place updates, none are replacement properties. `deploy.sh:228` FunctionDescription is in-place too. Do NOT change `FunctionName`, `TableName`, bucket names, SSM parameter names or export names while editing; run the existing changeset gate (`providers/aws/ops/test/changeset_gate_test.sh`, `changeset_replacements.jq`) on a real change set before the first deploy from the new layout.
- `vm_release.sh` derives the object key from the short SHA of the superproject (it uses `rev-parse --show-superproject-working-tree`), so the key is `vm/cbase-serve-<foolish sha>` as today. `require_main.sh` fetches origin main of `$tree` (the foolish clone) and checks ancestry: correct only if `$tree` resolves to the foolish repo, not to the cbase submodule. Verify with the deploy_gate_test layout (`cbase/` inside a product).
- `ops_env.sh` default secrets file is `~/.config/cbase/ops.env` (unchanged), the legacy `c_base/ops/.env` becomes `cbase/ops/.env`, inside the submodule where it is ignored.
- The binary name in `vm_release.sh` / artifact names (`cbase-serve-linux-arm64`) must not change, and the CloudFront `/health` and `/ready` checks are unchanged.

## 5. Other non-workflow references (by function)

- `shared/tools/name_gate/allow.txt` (220 lines) lists paths `c_base/...` allowed to hold banned words. cbase has its own copy at tools/name_gate/allow.txt (460 lines different). Foolish's list must repoint to `cbase/...` or be dropped when the gate runs from cbase, and `shared/tools/name_gate/words.txt` (identical to cbase's) must have exactly one owner. `foolish/e2e/validation/shared_is_shared_validation.test.ts:58` reads `words.txt`'s [products] list.
- Root scripts and files: `README.md:9` table row, `VM_FIXES.md`, `VM_FIXES_HANDOFF.md` (25 lines), `CBASE_SPEEDUPS.md`, `docs/ARCHITECTURE_AS_A_PATTERN.md`: prose handoff documents; repoint or leave as history.
- `foolish/README.md` (14 lines), `foolish/docs/*` (14), `foolish/c/README.md`, `foolish/c/FUSION.md:197`, `foolish/e2e/README.md`, `foolish/ROADMAP.md`, `foolish/foolyard/SCALE_PROJECTION.md:7`, `foolish/server/migrate/README.md:43-45`: doc-only, but the README at lines 241-263 names `.github/workflows/c_base.yml`, `c_base/providers/aws/ops/github_oidc.yaml`, `c_base/RUNBOOK.md`, `c_base/DESIGN.md` (relative `../c_base/...`): link repoints.
- `foolish/next.config.mjs:27,41,44,67` and `foolish/.env.production:4`: comments naming `c_base/providers/aws/ops/site/web.yaml`, `web_deploy.sh`, `c_base/docs/VERCEL.md`. If web.yaml and web_deploy.sh move into foolish/server these links change to the new foolish location, not to cbase/.
- Source comments: foolish/c (17 files), foolish/e2e (14), foolish/ios (9), foolish/src (6), foolish/server (34), foolish/scripts (3), foolish/sdk (1), uttt (3): prose only, e.g. `foolish/c/src/netwire.h:9` ("c_base/DESIGN.md Binary surface").
- Scripts with real paths: `foolish/scripts/collect_metrics.mjs:20` (comment about `cbase-bench roundtrip`, a live name), `foolish/scripts/run_e2e.mjs:8-9`, `foolish/scripts/check_determinism.mjs:7,242` (output text mentions `c_base/src/app/table_ops.c`, which is now `foolish/server/src/app/table_ops.c`; fix the text), `foolish/e2e/helpers/{detid,rng}.ts` same stale path.
- Tests that read paths: `foolish/e2e/validation/no_json_validation.test.ts:70-71` has `file: 'c_base/client/swift/Example/Sources/FakeApple.swift'`: a real path, repoint to `cbase/client/...`. `foolish/e2e/validation/one_rng_validation.test.ts:38,43,80,117` has `HOME = {'shared/c/rng.h','shared/c/rng_test.c'}` and a `PRODUCTS` list containing `'c_base'` that it scans: HOME must become `cbase/lib/rng.h`, `cbase/lib/rng_test.c`, `'c_base'` becomes `'cbase'`, and the scan must either descend into the submodule or skip it. `foolish/e2e/validation/shared_headers_reachable_validation.test.ts` (10+ shared/c references, asserts every relative include in the repo lands in `shared/c/`, lists `le_bytes_test.c` and `shared/c/*.c` Makefile wildcards) must be rewritten to accept `cbase/lib/`.
- `foolish/server/test/lib_store_backend.c:43` runs `python3 -I providers/aws/ops/dynamo_table.py providers/aws/ops/cbase.yaml` relative to the cwd (the c_base root). When the tests build from foolish/server, cwd or path must become `cbase/providers/aws/ops/...`.
- `foolish/server/test/gate/product_includes_aws.c` and `core_includes_product.c`: compiled by the compiler gate with product flags; `product_includes_aws.c` comment says `c_base/Makefile's compiler-gate`: cbase.mk keeps the gate (APP_GATE_HEADER).
- `foolish/server/src/app/table_ops.c:45` `static_assert` message names `c_base/Makefile KERNEL_CAPS`, where kernel caps now come from foolish/server's own Makefile.
- iOS/Xcode: no Xcode project, xcconfig or project.yml in foolish names c_base or the moving shared/c files (they only name shared/c/msg_stage, motion_ruler and similar, which stay). `foolish/ios/Config/*.xcconfig`, `Info.plist`, `.gitignore`, Swift sources: comments about the backend only.
- `foolish/sdk/ts/wasm/wasm_asset.ts:15`, `foolish/c/wasm/wasm_api.c:697`, `foolish/c/ios/include/ios_api.h:1450`: comments.
- `foolish/c/Makefile:1902,1993-1994` COV_FLAGS contains `-I../../c_base/core/test`: a real build path to `c_base/core/test/t.h` (the unit test header used by foolish's coverage lane of the server tests). Repoint to `-I../../cbase/core/test`.

## 6. Build wiring that moves

- Today the monorepo `c_base/Makefile` (C_ROOT=..) lists foolish/c as KERNEL_SRC, `foolish/server/{src,test,bench,migrate,tools,fuzz}` as product roots, `-I$(C_ROOT)/shared/c` as SHARED_INC, and `SHARED_SRC` of `shared/c/{b64,hex,crc32c,stats/hist,proc}` (Makefile lines 112-188). The new foolish/server/Makefile must supply: CBASE := ../../cbase, APP_SRC_ROOTS := src, APP_TEST_ROOTS := test, APP_INC := -Isrc -I../c/src, APP_CFLAGS := the kernel's CD_LEAFBOOK and print-human-caps `-DMAX_*` flags, APP_EXTRA_SRC := the kernel sources (read through `make -C ../c print-kernel`), APP_LIB_SRC := `../../shared/c/crc32c/crc32c.c`, APP_LDLIBS for Accelerate on macOS, APP_EMBED for `sql/schema.sql` (symbol `cb_foolish_schema_sql`), APP_PROGS for `cbase-import` and `cbase-export` (migrate/), APP_TOOLS for unfurl-payloads (tools/), APP_BENCH_ROOTS := bench, APP_FUZZ_ROOTS := fuzz, APP_FUZZ_SEEDS, APP_SERVE := cbase (live binary name), APP_NAME_ALLOW, APP_GATE_HEADER (the rows header).
- Fuzz corpora: `foolish/server/fuzz/corpus/**` (about 5,200 files) stay; `c_base/core/fuzz/corpus/**` (about 3,300) and `c_base/client/fuzz/corpus/client` (1,035) move with cbase.
- The 11 tests to foolish/server/test: `lib_admin_fakestore.{c,h}`, `unit_admin_edges.c`, `smoke_lambda_after.c`, `unit_aws_calls.c`, `unit_conf_store_dynamo.c`, `unit_dynamo_ops.c`, `unit_dynamo.c`, `unit_items.c`, `unit_rt_iot.c`, `unit_rt_local.c`, `unit_stream.c`. Also with them: `providers/aws/test/golden/dynamo_shapes.txt` (read by the dynamo tests; absent from cbase), and `core/test/gate/core_includes_product.c` (absent from cbase; cbase.mk's APP_GATE_HEADER case now covers it).
- Because the tests include aws headers (`aws/aws_call.h`, `aws/runtime_api.h`, `aws/event.h`) foolish/server's test roots need `CBASE_PROVIDERS := local aws` and the aws-side include roots; `product_includes_aws.c` shows the product must NOT include them from `src/`, only from `test/`.
- `foolish/rust/Makefile:26` hardcodes `../../shared/c/sha256.c` -> `../../cbase/lib/sha256.c`; `foolish/rust/bench_common.h:12-13` includes `../../shared/c/{fnv,clock}.h`.
- `foolish/foolyard/Makefile:13,20` `SHARED := ../../shared/c` with `$(SHARED)/mqtt/mqtt.c`; `foolish/foolyard/include/rng.h:5` includes `../../../shared/c/rng.h`.
- `foolish/c/Makefile`: `SHARED := ../../shared/c` (line 7) plus `$(SHARED)/sha256.c` (92, 501, 1265, 1390), `$(SHARED)/mqtt/mqtt.c` (95, 504, 1401), `-I../../c_base/core/test` (1902); bump_alloc, libc, b32, deal_rng stay. Add `CBLIB := ../../cbase/lib`.
- shared/fuzz/Makefile uses `$(SH)/{deal_rng,sha256,b32,mixrad,text_util/text_util,wire_check/wire_check}.c`: only sha256.c moves (lines 33, 44, 46).
- shared/tools/shared_tests.mk (and cbase's lib_tests.mk): lists `SHARED_TEST_SRC_*` for check, le_bytes, rng, fnv, clock, percentile, sha256, b64, hex, hist, proc, stream_frame, libc tests (lines 32-59, 64): when the moved *_test.c files leave, foolish's list must drop them (leaving check_test, collapse, msg_stage, ease, wire_check, msg_whole, libc) and cbase's lib_tests.mk runs the others. libc_test stays foolish (wasm/libc_test.c).

## 7. Risks and surprises

1. Submodule is invisible to path-based CI: GitHub `paths:` filters see a submodule bump as the single path `cbase`; `cbase/**` never matches. Every workflow filter that mentions `c_base/**`, `shared/c/**`, `shared/tools/**` needs the literal `cbase` added. `git diff --name-only` selectors (fuzz_select.sh, metrics.yml's base/PR comparison, `tools/auth_route_diff.sh`) see no inner files and `git archive`/`git ls-tree` do not descend into submodules.
2. Cross-repo atomicity: a foolish change that needs a cbase change now takes two PRs and a gitlink bump; "ship the kernel and server together" CI (c_base.yml builds foolish/c + foolish/server + c_base) becomes: cbase CI proves the example app, foolish CI proves the pinned cbase with the product. Decide who gates a cbase PR against foolish.
3. About 150 relative includes `../../../shared/c/<moved>.h` (rng.h alone: 67 files) in chuiniu, pickemup, tallybones, uttt, werewolf, foolish/c, foolyard, rust, shared/fuzz and foolish/server. One mechanical rewrite, but uttt, tallybones, pickemup, chuiniu CI now also need the submodule even though they do not use c_base.
4. Duplicated and diverged copies: identical helper copies (so no drift today), but after cutover there must be exactly one; `lib/wasm` and `tools/datagen` copies in cbase are to be deleted; `tools/{c_rules,gen_tools,llvm,wasm_cc}.mk`, `ci_llvm.sh`, `lib_tests.mk` in cbase are forks, not copies, of the foolish `shared/` ones (C_ROOT default `.` vs `../..`; gen_tools.mk reads `$(CB)`, not `$(C_ROOT)`). If foolish products include cbase's c_rules.mk they must set `C_ROOT`; if they keep their own, two compile-rule owners exist (the "double Band-Aid" case) and one should be deleted.
5. Generated files and ignored build dirs: `.gitignore:88` (`shared/tools/structgen/build/`), datagen build dir, `foolish/ios/Generated`, `foolish/server/migrate/out/`; structgen's layout hash covers kernel headers only, so moving shared helper headers does not change generated hashes unless a struct header is touched (verify with `foolish/tools/structgen/gen.sh --check`, `layout_hash.test.ts`). Moved sources change object paths under the build dirs, so every wasm and native object rebuilds once; a path change alone should not change wasm bytes (wasm-ld embeds the output basename, not source paths), but verify the shipped `bots.wasm` size gate and the layout-hash checks after the move.
6. `git worktree add` friction: a fresh worktree has an empty `cbase/` until `git submodule update --init` runs in it (every worktree needs its own submodule checkout and `git config submodule.recurse`); agents running parallel worktrees share the superproject's `.git/modules/cbase` store, so a fetch/branch operation in one worktree's submodule affects another, and `git worktree remove` fails on a worktree with initialized submodules unless `--force`. The existing rule against `git stash` in worktrees is more important than before (stash is shared). The CLAUDE.md branch cleanup flow must also delete the submodule branch.
7. Scripts that assume c_base is a plain directory: `bash` scripts that `cd "$(dirname "$0")/../../.."` (vm_release.sh, deploy.sh, package.sh, lambda_deploy.sh, dynamo_local.sh, role_covers_release.sh) compute the c_base root, which is fine for a submodule, but any that then take `git rev-parse --show-toplevel` get cbase's repo instead of foolish's (vm_release.sh already handles this with `--show-superproject-working-tree`; `require_main.sh`'s `$tree` argument and web_deploy.sh need the same check); `c_base/bench/nano/build_client_linux2.sh` and `build_bootstrap.sh` tar `./c_base ./foolish/c ./foolish/server ./shared` into docker; `bench/nano/build.sh:4` uses `git rev-parse --show-toplevel`.
8. Live-stack and rename traps: do not sed bare `c_base`; the only live strings containing the substring `cbase` are the names listed in the header, and `c_base` appears in CloudFormation only in Description/Comment/DisplayName/FunctionDescription. Run the changeset gate before the first deploy from the new layout. The ops settings file default is `~/.config/cbase/ops.env`, so no secret moves. Release tags `cbase-serve-<sha>` are keyed on the foolish SHA.
9. cbase HEAD is a moving, not-yet-final target: it still has Games/Bots console, game/bot admin shapes, CBASE_BOT_* knobs, `rows_foolish.h` includes in seeds/fuzz/bench, `lib/wasm`, `tools/datagen`, the vm/aws stack ops, and 2 of the 11 tests. This map's section 3 list is the checklist; re-run the checks against the hand-over list.
10. Stray tracked files: `c_base/cbase.sqlite.serve-lock` is tracked in foolish origin/main (and not in cbase), and `c_base/bench/results/**`, `bench/calib/**` (about 300 files) were dropped from the cbase check only because I excluded them; confirm they moved. They bloat nothing in foolish once deleted but `git log -- c_base` history is lost to foolish unless cbase was created with `git filter-repo` or `subtree split`; the foolish history before the cutover keeps the old paths, so `git log --follow` and blame across the move work only by path `c_base/`.
11. Name gate and repo gate: cbase's `tools/repo_gate_test.sh` forbids `foolish/` paths and `../..` climbs out of cbase, so the foolish-side `foolish/server/Makefile` must carry every foolish reference; and foolish's `shared/tools/name_gate/allow.txt` (220 lines) must follow.
12. Checkout token: if alexsave/cbase is private, `actions/checkout` needs a PAT or deploy key with `submodules: recursive`, and the deploy job (OIDC, `permissions: contents: read`) has no extra secret for it today.
