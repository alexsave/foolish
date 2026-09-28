/* rs_scan.c - what a Release build must not carry, found in raw bytes.
 *
 *   rs_scan FILE...
 *
 * For every file, prints one line per finding and exits 1 if there was any:
 *   FILE: dev-file NAME      a `dev.<name>` dev-file name, or a bare "dev."
 *                            string a name is appended to at run time (DEBUG
 *                            plumbing that leaked into a shipping build)
 *   FILE: dev-flags          the DevFlags type's name: the dev-file reader
 *                            itself is compiled in (a Swift type keeps its
 *                            name in its descriptor even in a stripped binary)
 *   FILE: em-dash "TEXT"     U+2014, with the readable text around it
 *
 * Raw bytes, not `strings`: `strings` prints ASCII runs only, so it can never
 * see an em dash. Text reaches a binary in four shapes, and each is read:
 *   - UTF-8 in a row: a C string, a long Swift literal (__cstring), a text
 *     resource, an asset catalog's names;
 *   - UTF-16, either byte order and either alignment: an Objective-C @"..."
 *     with non-ASCII (__ustring) and UTF-16 text resources. Only ASCII
 *     neighbours count as its readable context, because in machine code
 *     almost any 16-bit unit is some CJK character;
 *   - a Swift SMALL string (15 UTF-8 bytes or fewer), which never lies in a
 *     row: arm64 code builds its two 64-bit words from mov/movk immediates.
 *     Those words are rebuilt from the instructions of every Mach-O file and
 *     read as text (arm64 only: it is the one architecture a device runs);
 *   - a string built at run time from pieces: only its "dev." piece is
 *     caught, as a whole string of its own.
 * An em dash is reported only when it sits in readable text (at least
 * MIN_CTX printable bytes around it), because E2 80 94 also occurs by chance
 * inside machine code and compressed images; the text is printed so a reader
 * can judge each hit. A PAIR of em dashes is the Chinese dash and is allowed
 * only beside non-ASCII text. The caller converts binary plists and .strings
 * to XML first. The structural check - DevFlags symbols in `nm` - is the
 * caller's (release_strings.sh); this file finds the type's name in the
 * bytes, which a stripped binary keeps.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { MIN_CTX = 3, MAX_CTX = 48, MAX_NAMES = 256, MAX_DASHES = 256 };

static int is_text(unsigned char c) { return (c >= 0x20 && c < 0x7f) || c >= 0x80; }
static int is_word(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}
static int is_name(unsigned char c) { return is_word(c); }

static unsigned char *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    size_t cap = 1 << 16, len = 0;
    unsigned char *b = malloc(cap);
    size_t r;
    while (b && (r = fread(b + len, 1, cap - len, f)) > 0) {
        len += r;
        if (len == cap) { cap *= 2; unsigned char *nb = realloc(b, cap); if (!nb) { free(b); b = NULL; } else b = nb; }
    }
    fclose(f);
    *n = len;
    return b;
}

/* Findings for one file, each reported once. */
typedef struct {
    const char *path;
    int found;
    int nnames, ndashes, flags;
    char names[MAX_NAMES][64];
    char dashes[MAX_DASHES][2 * MAX_CTX + 8];
} report;

static void dev_name(report *r, const char *name, const char *how) {
    for (int j = 0; j < r->nnames; j++) if (!strcmp(r->names[j], name)) return;
    if (r->nnames < MAX_NAMES) { strncpy(r->names[r->nnames], name, 63); r->names[r->nnames][63] = 0; r->nnames++; }
    printf("%s: dev-file dev.%s%s\n", r->path, name, how);
    r->found = 1;
}

static void em_dash(report *r, const char *text, const char *how) {
    for (int j = 0; j < r->ndashes; j++) if (!strcmp(r->dashes[j], text)) return;
    if (r->ndashes < MAX_DASHES) {
        strncpy(r->dashes[r->ndashes], text, sizeof r->dashes[0] - 1);
        r->dashes[r->ndashes][sizeof r->dashes[0] - 1] = 0;
        r->ndashes++;
    }
    printf("%s: em-dash \"%s\"%s\n", r->path, text, how);
    r->found = 1;
}

/* UTF-8 text in a row: b[0..n). how tags the shape it came from. */
static void scan_utf8(report *r, const unsigned char *b, size_t n, const char *how) {
    for (size_t i = 0; i < n; i++) {
        /* dev.<name>, not part of a longer word (so "abcdev.x" is not one) */
        if (i + 4 < n && b[i] == 'd' && b[i + 1] == 'e' && b[i + 2] == 'v' && b[i + 3] == '.'
            && (i == 0 || !is_word(b[i - 1]))) {
            if (is_name(b[i + 4]) && b[i + 4] != '_') {
                char name[64];
                size_t k = 0;
                while (i + 4 + k < n && k < sizeof name - 1 && is_name(b[i + 4 + k])) { name[k] = (char)b[i + 4 + k]; k++; }
                name[k] = 0;
                dev_name(r, name, how);
            } else if (b[i + 4] == 0 && i > 0 && b[i - 1] == 0) {
                /* "dev." as a whole string: the prefix of a name joined at run time */
                dev_name(r, "", how);
            }
        }
        if (i + 8 <= n && !memcmp(b + i, "DevFlags", 8) && !r->flags) {
            printf("%s: dev-flags (the DevFlags type is compiled in)%s\n", r->path, how);
            r->flags = 1;
            r->found = 1;
        }
    }
    for (size_t i = 0; i < n; i++) {
        if (i + 2 < n && b[i] == 0xE2 && b[i + 1] == 0x80 && b[i + 2] == 0x94) {
            size_t end = i + 3;
            while (end + 2 < n && b[end] == 0xE2 && b[end + 1] == 0x80 && b[end + 2] == 0x94) end += 3;
            /* A PAIR beside non-ASCII text is the Chinese dash (U+2014 twice), correct
             * punctuation in the zh strings (owner, 2026-09-26); only a lone
             * em dash, or a pair in English, is English. */
            if (end - i == 6 && ((i > 0 && b[i - 1] >= 0x80) || (end < n && b[end] >= 0x80))) {
                i = end - 1;
                continue;
            }
            size_t lo = i, hi = end;
            while (lo > 0 && i - lo < MAX_CTX && is_text(b[lo - 1])) lo--;
            while (hi < n && hi - end < MAX_CTX && is_text(b[hi])) hi++;
            if ((i - lo) + (hi - end) >= MIN_CTX) {
                char text[2 * MAX_CTX + 8];
                size_t t = 0;
                for (size_t k = lo; k < hi && t < sizeof text - 1; k++) text[t++] = b[k] == '"' ? '\'' : (char)b[k];
                text[t] = 0;
                em_dash(r, text, how);
                i = hi - 1;     /* one report per run of text, not per dash in it */
                continue;
            }
            i = end - 1;
        }
    }
}

/* UTF-16 in either byte order at either alignment. A unit is kept only when
 * it is printable ASCII or U+2014, and written out as UTF-8; everything else
 * becomes a NUL, so machine code that happens to hold 14 20 has no readable
 * context and is not reported. */
static void scan_utf16(report *r, const unsigned char *b, size_t n) {
    if (n < 2) return;
    unsigned char *t = malloc(3 * (n / 2) + 1);
    if (!t) return;
    static const char *const how[2] = { " (utf-16le)", " (utf-16be)" };
    for (int be = 0; be < 2; be++) {
        for (size_t start = 0; start < 2; start++) {
            size_t m = 0;
            for (size_t i = start; i + 1 < n; i += 2) {
                unsigned u = be ? (unsigned)b[i] << 8 | b[i + 1] : (unsigned)b[i + 1] << 8 | b[i];
                if (u >= 0x20 && u < 0x7f) t[m++] = (unsigned char)u;
                else if (u == 0x2014) { t[m++] = 0xE2; t[m++] = 0x80; t[m++] = 0x94; }
                else t[m++] = 0;
            }
            scan_utf8(r, t, m, how[be]);
        }
    }
    free(t);
}

/* Swift small strings in arm64 code. A small string is two 64-bit words:
 * bytes 0-7 in the first, bytes 8-14 in the low seven bytes of the second,
 * whose top byte is the discriminator 0xE0 | count (ASCII) or 0xA0 | count.
 * The compiler builds each word with movz/movn and up to three movk, so the
 * words are rebuilt per register, and every word whose top byte is a
 * discriminator is paired with the other registers written close before or
 * after it; a pair whose bytes are exactly `count` bytes of text followed by
 * zeros is a string. */
static int is_macho(const unsigned char *b, size_t n) {
    if (n < 4) return 0;
    uint32_t m = (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
    return m == 0xFEEDFACFu || m == 0xFEEDFACEu || m == 0xBEBAFECAu || m == 0xBFBAFECAu;
}

static int small_string(uint64_t w1, uint64_t w2, unsigned char *out) {
    unsigned top = (unsigned)(w2 >> 56);
    if ((top & 0xF0) != 0xE0 && (top & 0xF0) != 0xA0) return 0;
    unsigned count = top & 0x0F;
    if (count == 0) return 0;
    unsigned char s[16];
    for (int k = 0; k < 8; k++) s[k] = (unsigned char)(w1 >> (8 * k));
    for (int k = 0; k < 7; k++) s[8 + k] = (unsigned char)(w2 >> (8 * k));
    for (unsigned k = 0; k < 15; k++) {
        if (k < count) { if (s[k] < 0x20 || s[k] == 0x7f) return 0; }
        else if (s[k]) return 0;
    }
    int ascii = 1;
    for (unsigned k = 0; k < count; k++) if (s[k] >= 0x80) ascii = 0;
    if (ascii != ((top & 0xF0) == 0xE0)) return 0;
    memcpy(out, s, count);
    return (int)count;
}

/* Every value a move wrote in the last WINDOW instructions, superseded ones
 * included: a first word is often moved elsewhere (csel, mov) and its
 * register reused before the second word is built. */
enum { WINDOW = 24, RING = 64 };

typedef struct { long idx; unsigned seq; uint64_t v; } moved;

static void scan_small_strings(report *r, const unsigned char *b, size_t n) {
    uint64_t reg[32] = { 0 };
    unsigned seq[32] = { 0 }, next_seq = 1;
    moved ring[RING];
    int head = 0, filled = 0;
    /* Rebuilt strings, NUL-separated, then read as UTF-8 text. */
    size_t cap = 1 << 12, m = 1;
    unsigned char *t = malloc(cap);
    if (!t) return;
    t[0] = 0;
    long idx = 0;
    for (size_t i = 0; i + 3 < n; i += 4, idx++) {
        uint32_t w = (uint32_t)b[i] | (uint32_t)b[i + 1] << 8 | (uint32_t)b[i + 2] << 16 | (uint32_t)b[i + 3] << 24;
        if ((w & 0x1F800000u) != 0x12800000u) continue;   /* not a move-wide immediate */
        int wide = (w >> 31) & 1;
        unsigned opc = (w >> 29) & 3;
        if (opc == 1) continue;              /* unallocated */
        unsigned hw = (w >> 21) & 3, rd = w & 31;
        if (!wide && hw > 1) continue;
        if (rd == 31) continue;
        uint64_t imm = (uint64_t)((w >> 5) & 0xFFFF) << (16 * hw);
        uint64_t v;
        if (opc == 0) v = ~imm;                                    /* movn */
        else if (opc == 2) v = imm;                                /* movz */
        else v = (reg[rd] & ~((uint64_t)0xFFFF << (16 * hw))) | imm; /* movk */
        if (!wide) v &= 0xFFFFFFFFu;
        if (opc != 3) seq[rd] = next_seq++;  /* movz / movn start a new value */
        reg[rd] = v;
        for (int k = 0; k < filled; k++) {
            const moved *e = &ring[k];
            if (e->seq == seq[rd] || idx - e->idx > WINDOW) continue;
            unsigned char s[16];
            int c = small_string(e->v, v, s);
            if (!c) c = small_string(v, e->v, s);
            if (!c) continue;
            if (m + (size_t)c + 1 > cap) {
                cap *= 2;
                unsigned char *nt = realloc(t, cap);
                if (!nt) { free(t); return; }
                t = nt;
            }
            memcpy(t + m, s, (size_t)c);
            m += (size_t)c;
            t[m++] = 0;
        }
        ring[head] = (moved){ idx, seq[rd], v };
        head = (head + 1) % RING;
        if (filled < RING) filled++;
    }
    scan_utf8(r, t, m, " (swift small string)");
    free(t);
}

static int scan(const char *path) {
    size_t n = 0;
    unsigned char *b = slurp(path, &n);
    if (!b) { fprintf(stderr, "rs_scan: cannot read %s\n", path); return 2; }
    report *r = calloc(1, sizeof *r);
    if (!r) { free(b); fprintf(stderr, "rs_scan: out of memory\n"); return 2; }
    r->path = path;
    scan_utf8(r, b, n, "");
    scan_utf16(r, b, n);
    if (is_macho(b, n)) scan_small_strings(r, b, n);
    int found = r->found;
    free(r);
    free(b);
    return found;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: rs_scan FILE...\n"); return 2; }
    int rc = 0;
    for (int a = 1; a < argc; a++) {
        int r = scan(argv[a]);
        if (r > rc) rc = r;
    }
    return rc;
}
