// unit_smap.c - the string map (smap.h): put, get, replace, delete, growth through tombstones,
// and the walk.
#include "smap.h"
#include "t.h"

#include <stdint.h>

static bool count_each(const char *key, void *val, void *ud) { (*(int *)ud)++; return true; }
static int cmp_u64(const void *a, const void *b) { uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b; return x < y ? -1 : x > y; }
static bool del_each(const char *key, void *val, void *ud) { smap_del(ud, key); return true; }

int main(void) {
    smap m;
    smap_init(&m);
    T_CHECK(smap_get(&m, "x") == NULL && smap_size(&m) == 0);
    T_CHECK(smap_put(&m, "alice", (void *)1) && smap_put(&m, "bob", (void *)2));
    T_CHECK(smap_get(&m, "alice") == (void *)1 && smap_get(&m, "bob") == (void *)2 && smap_size(&m) == 2);
    T_CHECK(smap_put(&m, "alice", (void *)3) && smap_get(&m, "alice") == (void *)3 && smap_size(&m) == 2);
    T_CHECK(smap_del(&m, "alice") == (void *)3 && smap_get(&m, "alice") == NULL && smap_size(&m) == 1);
    T_CHECK(smap_del(&m, "alice") == NULL);
    // churn through many keys: every one found, deleted ones not, the table grows and rehashes
    char key[32];
    for (int i = 0; i < 5000; i++) {
        snprintf(key, sizeof key, "k%d", i);
        T_CHECK(smap_put(&m, key, (void *)(uintptr_t)(i + 1)));
    }
    T_CHECK(smap_size(&m) == 5001);
    for (int i = 0; i < 5000; i += 2) {
        snprintf(key, sizeof key, "k%d", i);
        T_CHECK(smap_del(&m, key) == (void *)(uintptr_t)(i + 1));
    }
    for (int i = 0; i < 5000; i++) {
        snprintf(key, sizeof key, "k%d", i);
        void *v = smap_get(&m, key);
        T_CHECK(i % 2 ? v == (void *)(uintptr_t)(i + 1) : v == NULL);
    }
    for (int i = 5000; i < 20000; i++) {   // more puts than the table had slots: tombstones get rehashed away
        snprintf(key, sizeof key, "k%d", i);
        T_CHECK(smap_put(&m, key, (void *)1));
    }
    int n = 0;
    smap_each(&m, count_each, &n);
    T_CHECK(n == (int)smap_size(&m) && n == 2500 + 15000 + 1);
    smap_each(&m, del_each, &m);   // deleting the current entry during the walk is allowed
    T_CHECK(smap_size(&m) == 0);
    T_CHECK(smap_get(&m, "bob") == NULL);
    // two different keys with the same hash stay two entries (found by brute force: about 2^16
    // random keys for a 32-bit birthday collision)
    {
        enum { NK = 1 << 17 };
        static char keys[NK][12];
        static uint64_t hk[NK];   // hash << 32 | index, sorted so equal hashes are neighbours
        for (int i = 0; i < NK; i++) {
            snprintf(keys[i], sizeof keys[i], "c%x", (unsigned)i * 2654435761u);
            hk[i] = (uint64_t)smap_hash(keys[i]) << 32 | (uint32_t)i;
        }
        qsort(hk, NK, sizeof hk[0], cmp_u64);
        int a = -1, b = -1;
        for (int i = 1; i < NK && a < 0; i++)
            if ((hk[i] >> 32) == (hk[i - 1] >> 32)) { a = (int)(uint32_t)hk[i - 1]; b = (int)(uint32_t)hk[i]; }
        T_CHECK_MSG(a >= 0, "no FNV collision among 2^17 keys");
        if (a >= 0) {
            T_CHECK(smap_put(&m, keys[a], (void *)1) && smap_put(&m, keys[b], (void *)2));
            T_CHECK(smap_size(&m) == 2 && smap_get(&m, keys[a]) == (void *)1 && smap_get(&m, keys[b]) == (void *)2);
            T_CHECK(smap_del(&m, keys[a]) == (void *)1 && smap_get(&m, keys[b]) == (void *)2 && smap_get(&m, keys[a]) == NULL);
            T_CHECK(smap_del(&m, keys[b]) == (void *)2 && smap_size(&m) == 0);
        }
    }
    smap_free(&m);
    T_CHECK(m.cap == 0);
    return t_done("unit_smap");
}
