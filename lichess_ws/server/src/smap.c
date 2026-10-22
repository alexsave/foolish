// smap.c - smap.h.
#include "smap.h"

#include <stdlib.h>
#include <string.h>

#define SMAP_TOMB 1u

uint32_t smap_hash(const char *s) {
    uint32_t h = 2166136261u;
    for (; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    if (h < 2) h += 2;   // 0 and 1 are the empty and tombstone marks
    return h;
}

void smap_init(smap *m) { memset(m, 0, sizeof *m); }

void smap_free(smap *m) {
    for (size_t i = 0; i < m->cap; i++)
        if (m->slots[i].hash > SMAP_TOMB) free(m->slots[i].key);
    free(m->slots);
    smap_init(m);
}

static smap_slot *find(const smap *m, const char *key, uint32_t h) {
    if (!m->cap) return NULL;
    size_t mask = m->cap - 1, i = h & mask;
    for (;;) {
        smap_slot *s = &m->slots[i];
        if (s->hash == 0) return NULL;
        if (s->hash == h && strcmp(s->key, key) == 0) return s;
        i = (i + 1) & mask;
    }
}

void *smap_get(const smap *m, const char *key) {
    smap_slot *s = find(m, key, smap_hash(key));
    return s ? s->val : NULL;
}

static bool grow(smap *m) {
    size_t ncap = m->cap ? m->cap * 2 : 16;
    if (m->n * 2 < m->cap) ncap = m->cap;   // only tombstones: rehash in place
    smap_slot *ns = calloc(ncap, sizeof *ns);
    if (!ns) return false;
    size_t mask = ncap - 1;
    for (size_t i = 0; i < m->cap; i++) {
        smap_slot *s = &m->slots[i];
        if (s->hash <= SMAP_TOMB) continue;
        size_t j = s->hash & mask;
        while (ns[j].hash) j = (j + 1) & mask;
        ns[j] = *s;
    }
    free(m->slots);
    m->slots = ns;
    m->cap = ncap;
    m->used = m->n;
    return true;
}

bool smap_put(smap *m, const char *key, void *val) {
    uint32_t h = smap_hash(key);
    smap_slot *s = find(m, key, h);
    if (s) { s->val = val; return true; }
    if ((m->used + 1) * 4 > m->cap * 3 && !grow(m)) return false;
    char *k = strdup(key);
    if (!k) return false;
    size_t mask = m->cap - 1, i = h & mask;
    while (m->slots[i].hash > SMAP_TOMB) i = (i + 1) & mask;
    if (m->slots[i].hash == 0) m->used++;
    m->slots[i].hash = h;
    m->slots[i].key = k;
    m->slots[i].val = val;
    m->n++;
    return true;
}

void *smap_del(smap *m, const char *key) {
    smap_slot *s = find(m, key, smap_hash(key));
    if (!s) return NULL;
    void *v = s->val;
    free(s->key);
    s->key = NULL;
    s->val = NULL;
    s->hash = SMAP_TOMB;
    m->n--;
    return v;
}

void smap_each(smap *m, smap_fn fn, void *ud) {
    for (size_t i = 0; i < m->cap; i++) {
        smap_slot *s = &m->slots[i];
        if (s->hash <= SMAP_TOMB) continue;
        if (!fn(s->key, s->val, ud)) return;
    }
}
