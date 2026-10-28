/* cdom_store: a key/value store that outlives the page (IndexedDB), bytes in and bytes out. Link
 * src/cdom_store.c to use it. Keys are the app's: put a version, and whatever the bytes depend on,
 * in the key, and a changed app reads a miss rather than old bytes. Nothing is parsed: the value
 * comes back as the bytes put. */
#ifndef CDOM_STORE_H
#define CDOM_STORE_H
#include <stdint.h>

/* Keep len bytes at p (read during the call) under key; the answer is cd_on_store_put(token, ok),
 * ok 1 when they are kept and 0 when the put failed (a quota, a closed database): an app that keeps
 * something it must not lose (a saved game) checks it; one that keeps a cache may ignore it. */
void cd_store_put(uint32_t token, const char *key, uint32_t klen, const void *p, uint32_t len);
/* Ask for the bytes under key; the answer is cd_on_store(token, p, len), p valid until it returns,
 * or len -1 for a miss, an error, or no answer within deadline_ms (indexedDB.open can hang). */
void cd_store_get(uint32_t token, const char *key, uint32_t klen, uint32_t deadline_ms);
#define CD_STORE_DEADLINE_MS 1500
/* Forget whatever is under key; the answer is cd_on_store_del(token, ok), as a put's: 1 when the
 * key holds nothing now (deleting a missing key is kept too), 0 when the delete failed. */
void cd_store_del(uint32_t token, const char *key, uint32_t klen);

/* The app's hooks (empty weak defaults). */
void cd_on_store(uint32_t token, const uint8_t *p, int32_t len);
void cd_on_store_put(uint32_t token, uint32_t ok);
void cd_on_store_del(uint32_t token, uint32_t ok);
#endif
