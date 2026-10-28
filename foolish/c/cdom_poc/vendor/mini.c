/* The two libc symbols clang emits calls to; memcpy and memset lower to bulk memory ops. */
#include <stddef.h>
__attribute__((no_builtin)) int memcmp(const void *a, const void *b, size_t n) {
  const unsigned char *x = a, *y = b;
  for (size_t i = 0; i < n; i++)
    if (x[i] != y[i]) return x[i] - y[i];
  return 0;
}
__attribute__((no_builtin)) size_t strlen(const char *s) {
  size_t n = 0;
  while (s[n]) n++;
  return n;
}
