// ctl_wire_test.c - the control-plane codec against itself and against
// hostile bytes.
//
// Two properties, and they are the two the old JSON scrapers could not have:
//   1. ROUND TRIP. Every frame the server can emit decodes back to exactly the
//      values it was built from - so a client and the server can never read
//      the same bytes differently.
//   2. REFUSAL, not repair. A frame with the wrong version, a length that
//      disagrees with the bytes present, a field that runs off the end, or a
//      string too long for the reader's buffer is REFUSED WHOLE. The scraper
//      this replaced would happily return half a token.
//
// Kernel-free and socket-free, so it builds and runs on macOS too (no epoll).
//   make ctl-wire-test
#include <stdio.h>
#include <string.h>

#include "ctl_wire.h"

static int g_fail = 0;

#define CHECK(cond, ...) do { \
    if (!(cond)) { g_fail++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
                   fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } \
} while (0)

// ---------------------------------------------------------------------------

static void test_auth(void) {
    unsigned char f[CTL_FRAME_MAX];
    const int n = ctl_enc_auth("alice", f, sizeof f);
    CHECK(n == CTL_HEAD_LEN + 1 + 5, "auth frame is %d bytes, expected %d", n, CTL_HEAD_LEN + 6);
    CHECK(ctl_kind(f, n) == CTL_AUTH, "auth kind is %d", ctl_kind(f, n));

    char name[CTL_NAME_CAP] = {0};
    CHECK(ctl_dec_auth(f, n, name, sizeof name) == 1, "auth did not decode");
    CHECK(strcmp(name, "alice") == 0, "auth username came back \"%s\"", name);

    // An empty username is a legal frame (the server then refuses it on the
    // roster's rule, not the wire's).
    const int e = ctl_enc_auth("", f, sizeof f);
    char empty[CTL_NAME_CAP] = {0};
    CHECK(ctl_dec_auth(f, e, empty, sizeof empty) == 1, "empty-username auth did not decode");
    CHECK(empty[0] == 0, "empty username came back \"%s\"", empty);

    // A username longer than the reader's buffer is REFUSED, never truncated:
    // half a name is worse than none because it looks usable.
    char big[64];
    memset(big, 'A', sizeof big - 1); big[sizeof big - 1] = 0;
    const int b = ctl_enc_auth(big, f, sizeof f);
    char small[8];
    CHECK(b > 0, "a 63-char username should still encode");
    CHECK(ctl_dec_auth(f, b, small, sizeof small) == 0, "an oversized username was not refused");
    // Same refusal at the codec's OWN field cap, not just the caller's: a
    // 63-char name does not fit CTL_NAME_CAP either, and must come back
    // refused rather than as its first 23 characters.
    char full[CTL_NAME_CAP] = {0};
    CHECK(ctl_dec_auth(f, b, full, sizeof full) == 0,
          "a username past CTL_NAME_CAP was truncated to \"%s\" instead of refused", full);

    // A string longer than the one-byte length prefix can express is not a
    // truncation decision either - the shape simply cannot carry it, so the
    // encode fails. Given a buffer with PLENTY of room (so only the prefix's
    // own limit can refuse it): a 299-byte string whose length wrapped to 43
    // would put a frame on the wire that claims 43 bytes and carries 299.
    char huge[300];
    memset(huge, 'B', sizeof huge - 1); huge[sizeof huge - 1] = 0;
    unsigned char roomy[512];
    CHECK(ctl_enc_auth(huge, roomy, (int)sizeof roomy) == -1, "a 299-char username was encoded anyway");
}

static void test_meta(void) {
    unsigned char f[CTL_FRAME_MAX];
    int n = ctl_enc_meta(CTL_META_ADD_BOT, "abc123def456", "cordite", f, sizeof f);
    CtlMeta m;
    memset(&m, 0xAB, sizeof m);
    CHECK(ctl_dec_meta(f, n, &m) == 1, "add-bot meta did not decode");
    CHECK(m.verb == CTL_META_ADD_BOT, "verb came back %d", m.verb);
    CHECK(strcmp(m.game_id, "abc123def456") == 0, "game_id came back \"%s\"", m.game_id);
    CHECK(strcmp(m.strategy, "cordite") == 0, "strategy came back \"%s\"", m.strategy);

    // No strategy: the field is present and empty, which is how the server
    // reads "default to random" - the same thing a missing JSON key meant.
    n = ctl_enc_meta(CTL_META_START, "abc123def456", "", f, sizeof f);
    memset(&m, 0xAB, sizeof m);
    CHECK(ctl_dec_meta(f, n, &m) == 1, "start meta did not decode");
    CHECK(m.verb == CTL_META_START, "verb came back %d", m.verb);
    CHECK(m.strategy[0] == 0, "strategy should be empty, came back \"%s\"", m.strategy);

    // NULL strategy encodes as the empty one, so a caller with nothing to say
    // does not have to invent a string.
    n = ctl_enc_meta(CTL_META_JOIN, "abc123def456", NULL, f, sizeof f);
    memset(&m, 0xAB, sizeof m);
    CHECK(ctl_dec_meta(f, n, &m) == 1, "join meta with a NULL strategy did not decode");
    CHECK(m.strategy[0] == 0, "NULL strategy came back \"%s\"", m.strategy);

    // An UNRECOGNIZED verb is a well-formed frame. The decoder must not refuse
    // it: the server's own switch simply matches no branch and answers anyway,
    // exactly as an unrecognized JSON "type" string always did. A decoder that
    // rejected here would turn that 200 into a 404.
    n = ctl_enc_meta(200, "abc123def456", "", f, sizeof f);
    memset(&m, 0xAB, sizeof m);
    CHECK(ctl_dec_meta(f, n, &m) == 1, "an unknown verb must still decode");
    CHECK(m.verb == 200, "unknown verb came back %d", m.verb);
}

static void test_responses(void) {
    unsigned char f[CTL_FRAME_MAX];

    const int sn = ctl_enc_session("0123456789ab", "alice", "tok.en-VALUE_9", f, sizeof f);
    CtlSession s;
    memset(&s, 0xAB, sizeof s);
    CHECK(ctl_dec_session(f, sn, &s) == 1, "session did not decode");
    CHECK(strcmp(s.user_id, "0123456789ab") == 0, "user_id came back \"%s\"", s.user_id);
    CHECK(strcmp(s.username, "alice") == 0, "username came back \"%s\"", s.username);
    CHECK(strcmp(s.token, "tok.en-VALUE_9") == 0, "token came back \"%s\"", s.token);

    const int gn = ctl_enc_game("feedfacecafe", f, sizeof f);
    char gid[CTL_ID_CAP] = {0};
    CHECK(ctl_dec_game(f, gn, gid, sizeof gid) == 1, "game did not decode");
    CHECK(strcmp(gid, "feedfacecafe") == 0, "game_id came back \"%s\"", gid);

    const int ln = ctl_enc_lobby("feedfacecafe", 1, f, sizeof f);
    char lgid[CTL_ID_CAP] = {0}; int status = -99;
    CHECK(ctl_dec_lobby(f, ln, lgid, sizeof lgid, &status) == 1, "lobby did not decode");
    CHECK(strcmp(lgid, "feedfacecafe") == 0, "lobby game_id came back \"%s\"", lgid);
    CHECK(status == 1, "lobby status came back %d", status);

    const int an = ctl_enc_applied(true, 2, f, sizeof f);
    bool ok = false; int st = -99;
    CHECK(ctl_dec_applied(f, an, &ok, &st) == 1, "applied did not decode");
    CHECK(ok == true, "applied ok came back false");
    CHECK(st == 2, "applied status came back %d", st);

    const int an2 = ctl_enc_applied(false, 0, f, sizeof f);
    ok = true; st = -99;
    CHECK(ctl_dec_applied(f, an2, &ok, &st) == 1, "rejected applied did not decode");
    CHECK(ok == false, "rejected applied ok came back true");
    CHECK(st == 0, "rejected applied status came back %d", st);

    // /status answers -1 for a game that does not exist. A plain byte would
    // report that as 255, which is why the field is signed.
    for (int want = -1; want <= 2; want++) {
        const int n = ctl_enc_status(want, f, sizeof f);
        int got = -99;
        CHECK(ctl_dec_status(f, n, &got) == 1, "status %d did not decode", want);
        CHECK(got == want, "status %d came back %d", want, got);
    }

    const int hn = ctl_enc_health(f, sizeof f);
    CHECK(hn == CTL_HEAD_LEN, "health frame is %d bytes, expected %d", hn, CTL_HEAD_LEN);
    CHECK(ctl_kind(f, hn) == CTL_HEALTH, "health kind is %d", ctl_kind(f, hn));

    const int en = ctl_enc_error(CTL_ERR_NOT_YOUR_SEAT, f, sizeof f);
    int reason = -99;
    CHECK(ctl_dec_error(f, en, &reason) == 1, "error did not decode");
    CHECK(reason == CTL_ERR_NOT_YOUR_SEAT, "error reason came back %d", reason);
}

static void test_stats(void) {
    CtlStats in = {
        .live_connections = 7, .max_connections = 0, .games = 1234, .games_live = 1200,
        .free_slots = 34, .users = 999,
        .games_reclaimed = 0x0102030405060708ULL, .moves_applied = 1ULL << 40,
        .bot_decisions = 42, .octogen_decisions = 0,
    };
    unsigned char f[CTL_FRAME_MAX];
    const int n = ctl_enc_stats(&in, f, sizeof f);
    CHECK(n == CTL_HEAD_LEN + CTL_STATS_PAYLOAD_LEN, "stats frame is %d bytes, expected %d",
          n, CTL_HEAD_LEN + CTL_STATS_PAYLOAD_LEN);
    CtlStats out;
    memset(&out, 0xAB, sizeof out);
    CHECK(ctl_dec_stats(f, n, &out) == 1, "stats did not decode");
    CHECK(memcmp(&in, &out, sizeof in) == 0, "stats did not round-trip byte-identically");

    // The wire is little-endian by contract, not by whatever this host is.
    // games_reclaimed sits at payload offset 24; its low byte must be 0x08.
    CHECK(f[CTL_HEAD_LEN + 24] == 0x08, "games_reclaimed is not little-endian (first byte 0x%02x)",
          f[CTL_HEAD_LEN + 24]);
    CHECK(f[CTL_HEAD_LEN + 31] == 0x01, "games_reclaimed is not little-endian (last byte 0x%02x)",
          f[CTL_HEAD_LEN + 31]);
}

// The half of the contract a scraper could never hold: malformed input is
// REFUSED, not repaired.
static void test_refusals(void) {
    unsigned char f[CTL_FRAME_MAX];
    const int n = ctl_enc_session("0123456789ab", "alice", "tok", f, sizeof f);
    CtlSession s;

    unsigned char bad[CTL_FRAME_MAX];

    memcpy(bad, f, (size_t)n); bad[0] = CTL_WIRE_VERSION + 1;
    CHECK(ctl_dec_session(bad, n, &s) == 0, "a wrong version was accepted");
    CHECK(ctl_kind(bad, n) == -1, "a wrong version reported a kind");

    memcpy(bad, f, (size_t)n); bad[1] = CTL_GAME;
    CHECK(ctl_dec_session(bad, n, &s) == 0, "a session decode accepted a game frame");

    // Truncated: the declared payload length no longer matches the bytes here.
    CHECK(ctl_dec_session(f, n - 1, &s) == 0, "a truncated frame was accepted");
    // Trailing junk: same disagreement, the other way.
    memcpy(bad, f, (size_t)n); bad[n] = 0x7f;
    CHECK(ctl_dec_session(bad, n + 1, &s) == 0, "a frame with trailing junk was accepted");

    // A head alone, and less than a head.
    CHECK(ctl_dec_session(f, 0, &s) == 0, "a zero-length body was accepted");
    CHECK(ctl_dec_session(f, 3, &s) == 0, "a short head was accepted");
    CHECK(ctl_kind(NULL, 0) == -1, "a NULL body reported a kind");

    // A payload whose declared length is HONEST but which carries more than
    // the frame's fields account for. The head check cannot see this one - the
    // bytes really are there - so it is the "every field read, nothing left
    // over" rule that has to refuse it. CTL_ERROR is one byte of payload;
    // declare two.
    unsigned char extra[CTL_HEAD_LEN + 2];
    extra[0] = CTL_WIRE_VERSION; extra[1] = CTL_ERROR; extra[2] = 2; extra[3] = 0;
    extra[CTL_HEAD_LEN + 0] = CTL_ERR_AUTH; extra[CTL_HEAD_LEN + 1] = 0x99;
    int spurious = -99;
    CHECK(ctl_dec_error(extra, (int)sizeof extra, &spurious) == 0,
          "an error frame with an unaccounted-for trailing byte was accepted (reason %d)", spurious);

    // The mirror of that for a string frame: the head length is honest, every
    // string decodes, and one byte is still left over.
    unsigned char over[CTL_HEAD_LEN + 5];
    over[0] = CTL_WIRE_VERSION; over[1] = CTL_GAME; over[2] = 5; over[3] = 0;
    over[CTL_HEAD_LEN + 0] = 3;
    over[CTL_HEAD_LEN + 1] = 'a'; over[CTL_HEAD_LEN + 2] = 'b'; over[CTL_HEAD_LEN + 3] = 'c';
    over[CTL_HEAD_LEN + 4] = 'X';
    char leftover[CTL_ID_CAP] = {0};
    CHECK(ctl_dec_game(over, (int)sizeof over, leftover, sizeof leftover) == 0,
          "a game frame with an unaccounted-for trailing byte was accepted (\"%s\")", leftover);

    // A string whose length prefix runs past the payload. Hand-build it: head
    // says 4 payload bytes, the first string claims 40.
    unsigned char run[CTL_HEAD_LEN + 4];
    run[0] = CTL_WIRE_VERSION; run[1] = CTL_SESSION; run[2] = 4; run[3] = 0;
    run[CTL_HEAD_LEN + 0] = 40; run[CTL_HEAD_LEN + 1] = 'x';
    run[CTL_HEAD_LEN + 2] = 'y'; run[CTL_HEAD_LEN + 3] = 'z';
    CHECK(ctl_dec_session(run, (int)sizeof run, &s) == 0, "a string running past the payload was accepted");

    // A declared payload length that LIES the other way: the bytes of a real
    // session frame, with the head claiming an empty payload. Only the head's
    // own length check can see this - every field still reads cleanly.
    memcpy(bad, f, (size_t)n); bad[2] = 0; bad[3] = 0;
    CHECK(ctl_dec_session(bad, n, &s) == 0, "a frame whose declared length lies was accepted");

    // Encoding into a buffer that cannot hold the frame returns -1 and writes
    // no usable frame, rather than a truncated one.
    CHECK(ctl_enc_session("0123456789ab", "alice", "tok", f, 6) == -1, "a too-small encode buffer was accepted");
    CHECK(ctl_enc_health(f, 2) == -1, "a too-small health encode was accepted");
    // A frame of nothing but fixed bytes (no strings), one byte short of room:
    // the per-field cap check is the only thing that can refuse this one.
    CHECK(ctl_enc_applied(true, 1, f, CTL_HEAD_LEN + 1) == -1, "a one-byte-short applied encode was accepted");
}

int main(void) {
    test_auth();
    test_meta();
    test_responses();
    test_stats();
    test_refusals();
    if (g_fail) { fprintf(stderr, "ctl_wire_test: FAIL (%d)\n", g_fail); return 1; }
    fprintf(stderr, "ctl_wire_test: OK\n");
    return 0;
}
