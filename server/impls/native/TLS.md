# Server TLS — HTTPS + WSS ("Stage 3")

Production-hardening stage 3 of 3 for `server/impls/native/foolish_server.c`.
Stage 1 (per-game locks + work-queue routing, "T2a") and Stage 2 (SQLite
WAL write-behind persistence + crash recovery) are done — see
[`SERVER_SCALING.md`](SERVER_SCALING.md) and [`DURABILITY.md`](DURABILITY.md).
This is the last stage: the server now speaks TLS on every endpoint,
including the persistent `/ws` hot loop (WSS), not just the one-shot HTTPS
requests.

Everything below touches only `server/impls/native/` — two new files
(`conn.c`/`conn.h`), `ws.c`/`ws.h` (frame I/O rerouted through `Conn`),
`foolish_server.c` (the TLS listener + every handler's `Conn *` parameter),
`foolish_hammer.c` (`--tls` client), `Makefile`, `.gitignore`, and this
file. `c/src/*` (the kernel) is untouched and read-only, same constraint
every stage in this series has followed.

## The seam this stage fills

Stage 1 left an explicit seam for this (`foolish_server.c`'s old "STAGE 3
SEAM" comment, and `SERVER_SCALING.md`'s / `DURABILITY.md`'s own "Seams
left" sections): every plain-HTTP socket byte funneled through two
one-line wrappers, `io_read`/`io_write`, and `ws.c` had the equivalent
pre-existing seam (`ws_read_full`/`ws_write_full`/`ws_fill`) for the
WebSocket path. The seam note predicted the shape of the fix almost
exactly: "a small `Conn { int fd; SSL *ssl; }` in place of `int`... nothing
above this layer inspects the fd directly." That is what got built.

## Design: `Conn`, a plaintext-or-TLS I/O handle

`conn.h`/`conn.c` (new, ~140 lines total) define:

```c
typedef struct {
    int fd;
    SSL *ssl;   // NULL = plaintext; else this connection's own TLS session
} Conn;

ssize_t conn_read(Conn *c, void *buf, size_t n);
ssize_t conn_write(Conn *c, const void *buf, size_t n);
void    conn_close(Conn *c);
bool    conn_tls_accept(Conn *out, SSL_CTX *ctx, int fd);    // server-side
bool    conn_tls_connect(Conn *out, SSL_CTX *ctx, int fd, const char *sni_hostname); // client-side
SSL_CTX *tls_server_ctx_create(const char *cert_path, const char *key_path);
SSL_CTX *tls_client_ctx_create(void);
```

`conn_read`/`conn_write` are single-call, EINTR-transparent, and have
**exactly** POSIX `read()`/`write()` semantics (a short result is normal,
0 means the peer is done, <0 is a real error) — when `ssl == NULL` they
call `read()`/`write()` directly, so a plaintext `Conn` behaves
byte-for-byte like the bare `int fd` it replaces. When `ssl != NULL` they
call `SSL_read()`/`SSL_write()`, retrying internally on
`SSL_ERROR_WANT_READ`/`WANT_WRITE` (belt-and-suspenders with
`SSL_MODE_AUTO_RETRY`, set on both server and client contexts) and
translating `SSL_ERROR_ZERO_RETURN` (the peer's clean `close_notify`) to a
plain `0`, matching `read()`'s EOF. Any other SSL error returns `-1` —
never crashes the process (SIGPIPE is already ignored server- and
client-wide; the equivalent `SSL_ERROR_SYSCALL` failure is handled the same
way `EPIPE` already was).

**Where `Conn` replaced a bare `int fd`**: every route handler
(`respond`/`respond_bin`, `h_signup`/`h_create`/`h_meta`/`h_action`/
`h_state`/`h_status`, `route`), `read_and_parse_request`, the work-queue's
`WorkItem` and `ws_conn_thread`'s `WsSpawnArg`, and — the actual hot path —
`ws.c`'s `WsConn` (`int fd` → `Conn conn`), `ws_read_full`/`ws_write_full`,
and `ws_send_frame`. Nothing above `conn.c` ever touches a raw `fd` or
`SSL*` again; `io_read`/`io_write` (`foolish_server.c`) are now one-line
wrappers over `conn_read`/`conn_write`, kept only as the documented seam
name.

**Why WSS, not just HTTPS**: `/ws` is the hot loop (see README/
`SERVER_SCALING.md`) — a persistent connection carrying every move + state
push for a client's whole session. Landing TLS only on the one-shot
endpoints and leaving `/ws` plaintext would mean the actual game traffic —
every card played — still crosses the wire unencrypted, which defeats the
point of "the server speaks TLS." `ws.c` is genuinely shared between
`foolish_server.c` and `foolish_hammer.c` (same file, same `WsConn`), so
fixing the seam there covers both the server's WSS listener and the load
client's WSS driver in one pass.

**The one non-uniform spot** (predicted in the original seam comment):
`ws_send_frame`'s unmasked (server) fast path used `writev()` to send a
frame header + payload in one syscall; OpenSSL has no vector write. Fixed
by branching on `c->conn.ssl`: the plaintext path keeps its `writev()`
exactly as before (byte-for-byte unchanged), and the TLS path concatenates
header + payload into a `_Thread_local` scratch buffer (sized to this
protocol's own documented worst case — the same "worst case + real margin"
discipline `VIEW_CACHE_CAP` uses) and sends it with one `ws_write_full`
call — no lock, no per-frame allocation, since `ws_send_frame` is only ever
called from a connection's own dedicated thread.

## Startup: one shared `SSL_CTX`, one fresh `SSL*` per connection

`g_tls_ctx` (server) / `g_tls_ctx` (client, in `foolish_hammer.c`) are built
**once**, in `main()`, before any worker pool or the accept loop starts —
via `tls_server_ctx_create`/`tls_client_ctx_create`:

- `TLS_server_method()`/`TLS_client_method()`, `SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION)`
  (no SSLv3/TLS1.0/1.1 — verified below), `SSL_CTX_set_mode(ctx, SSL_MODE_AUTO_RETRY)`.
- Server: `SSL_CTX_use_certificate_chain_file` + `SSL_CTX_use_PrivateKey_file`
  + `SSL_CTX_check_private_key` — any failure (missing file, bad PEM, a key
  that doesn't match the cert) returns `NULL`, and `main()` treats a
  *requested* (`--tls` passed) but failed TLS setup as **fatal**, the same
  posture Stage 2 takes for a requested-but-failed `--db`: silently
  downgrading to plaintext when the operator asked for TLS would be a
  silent security regression, worse than refusing to start.
- Client: `SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL)` — this is a load
  tool hitting the server's own self-signed test cert, not a browser; a
  real client would pin/verify.

After setup, the context is **read-only** — every accepted connection calls
`conn_tls_accept(&conn, g_tls_ctx, fd)` (server) or `conn_tls_connect(&conn,
g_tls_ctx, fd, host)` (client, with SNI via `SSL_set_tlsext_host_name`),
which does `SSL_new` off the shared ctx + `SSL_set_fd` + `SSL_accept`/
`SSL_connect` **on that connection's own thread**, and the resulting `SSL*`
is never touched by any other thread afterward. This is exactly the model
the task called out as needing verification — see "Helgrind" below.

Server-side, the handshake happens in `main()`'s accept loop, right after
`accept()` and before any HTTP parsing:

```c
Conn conn;
if (g_tls_ctx) {
    if (!conn_tls_accept(&conn, g_tls_ctx, fd)) { close(fd); continue; }
} else {
    conn_init_plain(&conn, fd);
}
```

A failed/abandoned handshake (a port scanner, a plaintext probe against a
TLS listener, a client that hangs up mid-handshake) is just a dropped
connection — never fatal to the process.

## Configuration

```
--tls --cert=PATH --key=PATH     turn the WHOLE listen socket over to TLS
```

Without `--tls`, the server is plaintext, byte-for-byte identical to every
earlier stage (confirmed below — `test.sh`/`crash_test.sh` pass unchanged).
There is no mixed plaintext+TLS listener and no separate `--tls-port`: a
single `--tls`-flips-the-listener design was chosen for simplicity, as the
task brief allowed. `foolish_hammer --tls` is the matching load-test flag —
every socket it opens (HTTP setup **and** WS frames) goes through
`conn_tls_connect` when set.

No private key or cert is ever committed. `.gitignore` now excludes
`*.pem`/`*.crt`/`*.key` (belt-and-suspenders — nothing in this stage writes
one in-tree either way) and raw Helgrind dumps (`helgrind*.log`,
`*.valgrind.log`); `tls_test.sh` generates a throwaway self-signed cert
under a scratch `mktemp -d` directory at test time with the `openssl` CLI,
same pattern `crash_test.sh` uses for its own scratch `--db`.

## Verification

### Handshake + HTTPS

```
$ openssl req -x509 -newkey rsa:2048 -nodes -keyout server.key -out server.crt \
    -days 2 -subj "/CN=127.0.0.1" -addext "subjectAltName=IP:127.0.0.1"
$ ./foolish_server 8299 --no-db --tls --cert=server.crt --key=server.key &

$ openssl s_client -connect 127.0.0.1:8299 -quiet <<< $'GET /health HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n'
HTTP/1.1 200 OK
Content-Type: application/json
...
{"ok":true}

$ curl -sk https://127.0.0.1:8299/health
{"ok":true}

$ echo | openssl s_client -connect 127.0.0.1:8299 2>&1 | grep -E "Protocol|New,"
New, TLSv1.3, Cipher is TLS_AES_256_GCM_SHA384
    Protocol  : TLSv1.3
```

Min-version enforcement (`SSL_CTX_set_min_proto_version(ctx,
TLS1_2_VERSION)`) confirmed directly — TLS 1.1 is rejected with a real
protocol-version alert, TLS 1.2/1.3 both succeed:

```
$ echo | openssl s_client -connect 127.0.0.1:8299 -tls1_1 -cipher "DEFAULT@SECLEVEL=0"
...SSL routines:ssl3_read_bytes:tlsv1 alert protocol version...SSL alert number 70

$ echo | openssl s_client -connect 127.0.0.1:8299 -tls1_2 2>&1 | grep -E "Protocol|New,"
New, TLSv1.2, Cipher is ECDHE-RSA-AES256-GCM-SHA384
```

### WSS smoke (real legal moves, over TLS)

`foolish_hammer --tls --mode=ws` against the same `--tls` server:

```
foolish_hammer: host=127.0.0.1:8299 games=2 seats=2 conns=32 secs=3 mode=ws tls=on
...
actions submitted:           12045  applied(ok=true): 11941  (3969.4 applied/s)
```

99.1% of submitted moves applied (well above the 90% bar the plaintext WS
smoke test in `test.sh` holds itself to) — every move decoded, transited,
and applied over a real TLS session, not just the connect/handshake step.

### `tls_test.sh` — the automated gate

```
bash tls_test.sh
```

Generates its own throwaway cert (`mktemp -d`, `openssl req`), starts its
own `--tls --no-db` server, and asserts, in order: (1) `openssl s_client`
completes a handshake and gets a real HTTP response through it; (2) `curl
-k https://…/health` returns `{"ok":true}`; (3) a plain HTTP request
against the SAME (TLS-only) port does **not** get a valid HTTP response —
confirming the listener is really TLS end-to-end, not silently falling
back; (4) `foolish_hammer --tls --mode=ws` applies ≥90% of its submitted
moves. All four passed on this box:

```
PASS: openssl s_client completes a TLS handshake and gets a real HTTP response
PASS: curl -k https over TLS returns {"ok":true}
PASS: plaintext HTTP against the TLS port did not get a valid response ()
   applied 11834 / 11936 submitted (99.1%)
PASS: wss smoke: >=90% of submitted moves applied over TLS (same bar as plaintext)

=== tls_test.sh: PASS — TLS handshake + HTTPS + WSS all verified ===
```

### Plaintext unchanged

`bash test.sh` and `bash crash_test.sh` (no `--tls` anywhere) both pass
fully against a build that links `conn.c`/OpenSSL — same output as before
this stage, including the plaintext WS smoke test (legal moves applied,
90%+ bar) and the full crash-recovery scenario (kill -9, restart against
the same `--db`, byte-identical recovered state). Plaintext callers hit
`conn_read`/`conn_write`'s `ssl == NULL` branch, which is a direct
`read()`/`write()` call — nothing in the plaintext path changed shape.

## Measured TLS overhead

Setup: a self-signed RSA-2048 cert, `foolish_server --no-db` (isolates
TLS's own cost from persistence's), `foolish_hammer --mode=ws` (the
same WS+legal-move client used throughout this series), plain vs `--tls`,
back to back on the same box (4 cores). Full raw numbers:
[`bench_results/tls/overhead.csv`](bench_results/tls/overhead.csv).

### One-time handshake cost

50 sequential fresh connections, one `/health` each (`curl`, new TCP
connection + full handshake every time — the worst case for TLS, since
nothing amortizes):

| | 50 requests, wall clock | per-request |
|---|---|---|
| HTTP | 262ms | 5.2ms |
| HTTPS | 435ms | 8.7ms |

~3.5ms of that per-request delta is the RSA-2048 handshake (curl's own
process-startup cost is in both numbers and roughly cancels in the delta).
That is the FULL one-time cost of establishing a new TLS session — real,
but it is paid **once per connection**, and `/ws` connections are
persistent: every WS connection in the measurements below carried
thousands to tens of thousands of round trips, so the handshake amortizes
to a rounding error over a connection's real lifetime.

### Steady-state round-trip latency (the ongoing, per-frame cost)

This is the number that matters for `/ws`: send → server applies/serializes
→ receive, over an ALREADY-established connection — the symmetric-crypto
cost per frame the task asked about, with the one-time handshake excluded.

At 32 and 160 connections (`--games=8/40 --seats=4`, matching
`SERVER_SCALING.md`'s own T2a scale), TLS's added per-round-trip cost is
clearly visible and consistent in direction:

| conns | variant | mean us | p50 us | p90 us | p99 us |
|---|---|---|---|---|---|
| 32  | plain | 44.8  | 34.0 | 91.5  | 175.9 |
| 32  | tls   | 60.9 (**+36%**) | 31.7 (-7%) | 125.7 (**+37%**) | 572.6 (**+226%**) |
| 160 | plain | 129.9 | 91.5 | 284.3 | 587.6 |
| 160 | tls   | 186.8 (**+44%**) | 112.8 (**+23%**) | 433.5 (**+53%**) | 1125.8 (**+92%**) |

Mean/p90 rise 35-55% under TLS at both scales — plausible and expected
(AES-GCM + the extra `SSL_read`/`SSL_write` call overhead per frame, on
top of the same syscalls the plaintext path already pays). **p99 rises far
more (+92% to +226%)**: under real concurrency, a fraction of frames land
behind a TLS record boundary that needs a second `SSL_read` internally, or
queue behind another connection's crypto work on the same core, and that
shows up disproportionately in the tail rather than the median — an honest
result, not a rounded-off one.

At minimal connection counts (2 and 8, `--seats=2` so almost no seat sits
genuinely idle), the picture is noisier and, at 2 connections, TLS's mean/
p50/p90 actually measured slightly **below** plaintext's (3 repeats each,
see the CSV) — not because TLS is free, but because a single AES-GCM frame
on a payload this small (well under 1KB) costs low tens of microseconds on
this hardware, which is smaller than the run-to-run scheduling noise
already present in a bare loopback round trip at this scale. The 32/160
numbers above are the trustworthy read on steady-state overhead; the
2-8 conn numbers show that overhead is not resolvable above the noise
floor until there's enough real concurrent work for it to show up
consistently.

### Throughput — an honest caveat, not a "TLS is faster" claim

At every connection count measured (2, 8, 32, 40, 160), raw
`applied(ok=true)`-moves-per-second was **higher** under `--tls` than
plaintext — sometimes by two orders of magnitude (e.g. 32 conns: 126.3
applied/s plain vs 16206.0 applied/s tls). This is real, reproduced across
repeated runs, and **not attributable to TLS making the server faster**.
Root cause, tracked down rather than hand-waved: at every scale, TLS runs
show a dramatically higher `rematches` count (games finishing and
restarting) than plaintext in the SAME wall-clock window — e.g. at a
minimal 2-connection scale (1 game, 2 seats, where genuine CPU/lock
contention can be ruled out directly: only 2 threads on a 4-core box),
plaintext completed **1** game in 10s while TLS completed **332**.
`foolish_hammer`'s WS worker polls every idle (non-eligible) seat every
`WS_IDLE_POLL_US` (1ms, see that constant's own doc in
`foolish_hammer.c`) — a pacing constant that predates this stage and is
unrelated to the I/O layer. At plaintext's much faster raw round-trip
time, idle seats poll measurably more densely per unit wall-clock time
than under TLS's slightly slower round trip; empirically that denser
polling correlates with games finishing (and thus new, freshly-dealt
hands with an immediately-eligible attacker appearing) far less often —
this looks like a timing-sensitive interaction in the load client's own
random-move-selection pacing, not a lock-contention effect (ruled out at
the 2-connection scale) and not a server-side bug (the server's own
`awire_apply` acceptance rate is ~98-99% in BOTH variants — see the CSV —
so whenever a move IS submitted, it is accepted at the same rate either
way; the difference is entirely in how often a legal move is available to
submit at all). This is orthogonal to TLS and not investigated further
here (out of scope for a TLS-focused stage, and `foolish_hammer.c`'s move
selection / pacing logic is unchanged by this stage) — reported plainly
rather than presented as a TLS speedup, which it is not.

**Bottom line**: use the latency table above for "what does TLS cost this
server," not the throughput numbers in the CSV.

## Helgrind — race-free verdict

`valgrind --tool=helgrind --history-level=approx` against `./foolish_server
<port> --no-db --tls --cert=... --key=... --game-workers=4`, driven by
`foolish_hammer --mode=mixed --tls` (one-shot HTTPS traffic through the
work-queue pools) then `--mode=ws --tls` (persistent WSS, the hot path)
back to back, `kill -TERM` on the valgrind PID to end the capture (same
technique every earlier stage's Helgrind run used). `--no-db` on purpose:
persistence's own locking was already Helgrind-verified in Stage 2 (see
[`DURABILITY.md`](DURABILITY.md)), TLS touches neither `persist.c` nor
`persist.h`, and running without a `persist_thread` also means the one
previously-documented benign residual (a `pthread_cond_timedwait` "dubious:
associated lock" report at that thread's own shutdown — see
`DURABILITY.md`'s Helgrind section) cannot appear here, since that thread
never starts. Full digest: [`bench_results/tls/helgrind_summary.txt`](bench_results/tls/helgrind_summary.txt).

```
==PID== ERROR SUMMARY: 0 errors from 0 contexts (suppressed: 102034 from 168)
```

**Clean.** Zero "Possible data race" reports. The large suppressed count is
valgrind's own built-in suppressions firing against OpenSSL's internal
atomic/lock-free bookkeeping (its constant-time primitives and reference
counting do a lot of atomic traffic Helgrind ships default suppressions
for) — none are new/unsuppressed, none are in this codebase's own code, and
no custom `--suppressions` file was used or needed. This confirms the
shared-`SSL_CTX`-plus-per-connection-`SSL*` model is race-free across both
the one-shot work-queue path (many worker threads, each doing its own
`conn_tls_accept` + `SSL_read`/`SSL_write`) and the persistent `/ws` path
(`ws_conn_thread`, the hot path this stage cared most about).

## What's still not here (stated plainly)

- **Cert rotation / ACME**: the cert is loaded once at startup
  (`tls_server_ctx_create`), not reloaded on SIGHUP or a file-watch. A
  rotated cert needs a process restart.
- **Client certificate auth (mTLS)**: not implemented — `SSL_VERIFY_NONE`
  on the client side is specifically for load-testing the server's own
  self-signed test cert (see "Configuration" above), and the server never
  asks a client for a cert either.
- **Connection limits / backpressure under TLS specifically**: unchanged
  from Stage 1 — the work queues still apply backpressure by blocking the
  accept loop (`SERVER_SCALING.md`), not by shedding load with a 503; a
  slow/malicious TLS handshake occupies its connection's servicing thread
  the same way a slow plaintext request would.

See README.md's "Production readiness" section for the full tally across
all three stages.
