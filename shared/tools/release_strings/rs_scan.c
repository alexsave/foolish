/* rs_scan.c - what a Release build must not carry, found in raw bytes.
 *
 *   rs_scan FILE...
 *
 * For every file, prints one line per finding and exits 1 if there was any:
 *   FILE: dev-file NAME      a `dev.<name>` dev-file name (DEBUG plumbing that
 *                            leaked into a shipping build)
 *   FILE: em-dash "TEXT"     U+2014 in UTF-8, with the readable text around it
 *
 * Raw bytes, not `strings`: `strings` prints ASCII runs only, so it can never
 * see an em dash, and a Swift literal is UTF-8 in __cstring. An em dash is
 * reported only when it sits in readable text (at least MIN_CTX printable
 * bytes around it), because E2 80 94 also occurs by chance inside machine code
 * and compressed images; the text is printed so a reader can judge each hit.
 * The caller converts binary plists and .strings to XML first (they hold
 * non-ASCII as UTF-16).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { MIN_CTX = 3, MAX_CTX = 48, MAX_NAMES = 256 };

static int is_text(unsigned char c) { return (c >= 0x20 && c < 0x7f) || c >= 0x80; }
static int is_word(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}
static int is_name(unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; }

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

static int scan(const char *path) {
    size_t n = 0;
    unsigned char *b = slurp(path, &n);
    if (!b) { fprintf(stderr, "rs_scan: cannot read %s\n", path); return 2; }
    int found = 0;
    char names[MAX_NAMES][64];
    int nnames = 0;
    for (size_t i = 0; i + 4 < n; i++) {
        /* dev.<name>, not part of a longer word (so "abcdev.x" is not one) */
        if (b[i] == 'd' && b[i + 1] == 'e' && b[i + 2] == 'v' && b[i + 3] == '.' && is_name(b[i + 4])
            && b[i + 4] != '_' && (i == 0 || !is_word(b[i - 1]))) {
            char name[64];
            size_t k = 0;
            while (i + 4 + k < n && k < sizeof name - 1 && is_name(b[i + 4 + k])) { name[k] = (char)b[i + 4 + k]; k++; }
            name[k] = 0;
            int seen = 0;
            for (int j = 0; j < nnames; j++) if (!strcmp(names[j], name)) { seen = 1; break; }
            if (!seen) {
                if (nnames < MAX_NAMES) strcpy(names[nnames++], name);
                printf("%s: dev-file dev.%s\n", path, name);
                found = 1;
            }
        }
        if (b[i] == 0xE2 && b[i + 1] == 0x80 && b[i + 2] == 0x94) {
            size_t lo = i, hi = i + 3;
            while (lo > 0 && i - lo < MAX_CTX && is_text(b[lo - 1])) lo--;
            while (hi < n && hi - (i + 3) < MAX_CTX && is_text(b[hi])) hi++;
            if ((i - lo) + (hi - i - 3) >= MIN_CTX) {
                printf("%s: em-dash \"", path);
                for (size_t k = lo; k < hi; k++) putchar(b[k] == '"' ? '\'' : b[k]);
                printf("\"\n");
                found = 1;
                i = hi - 1;     /* one report per run of text, not per dash in it */
                continue;
            }
            i += 2;
        }
    }
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
