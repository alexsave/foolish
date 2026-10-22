/* t.h - c_chess's tests: shared/c/test/check.h, with a run of zero checks a failure, and a string
 * comparison that prints both sides. */
#ifndef C_CHESS_T_H
#define C_CHESS_T_H
#define CHECK_ZERO_IS_FAIL
#include "../../../shared/c/test/check.h"
#define STREQ(a, b) CHECK(strcmp((a), (b)) == 0, "got \"%s\", want \"%s\"", (a), (b))
#endif
