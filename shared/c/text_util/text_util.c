// See text_util.h.
#include "text_util.h"
#include <string.h>

unsigned text_next_cp(const unsigned char *s, int *len)
{
    if (s[0] < 0x80) { *len = 1; return s[0]; }
    int n = s[0] >= 0xf0 ? 4 : s[0] >= 0xe0 ? 3 : 2;
    unsigned c = s[0] & (0x3fu >> (n - 1));
    for (int i = 1; i < n; i++) {
        if ((s[i] & 0xc0) != 0x80) { *len = i; return 0xfffd; }
        c = (c << 6) | (s[i] & 0x3f);
    }
    *len = n;
    return c;
}

int text_cp_cols(unsigned c)
{
    if ((c >= 0x0300 && c <= 0x036f) || c == 0x200b || c == 0x200d || c == 0x200e
        || c == 0x200f || (c >= 0xfe00 && c <= 0xfe0f))
        return 0;
    if ((c >= 0x1100 && c <= 0x115f) || (c >= 0x2e80 && c <= 0xa4cf) ||
        (c >= 0xac00 && c <= 0xd7a3) || (c >= 0xf900 && c <= 0xfaff) ||
        (c >= 0xfe30 && c <= 0xfe4f) || (c >= 0xff00 && c <= 0xff60) ||
        (c >= 0xffe0 && c <= 0xffe6) || c >= 0x1f300)
        return 2;
    return 1;
}

int text_cols(const char *s)
{
    int cols = 0;
    for (const unsigned char *p = (const unsigned char *)s; p && *p;) {
        int n;
        cols += text_cp_cols(text_next_cp(p, &n));
        p += n;
    }
    return cols;
}

int text_itoa(int v, char *out, int cap)
{
    char tmp[12];
    int n = 0, neg = v < 0;
    unsigned u = neg ? 0u - (unsigned)v : (unsigned)v;
    do { tmp[n++] = (char)('0' + u % 10u); u /= 10u; } while (u);
    if (neg) tmp[n++] = '-';
    if (!out || n + 1 > cap) return -1;
    for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    out[n] = 0;
    return n;
}

int text_put(char *out, int cap, const char *s)
{
    int n = (int)strlen(s);
    if (!out || n >= cap) return -1;
    memcpy(out, s, (size_t)n + 1);
    return n;
}

// The value `name` (len bytes, not NUL-terminated) has in `kv`, or NULL.
static const char *lookup(const char *const *kv, const char *name, size_t len)
{
    for (int i = 0; kv && kv[i]; i += 2)
        if (len == strlen(kv[i]) && !strncmp(name, kv[i], len))
            return kv[i + 1] ? kv[i + 1] : "";
    return 0;
}

int text_fill(char *out, int cap, const char *t, const char *const *kv, const char *const *fallback)
{
    if (!out || cap < 1 || !t) return -1;
    int o = 0;
    for (const char *p = t; *p;) {
        const char *v = 0;
        int skip = 1;
        if (*p == '{') {
            const char *e = p + 1;
            while (*e && *e != '}' && *e != '{') e++;
            if (*e == '}') {
                size_t len = (size_t)(e - p - 1);
                v = lookup(kv, p + 1, len);
                if (!v) v = lookup(fallback, p + 1, len);
                if (v) skip = (int)(e - p) + 1;
            }
        }
        if (v) {
            int n = (int)strlen(v);
            if (o + n >= cap) return -1;
            memcpy(out + o, v, (size_t)n);
            o += n;
        } else {
            if (o + 1 >= cap) return -1;
            out[o++] = *p;
        }
        p += skip;
    }
    out[o] = 0;
    return o;
}
