# VM fixes batch: production-hardening `cbase serve` for a t4g.nano

Shared notes of the batch.
The final agent turns this into the PR description and deletes it.

## 1. The JWT secret

`cbase serve` now refuses to start without CBASE_JWT_SECRET, like the Lambda.
The explicit dev and test mode is `CBASE_DEV=1`: only there may serve make a random per-process secret (still logged as a warning) and default the public URL.
`cb_config_check_secrets` refuses a generated secret when `lambda` is true (always) or when not in dev mode.
`cb_config_from_env` itself only checks lengths now, so the unit tests that read a config without a secret still work.
`cb_process_main` calls the new `cb_config_check_serve` before it logs or listens.
Other secrets: CBASE_ADMIN_TOKEN unset means the console is refused and CBASE_ORIGIN_SECRET unset means the origin is open.
Both are valid states (a serve without CloudFront), so they stay optional; they must still be 32+ characters when set.
Test and smoke lanes that start `cbase serve` (grep of every spawner):
- foolish/server/test/smoke.c, smoke_matrix.c, smoke_authz.c already pass CBASE_JWT_SECRET and CBASE_LISTEN=127.0.0.1:0, but NOT CBASE_PUBLIC_URL, so they now need `CBASE_DEV=1` too (see "tests to fix").
- c_base/core/test/smoke_boot.c: edited here (bad_db and busy cases got CBASE_DEV=1; two new refusal cases).
- The serve_binary job of .github/workflows/c_base.yml only builds and checksums; it never runs serve.

## 2. Sign-up limits as settings

Every rate-limit burst (rate_limits.c) is a setting, `CBASE_LIMIT_<KIND>`, range 1..1000000, parsed with num_parse, default = today's value.
The window of each rule stays fixed.
The admin one was already CBASE_ADMIN_FAIL_LIMIT.

| setting | default | window |
|---|---|---|
| CBASE_LIMIT_SIGNIN_IP | 20 | 5 min |
| CBASE_LIMIT_SIGNIN_NAME | 10 | 5 min |
| CBASE_LIMIT_SIGNUP_IP | 5 | 1 h |
| CBASE_LIMIT_SIGNUP_ALL | 60 | 1 h |
| CBASE_LIMIT_REFRESH_IP | 60 | 5 min |
| CBASE_LIMIT_PASSWORD_USER | 5 | 1 h |
| CBASE_LIMIT_CREATE_USER | 20 | 10 min |
| CBASE_LIMIT_CHAT_USER | 30 | 1 min |
| CBASE_LIMIT_SUB_USER | 120 | 1 min |
| CBASE_LIMIT_BUMP_USER | 30 | 1 min |

What a single-VM deploy should set: the table is per process and a VM is one process for everyone, so SIGNUP_ALL is the whole site's hourly sign-up budget.
A sign-up costs 60 to 80 ms of CPU (bcrypt cost 10) on a nano, so 600 an hour is about 48 s of CPU an hour: `CBASE_LIMIT_SIGNUP_ALL=600` is safe, and 300 is the cautious figure.
`CBASE_LIMIT_SIGNUP_IP=20` is a reasonable figure for school and household NATs (one address, many people).
Leave the per-account ones alone.

## 3. A real health endpoint

- `GET|HEAD /health` and `/ready` from a loopback socket peer (127.0.0.1, ::1, ::ffff:127.0.0.1) skip the origin check; every other route, and these from any other address, still need X-Cbase-Origin when CBASE_ORIGIN_SECRET is set. The peer is the socket's, not a header.
  Caveat: a reverse proxy on the same box would make outside traffic look loopback, which exposes only those two bodies.
- `/health` bytes are unchanged ("ok cbase <version> mode=... uptime_s=..."; CI's deploy smoke greps "ok cbase"). Still liveness only, still on the loop thread.
- New `GET /ready`: readiness. One strongly consistent item read of a key that is never written (`READY#probe`/`PROBE`) through the store's item layer, so it is a SQLite read or a DynamoDB GetItem. NOT_FOUND is healthy. Anything else (full or locked or corrupt database) answers 503 "not ready: store ...". 200 body: "ready cbase <version> mode=... store=...". It runs on the request pool, not the loop, because the read may wait on SQLite's busy timeout.

## 4. The public URL default

CBASE_PUBLIC_URL is read by `cb_auth_new` (auth.c): it is the JWT issuer (`<url>/auth/v1`), and by the admin console's Overview (admin.c).
So http://0.0.0.0:8080 as the issuer on a VM would mint tokens under a meaningless issuer, and changing it later signs everyone out.
`cb_config_check_serve` outside dev mode now requires it to be set explicitly and refuses a wildcard host (0.0.0.0, [::]).
Under dev mode the default stays http://<CBASE_LISTEN>.

## 5. The connection cap

CBASE_MAX_CONNS (default 4096, 1..1000000) and CBASE_LISTEN_BACKLOG (default 512, 1..65535) feed `cb_server_opts` (max_conns, new field backlog).
A connection refused by the cap is counted and logged: one WARN line a minute, "connection cap N reached: K refused since the last report (CBASE_MAX_CONNS)".
Note for a nano: each client holds two connections (HTTP plus MQTT), and the process's fd limit (systemd LimitNOFILE) must exceed the cap.

## 6. Build hygiene

- rows_foolish.c:1332 (gcc 11 `-Werror=format-truncation`): the `%.*s` snprintf of the snapshot id became `cb_strlcpy` (same cut). Checked with gcc 16 only; the AL2023 gcc 11 build was not rerun.
- The kernel's 18 warnings (found with gcc 16 on macOS, which prints the same 18 as the Linux build; gcc 13 in docker is clean after the fixes):
  - client_table.c `pick` maybe-uninitialized: zero-initialized; `to_table` set but never read: dropped, only its count is kept.
  - cordite_sim.c:1024 (14 warnings): the cover copy loop is bounded by the size of `cards`.
  - table.c:736 (2): `table_rankings` clamps the eliminated count to MAX_PLAYERS before the copy.
  - winprob.c:497 (gcc 13 only): `winprob_step` checks the player count at the read site.
- An AL2023 build needs `diffutils` (the Makefile uses `cmp`), plus gcc, make, openssl-devel and sqlite-devel.

## Owed tests

- unit: `/ready` answers 200 on a good store and 503 when the store read fails (a fake store whose item_get returns CB_E_DB); mutation check: make the handler ignore the read result.
- unit: loopback `/health` and `/ready` skip the origin check; a non-loopback peer and `/v1/...` from loopback do not (handler.c is_local_probe). Mutation: drop the peer test.
- unit: CBASE_LIMIT_* change `cb_limit_rules` after boot (`cb_limits_configure`); out-of-range values are refused.
- unit: the connection cap logs once a minute (needs a small max_conns and a few sockets); the backlog setting reaches listen(2).
- smoke: `cbase serve` without CBASE_JWT_SECRET exits 2 (written in smoke_boot.c, not run).

## Tests to fix

- foolish/server/test/smoke.c, smoke_matrix.c, smoke_authz.c: add `CBASE_DEV=1` to the serve environment (they give a JWT secret but no CBASE_PUBLIC_URL).
- Any unit test that sends /health with a loopback peer and expects 403 under CBASE_ORIGIN_SECRET (unit_v1.c around line 812): give the request a non-loopback peer or check the new behavior.
- Edited but never run in this batch: core/test/unit_config.c (secrets_rule), core/test/unit_edges.c (VARS), core/test/smoke_boot.c.

## Fixes to make (found on the way)

- shared/c/wasm/libm.c `quad()` casts a NaN or infinite double to long long: undefined behaviour and a trap in the wasm module. Guard non-finite input.
- c_base/DESIGN.md:394 says the kernel section is "per-thread scratch", but the code callocs a fresh cb_section per operation (table_ops.c:161). DESIGN.md:426 says "no Lambda yet", which is stale. Fix both with the batch/section reuse item.
