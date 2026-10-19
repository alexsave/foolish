/* check_ok_test.c - check.h's own OK(cond, "what"): it counts, it names the test and the
 * line, and it prints `what` VERBATIM (a "%d" in the text is text, not a format). check_test.c
 * includes twophone.h, which redefines OK for its printf-style messages, so the plain one
 * is held here. stderr is caught in a temporary file. */
#define _POSIX_C_SOURCE 200809L
#include <unistd.h>
#include "check.h"

int main(void)
{
    fflush(stderr);
    FILE *caught = tmpfile();
    if (!caught) return 2;
    const int saved = dup(2);
    dup2(fileno(caught), 2);

    TEST("plain ok");
    OK(1 + 1 == 2, "never printed");
    OK(2 + 2 == 5, "the 100% literal text %d %s");
    const int checks = g_checks, fails = g_fails;

    fflush(stderr);
    dup2(saved, 2);
    close(saved);
    rewind(caught);
    char err[512] = "";
    const size_t n = fread(err, 1, sizeof err - 1, caught);
    err[n] = 0;
    fclose(caught);

    int bad = 0;
#define EXPECT(c, m) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s: %s\n", __FILE__, __LINE__, #c, m); bad++; } } while (0)
    EXPECT(checks == 2 && fails == 1, "OK counts every call and every failure");
    EXPECT(strstr(err, "[plain ok]") != NULL, "a failure names its test");
    EXPECT(strstr(err, "2 + 2 == 5") != NULL, "and its condition");
    EXPECT(strstr(err, "the 100% literal text %d %s") != NULL, "and prints the message as written");
    EXPECT(strstr(err, "never printed") == NULL, "a pass prints nothing");
    EXPECT(strstr(err, "check_ok_test.c:") != NULL, "and the file and line");
    printf("check_ok_test: %d held, %d failed\n", 6 - bad, bad);
    return bad ? 1 : 0;
}
