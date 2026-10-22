// unit_bson.c - the BSON writer and reader (bson.h): a document written is read back field by
// field, nested documents and arrays included, and a cut or malformed buffer is refused.
#include "bson.h"
#include "t.h"

int main(void) {
    cb_buf b;
    cb_buf_init(&b);
    bson_w w;
    bson_w_init(&w, &b);
    bson_w_begin(&w);
    bson_w_str(&w, "_id", "abcdefgh");
    bson_w_bool(&w, "up", true);
    bson_w_int32(&w, "n", -5);
    bson_w_int64(&w, "big", 1LL << 40);
    bson_w_date(&w, "at", 1700000000000LL);
    bson_w_array(&w, "us");
    bson_w_str(&w, "0", "alice");
    bson_w_str(&w, "1", "bob");
    bson_w_end(&w);
    bson_w_doc(&w, "plan");
    bson_w_bool(&w, "active", false);
    bson_w_end(&w);
    bson_w_end(&w);
    T_CHECK(bson_w_ok(&w));
    // the spec's own example: {"hello": "world"} is 16 00 00 00 02 hello 00 06 00 00 00 world 00 00
    cb_buf h;
    cb_buf_init(&h);
    bson_w hw;
    bson_w_init(&hw, &h); bson_w_begin(&hw); bson_w_str(&hw, "hello", "world"); bson_w_end(&hw);
    static const uint8_t HELLO[] = { 0x16, 0, 0, 0, 0x02, 'h', 'e', 'l', 'l', 'o', 0, 0x06, 0, 0, 0, 'w', 'o', 'r', 'l', 'd', 0, 0 };
    T_CHECK(h.len == sizeof HELLO && memcmp(h.p, HELLO, sizeof HELLO) == 0);
    cb_buf_free(&h);

    bson_doc d;
    T_CHECK(bson_doc_init(&d, b.p, b.len));
    T_CHECK(d.len == b.len);
    bson_el el;
    const char *s; size_t n; bool v; int64_t i;
    T_CHECK(bson_find(&d, "_id", &el) && bson_el_str(&el, &s, &n) && n == 8 && memcmp(s, "abcdefgh", 8) == 0 && s[8] == 0);
    T_CHECK(bson_find(&d, "up", &el) && bson_el_bool(&el, &v) && v);
    T_CHECK(bson_find(&d, "n", &el) && bson_el_int(&el, &i) && i == -5);
    T_CHECK(bson_find(&d, "big", &el) && bson_el_int(&el, &i) && i == (1LL << 40));
    T_CHECK(bson_find(&d, "at", &el) && el.type == BSON_DATE && bson_el_int(&el, &i) && i == 1700000000000LL);
    T_CHECK(!bson_find(&d, "missing", &el));
    T_CHECK(bson_find(&d, "up", &el) && !bson_el_str(&el, &s, &n) && !bson_el_int(&el, &i));
    bson_doc us;
    T_CHECK(bson_find(&d, "us", &el) && el.type == BSON_ARRAY && bson_el_doc(&el, &us));
    size_t it = 0;
    int count = 0;
    while (bson_next(&us, &it, &el)) {
        T_CHECK(bson_el_str(&el, &s, &n));
        if (count == 0) T_CHECK(n == 5 && memcmp(s, "alice", 5) == 0 && strcmp(el.key, "0") == 0);
        if (count == 1) T_CHECK(n == 3 && memcmp(s, "bob", 3) == 0);
        count++;
    }
    T_CHECK(count == 2);
    bson_doc plan;
    T_CHECK(bson_find(&d, "plan", &el) && bson_el_doc(&el, &plan) && bson_find(&plan, "active", &el) && bson_el_bool(&el, &v) && !v);
    // a double with no fraction reads as an int (the "ok": 1.0 of every reply)
    cb_buf o;
    cb_buf_init(&o);
    static const uint8_t OK[] = { 0x11, 0, 0, 0, 0x01, 'o', 'k', 0, 0, 0, 0, 0, 0, 0, 0xF0, 0x3F, 0 };   // {ok: 1.0}
    T_CHECK(bson_doc_init(&d, OK, sizeof OK) && bson_find(&d, "ok", &el) && bson_el_int(&el, &i) && i == 1);
    cb_buf_free(&o);
    // malformed: a length longer than the buffer, a missing terminator, an unknown type, a string overrunning
    T_CHECK(!bson_doc_init(&d, b.p, b.len - 1));
    uint8_t bad[sizeof HELLO];
    memcpy(bad, HELLO, sizeof bad);
    bad[sizeof bad - 1] = 1;
    T_CHECK(!bson_doc_init(&d, bad, sizeof bad));
    memcpy(bad, HELLO, sizeof bad);
    bad[4] = 0x7F;   // type nobody reads
    T_CHECK(bson_doc_init(&d, bad, sizeof bad) && !bson_find(&d, "hello", &el));
    memcpy(bad, HELLO, sizeof bad);
    bad[11] = 0x40;  // string length past the document
    T_CHECK(bson_doc_init(&d, bad, sizeof bad) && !bson_find(&d, "hello", &el));
    T_CHECK(!bson_doc_init(&d, HELLO, 3));
    cb_buf_free(&b);
    return t_done("unit_bson");
}
