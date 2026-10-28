/* cdom_net: the network imports and the exports that answer them (cdom_net.h). Separate from
 * cdom.c so an app without a network does not carry its exports. */
#include "cdom.h"
#include "cdom_net.h"

#define IMPORT(n) __attribute__((import_module("cdom"), import_name(n)))
#define EXPORT(n) __attribute__((export_name(n)))

IMPORT("ws_open") uint32_t j_ws_open(const char *url, uint32_t len);
IMPORT("ws_send") void j_ws_send(uint32_t id, const void *p, uint32_t len);
IMPORT("ws_close") void j_ws_close(uint32_t id);
IMPORT("ws_buffered") uint32_t j_ws_buffered(uint32_t id);
IMPORT("http") void j_http(uint32_t token, const char *m, uint32_t mlen, const char *url, uint32_t ulen,
                           const char *headers, uint32_t hlen, const void *body, uint32_t blen, uint32_t deadline_ms);
IMPORT("epoch") double j_epoch(void);

uint32_t cd_ws_open(const char *url, uint32_t len) { return j_ws_open(url, len); }
void cd_ws_send(uint32_t id, const void *p, uint32_t len) { j_ws_send(id, p, len); }
void cd_ws_close(uint32_t id) { j_ws_close(id); }
uint32_t cd_ws_buffered(uint32_t id) { return j_ws_buffered(id); }
void cd_http(uint32_t token, const char *method, uint32_t mlen, const char *url, uint32_t ulen,
             const char *headers, uint32_t hlen, const void *body, uint32_t blen, uint32_t deadline_ms) {
  j_http(token, method, mlen, url, ulen, headers, hlen, body, blen, deadline_ms);
}
double cd_epoch_ms(void) { return j_epoch(); }

__attribute__((weak)) void cd_on_ws_open(uint32_t id) { (void)id; }
__attribute__((weak)) void cd_on_ws_msg(uint32_t id, const uint8_t *p, uint32_t len) { (void)id; (void)p; (void)len; }
__attribute__((weak)) void cd_on_ws_close(uint32_t id, uint32_t code) { (void)id; (void)code; }
__attribute__((weak)) void cd_on_http(uint32_t token, uint32_t status, const uint8_t *p, uint32_t len) {
  (void)token; (void)status; (void)p; (void)len;
}

/* The shim copies a message into cdom's room for incoming bytes (cd_bytes_buf), then calls these. */
EXPORT("cd_net_open") void cd_net_open(uint32_t id) { cd_on_ws_open(id); cd_flush(); }
EXPORT("cd_net_msg") void cd_net_msg(uint32_t id, const uint8_t *p, uint32_t len) { cd_on_ws_msg(id, p, len); cd_flush(); }
EXPORT("cd_net_close") void cd_net_close(uint32_t id, uint32_t code) { cd_on_ws_close(id, code); cd_flush(); }
EXPORT("cd_net_http") void cd_net_http(uint32_t token, uint32_t status, const uint8_t *p, uint32_t len) {
  cd_on_http(token, status, p, len);
  cd_flush();
}
