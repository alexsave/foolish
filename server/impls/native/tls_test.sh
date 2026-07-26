#!/usr/bin/env bash
# tls_test.sh — Stage 3's correctness gate: generates a throwaway self-signed
# cert with the `openssl` CLI, starts foolish_server with --tls against it,
# and checks:
#   1) a real TLS handshake completes (`openssl s_client`);
#   2) HTTPS actually answers a request (`curl -k https://.../health`);
#   3) WSS (WebSocket-over-TLS) carries the hot loop end-to-end, applying
#      genuinely LEGAL moves through foolish_hammer --tls --mode=ws — same
#      90%+-applied bar test.sh's plaintext ws smoke test holds itself to.
#
# Nothing here is committed: the cert/key/db live under a scratch tmp dir
# (same pattern crash_test.sh uses for its own --db), and *.pem/*.crt/*.key
# are gitignored regardless.
set -uo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$DIR"

PORT="${1:-8199}"
H="https://127.0.0.1:$PORT"
WORKDIR="$(mktemp -d /tmp/foolish_tls_test.XXXXXX)"
CERT="$WORKDIR/server.crt"
KEY="$WORKDIR/server.key"
SRV_LOG="$WORKDIR/server.log"
PASS=true

cleanup() {
    [ -n "${SRV_PID:-}" ] && kill -9 "$SRV_PID" >/dev/null 2>&1 || true
}
trap cleanup EXIT

echo "== tls_test.sh: workdir=$WORKDIR port=$PORT =="

[ -x "$DIR/foolish_server" ] || (cd "$DIR" && make foolish_server >/dev/null)
[ -x "$DIR/foolish_hammer" ] || (cd "$DIR" && make foolish_hammer >/dev/null)

# ── generate a throwaway self-signed cert (2 days, RSA 2048, CN=127.0.0.1 +
#    an IP SAN so curl/openssl's hostname check against 127.0.0.1 is happy) ──
echo "-- generating self-signed test cert"
openssl req -x509 -newkey rsa:2048 -nodes -keyout "$KEY" -out "$CERT" -days 2 \
    -subj "/CN=127.0.0.1" -addext "subjectAltName=IP:127.0.0.1" >/dev/null 2>&1
if [ ! -s "$CERT" ] || [ ! -s "$KEY" ]; then
    echo "FAIL: openssl req did not produce a cert/key"
    exit 1
fi

# ── start the server with --tls (--no-db: this test is about TLS, not persistence) ──
"$DIR/foolish_server" "$PORT" --no-db --tls --cert="$CERT" --key="$KEY" > "$SRV_LOG" 2>&1 &
SRV_PID=$!
for _ in $(seq 1 50); do
    curl -sk -o /dev/null "$H/health" && break
    sleep 0.1
done
echo "-- server up, pid=$SRV_PID"

check() {
    if [ "$2" = "0" ]; then echo "PASS: $1"; else echo "FAIL: $1"; PASS=false; fi
}

# ── 1) a real TLS handshake (openssl s_client) ──
echo "── openssl s_client handshake"
SCLIENT_OUT="$(echo -e "GET /health HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n" \
    | timeout 5 openssl s_client -connect "127.0.0.1:$PORT" -quiet 2>"$WORKDIR/sclient.err")"
echo "$SCLIENT_OUT" | head -3
echo "$SCLIENT_OUT" | grep -q '"ok":true'
check "openssl s_client completes a TLS handshake and gets a real HTTP response" "$?"
grep -qE 'TLSv1\.[23]' "$WORKDIR/sclient.err" 2>/dev/null || \
    echo | timeout 5 openssl s_client -connect "127.0.0.1:$PORT" 2>&1 | grep -E "Protocol|New," | head -2

# ── 2) curl -k https://.../health ──
echo "── curl -k https://…/health"
CURL_OUT="$(curl -sk "$H/health")"
echo "$CURL_OUT"
[ "$CURL_OUT" = '{"ok":true}' ]
check "curl -k https over TLS returns {\"ok\":true}" "$?"

# ── 3) plain HTTP against the SAME (TLS-only) port must NOT parse as HTTP —
#    confirms the listener really is TLS end-to-end, not falling back ──
echo "── plaintext HTTP against the TLS-only port (expect garbage/no valid response)"
PLAIN_OUT="$(curl -s --max-time 2 "http://127.0.0.1:$PORT/health" 2>&1 || true)"
if [ "$PLAIN_OUT" = '{"ok":true}' ]; then
    echo "FAIL: plaintext HTTP against the TLS port got a real HTTP response — TLS is not actually enforced"
    PASS=false
else
    echo "PASS: plaintext HTTP against the TLS port did not get a valid response ($PLAIN_OUT)"
fi

# ── 4) WSS smoke: foolish_hammer --tls --mode=ws, real legal moves ──
echo "── wss smoke test (foolish_hammer --tls --mode=ws, tiny scale, port $PORT)"
WS_OUT=$("$DIR/foolish_hammer" --host=127.0.0.1 --port="$PORT" --games=2 --seats=2 --secs=3 --mode=ws --tls)
echo "$WS_OUT" | tail -14
SENT=$(echo "$WS_OUT" | grep -o 'actions submitted:[[:space:]]*[0-9]*' | grep -o '[0-9]*$' || echo 0)
APPLIED=$(echo "$WS_OUT" | grep -o 'applied(ok=true):[[:space:]]*[0-9]*' | grep -o '[0-9]*$' || echo 0)
if [ -n "$SENT" ] && [ "$SENT" -gt 0 ]; then
    PCT=$(awk -v a="$APPLIED" -v s="$SENT" 'BEGIN { printf "%.1f", (a*100.0)/s }')
    echo "   applied $APPLIED / $SENT submitted ($PCT%)"
    AT_LEAST_90=$(awk -v a="$APPLIED" -v s="$SENT" 'BEGIN { print (a >= 0.90*s) ? "0" : "1" }')
    check "wss smoke: >=90% of submitted moves applied over TLS (same bar as plaintext)" "$AT_LEAST_90"
else
    echo "FAIL: wss smoke test sent no actions"
    PASS=false
fi

echo
if $PASS; then
    echo "=== tls_test.sh: PASS — TLS handshake + HTTPS + WSS all verified ==="
    exit 0
else
    echo "=== tls_test.sh: FAIL — see above ==="
    exit 1
fi
