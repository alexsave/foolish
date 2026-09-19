// ws_frame_test.c - the RFC 6455 frame header, read the way a hostile peer
// writes it.
//
// The 8-byte ("127") length form is eight bytes of attacker-controlled data
// assembled into one integer. It used to be assembled into a SIGNED int64, so
// the last shift could set the sign bit: signed overflow, undefined behaviour,
// and UB lets the compiler assume it cannot happen - which licenses it to
// delete the `if (len < 0) return -1;` on the very next line. The guard that
// made the parser safe was the guard the UB was allowed to remove.
//
// THAT IS WHY THIS TEST IS BUILT UNDER UBSan with -fno-sanitize-recover
// (`make ws-frame-test`). The pre-fix parser still *returns* -1 for these
// frames in a debug build, because that build happened not to delete the
// guard - so a plain return-value assertion goes green against the defect it
// exists to catch. What actually distinguishes the two is the undefined
// behaviour itself, and UBSan is what sees it: the old code aborts here, the
// fixed code runs clean.
//
// No socket: ws_conn_prime seeds the bytes a caller already read, and ws_fill
// drains those before it ever touches the fd (which is -1 here, so any read
// past the primed bytes simply fails, exactly as a peer that hung up would).

#include <stdio.h>
#include <string.h>

#include "ws.h"

static int g_fail = 0;

static void check(int ok, const char *what) {
    if (!ok) { printf("FAIL %s\n", what); g_fail++; }
    else     { printf("ok   %s\n", what); }
}

// One WsConn holding `frame` as already-read bytes, on a dead fd.
static void primed(WsConn *c, const unsigned char *frame, int n) {
    Conn conn;
    conn_init_plain(&conn, -1);
    ws_conn_init(c, conn, /*mask_outgoing=*/0);
    ws_conn_prime(c, frame, n);
}

// A binary frame header using the 8-byte length form, with `len` written into
// it verbatim - including lengths no honest peer would ever send.
static int hdr_127(unsigned char *out, unsigned long long len) {
    int n = 0;
    out[n++] = 0x82;   // FIN | binary
    out[n++] = 127;    // 8-byte extended length follows
    for (int i = 7; i >= 0; i--) out[n++] = (unsigned char)((len >> (8 * i)) & 0xFF);
    return n;
}

int main(void) {
    unsigned char frame[4096];
    unsigned char buf[2048];
    int opcode = -1;

    // The frame the guard exists for: the top bit of the 64-bit length set.
    // Assembling this into a signed int64 is the undefined behaviour.
    {
        WsConn c;
        const int n = hdr_127(frame, 0x8000000000000000ULL);
        primed(&c, frame, n);
        check(ws_recv_message(&c, buf, (int)sizeof buf, &opcode) == -1,
              "a 64-bit length with the top bit set is refused");
    }

    // Every bit set: the same sign-bit overflow, and the largest value the
    // form can carry.
    {
        WsConn c;
        const int n = hdr_127(frame, 0xFFFFFFFFFFFFFFFFULL);
        primed(&c, frame, n);
        check(ws_recv_message(&c, buf, (int)sizeof buf, &opcode) == -1,
              "a 64-bit length of all ones is refused");
    }

    // Just past INT64_MAX, the first value the sign bit claims.
    {
        WsConn c;
        const int n = hdr_127(frame, 0x8000000000000001ULL);
        primed(&c, frame, n);
        check(ws_recv_message(&c, buf, (int)sizeof buf, &opcode) == -1,
              "INT64_MAX + 1 as a frame length is refused");
    }

    // A length that is positive but far past the caller's buffer: refused
    // before a byte of it is read, not after filling `cap` bytes.
    {
        WsConn c;
        const int n = hdr_127(frame, (unsigned long long)sizeof buf + 1);
        primed(&c, frame, n);
        check(ws_recv_message(&c, buf, (int)sizeof buf, &opcode) == -1,
              "a length past the caller's buffer is refused");
    }

    // And the form still works for a frame that is merely large: the fix must
    // not have turned the 8-byte length into a blanket refusal.
    {
        WsConn c;
        const int payload = 200;
        int n = hdr_127(frame, (unsigned long long)payload);
        for (int i = 0; i < payload; i++) frame[n + i] = (unsigned char)(i & 0xFF);
        n += payload;
        primed(&c, frame, n);
        opcode = -1;
        const int r = ws_recv_message(&c, buf, (int)sizeof buf, &opcode);
        check(r == payload && opcode == WS_OP_BIN, "a legal 8-byte-form frame still reads");
        int same = 1;
        for (int i = 0; i < payload; i++) if (buf[i] != (unsigned char)(i & 0xFF)) same = 0;
        check(same, "and its payload arrives byte for byte");
    }

    // The short form is untouched.
    {
        WsConn c;
        frame[0] = 0x81;   // FIN | text
        frame[1] = 3;
        frame[2] = 'a'; frame[3] = 'b'; frame[4] = 'c';
        primed(&c, frame, 5);
        opcode = -1;
        check(ws_recv_message(&c, buf, (int)sizeof buf, &opcode) == 3 && opcode == WS_OP_TEXT,
              "a short-form frame still reads");
    }

    // The 2-byte form is untouched.
    {
        WsConn c;
        const int payload = 300;
        frame[0] = 0x82; frame[1] = 126;
        frame[2] = (unsigned char)(payload >> 8); frame[3] = (unsigned char)(payload & 0xFF);
        for (int i = 0; i < payload; i++) frame[4 + i] = (unsigned char)(i & 0xFF);
        primed(&c, frame, 4 + payload);
        check(ws_recv_message(&c, buf, (int)sizeof buf, &opcode) == payload,
              "a 2-byte-form frame still reads");
    }

    printf("%s\n", g_fail ? "FAILED" : "all ws frame checks passed");
    return g_fail ? 1 : 0;
}
