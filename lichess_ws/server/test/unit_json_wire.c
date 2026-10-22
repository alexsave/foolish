// unit_json_wire.c - the client protocol codec (json_wire.h): what each message parses to, what
// each frame looks like, the dests stripping and the position regexes of Fens.
#include "json_wire.h"
#include "t.h"

static void parse(const char *s, co_msg *m) { co_parse(s, strlen(s), m); }

static void test_parse(void) {
    co_msg m;
    parse("p", &m); T_CHECK(m.kind == CO_PING && !m.has_lag);
    parse("null", &m); T_CHECK(m.kind == CO_PING);
    parse("{\"t\":\"p\"}", &m); T_CHECK(m.kind == CO_PING && !m.has_lag);
    parse("{\"t\":\"p\",\"l\":57}", &m); T_CHECK(m.kind == CO_PING && m.has_lag && m.lag_ms == 57);
    parse("{\"l\":5,\"t\":\"p\"}", &m); T_CHECK(m.kind == CO_PING && m.has_lag && m.lag_ms == 5);
    parse("[1,2]", &m); T_CHECK(m.kind == CO_UNEXPECTED);
    parse("{\"d\":1}", &m); T_CHECK(m.kind == CO_UNEXPECTED);
    parse("{\"t\":\"whatever\"}", &m); T_CHECK(m.kind == CO_UNEXPECTED);
    parse("not json", &m); T_CHECK(m.kind == CO_UNEXPECTED);
    parse("{\"t\":\"startWatching\",\"d\":\"abcdefgh ijklmnop abcdefgh\"}", &m);
    T_CHECK(m.kind == CO_WATCH && m.n_watch == 2 && strcmp(m.watch[0], "abcdefgh") == 0 && strcmp(m.watch[1], "ijklmnop") == 0);
    parse("{\"t\":\"startWatching\"}", &m); T_CHECK(m.kind == CO_IGNORE);
    parse("{\"t\":\"moveLat\"}", &m); T_CHECK(m.kind == CO_MOVELAT);
    parse("{\"t\":\"notified\"}", &m); T_CHECK(m.kind == CO_NOTIFIED);
    parse("{\"t\":\"following_onlines\"}", &m); T_CHECK(m.kind == CO_FOLLOWING && m.flag);
    parse("{\"t\":\"following_onlines\",\"d\":false}", &m); T_CHECK(m.kind == CO_FOLLOWING && !m.flag);
    parse("{\"t\":\"msgType\",\"d\":\"bob\"}", &m); T_CHECK(m.kind == CO_MSGTYPE && strcmp(m.user, "bob") == 0);
    parse("{\"t\":\"msgSend\",\"d\":{\"dest\":\"bob\",\"text\":\"hi\"}}", &m); T_CHECK(m.kind == CO_USER_FORWARD);
    parse("{\"t\":\"idle\",\"d\":true}", &m); T_CHECK(m.kind == CO_LOBBY_IDLE && m.flag);
    parse("{\"t\":\"idle\"}", &m); T_CHECK(m.kind == CO_UNEXPECTED);
    parse("{\"t\":\"join\",\"d\":{\"id\":\"x\"}}", &m); T_CHECK(m.kind == CO_LOBBY_JOIN);
    parse("{\"t\":\"hookIn\"}", &m); T_CHECK(m.kind == CO_LOBBY_FORWARD);
    parse("{\"t\":\"anaMove\",\"d\":{}}", &m); T_CHECK(m.kind == CO_STUDY_FORWARD);
    // moves
    parse("{\"t\":\"move\",\"d\":{\"u\":\"e2e4\",\"b\":1,\"l\":30,\"s\":\"1a\",\"a\":3}}", &m);
    T_CHECK(m.kind == CO_ROUND_MOVE && strcmp(m.uci, "e2e4") == 0 && m.blur && m.has_lag_ms && m.lag_ms == 30 && m.has_mt && m.mt_centis == 46 && m.has_ack && m.ack == 3);
    parse("{\"t\":\"move\",\"d\":{\"u\":\"e7e8q\"}}", &m);
    T_CHECK(m.kind == CO_ROUND_MOVE && strcmp(m.uci, "e7e8q") == 0 && !m.blur && !m.has_lag_ms && !m.has_mt && !m.has_ack);
    parse("{\"t\":\"move\",\"d\":{\"from\":\"g1\",\"to\":\"f3\"}}", &m); T_CHECK(m.kind == CO_ROUND_MOVE && strcmp(m.uci, "g1f3") == 0);
    parse("{\"t\":\"move\",\"d\":{\"from\":\"a7\",\"to\":\"a8\",\"promotion\":\"n\"}}", &m); T_CHECK(m.kind == CO_ROUND_MOVE && strcmp(m.uci, "a7a8n") == 0);
    parse("{\"t\":\"move\",\"d\":{\"u\":\"e2e9\"}}", &m); T_CHECK(m.kind == CO_UNEXPECTED);
    parse("{\"t\":\"move\",\"d\":{\"u\":\"e2e4e5\"}}", &m); T_CHECK(m.kind == CO_UNEXPECTED);
    parse("{\"t\":\"move\",\"d\":{\"b\":1}}", &m); T_CHECK(m.kind == CO_UNEXPECTED);
    parse("{\"t\":\"move\",\"d\":\"e2e4\"}", &m); T_CHECK(m.kind == CO_UNEXPECTED);
    parse("{\"t\":\"drop\",\"d\":{\"role\":\"knight\",\"pos\":\"f3\",\"a\":1}}", &m);
    T_CHECK(m.kind == CO_ROUND_MOVE && strcmp(m.uci, "N@f3") == 0 && m.has_ack && m.ack == 1);
    parse("{\"t\":\"drop\",\"d\":{\"role\":\"dragon\",\"pos\":\"f3\"}}", &m); T_CHECK(m.kind == CO_UNEXPECTED);
    parse("{\"t\":\"hold\",\"d\":{\"mean\":200,\"sd\":15}}", &m); T_CHECK(m.kind == CO_ROUND_HOLD && m.mean == 200 && m.sd == 15);
    parse("{\"t\":\"hold\",\"d\":{\"mean\":200}}", &m); T_CHECK(m.kind == CO_UNEXPECTED);
    parse("{\"t\":\"berserk\",\"d\":{\"a\":9}}", &m); T_CHECK(m.kind == CO_ROUND_BERSERK && m.has_ack && m.ack == 9);
    parse("{\"t\":\"berserk\"}", &m); T_CHECK(m.kind == CO_ROUND_BERSERK && !m.has_ack);
    parse("{\"t\":\"rep\",\"d\":{\"n\":\"shift\"}}", &m); T_CHECK(m.kind == CO_ROUND_REPORT && strcmp(m.text, "shift") == 0);
    parse("{\"t\":\"flag\",\"d\":\"white\"}", &m); T_CHECK(m.kind == CO_ROUND_FLAG && m.color == 0);
    parse("{\"t\":\"flag\",\"d\":\"black\"}", &m); T_CHECK(m.kind == CO_ROUND_FLAG && m.color == 1);
    parse("{\"t\":\"flag\",\"d\":\"green\"}", &m); T_CHECK(m.kind == CO_UNEXPECTED);
    parse("{\"t\":\"bye2\"}", &m); T_CHECK(m.kind == CO_ROUND_BYE);
    parse("{\"t\":\"resign\"}", &m); T_CHECK(m.kind == CO_ROUND_FORWARD);
    parse("{\"t\":\"draw-yes\"}", &m); T_CHECK(m.kind == CO_ROUND_FORWARD);
    parse("{\"t\":\"talk\",\"d\":\"gg \\\"wp\\\"\"}", &m); T_CHECK(m.kind == CO_CHAT_SAY && strcmp(m.text, "gg \"wp\"") == 0);
    parse("{\"t\":\"timeout\",\"d\":{\"userId\":\"troll\",\"reason\":\"spam\",\"text\":\"buy\"}}", &m);
    T_CHECK(m.kind == CO_CHAT_TIMEOUT && strcmp(m.user, "troll") == 0 && strcmp(m.reason, "spam") == 0 && strcmp(m.text, "buy") == 0);
    parse("{\"t\":\"ping\"}", &m); T_CHECK(m.kind == CO_CHALLENGE_PING);
    parse("{\"t\":\"opening\"}", &m); T_CHECK(m.kind == CO_IGNORE);
    parse("{\"t\":\"evalGet\",\"d\":{}}", &m); T_CHECK(m.kind == CO_IGNORE);
    const char *raw = "{\"t\":\"cancel\",\"d\":1}";
    parse(raw, &m); T_CHECK(m.kind == CO_LOBBY_FORWARD && m.raw.n == strlen(raw) && memcmp(m.raw.p, raw, m.raw.n) == 0);
}

static void test_write(void) {
    cb_buf b;
    cb_buf_init(&b);
    ci_msg_raw(&b, "mlat", "12", 2); T_STREQ(cb_buf_cstr(&b), "{\"t\":\"mlat\",\"d\":12}"); cb_buf_reset(&b);
    ci_msg_str(&b, "msgType", "al\"ice"); T_STREQ(cb_buf_cstr(&b), "{\"t\":\"msgType\",\"d\":\"al\\\"ice\"}"); cb_buf_reset(&b);
    ci_msg_int(&b, "ack", 5); T_STREQ(cb_buf_cstr(&b), "{\"t\":\"ack\",\"d\":5}"); cb_buf_reset(&b);
    ci_msg_bool(&b, "gone", false); T_STREQ(cb_buf_cstr(&b), "{\"t\":\"gone\",\"d\":false}"); cb_buf_reset(&b);
    ci_msg_bare(&b, "resync"); T_STREQ(cb_buf_cstr(&b), "{\"t\":\"resync\"}"); cb_buf_reset(&b);
    ci_versioned(&b, "move", 7, "{\"a\":1}", 7); T_STREQ(cb_buf_cstr(&b), "{\"t\":\"move\",\"v\":7,\"d\":{\"a\":1}}"); cb_buf_reset(&b);
    ci_skip(&b, 7); T_STREQ(cb_buf_cstr(&b), "{\"v\":7}"); cb_buf_reset(&b);
    ci_room_versioned(&b, 3, "{\"t\":\"x\",\"d\":1}", 15); T_STREQ(cb_buf_cstr(&b), "{\"v\":3,\"t\":\"x\",\"d\":1}"); cb_buf_reset(&b);
    ci_lobby_pong(&b, 1200, 34); T_STREQ(cb_buf_cstr(&b), "{\"t\":\"n\",\"d\":1200,\"r\":34}"); cb_buf_reset(&b);
    ci_fen(&b, "abcdefgh", "e2e4", "8/8/8/8/8/8/8/8", 'b', true, 60, 59);
    T_STREQ(cb_buf_cstr(&b), "{\"t\":\"fen\",\"d\":{\"id\":\"abcdefgh\",\"lm\":\"e2e4\",\"fen\":\"8/8/8/8/8/8/8/8 b\",\"wc\":60,\"bc\":59}}"); cb_buf_reset(&b);
    ci_fen(&b, "abcdefgh", "e2e4", "8/8/8/8/8/8/8/8", 'w', false, 0, 0);
    T_STREQ(cb_buf_cstr(&b), "{\"t\":\"fen\",\"d\":{\"id\":\"abcdefgh\",\"lm\":\"e2e4\",\"fen\":\"8/8/8/8/8/8/8/8 w\"}}"); cb_buf_reset(&b);
    ci_finish(&b, "abcdefgh", -1); T_STREQ(cb_buf_cstr(&b), "{\"t\":\"finish\",\"d\":{\"id\":\"abcdefgh\",\"win\":null}}"); cb_buf_reset(&b);
    ci_finish(&b, "abcdefgh", 1); T_STREQ(cb_buf_cstr(&b), "{\"t\":\"finish\",\"d\":{\"id\":\"abcdefgh\",\"win\":\"b\"}}"); cb_buf_reset(&b);
    ci_redirect(&b, "abcdefghijkl"); T_STREQ(cb_buf_cstr(&b), "{\"t\":\"redirect\",\"d\":{\"id\":\"abcdefghijkl\",\"url\":\"/abcdefghijkl\"}}"); cb_buf_reset(&b);
    const char *names[2] = { "GM Alice", "Bob" };
    ci_round_crowd(&b, true, false, 0, NULL, 0); T_STREQ(cb_buf_cstr(&b), "{\"t\":\"crowd\",\"d\":{\"white\":true,\"black\":false,\"watchers\":{}}}"); cb_buf_reset(&b);
    ci_round_crowd(&b, true, true, 2, names, 2); T_STREQ(cb_buf_cstr(&b), "{\"t\":\"crowd\",\"d\":{\"white\":true,\"black\":true,\"watchers\":{\"users\":[\"GM Alice\",\"Bob\"]}}}"); cb_buf_reset(&b);
    ci_round_crowd(&b, false, false, 11, names, 2); T_STREQ(cb_buf_cstr(&b), "{\"t\":\"crowd\",\"d\":{\"white\":false,\"black\":false,\"watchers\":{\"nb\":11}}}"); cb_buf_reset(&b);
    // dests stripping: the regex ,"dests":\{[^\}]+} removes each match, keeps an empty object
    const char *full = "{\"t\":\"move\",\"v\":1,\"d\":{\"uci\":\"e2e4\",\"dests\":{\"a7\":\"a6a5\",\"b8\":\"a6c6\"},\"clock\":{\"white\":1}}}";
    ci_strip_dests(&b, full, strlen(full));
    T_STREQ(cb_buf_cstr(&b), "{\"t\":\"move\",\"v\":1,\"d\":{\"uci\":\"e2e4\",\"clock\":{\"white\":1}}}"); cb_buf_reset(&b);
    const char *empty = "{\"d\":{\"uci\":\"e2e4\",\"dests\":{}}}";
    ci_strip_dests(&b, empty, strlen(empty)); T_STREQ(cb_buf_cstr(&b), empty); cb_buf_reset(&b);
    const char *none = "{\"d\":{\"uci\":\"e2e4\"}}";
    ci_strip_dests(&b, none, strlen(none)); T_STREQ(cb_buf_cstr(&b), none); cb_buf_reset(&b);
    cb_buf_free(&b);
}

static void test_position(void) {
    char uci[8], fen[96];
    bool clock; int wc, bc;
    const char *ev = "{\"uci\":\"h2g2\",\"san\":\"Rg2\",\"fen\":\"r2qb1k1/p2nbrpn/6Np/3pPp1P/1ppP1P2/2P1B3/PP2B1R1/R2Q1NK1\",\"ply\":40,\"dests\":{},\"clock\":{\"white\":121.88,\"black\":120.94}}";
    T_CHECK(co_move_position(ev, strlen(ev), uci, fen, &clock, &wc, &bc));
    T_STREQ(uci, "h2g2");
    T_STREQ(fen, "r2qb1k1/p2nbrpn/6Np/3pPp1P/1ppP1P2/2P1B3/PP2B1R1/R2Q1NK1");
    T_CHECK(clock && wc == 121 && bc == 120);
    const char *noclock = "{\"uci\":\"e2e4\",\"san\":\"e4\",\"fen\":\"8/8/8/8/8/8/8/8\",\"ply\":1}";
    T_CHECK(co_move_position(noclock, strlen(noclock), uci, fen, &clock, &wc, &bc) && !clock && strcmp(uci, "e2e4") == 0);
    // crazyhouse pockets name the colors with objects, not numbers: the clock still wins
    const char *zh = "{\"uci\":\"e2e4\",\"fen\":\"8/8/8/8/8/8/8/8\",\"clock\":{\"white\":30,\"black\":31},\"crazyhouse\":{\"pockets\":{\"white\":{\"pawn\":1},\"black\":{}}}}";
    T_CHECK(co_move_position(zh, strlen(zh), uci, fen, &clock, &wc, &bc) && clock && wc == 30 && bc == 31);
    const char *bad = "{\"san\":\"e4\"}";
    T_CHECK(!co_move_position(bad, strlen(bad), uci, fen, &clock, &wc, &bc));
}

int main(void) {
    test_parse();
    test_write();
    test_position();
    return t_done("unit_json_wire");
}
