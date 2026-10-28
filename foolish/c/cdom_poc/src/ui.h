/* ui.h - the small layer the table is written in: element shorthands over cdom's per-site
 * literals (CD_LIT), and a string builder for inline styles whose result crosses as a one-off
 * (cd_tmp), so a position never becomes an interned string. */
#ifndef CDOM_POC_UI_H
#define CDOM_POC_UI_H

#include "../vendor/cdom.h"
#include <stdint.h>

#define ZS(lit) CD_LIT(lit)
#define OPEN(tag) cd_open(CD_LIT(tag))
#define CLOSE() cd_close()
#define ATTR(a, v) cd_attr(CD_LIT(a), CD_LIT(v))
#define STYLE(v) cd_attr(CD_LIT("style"), CD_LIT(v))
#define CLASS(v) cd_class(CD_LIT(v))
#define TEXT(v) cd_text(CD_LIT(v))

/* A string builder for styles and labels. Overflow truncates; the cap is far
 * above the longest style the table writes. */
typedef struct { char b[2048]; uint32_t n; } SB;
static inline void sb_reset(SB *s) { s->n = 0; }
__attribute__((noinline)) static void sb_s(SB *s, const char *t) { while (*t && s->n < sizeof s->b) s->b[s->n++] = *t++; }
/* A number as CSS writes it (cdom's cd_fmt: four decimals at most, trailing zeros dropped). */
__attribute__((noinline)) static void sb_num(SB *s, double v, uint32_t unit) {
  if (s->n + 24 <= sizeof s->b) s->n += cd_fmt_len(s->b + s->n, v, unit);
}
#define sb_i(s, v) sb_num(s, (double)(v), CD_UNIT_NONE)
__attribute__((noinline)) static void sb_px(SB *s, const char *prop, double v) { sb_s(s, prop); sb_num(s, v, CD_UNIT_PX); sb_s(s, "; "); }
/* The built string as a one-off for the next op (the last "; " keeps its ';' only). */
__attribute__((noinline)) static cd_str sb_tmp(SB *s) {
  uint32_t n = s->n;
  while (n && s->b[n - 1] == ' ') n--;
  return cd_tmp(s->b, n);
}

#endif
