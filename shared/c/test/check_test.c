/* The harness tested by hand, not by itself: CHECK and OK are run against
 * known conditions with stderr and stdout caught in temporary files, and
 * what they counted and printed is compared with what they should have,
 * through this file's own EXPECT. The capacity is lowered to 3 here so the
 * named-failure fan-out is driven past its cap in a few lines. */
#define _POSIX_C_SOURCE 200809L
#define CHECK_NAMED_CAP 3
#include "twophone.h"
#include <unistd.h>

static int t_checks, t_fails;
#define EXPECT(cond, ...) do {                                                  \
        t_checks++;                                                             \
        if (!(cond)) {                                                          \
            t_fails++;                                                          \
            fprintf(stderr, "FAIL %s:%d %s: ", __FILE__, __LINE__, #cond);      \
            fprintf(stderr, __VA_ARGS__);                                       \
            fprintf(stderr, "\n");                                              \
        }                                                                       \
    } while (0)

/* Point fd at a fresh temporary file; catch_end puts it back and hands the
 * caught text over, NUL-terminated. */
static FILE *caught;
static int   saved_fd;
static void catch_begin(FILE *stream)
{
    fflush(stream);
    caught = tmpfile();
    if (!caught) { perror("tmpfile"); exit(2); }
    saved_fd = dup(fileno(stream));
    dup2(fileno(caught), fileno(stream));
}
static void catch_end(FILE *stream, char *out, size_t cap)
{
    fflush(stream);
    dup2(saved_fd, fileno(stream));
    close(saved_fd);
    rewind(caught);
    size_t n = fread(out, 1, cap - 1, caught);
    out[n] = 0;
    fclose(caught);
}

/* How many lines of `text` contain `needle`. */
static int lines_with(const char *text, const char *needle)
{
    int n = 0;
    for (const char *p = text; *p;) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        char line[512];
        if (len >= sizeof line) len = sizeof line - 1;
        memcpy(line, p, len);
        line[len] = 0;
        if (strstr(line, needle)) n++;
        p += len + (eol ? 1 : 0);
    }
    return n;
}

static int calls;
static int counted_call(int v) { calls++; return v; }

static char err[16384], out[1024];

static void test_check(void)
{
    catch_begin(stderr);
    TEST("pass");
    CHECK(1 + 1 == 2, "never printed");
    CHECK(counted_call(1), "never printed");
    catch_end(stderr, err, sizeof err);
    EXPECT(g_checks == 2 && g_fails == 0, "two passes count two assertions, no failure (%d, %d)", g_checks, g_fails);
    EXPECT(err[0] == 0, "a pass prints nothing (\"%s\")", err);
    EXPECT(calls == 1, "CHECK evaluates its condition once (%d)", calls);

    catch_begin(stderr);
    TEST("alpha");
    for (int i = 0; i < 7; i++) CHECK(i < 0, "alpha %d", i);
    catch_end(stderr, err, sizeof err);
    EXPECT(g_checks == 9 && g_fails == 7, "seven failures counted (%d, %d)", g_checks, g_fails);
    EXPECT(lines_with(err, "[alpha]") == 5, "a test prints its first 5 failures only (%d)", lines_with(err, "[alpha]"));
    EXPECT(lines_with(err, "[alpha] i < 0: alpha 0") == 1 && lines_with(err, "alpha 4") == 1
           && lines_with(err, "alpha 5") == 0, "the line names the test, the condition and the message");
    EXPECT(lines_with(err, "FAIL ") == 5 && lines_with(err, "check_test.c:") == 5, "and the file and line");
}

static void test_fan_out(void)
{
    /* alpha holds slot 1 of 3; beta and gamma fill the rest, delta is past
     * the cap: counted, never printed. */
    char beta_copy[] = "beta";
    catch_begin(stderr);
    TEST("beta");  CHECK(0, "beta 1");
    TEST("gamma"); CHECK(0, "gamma 1");
    TEST("delta"); CHECK(0, "delta 1"); CHECK(0, "delta 2");
    TEST(beta_copy); CHECK(0, "beta 2");      /* the same name, another pointer */
    TEST("alpha"); CHECK(0, "alpha 8");       /* still capped at 5 */
    catch_end(stderr, err, sizeof err);
    EXPECT(g_fails == 7 + 6, "every failure counts, printed or not (%d)", g_fails);
    EXPECT(lines_with(err, "[beta]") == 2, "a name is matched by its text, not its pointer (%d)", lines_with(err, "[beta]"));
    EXPECT(lines_with(err, "[gamma]") == 1, "the third name still prints (%d)", lines_with(err, "[gamma]"));
    EXPECT(lines_with(err, "[delta]") == 0, "a name past CHECK_NAMED_CAP prints nothing (%d)", lines_with(err, "[delta]"));
    EXPECT(lines_with(err, "[alpha]") == 0, "a name past its 5 prints nothing (%d)", lines_with(err, "[alpha]"));
}

static void test_report(void)
{
    char want[128];
    catch_begin(stdout);
    int red = report("probe");
    catch_end(stdout, out, sizeof out);
    snprintf(want, sizeof want, "probe: %d assertions, %d failed\n", g_checks, g_fails);
    EXPECT(red == 1, "report exits 1 with a failure counted (%d)", red);
    EXPECT(!strcmp(out, want), "report prints the tally (\"%s\")", out);

    int checks = g_checks;
    g_fails = 0;
    catch_begin(stdout);
    int green = report("probe");
    catch_end(stdout, out, sizeof out);
    snprintf(want, sizeof want, "probe: %d assertions, 0 failed\n", checks);
    EXPECT(green == 0, "report exits 0 with no failure (%d)", green);
    EXPECT(!strcmp(out, want), "and prints 0 failed (\"%s\")", out);
}

static void test_ok(void)
{
    g_checks = g_fails = 0;
    calls = 0;
    catch_begin(stderr);
    g_quiet = 1;
    STEP("rehearsal");
    OK(counted_call(0), "never printed");
    g_quiet = 0;
    STEP("walk");
    OK(counted_call(1), "never printed");
    for (int i = 0; i < 45; i++) OK(i < 0, "walk %d", i);
    catch_end(stderr, err, sizeof err);
    EXPECT(calls == 2, "a quiet OK still evaluates its condition, once (%d)", calls);
    EXPECT(g_checks == 46 && g_fails == 45, "a quiet OK counts nothing (%d, %d)", g_checks, g_fails);
    EXPECT(lines_with(err, "[rehearsal]") == 0, "and prints nothing");
    EXPECT(lines_with(err, "[walk] i < 0: walk") == 40, "OK prints the first 40 failures of the binary (%d)",
           lines_with(err, "[walk]"));
}

int main(void)
{
    test_check();
    test_fan_out();
    test_report();
    test_ok();
    printf("check_test: %d assertions, %d failed\n", t_checks, t_fails);
    return t_fails ? 1 : 0;
}
