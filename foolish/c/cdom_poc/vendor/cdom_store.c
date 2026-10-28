/* cdom_store: the store's imports and the export that answers them (cdom_store.h). Separate from
 * cdom.c so an app without a store does not carry its export. */
#include "cdom.h"
#include "cdom_store.h"

#define IMPORT(n) __attribute__((import_module("cdom"), import_name(n)))
#define EXPORT(n) __attribute__((export_name(n)))

IMPORT("store_put") void j_store_put(const char *key, uint32_t klen, const void *p, uint32_t len);
IMPORT("store_get") void j_store_get(uint32_t token, const char *key, uint32_t klen, uint32_t ms);

void cd_store_put(const char *key, uint32_t klen, const void *p, uint32_t len) { j_store_put(key, klen, p, len); }
void cd_store_get(uint32_t token, const char *key, uint32_t klen, uint32_t deadline_ms) { j_store_get(token, key, klen, deadline_ms); }

__attribute__((weak)) void cd_on_store(uint32_t token, const uint8_t *p, int32_t len) { (void)token; (void)p; (void)len; }
EXPORT("cd_store_got") void cd_store_got(uint32_t token, const uint8_t *p, int32_t len) {
  cd_on_store(token, p, len);
  cd_flush();
}
