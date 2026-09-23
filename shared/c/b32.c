#include "b32.h"

static const char ALPHA[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";

int b32_decode(const char *s, unsigned char *out, int cap) {
    int bits = 0, value = 0, n = 0;
    for (; s && *s; s++) {
        char c = *s;
        if (c == '-') break;                 // a suffix begins here
        int idx;
        if (c >= 'A' && c <= 'Z') idx = c - 'A';
        else if (c >= 'a' && c <= 'z') idx = c - 'a';
        else if (c >= '2' && c <= '7') idx = c - '2' + 26;
        else continue;                       // stray chars ('.', '/', ...) are skipped
        value = ((value << 5) | idx) & 0xfff; // at most 12 live bits: no overflow
        bits += 5;
        if (bits >= 8) {
            if (n >= cap) return -1;
            out[n++] = (unsigned char)((value >> (bits - 8)) & 0xFF);
            bits -= 8;
        }
    }
    return n;
}

int b32_encode(const unsigned char *in, int n, char *out, int cap) {
    int bits = 0, value = 0, w = 0;
    if (cap < 1) return -1;
    for (int i = 0; i < n; i++) {
        value = ((value << 8) | in[i]) & 0xfff; // at most 12 live bits
        bits += 8;
        while (bits >= 5) {
            if (w >= cap - 1) return -1;
            out[w++] = ALPHA[(value >> (bits - 5)) & 31];
            bits -= 5;
        }
    }
    if (bits > 0) {
        if (w >= cap - 1) return -1;
        out[w++] = ALPHA[(value << (5 - bits)) & 31];
    }
    out[w] = 0;
    return w;
}
