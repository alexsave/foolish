/* cdom_store: a key/value store that outlives the page (IndexedDB), bytes in and bytes out. Link
 * src/cdom_store.c to use it. Keys are the app's: put a version, and whatever the bytes depend on,
 * in the key, and a changed app reads a miss rather than old bytes. Nothing is parsed: the value
 * comes back as the bytes put. */
#ifndef CDOM_STORE_H
#define CDOM_STORE_H
#include <stdint.h>

/* Keep len bytes at p (read during the call) under key; nothing answers, and a failure is silent. */
void cd_store_put(const char *key, uint32_t klen, const void *p, uint32_t len);
/* Ask for the bytes under key; the answer is cd_on_store(token, p, len), p valid until it returns,
 * or len -1 for a miss, an error, or no answer within deadline_ms (indexedDB.open can hang). */
void cd_store_get(uint32_t token, const char *key, uint32_t klen, uint32_t deadline_ms);
#define CD_STORE_DEADLINE_MS 1500

void cd_on_store(uint32_t token, const uint8_t *p, int32_t len); /* the app's hook (empty weak default) */
#endif
