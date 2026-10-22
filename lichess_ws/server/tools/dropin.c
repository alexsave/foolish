// dropin.c - the drop-in check against the real lila: two players' sockets opened through lila's
// own front door (Caddy on the mono container, which proxies WebSocket upgrades to whatever
// listens on lila-ws's port: our gateway, during this check) with real session cookies for a game
// real lila created, a move sent by each side the way the browser sends it, and lila's move event
// seen to come back through our gateway to both sides, the mover without dests and the opponent
// with them.
//
//   build/dropin --port 8080 --full-a <fullId> --cookie-a <lila2 value> --full-b <fullId> --cookie-b <lila2 value>
//
// tools/dropin.sh does the HTTP part (login, the challenge) and calls this.
#include "lib_client.h"
#include "net/ws.h"
#include "util/str.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; fprintf(stderr, "FAIL: " __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

// Reads frames for up to timeout_ms, printing each, until one contains `needle` (returned in out).
static bool wait_frame(int fd, const char *who, const char *needle, int timeout_ms, cb_buf *out) {
    for (int i = 0; i < 50; i++) {
        int op = tc_ws_recv(fd, timeout_ms, out);
        if (op < 0) return false;
        if (op == CB_WS_OP_PING) { tc_ws_send(fd, CB_WS_OP_PONG, out->p, out->len); continue; }
        if (op != CB_WS_OP_TEXT) { printf("  %s <- opcode %d\n", who, op); continue; }
        printf("  %s <- %.*s%s\n", who, (int)(out->len < 160 ? out->len : 160), (char *)out->p, out->len > 160 ? "..." : "");
        if (strstr(cb_buf_cstr(out), needle)) return true;
    }
    return false;
}

int main(int argc, char **argv) {
    int port = 8080;
    const char *full[2] = { NULL, NULL }, *cookie[2] = { NULL, NULL };
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--port")) port = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--full-a")) full[0] = argv[i + 1];
        else if (!strcmp(argv[i], "--full-b")) full[1] = argv[i + 1];
        else if (!strcmp(argv[i], "--cookie-a")) cookie[0] = argv[i + 1];
        else if (!strcmp(argv[i], "--cookie-b")) cookie[1] = argv[i + 1];
    }
    if (!full[0] || !full[1] || !cookie[0] || !cookie[1]) { fprintf(stderr, "usage: dropin --port P --full-a F --cookie-a C --full-b F --cookie-b C\n"); return 2; }
    signal(SIGPIPE, SIG_IGN);
    int fd[2];
    cb_buf m;
    cb_buf_init(&m);
    for (int k = 0; k < 2; k++) {
        char path[128], hdr[1024];
        snprintf(path, sizeof path, "/play/%s/v6?sri=dropin%c0001", full[k], 'a' + k);
        snprintf(hdr, sizeof hdr, "Cookie: lila2=%s\r\nOrigin: http://localhost:%d\r\n", cookie[k], port);
        fd[k] = tc_ws_open(port, path, hdr);
        CHECK(fd[k] >= 0, "socket %c did not open at %s", 'a' + k, path);
        if (fd[k] < 0) return 1;
        printf("%c: socket open at %s\n", 'A' + k, path);
    }
    // the crowd, once both are in
    CHECK(wait_frame(fd[0], "A", "\"t\":\"crowd\"", 3000, &m), "no crowd on A");
    CHECK(wait_frame(fd[1], "B", "\"t\":\"crowd\"", 3000, &m), "no crowd on B");
    // white (A) moves e2e4: an ack to A, then lila's move event to both
    const char *move1 = "{\"t\":\"move\",\"d\":{\"u\":\"e2e4\",\"b\":0,\"l\":20,\"a\":1}}";
    printf("A -> %s\n", move1);
    tc_ws_send(fd[0], CB_WS_OP_TEXT, move1, strlen(move1));
    CHECK(wait_frame(fd[0], "A", "{\"t\":\"ack\",\"d\":1}", 3000, &m), "no ack on A");
    CHECK(wait_frame(fd[0], "A", "\"uci\":\"e2e4\"", 5000, &m), "lila's e2e4 event did not reach A");
    CHECK(!strstr(cb_buf_cstr(&m), "\"dests\""), "the mover got dests");
    CHECK(wait_frame(fd[1], "B", "\"uci\":\"e2e4\"", 5000, &m), "lila's e2e4 event did not reach B");
    CHECK(strstr(cb_buf_cstr(&m), "\"dests\"") != NULL, "the opponent got no dests");
    // black (B) answers e7e5
    const char *move2 = "{\"t\":\"move\",\"d\":{\"u\":\"e7e5\",\"b\":0,\"a\":2}}";
    printf("B -> %s\n", move2);
    tc_ws_send(fd[1], CB_WS_OP_TEXT, move2, strlen(move2));
    CHECK(wait_frame(fd[1], "B", "{\"t\":\"ack\",\"d\":2}", 3000, &m), "no ack on B");
    CHECK(wait_frame(fd[1], "B", "\"uci\":\"e7e5\"", 5000, &m), "lila's e7e5 event did not reach B");
    CHECK(wait_frame(fd[0], "A", "\"uci\":\"e7e5\"", 5000, &m), "lila's e7e5 event did not reach A");
    CHECK(strstr(cb_buf_cstr(&m), "\"dests\"") != NULL, "A, to move, got no dests");
    // a ping, and a chat line (lila echoes chat lines as a room event)
    tc_ws_send(fd[0], CB_WS_OP_TEXT, "p", 1);
    CHECK(wait_frame(fd[0], "A", "0", 3000, &m) && m.len == 1, "no pong");
    const char *talk = "{\"t\":\"talk\",\"d\":\"hello from the drop-in check\"}";
    printf("A -> %s\n", talk);
    tc_ws_send(fd[0], CB_WS_OP_TEXT, talk, strlen(talk));
    CHECK(wait_frame(fd[1], "B", "hello from the drop-in check", 5000, &m), "the chat line did not reach B");
    // A resigns: lila ends the game and says so to both
    const char *resign = "{\"t\":\"resign\"}";
    printf("A -> %s\n", resign);
    tc_ws_send(fd[0], CB_WS_OP_TEXT, resign, strlen(resign));
    CHECK(wait_frame(fd[1], "B", "\"t\":\"endData\"", 5000, &m), "the end did not reach B");
    close(fd[0]); close(fd[1]);
    cb_buf_free(&m);
    if (fails) { printf("dropin: %d checks FAILED\n", fails); return 1; }
    printf("dropin: a game played through real lila with our gateway in lila-ws's place\n");
    return 0;
}
