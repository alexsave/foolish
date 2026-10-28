/* ui.h - the small layer the table is written in: a string builder for inline
 * styles, literal strings interned once per call site, and element shorthands.
 * Everything here is what the app wished cdom had (README.md, the gaps list). */
#ifndef CDOM_POC_UI_H
#define CDOM_POC_UI_H

#include "../vendor/cdom.h"
#include <stdint.h>

/* A literal interned once per call site: the hash lookup cd_internz does is
 * paid the first time only. */
#define ZS(lit) ({ static cd_str _zs; if (!_zs) _zs = cd_internz(lit); _zs; })

#define OPEN(tag) cd_open(ZS(tag))
#define CLOSE() cd_close()
#define ATTR(a, v) cd_attr(ZS(a), ZS(v))
#define STYLE(v) cd_attr(ZS("style"), ZS(v))
#define CLASS(v) cd_class(ZS(v))
#define TEXT(v) cd_text(ZS(v))

/* A string builder for styles and labels. Overflow truncates; the cap is far
 * above the longest style the table writes. */
typedef struct { char b[2048]; uint32_t n; } SB;
static inline void sb_reset(SB *s) { s->n = 0; }
static inline void sb_c(SB *s, char c) { if (s->n < sizeof s->b) s->b[s->n++] = c; }
static inline void sb_s(SB *s, const char *t) { while (*t) sb_c(s, *t++); }
static inline void sb_i(SB *s, int32_t v) {
  char t[12];
  int k = 0;
  uint32_t u = v < 0 ? 0u - (uint32_t)v : (uint32_t)v;
  if (v < 0) sb_c(s, '-');
  do t[k++] = (char)('0' + u % 10); while ((u /= 10));
  while (k) sb_c(s, t[--k]);
}
/* A length as CSS writes it: up to four decimals, trailing zeros dropped. */
static inline void sb_f(SB *s, double v) {
  if (v < 0) { sb_c(s, '-'); v = -v; }
  uint64_t q = (uint64_t)(v * 10000.0 + 0.5);
  sb_i(s, (int32_t)(q / 10000));
  uint32_t f = (uint32_t)(q % 10000);
  if (!f) return;
  sb_c(s, '.');
  char d[4];
  for (int i = 3; i >= 0; i--) { d[i] = (char)('0' + f % 10); f /= 10; }
  int last = 3;
  while (d[last] == '0') last--;
  for (int i = 0; i <= last; i++) sb_c(s, d[i]);
}
static inline void sb_px(SB *s, const char *prop, double v) { sb_s(s, prop); sb_f(s, v); sb_s(s, "px; "); }
static inline cd_str sb_str(SB *s) {
  uint32_t n = s->n;
  while (n && s->b[n - 1] == ' ') n--; /* the last "; " keeps its ';' only */
  return cd_intern(s->b, n);
}

#endif
