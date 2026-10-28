/* The two libc symbols clang emits calls to; memcpy and memset lower to bulk memory ops.
 * Weak: an app that links its own libc keeps its own, and links cleanly with this file. */
#include <stddef.h>
__attribute__((weak, no_builtin)) int memcmp(const void *a, const void *b, size_t n) {
  const unsigned char *x = a, *y = b;
  for (size_t i = 0; i < n; i++)
    if (x[i] != y[i]) return x[i] - y[i];
  return 0;
}
__attribute__((weak, no_builtin)) size_t strlen(const char *s) {
  size_t n = 0;
  while (s[n]) n++;
  return n;
}
