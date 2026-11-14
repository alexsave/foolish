// unit_random_gate.c - the one-random-source gate (util/uuid.h cb_random_bytes): every random draw
// the server makes, and every one its tests make, goes through cb_random_bytes, so a seeded test
// (c_base/Makefile UNIT_SEEDED, which links core/test/seeded_random.c in place of random.c) fixes
// all of them with its one seed. This test reads every C source under core/, providers/,
// ../foolish/server/, ../foolish/c/src/ and ../shared/c/ and fails, naming the file and line, when
// one draws randomness any other way:
//
//   * libc's generators: rand, srand, rand_r, random, srandom, and the *rand48 family,
//   * the system's: arc4random and its _buf and _uniform, getentropy, getrandom, and a string
//     literal naming /dev/random or /dev/urandom,
//   * OpenSSL's: RAND_bytes, RAND_priv_bytes, RAND_pseudo_bytes, RAND_seed, RAND_add, RAND_poll,
//     and <openssl/rand.h> (its `rand` is the name of the first item).
//
// The two definitions of cb_random_bytes may draw directly: core/src/util/random.c (OpenSSL's
// CSPRNG, every program) and core/test/seeded_random.c (the seeded tests). The game kernel's own
// generator is seeded from the deal seed, which cb_random_bytes draws, so it is not a stray.
//
// Comments and character literals are skipped, and string literals are read only for the device
// names, so a comment may name rand. It runs from c_base/ (make test's directory).
#include "t.h"

#include <ctype.h>
#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const char *const BANNED[] = {
    "rand",        "srand",       "rand_r",           "random",          "srandom",   "drand48",
    "erand48",     "lrand48",     "nrand48",          "mrand48",         "jrand48",   "srand48",
    "seed48",      "lcong48",     "arc4random",       "arc4random_buf",  "arc4random_uniform",
    "getentropy",  "getrandom",   "RAND_bytes",       "RAND_priv_bytes", "RAND_pseudo_bytes",
    "RAND_seed",   "RAND_add",    "RAND_poll",
};
// Split in two literals, so this file does not name them in one (the scanner reads one literal at a time).
static const char *const DEVICES[] = { "/dev/" "random", "/dev/" "urandom" };
static const char *const ALLOWED[] = { "core/src/util/random.c", "core/test/seeded_random.c" };

static int  g_files, g_violations;
static bool g_quiet;   // planted text: counted, not printed

static bool in(const char *const *names, size_t k, const char *s, size_t n) {
    for (size_t i = 0; i < k; i++)
        if (strlen(names[i]) == n && memcmp(names[i], s, n) == 0) return true;
    return false;
}

static bool allowed(const char *path) {
    for (size_t i = 0; i < sizeof ALLOWED / sizeof ALLOWED[0]; i++)
        if (strcmp(path, ALLOWED[i]) == 0) return true;
    return false;
}

static void violation(const char *path, int line, const char *s, size_t n) {
    g_violations++;
    if (!g_quiet)
        fprintf(stderr, "random gate: %s:%d: %.*s (draw through cb_random_bytes, util/uuid.h)\n", path, line, (int)n, s);
}

static bool ident_char(char c) { return isalnum((unsigned char)c) || c == '_'; }

static bool holds(const char *s, size_t n, const char *needle) {
    size_t k = strlen(needle);
    for (size_t i = 0; i + k <= n; i++)
        if (memcmp(s + i, needle, k) == 0) return true;
    return false;
}

static void scan_text(const char *path, const char *s, size_t n) {
    if (allowed(path)) return;
    int line = 1;
    size_t i = 0;
    while (i < n) {
        char c = s[i];
        if (c == '\n') { line++; i++; continue; }
        if (c == '/' && i + 1 < n && s[i + 1] == '/') {
            while (i < n && s[i] != '\n') i++;
            continue;
        }
        if (c == '/' && i + 1 < n && s[i + 1] == '*') {
            i += 2;
            while (i + 1 < n && !(s[i] == '*' && s[i + 1] == '/')) { if (s[i] == '\n') line++; i++; }
            i += 2;
            continue;
        }
        if (c == '"' || c == '\'') {
            const size_t b = ++i;
            const int at = line;
            while (i < n && s[i] != c) {
                if (s[i] == '\n') line++;
                i += s[i] == '\\' ? 2 : 1;
            }
            if (c == '"')
                for (size_t d = 0; d < sizeof DEVICES / sizeof DEVICES[0]; d++)
                    if (holds(s + b, (i < n ? i : n) - b, DEVICES[d])) violation(path, at, DEVICES[d], strlen(DEVICES[d]));
            i++;
            continue;
        }
        if (isalpha((unsigned char)c) || c == '_') {
            size_t b = i;
            while (i < n && ident_char(s[i])) i++;
            if (in(BANNED, sizeof BANNED / sizeof BANNED[0], s + b, i - b)) violation(path, line, s + b, i - b);
            continue;
        }
        if (isdigit((unsigned char)c)) {   // a number's suffix is not an identifier
            while (i < n && ident_char(s[i])) i++;
            continue;
        }
        i++;
    }
}

static void scan_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { violation(path, 0, "(cannot be read)", 16); return; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *s = malloc((size_t)n + 1);
    if (!s || fread(s, 1, (size_t)n, f) != (size_t)n) abort();
    fclose(f);
    g_files++;
    scan_text(path, s, (size_t)n);
    free(s);
}

static void scan_dir(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char p[1024];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(p, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) { scan_dir(p); continue; }
        size_t L = strlen(p);
        if (L > 2 && p[L - 2] == '.' && (p[L - 1] == 'c' || p[L - 1] == 'h')) scan_file(p);
    }
    closedir(d);
}

// The gate itself: the tree as it is, the tests included.
static void only_cb_random_bytes_draws(void) {
    struct stat st;
    T_CHECK(stat("core/src/util/random.c", &st) == 0);   // run from c_base/, or nothing is scanned
    T_CHECK(stat("core/test/seeded_random.c", &st) == 0);   // each exemption names a real file
    scan_dir("core");
    scan_dir("providers");
    scan_dir("../foolish/server");
    scan_dir("../foolish/c/src");
    scan_dir("../shared/c");
    T_CHECK(g_files >= 300);   // a gate that read nothing proves nothing
    T_EQ_INT(g_violations, 0);
}

// The scanner against planted text, so a gate that stopped seeing would fail here too.
static int planted(const char *path, const char *text) {
    int before = g_violations;
    g_quiet = true;
    scan_text(path, text, strlen(text));
    g_quiet = false;
    int found = g_violations - before;
    g_violations = before;
    return found;
}

static void the_scanner_sees_each_kind(void) {
    T_EQ_INT(planted("(planted)", "int x = rand() % 6;\n"), 1);
    T_EQ_INT(planted("(planted)", "srand((unsigned)time(NULL)); long r = random();\n"), 2);
    T_EQ_INT(planted("(planted)", "double d = drand48(); uint32_t u = arc4random_uniform(6);\n"), 2);
    T_EQ_INT(planted("(planted)", "getentropy(buf, 16); getrandom(buf, 16, 0);\n"), 2);
    T_EQ_INT(planted("(planted)", "if (RAND_bytes(b, 12) != 1) return -1;\n"), 1);
    T_EQ_INT(planted("(planted)", "#include <openssl/rand.h>\n"), 1);
    T_EQ_INT(planted("(planted)", "int fd = open(\"/dev/" "urandom\", O_RDONLY);\n"), 1);
    T_EQ_INT(planted("(planted)", "int (*f)(void) = rand;\n"), 1);
    T_EQ_INT(planted("core/src/util/random.c", "if (RAND_bytes(p, k) != 1) abort();\n"), 0);
    T_EQ_INT(planted("core/test/seeded_random.c", "int x = rand();\n"), 0);
    // Not a draw: comments, other literals, longer names, a number's suffix.
    T_EQ_INT(planted("(planted)", "// rand() described in a comment\n/* arc4random, RAND_bytes */\n"), 0);
    T_EQ_INT(planted("(planted)", "const char *m = \"rand and random\"; char q = '\"'; int rand_count = 0;\n"), 0);
    T_EQ_INT(planted("(planted)", "cb_random_bytes(b, 4); game_random(g); random_strategy_random(); long l = 10l;\n"), 0);
}

int main(void) {
    T_RUN(the_scanner_sees_each_kind);
    T_RUN(only_cb_random_bytes_draws);
    return T_DONE();
}
