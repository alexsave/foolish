// smap.h - a string-keyed hash map (open addressing, linear probing, FNV-1a), the one map the
// gateway uses: users by id, rooms by id, sockets by sri, games by id. Keys are copied in;
// values are the caller's pointers. Not thread-safe: every map here lives on the loop thread,
// or behind the MongoDB cache's lock.
#ifndef LWS_SMAP_H
#define LWS_SMAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t hash;   // 0: empty slot; the tombstone is SMAP_TOMB
    char    *key;
    void    *val;
} smap_slot;

typedef struct {
    smap_slot *slots;
    size_t     cap;    // a power of two, or 0
    size_t     n;      // live entries
    size_t     used;   // live + tombstones
} smap;

void  smap_init(smap *m);
void  smap_free(smap *m);                       // frees the keys, not the values
void *smap_get(const smap *m, const char *key);
// Sets key to val (replacing a value). false: out of memory.
bool  smap_put(smap *m, const char *key, void *val);
// Removes key, returning its value (NULL when absent).
void *smap_del(smap *m, const char *key);
// Every live entry, in slot order; fn returns false to stop. The callback may smap_del the
// entry it was given (and no other).
typedef bool (*smap_fn)(const char *key, void *val, void *ud);
void  smap_each(smap *m, smap_fn fn, void *ud);
static inline size_t smap_size(const smap *m) { return m->n; }

uint32_t smap_hash(const char *s);

#endif
