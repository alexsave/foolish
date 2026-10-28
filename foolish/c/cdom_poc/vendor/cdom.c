/* cdom core: allocator, strings, DOM calls, node ids, regions, events, keyed diff. */
#include "cdom.h"
#include <stddef.h>

void *malloc(size_t n);
void free(void *p);
void *realloc(void *p, size_t n);
void *calloc(size_t n, size_t sz);

#define IMPORT(n) __attribute__((import_module("cdom"), import_name(n)))
#define EXPORT(n) __attribute__((export_name(n)))

IMPORT("input") uint32_t js_input(cd_id id, char *dst, uint32_t cap);

/* ---- memory: power-of-two size classes over memory.grow ----
 * These are malloc, realloc, free and calloc for an app that has none (weak: an app's own, all
 * four strong, replaces them), and cd_alloc goes through malloc either way, so cdom and the app
 * share one heap. free and realloc trap on a pointer this heap did not hand out: an app that
 * replaced malloc alone would otherwise feed its pointers to this free list. */
extern unsigned char __heap_base;
static uintptr_t heap_lo, heap_ptr, heap_end;
static void *free_lists[32];

static void *heap_alloc(uint32_t n) {
  uint32_t c = 4;
  while ((1u << c) < n + 8) c++;
  void *p = free_lists[c];
  if (p) {
    free_lists[c] = *(void **)p;
    return p;
  }
  if (!heap_ptr) {
    heap_lo = heap_ptr = ((uintptr_t)&__heap_base + 15) & ~(uintptr_t)15;
    heap_end = (uintptr_t)__builtin_wasm_memory_size(0) << 16;
  }
  uintptr_t need = heap_ptr + (1u << c);
  if (need > heap_end) {
    uint32_t pages = (uint32_t)((need - heap_end + 65535) >> 16);
    if (__builtin_wasm_memory_grow(0, pages) < 0) __builtin_trap();
    heap_end += (uintptr_t)pages << 16;
  }
  uint32_t *h = (uint32_t *)heap_ptr;
  heap_ptr = need;
  h[0] = c;
  return h + 2;
}

static uint32_t heap_class(void *p) {
  if ((uintptr_t)p < heap_lo + 8 || (uintptr_t)p >= heap_ptr) __builtin_trap(); /* not from this heap */
  return ((uint32_t *)p)[-2];
}

__attribute__((weak)) void *malloc(size_t n) { return n >> 31 ? 0 : heap_alloc((uint32_t)n); }

__attribute__((weak)) void free(void *p) {
  if (!p) return;
  uint32_t c = heap_class(p);
  *(void **)p = free_lists[c];
  free_lists[c] = p;
}

__attribute__((weak)) void *realloc(void *p, size_t n) {
  uint32_t cap = p ? (1u << heap_class(p)) - 8 : 0;
  if (p && cap >= n) return p;
  void *q = malloc(n);
  if (p && q) {
    __builtin_memcpy(q, p, cap);
    free(p);
  }
  return q;
}

__attribute__((weak)) void *calloc(size_t n, size_t sz) {
  size_t t = n * sz;
  if (sz && t / sz != n) return 0;
  void *p = malloc(t);
  if (p) __builtin_memset(p, 0, t);
  return p;
}

void *cd_alloc(uint32_t n) {
  void *p = malloc(n);
  if (!p) __builtin_trap();
  return p;
}

void cd_free(void *p) { free(p); }

void *cd_realloc(void *p, uint32_t n) {
  void *q = realloc(p, n);
  if (!q) __builtin_trap();
  return q;
}

/* ---- calls ----
 * Every DOM operation is its own import, called the moment C emits it. Only strings are
 * batched: a string crosses once (an id thereafter), and new strings are decoded together
 * right before the call that first needs them. Paired harness runs measured this against a
 * batched op list that the shim interpreted: the calls won on creates and clears
 * (PERF_LOG.md, the direct-call experiment), except for walks, which travel as one call. */
#define IMPORTS(n) __attribute__((import_module("cdom"), import_name(n)))
IMPORTS("strings") void j_strings(uint32_t n, uint32_t nbytes);
IMPORTS("at") void j_at(cd_id parent, cd_id before);
IMPORTS("open") void j_open(uint32_t tag);
IMPORTS("open_svg") void j_open_svg(uint32_t tag);
IMPORTS("close") void j_close(void);
IMPORTS("text") void j_text(uint32_t s);
IMPORTS("text_num") void j_text_num(uint32_t n);
IMPORTS("attr") void j_attr(uint32_t name, uint32_t v);
IMPORTS("class") void j_class(uint32_t v);
IMPORTS("name") void j_name(cd_id id);
IMPORTS("name_open") void j_name_open(cd_id id);
IMPORTS("listen") void j_listen(cd_id id, uint32_t ev);
IMPORTS("value") void j_value(uint32_t s);
IMPORTS("value_num") void j_value_num(uint32_t n);
IMPORTS("tpl_save") void j_tpl_save(uint32_t t);
IMPORTS("clone") void j_clone(uint32_t t);
IMPORTS("slot") void j_slot(uint32_t k);
IMPORTS("tpl_text") void j_tpl_text(uint32_t t, uint32_t k, uint32_t s);
IMPORTS("tpl_text_num") void j_tpl_text_num(uint32_t t, uint32_t k, uint32_t n);
IMPORTS("walk") void j_walk(cd_id from, uint32_t steps, uint32_t n);
IMPORTS("walk_value") void j_walk_value(cd_id from, uint32_t steps, uint32_t n, uint32_t s);
IMPORTS("walk_value_num") void j_walk_value_num(cd_id from, uint32_t steps, uint32_t n, uint32_t v);
IMPORTS("set_text") void j_set_text(cd_id id, uint32_t s);
IMPORTS("set_text_num") void j_set_text_num(cd_id id, uint32_t n);
IMPORTS("set_attr") void j_set_attr(cd_id id, uint32_t name, uint32_t v);
IMPORTS("set_class") void j_set_class(cd_id id, uint32_t v);
IMPORTS("style") void j_style(cd_id id, uint32_t prop, uint32_t v);
IMPORTS("style_num") void j_style_num(cd_id id, uint32_t prop, double v, uint32_t unit);
IMPORTS("image") void j_image(cd_id id, uint32_t prop, const uint8_t *rgba, uint32_t w, uint32_t h, uint32_t mime, uint32_t token);
IMPORTS("remove") void j_remove(cd_id id);
IMPORTS("free") void j_free(cd_id id);
IMPORTS("move") void j_move(cd_id id, cd_id before);
IMPORTS("clear") void j_clear(cd_id id);
IMPORTS("detach") void j_detach(cd_id id);
IMPORTS("attach") void j_attach(cd_id id);
IMPORTS("win") void j_win(uint32_t ev);
IMPORTS("viewport") void j_viewport(cd_viewport_rec *v);
IMPORTS("timer") void j_timer(uint32_t ms, uint32_t token);
IMPORTS("raf") void j_raf(uint32_t token);
IMPORTS("now") double j_now(void);

#define SB_CAP (1u << 18)
#define SM_CAP (1u << 12)
#define TRANSIENT 0x40000000u

static uint8_t sbytes[SB_CAP];
static uint32_t smeta[SM_CAP * 3];
static uint32_t nsb, nstrs, ntmp;
static cd_event_rec inbox;
static uint32_t layout[3];

/* Decodes the pending strings in JS. */
static void send_strings(void) {
  if (nstrs) j_strings(nstrs, nsb);
  nsb = nstrs = ntmp = 0;
}

/* ---- strings ---- */
typedef struct { uint32_t off, len, u16, hash; } str_ent;
static uint8_t *pool;
static uint32_t pool_len;
static str_ent *strs;
static uint32_t nstr_ids = 1; /* 0 is unused */
static uint8_t *sent;         /* JS holds the decoded string */
static uint32_t *htab, hcap;

static uint32_t hash_bytes(const char *s, uint32_t len) {
  uint32_t h = 2166136261u;
  for (uint32_t i = 0; i < len; i++) h = (h ^ (uint8_t)s[i]) * 16777619u;
  return h | 1;
}

static void htab_put(uint32_t h, uint32_t id) {
  uint32_t m = hcap - 1, i = h & m;
  while (htab[i]) i = (i + 1) & m;
  htab[i] = id;
}

static uint32_t utf16_len(const char *s, uint32_t len) {
  uint32_t u16 = 0;
  for (uint32_t i = 0; i < len; i++) {
    uint8_t c = (uint8_t)s[i];
    u16 += (c & 0xc0) != 0x80;
    u16 += c >= 0xf0; /* astral: a surrogate pair */
  }
  return u16;
}

static uint32_t push_str(cd_str s, uint32_t dest);
static cd_str svg_tag; /* "svg", once interned: cd_open compares ids, not bytes */
cd_str cd_intern(const char *s, uint32_t len) {
  uint32_t h = hash_bytes(s, len);
  if (hcap) {
    uint32_t m = hcap - 1;
    for (uint32_t i = h & m, id; (id = htab[i]); i = (i + 1) & m) {
      str_ent *e = &strs[id];
      if (e->hash == h && e->len == len && !__builtin_memcmp(pool + e->off, s, len)) return id;
    }
  }
  if (nstr_ids * 2 >= hcap) { /* grow and rehash */
    uint32_t ncap = hcap ? hcap * 2 : 1024;
    cd_free(htab);
    htab = cd_alloc(ncap * 4);
    __builtin_memset(htab, 0, ncap * 4);
    hcap = ncap;
    for (uint32_t id = 1; id < nstr_ids; id++) htab_put(strs[id].hash, id);
  }
  uint32_t id = nstr_ids++;
  strs = cd_realloc(strs, nstr_ids * sizeof(str_ent));
  sent = cd_realloc(sent, nstr_ids);
  sent[id] = 0;
  pool = cd_realloc(pool, pool_len + len);
  __builtin_memcpy(pool + pool_len, s, len);
  uint32_t u16 = utf16_len(s, len);
  strs[id] = (str_ent){pool_len, len, u16, h};
  pool_len += len;
  htab_put(h, id);
  if (len == 3 && s[0] == 's' && s[1] == 'v' && s[2] == 'g') svg_tag = id;
#if CDOM_INTERN_TEXT
  /* Queued now, not at first use: strings are interned in bursts (a list's labels) and then
   * used one call each, so they decode together in the call before the first use. */
  sent[id] = 1;
  push_str(id, id);
#endif
  return id;
}

cd_str cd_internz(const char *s) {
  uint32_t n = 0;
  while (s[n]) n++;
  return cd_intern(s, n);
}

const char *cd_bytes(cd_str s, uint32_t *len) {
  *len = strs[s].len;
  return (const char *)pool + strs[s].off;
}

static void push_raw(const uint8_t *b, uint32_t len, uint32_t u16, uint32_t dest) {
  if (len > SB_CAP) __builtin_trap();
  if (nsb + len > SB_CAP || nstrs + 1 > SM_CAP) send_strings();
  uint32_t *m = &smeta[nstrs++ * 3];
  m[0] = dest;
  m[1] = len;
  m[2] = u16;
  __builtin_memcpy(sbytes + nsb, b, len);
  nsb += len;
}

static uint32_t push_str(cd_str s, uint32_t dest) {
  str_ent *e = &strs[s];
  push_raw(pool + e->off, e->len, e->u16, dest);
  return dest;
}

/* A transient crosses in the batch that is pending when it is made, and JS keeps it only until
 * the next batch reuses its slot: so it must be used before any op sends that batch. */
cd_str cd_tmp(const char *s, uint32_t len) {
  if (nstrs + 1 > SM_CAP || nsb + len > SB_CAP) send_strings();
  uint32_t dest = TRANSIENT + ntmp++;
  push_raw((const uint8_t *)s, len, utf16_len(s, len), dest);
  return dest;
}
cd_str cd_tmpz(const char *s) {
  uint32_t n = 0;
  while (s[n]) n++;
  return cd_tmp(s, n);
}
static uint32_t tmp_check(cd_str s) {
  if (s - TRANSIENT >= ntmp) __builtin_trap(); /* its batch already went: the slot may hold another string */
  return s;
}

cd_str cd_lit_(cd_str *slot, const char *lit, uint32_t len) { return *slot ? *slot : (*slot = cd_intern(lit, len)); }
uint32_t cd_str_count(void) { return nstr_ids - 1; }
uint32_t cd_str_bytes(void) { return pool_len; }

/* ---- numbers ---- */
static const char UNIT_STR[CD_UNITS][4] = {"", "px", "%", "deg", "em", "rem", "vw", "vh", "ms", "s", "rad"};

uint32_t cd_fmt(char *d, int32_t v, uint32_t dec, uint32_t unit) {
  char t[12]; /* digits, least significant first */
  uint32_t k = 0, n = 0, u = v < 0 ? 0u - (uint32_t)v : (uint32_t)v;
  if (dec > 9 || unit >= CD_UNITS) __builtin_trap();
  do t[k++] = (char)('0' + u % 10); while ((u /= 10));
  while (k <= dec) t[k++] = '0';
  uint32_t lo = 0; /* fraction digits below lo are trailing zeros */
  while (lo < dec && t[lo] == '0') lo++;
  if (v < 0) d[n++] = '-';
  for (uint32_t i = k; i-- > dec;) d[n++] = t[i];
  if (lo < dec) {
    d[n++] = '.';
    for (uint32_t i = dec; i-- > lo;) d[n++] = t[i];
  }
  for (const char *c = UNIT_STR[unit]; *c; c++) d[n++] = *c;
  return n;
}

uint32_t cd_fmt_len(char *d, double v, uint32_t unit) {
  return cd_fmt(d, (int32_t)(v * 10000.0 + (v < 0 ? -0.5 : 0.5)), 4, unit);
}

/* A name (tag, attribute): always cached in JS. */
static uint32_t ref_name(cd_str s) {
  if (s >= TRANSIENT) return tmp_check(s);
  if (!sent[s]) {
    sent[s] = 1;
    push_str(s, s);
  }
  return s;
}

/* Content: cached or per-op, by build flag. */
static uint32_t ref_text(cd_str s) {
#if CDOM_INTERN_TEXT
  return ref_name(s);
#else
  if (s >= TRANSIENT) return tmp_check(s);
  return push_str(s, TRANSIENT + ntmp++);
#endif
}


/* ---- emitters ---- */
/* A walk (cd_sel, cd_down, cd_next, cd_up) is held here and crosses as one call: with the value
 * it leads to, or before the next op that reads the node it reached. Steps are 2 bits each:
 * 1 down, 2 next, 3 up. */
#define WALK_LAST 0xffffffffu /* walk from the last node rather than a named one */
static cd_id walk_from;
static uint32_t walk_steps, walk_n, walking;
static void walk_done(void) {
  if (walking) { walking = 0; j_walk(walk_from, walk_steps, walk_n); }
}
static void walk_step(uint32_t s) {
  if (walking && walk_n == 16) walk_done();
  if (!walking) { walking = 1; walk_from = WALK_LAST; walk_steps = walk_n = 0; }
  walk_steps |= s << (2 * walk_n++);
}
/* Strings an op takes are queued by ref_*; this sends them, and any held walk, before the call. */
static inline void ready(void) { walk_done(); send_strings(); }

void cd_flush(void) { ready(); }
uint32_t cd_pending(void) { return walking | nstrs; }

static cd_id at_parent = CD_DETACHED - 1, at_before = CD_DETACHED - 1;

void cd_at(cd_id parent, cd_id before) {
  if (parent == at_parent && before == at_before) return;
  ready(); j_at(parent, before);
  at_parent = parent;
  at_before = before;
}
/* Open elements inside an <svg>, the <svg> itself included: an id compare here rather than a
 * tag compare per open in JS (PERF_LOG.md, SVG). */
static uint32_t svg_depth;
void cd_open(cd_str tag) {
  uint32_t t = ref_name(tag);
  ready();
  if (svg_depth || tag == svg_tag) { svg_depth++; j_open_svg(t); } else j_open(t);
}
void cd_close(void) { if (svg_depth) svg_depth--; ready(); j_close(); }
void cd_text(cd_str s) { uint32_t v = ref_text(s); ready(); j_text(v); }
void cd_text_num(uint32_t n) { ready(); j_text_num(n); }
void cd_attr(cd_str name, cd_str v) { uint32_t b = ref_text(v), a = ref_name(name); ready(); j_attr(a, b); }
void cd_class(cd_str v) { uint32_t a = ref_text(v); ready(); j_class(a); }
void cd_name(cd_id id) { ready(); j_name(id); }
void cd_name_open(cd_id id) { ready(); j_name_open(id); }
void cd_listen(cd_id id, uint32_t ev) { ready(); j_listen(id, ev); }
void cd_value(cd_str s) {
  uint32_t v = ref_text(s);
  send_strings();
  if (walking) { walking = 0; j_walk_value(walk_from, walk_steps, walk_n, v); } else j_value(v);
}
void cd_value_num(uint32_t n) {
  send_strings();
  if (walking) { walking = 0; j_walk_value_num(walk_from, walk_steps, walk_n, n); } else j_value_num(n);
}
/* What each template slot holds now (kind 0 unknown, 1 string id, 2 number), so an unchanged
 * value is not written again. */
static uint32_t slot_val[CD_TPLS][CD_SLOTS];
static uint8_t slot_kind[CD_TPLS][CD_SLOTS];
void cd_tpl_save(uint32_t t) {
  if (t < CD_TPLS) __builtin_memset(slot_kind[t], 0, CD_SLOTS);
  ready(); j_tpl_save(t);
}
void cd_clone(uint32_t t) { if (svg_depth) svg_depth++; ready(); j_clone(t); }
void cd_slot(uint32_t k) {
  if (k >= CD_SLOTS) __builtin_trap();
  ready(); j_slot(k);
}
static int slot_same(uint32_t t, uint32_t k, uint8_t kind, uint32_t v) {
  if (t >= CD_TPLS || k >= CD_SLOTS) __builtin_trap();
  if (slot_kind[t][k] == kind && slot_val[t][k] == v) return 1;
  slot_kind[t][k] = kind;
  slot_val[t][k] = v;
  return 0;
}
void cd_tpl_text(uint32_t t, uint32_t k, cd_str s) {
  if (slot_same(t, k, 1, s)) return;
  uint32_t v = ref_text(s); ready(); j_tpl_text(t, k, v);
}
void cd_tpl_text_num(uint32_t t, uint32_t k, uint32_t n) {
  if (slot_same(t, k, 2, n)) return;
  ready(); j_tpl_text_num(t, k, n);
}
void cd_down(void) { walk_step(1); }
void cd_next(void) { walk_step(2); }
void cd_up(void) { walk_step(3); }
void cd_sel(cd_id id) { walk_done(); walking = 1; walk_from = id; walk_steps = walk_n = 0; }
void cd_set_text(cd_id id, cd_str s) { uint32_t v = ref_text(s); ready(); j_set_text(id, v); }
void cd_set_text_num(cd_id id, uint32_t n) { ready(); j_set_text_num(id, n); }
void cd_set_attr(cd_id id, cd_str name, cd_str v) { uint32_t b = ref_text(v), a = ref_name(name); ready(); j_set_attr(id, a, b); }
void cd_set_class(cd_id id, cd_str v) { uint32_t a = ref_text(v); ready(); j_set_class(id, a); }
void cd_set_style(cd_id id, cd_str prop, cd_str v) { uint32_t b = ref_text(v), a = ref_name(prop); ready(); j_style(id, a, b); }
static cd_str unit_ids[CD_UNITS];
void cd_set_style_num(cd_id id, cd_str prop, double v, uint32_t unit) {
  if (unit >= CD_UNITS) __builtin_trap();
  if (!unit_ids[unit]) unit_ids[unit] = cd_internz(UNIT_STR[unit]);
  uint32_t a = ref_name(prop), u = ref_name(unit_ids[unit]);
  ready(); j_style_num(id, a, v, u);
}
void cd_set_image(cd_id id, cd_str prop, const uint8_t *rgba, uint32_t w, uint32_t h, cd_str mime, uint32_t token) {
  uint32_t a = ref_name(prop), m = ref_name(mime);
  ready(); j_image(id, a, rgba, w, h, m, token);
}
void cd_move(cd_id id, cd_id before) { ready(); j_move(id, before); }
void cd_clear(cd_id id) { ready(); j_clear(id); }
void cd_detach(cd_id id) { ready(); j_detach(id); }
void cd_attach(cd_id id) { ready(); j_attach(id); }

/* ---- node ids ---- */
static cd_id next_id = 1;
static cd_id *free_ids;
static uint32_t nfree;
static uint32_t *tags, tags_cap;
static uint8_t *alive; /* a freed id freed again would be handed out twice */
static cd_region *recording;

static void record(cd_region *r, cd_id id) {
  if (r->n == r->cap) {
    r->cap = r->cap ? r->cap * 2 : 8;
    r->ids = cd_realloc(r->ids, r->cap * 4);
  }
  r->ids[r->n++] = id;
}

cd_id cd_id_new(void) {
  cd_id id;
  if (nfree) id = free_ids[--nfree];
  else {
    id = next_id++;
    if (id >= tags_cap) {
      tags_cap = tags_cap ? tags_cap * 2 : 4096;
      tags = cd_realloc(tags, tags_cap * 4);
      free_ids = cd_realloc(free_ids, tags_cap * 4);
      alive = cd_realloc(alive, tags_cap);
    }
  }
  alive[id] = 1;
  if (recording) record(recording, id);
  return id;
}

static void release(cd_id id) {
  if (id >= next_id || !alive[id]) __builtin_trap();
  alive[id] = 0;
  tags[id] = 0;
  free_ids[nfree++] = id;
}

uint32_t cd_ids_live(void) { return next_id - 1 - nfree; }
uint32_t cd_ids_high(void) { return next_id - 1; }

void cd_id_free(cd_id id) {
  ready(); j_free(id);
  release(id);
}

void cd_remove(cd_id id) {
  ready(); j_remove(id);
  release(id);
}

void cd_tag_set(cd_id id, uint32_t tag) { tags[id] = tag; }
uint32_t cd_tag(cd_id id) { return id < tags_cap ? tags[id] : 0; }

uint32_t cd_input_value(cd_id id, char *dst, uint32_t cap) { ready(); return js_input(id, dst, cap); }

/* ---- regions ---- */
int cd_region_show(cd_region *r, int want) {
  if (!want == !r->shown) return 0;
  if (!want) {
    if (r->parent) {
      cd_clear(r->parent);
      for (uint32_t i = 0; i < r->n; i++) cd_id_free(r->ids[i]);
    } else {
      cd_remove(r->root); /* ids[0] */
      for (uint32_t i = 1; i < r->n; i++) cd_id_free(r->ids[i]);
    }
    r->n = 0;
    r->root = 0;
    r->shown = 0;
    return 0;
  }
  if (recording) __builtin_trap(); /* regions do not nest */
  r->shown = 1;
  r->n = 0;
  recording = r;
  if (!r->parent) r->root = cd_id_new();
  return 1;
}
void cd_region_end(cd_region *r) {
  if (recording != r) __builtin_trap();
  recording = 0;
}
void cd_region_hide(cd_region *r) { cd_region_show(r, 0); }

/* ---- window and time ---- */
void cd_listen_window(uint32_t ev) { j_win(ev); }
void cd_viewport(cd_viewport_rec *v) { j_viewport(v); }
void cd_timer_start(uint32_t ms, uint32_t token) { j_timer(ms, token); }
void cd_raf_request(uint32_t token) { j_raf(token); }
double cd_now(void) { return j_now(); }
__attribute__((weak)) void cd_on_timer(uint32_t token) { (void)token; }
__attribute__((weak)) void cd_on_raf(uint32_t token, double t) { (void)token; (void)t; }

/* ---- entry points ---- */
EXPORT("cd_layout") uint32_t *cd_layout(void) {
  layout[0] = (uint32_t)(uintptr_t)sbytes;
  layout[1] = (uint32_t)(uintptr_t)smeta;
  layout[2] = (uint32_t)(uintptr_t)&inbox;
  return layout;
}

uint32_t cd_seed;
EXPORT("cd_start") void cd_start(uint32_t seed) {
  cd_seed = seed;
  cd_main();
  cd_flush();
}

/* Every entry from JS ends in a flush, here and in cdom_net.c: an app never calls cd_flush. */
EXPORT("cd_event") void cd_event(void) {
  cd_on_event(&inbox);
  cd_flush();
}
EXPORT("cd_timer") void cd_timer(uint32_t token) {
  cd_on_timer(token);
  cd_flush();
}
EXPORT("cd_raf") void cd_raf(uint32_t token, double t) {
  cd_on_raf(token, t);
  cd_flush();
}

/* A first fill this large builds off-document and attaches once. */
#ifndef CD_DETACH_MIN
#define CD_DETACH_MIN 64
#endif

/* ---- keyed list diff: common prefix and suffix, then a key map and an
 * LIS so only nodes off the longest increasing run move ---- */
static uint32_t *scr_map, scr_map_cap;
static int32_t *scr_src, *scr_tails, *scr_pred;
static uint32_t scr_cap;

static void scratch(uint32_t m) {
  if (m > scr_cap) {
    scr_cap = m * 2;
    scr_src = cd_realloc(scr_src, scr_cap * 4);
    scr_tails = cd_realloc(scr_tails, scr_cap * 4);
    scr_pred = cd_realloc(scr_pred, scr_cap * 4);
  }
  uint32_t hc = 16;
  while (hc < m * 2) hc <<= 1;
  if (hc > scr_map_cap) {
    scr_map_cap = hc;
    scr_map = cd_realloc(scr_map, hc * 8);
  }
  __builtin_memset(scr_map, 0xff, hc * 8);
  scr_map_cap = hc; /* lookups use exactly hc slots */
}

static inline uint32_t khash(uint32_t k) { k *= 0x9e3779b1u; return k ^ (k >> 15); }

static void map_put(uint32_t key, uint32_t idx) {
  uint32_t m = scr_map_cap - 1, i = khash(key) & m;
  while (scr_map[i * 2 + 1] != 0xffffffffu) i = (i + 1) & m;
  scr_map[i * 2] = key;
  scr_map[i * 2 + 1] = idx;
}

static uint32_t map_get(uint32_t key) {
  uint32_t m = scr_map_cap - 1, i = khash(key) & m;
  for (;;) {
    uint32_t v = scr_map[i * 2 + 1];
    if (v == 0xffffffffu || scr_map[i * 2] == key) return v;
    i = (i + 1) & m;
  }
}

/* Marks LIS members of src[0..m) (entries < 0 skipped) by setting pred[i] = -2. */
static void lis_mark(const int32_t *src, uint32_t m, int32_t *tails, int32_t *pred) {
  uint32_t len = 0;
  for (uint32_t i = 0; i < m; i++) {
    int32_t v = src[i];
    pred[i] = -1;
    if (v < 0) continue;
    uint32_t lo = 0, hi = len;
    while (lo < hi) {
      uint32_t mid = (lo + hi) >> 1;
      if (src[tails[mid]] < v) lo = mid + 1; else hi = mid;
    }
    pred[i] = lo ? tails[lo - 1] : -1;
    tails[lo] = (int32_t)i;
    if (lo == len) len++;
  }
  int32_t k = len ? tails[len - 1] : -1;
  while (k >= 0) {
    int32_t p = pred[k];
    pred[k] = -2;
    k = p;
  }
}

static void drop(cd_list *L, cd_item *it, const cd_list_fns *f, int dom) {
  if (dom) cd_remove(it->node); else cd_id_free(it->node);
  f->destroy(f->ctx, it);
  (void)L;
}

void cd_list_forget(cd_list *L, const cd_list_fns *f) {
  for (uint32_t i = 0; i < L->n; i++) {
    cd_id_free(L->items[i].node);
    f->destroy(f->ctx, &L->items[i]);
  }
  L->n = 0;
}

void cd_list_update(cd_list *L, const uint32_t *keys, uint32_t n, const cd_list_fns *f) {
  cd_region *rec = recording; /* the items' ids are the list's, not a region's */
  recording = 0;
  cd_id end = L->end; /* where the tail is: before the first fixed sibling, or the parent's end */
  cd_item *old = L->items;
  uint32_t on = L->n;
  cd_item *nw = cd_alloc((n ? n : 1) * sizeof(cd_item));
  uint32_t os = 0, oe = on, ns = 0, ne = n;

  /* Trim matching heads and tails. A head that became the tail (or the
   * reverse) moves in one op, but only when another survivor sits next to it
   * in both lists: then it is out of order with every other survivor, so no
   * longest in-order run can keep it and the move is one the LIS would make. */
  for (;;) {
    while (os < oe && ns < ne && old[os].key == keys[ns]) {
      nw[ns] = old[os++];
      f->patch(f->ctx, ns, &nw[ns]);
      ns++;
    }
    while (os < oe && ns < ne && old[oe - 1].key == keys[ne - 1]) {
      nw[--ne] = old[--oe];
      f->patch(f->ctx, ne, &nw[ne]);
    }
    if (oe - os < 2 || ne - ns < 2) break;
    int head_to_tail = old[os].key == keys[ne - 1];
    int tail_to_head = old[oe - 1].key == keys[ns];
    if (head_to_tail && tail_to_head && oe - os > 2 && ne - ns > 2 && old[os + 1].key == keys[ns + 1]) {
      cd_move(old[os].node, ne < n ? nw[ne].node : end); /* a swap of the two ends */
      cd_move(old[oe - 1].node, old[os + 1].node);
      nw[--ne] = old[os++];
      f->patch(f->ctx, ne, &nw[ne]);
      nw[ns] = old[--oe];
      f->patch(f->ctx, ns, &nw[ns]);
      ns++;
      continue;
    }
    if (head_to_tail && old[os + 1].key == keys[ns]) {
      cd_move(old[os].node, ne < n ? nw[ne].node : end);
      nw[--ne] = old[os++];
      f->patch(f->ctx, ne, &nw[ne]);
      continue;
    }
    if (tail_to_head && old[oe - 2].key == keys[ne - 1]) {
      cd_move(old[oe - 1].node, old[os].node);
      nw[ns] = old[--oe];
      f->patch(f->ctx, ns, &nw[ns]);
      ns++;
      continue;
    }
    break;
  }

  if (os == oe) { /* only inserts */
    cd_id before = ne < n ? nw[ne].node : end;
    int off = L->owns_parent && on == 0 && n >= CD_DETACH_MIN;
    if (off) cd_detach(L->parent);
    for (uint32_t i = ns; i < ne; i++) {
      cd_at(L->parent, before);
      nw[i].key = keys[i];
      nw[i].node = f->create(f->ctx, i, &nw[i].data);
    }
    if (off) cd_attach(L->parent);
  } else if (ns == ne) { /* only removes */
    if (L->owns_parent && n == 0) {
      cd_clear(L->parent);
      for (uint32_t i = os; i < oe; i++) drop(L, &old[i], f, 0);
    } else {
      for (uint32_t i = os; i < oe; i++) drop(L, &old[i], f, 1);
    }
  } else {
    uint32_t m = ne - ns;
    scratch(m);
    for (uint32_t i = 0; i < m; i++) {
      map_put(keys[ns + i], ns + i);
      scr_src[i] = -1;
    }
    /* Replacing the whole list with all new keys: one clear beats n removes. */
    int whole = L->owns_parent && os == 0 && oe == on && ns == 0 && ne == n;
    if (whole)
      for (uint32_t i = 0; i < on && whole; i++) whole = map_get(old[i].key) == 0xffffffffu;
    if (whole) {
      cd_clear(L->parent);
      for (uint32_t i = 0; i < on; i++) drop(L, &old[i], f, 0);
      int off = n >= CD_DETACH_MIN;
      if (off) cd_detach(L->parent);
      for (uint32_t i = 0; i < n; i++) {
        cd_at(L->parent, end);
        nw[i].key = keys[i];
        nw[i].node = f->create(f->ctx, i, &nw[i].data);
      }
      if (off) cd_attach(L->parent);
    } else {
      int moved = 0;
      uint32_t last = 0;
      for (uint32_t i = os; i < oe; i++) {
        uint32_t j = map_get(old[i].key);
        if (j == 0xffffffffu) {
          drop(L, &old[i], f, 1);
          continue;
        }
        scr_src[j - ns] = (int32_t)i;
        nw[j] = old[i];
        f->patch(f->ctx, j, &nw[j]);
        if (j < last) moved = 1; else last = j;
      }
      if (moved) lis_mark(scr_src, m, scr_tails, scr_pred);
      for (uint32_t k = m; k-- > 0;) {
        uint32_t pos = ns + k;
        cd_id before = pos + 1 < n ? nw[pos + 1].node : end;
        if (scr_src[k] < 0) {
          cd_at(L->parent, before);
          nw[pos].key = keys[pos];
          nw[pos].node = f->create(f->ctx, pos, &nw[pos].data);
        } else if (moved && scr_pred[k] != -2) {
          cd_move(nw[pos].node, before);
        }
      }
    }
  }
  cd_free(old);
  L->items = nw;
  L->n = n;
  L->cap = n;
  recording = rec;
}
