#!/usr/bin/env bash
# Smoke test for the QUIC/HTTP3/WebTransport front-end (quic_wt.c). Builds the
# QUIC server + the WebTransport test client, brings the server up on both TCP
# and UDP/QUIC, creates a game over the TCP HTTP path, then:
#   1. fetches /state over HTTP/3 and checks it is byte-identical to TCP /state,
#      and that a seat's view (its hand) goes only to that seat's owner,
#   2. opens a WebTransport session and round-trips view DATAGRAMs.
#
# Requires a local quiche C-FFI build. Point QUICHE_DIR at a directory holding
# include/quiche.h and lib/libquiche.a (see the foolish_server_quic target):
#   git clone --recursive https://github.com/cloudflare/quiche
#   ( cd quiche && cargo build --release --features ffi )
#   QUICHE_DIR=quiche/quiche  # (include/ lives here; libquiche.a in target/release)
# quiche-client (optional, from the same build's target/release) is used for the
# HTTP/3 check if present; the WebTransport check always uses ./wt_client.
set -euo pipefail
cd "$(dirname "$0")"

QUICHE_DIR="${QUICHE_DIR:-./quiche}"
TCP_PORT="${TCP_PORT:-8099}"
QUIC_PORT="${QUIC_PORT:-4433}"
TMP="$(mktemp -d)"
trap 'kill "${SRV:-0}" 2>/dev/null || true; rm -rf "$TMP"' EXIT

if [ ! -f "$QUICHE_DIR/lib/libquiche.a" ] || [ ! -f "$QUICHE_DIR/include/quiche.h" ]; then
  echo "SKIP: set QUICHE_DIR to a quiche FFI build (need lib/libquiche.a + include/quiche.h)"; exit 0
fi

echo "── building foolish_server_quic + wt_client"
make -s foolish_server_quic wt_client QUICHE_DIR="$QUICHE_DIR"

echo "── generating a self-signed TLS 1.3 test cert"
openssl req -x509 -newkey rsa:2048 -nodes -keyout "$TMP/key.pem" -out "$TMP/cert.pem" \
  -days 2 -subj "/CN=localhost" >/dev/null 2>&1

echo "── starting server (tcp :$TCP_PORT, quic udp :$QUIC_PORT)"
./foolish_server_quic --no-db --port "$TCP_PORT" --quic --quic-port="$QUIC_PORT" \
  --cert="$TMP/cert.pem" --key="$TMP/key.pem" >"$TMP/srv.log" 2>&1 &
SRV=$!
sleep 1.5

tok() { grep -o "\"$1\":\"[^\"]*\"" | head -1 | cut -d'"' -f4; }
H="http://127.0.0.1:$TCP_PORT"
AT=$(curl -s -XPOST "$H/auth/signup" -d '{"username":"alice"}' | tok token)
BT=$(curl -s -XPOST "$H/auth/signup" -d '{"username":"bob"}' | tok token)
GID=$(curl -s -XPOST "$H/create" -H "Authorization: Bearer $AT" | tok game_id)
curl -s -XPOST "$H/meta" -H "Authorization: Bearer $BT" -d "{\"type\":\"join\",\"game_id\":\"$GID\"}" >/dev/null
curl -s -XPOST "$H/meta" -H "Authorization: Bearer $AT" -d "{\"type\":\"start\",\"game_id\":\"$GID\"}" >/dev/null
curl -s -XPOST "$H/meta" -H "Authorization: Bearer $BT" -d "{\"type\":\"start\",\"game_id\":\"$GID\"}" >/dev/null
echo "── created and dealt game $GID (alice=seat 0, bob=seat 1), status $(curl -s "$H/status?game_id=$GID")"

# 1. HTTP/3 /state == TCP /state (only if quiche-client is available)
CLIENT="$QUICHE_DIR/../target/release/quiche-client"
[ -x "$CLIENT" ] || CLIENT="$(command -v quiche-client || true)"
if [ -n "${CLIENT:-}" ] && [ -x "$CLIENT" ]; then
  curl -s "http://127.0.0.1:$TCP_PORT/state?game_id=$GID&seat=-1" -o "$TMP/state_tcp.bin"
  "$CLIENT" "https://127.0.0.1:$QUIC_PORT/state?game_id=$GID&seat=-1" --no-verify --idle-timeout 4000 \
    >"$TMP/state_quic.bin" 2>/dev/null || true
  if cmp -s "$TMP/state_tcp.bin" "$TMP/state_quic.bin"; then
    echo "── HTTP/3 /state: PASS (byte-identical to TCP, $(wc -c <"$TMP/state_tcp.bin") bytes)"
  else
    echo "── HTTP/3 /state: FAIL"; exit 1
  fi
  # A seat's view holds that seat's hand: over HTTP/3 as over TCP, only its owner
  # (token=, the WebTransport CONNECT's own credential) may read it.
  h3() { "$CLIENT" "https://127.0.0.1:$QUIC_PORT/state?$1" --no-verify --idle-timeout 4000 2>/dev/null || true; }
  curl -s "$H/state?game_id=$GID&seat=0" -H "Authorization: Bearer $AT" -o "$TMP/seat0_tcp.bin"
  h3 "game_id=$GID&seat=0&token=$AT" >"$TMP/seat0_owner.bin"
  h3 "game_id=$GID&seat=0" >"$TMP/seat0_notoken.bin"
  h3 "game_id=$GID&seat=0&token=$BT" >"$TMP/seat0_other.bin"
  h3 "game_id=$GID" >"$TMP/noseat.bin"
  if cmp -s "$TMP/seat0_tcp.bin" "$TMP/state_tcp.bin"; then
    echo "── HTTP/3 seat privacy: FAIL (seat 0's view equals the spectator view: the game has no hand to protect)"; exit 1
  fi
  if cmp -s "$TMP/seat0_tcp.bin" "$TMP/seat0_owner.bin" && [ ! -s "$TMP/seat0_notoken.bin" ] \
     && [ ! -s "$TMP/seat0_other.bin" ] && cmp -s "$TMP/noseat.bin" "$TMP/state_tcp.bin"; then
    echo "── HTTP/3 seat privacy: PASS (the owner reads seat 0; no token and bob's token read nothing; no seat is the spectator view)"
  else
    echo "── HTTP/3 seat privacy: FAIL (owner $(wc -c <"$TMP/seat0_owner.bin") B vs TCP $(wc -c <"$TMP/seat0_tcp.bin") B; no token $(wc -c <"$TMP/seat0_notoken.bin") B; bob $(wc -c <"$TMP/seat0_other.bin") B; no seat $(wc -c <"$TMP/noseat.bin") B)"
    exit 1
  fi
else
  echo "── HTTP/3 /state and seat privacy: SKIP (quiche-client not found)"
fi

# 2. WebTransport session + datagram round-trip
echo "── WebTransport session:"
./wt_client 127.0.0.1 "$QUIC_PORT" "/wt?token=$AT&game_id=$GID&seat=0"

echo "── quic_test: done"
