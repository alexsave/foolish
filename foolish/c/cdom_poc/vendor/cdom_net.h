/* cdom_net.h - [poc] the network, as wasm imports (module "net") answered by exports.
 * Bytes go straight into C memory: the shim asks cd_net_buf(n) for room, copies the
 * message there and hands C (ptr, n). No JSON, no strings C did not write.
 * Every answer arrives from the event loop, so each export flushes (cd_flush) itself. */
#ifndef CDOM_NET_H
#define CDOM_NET_H
#include <stdint.h>

#define CD_NET_IMPORT(n) __attribute__((import_module("net"), import_name(n)))
CD_NET_IMPORT("ws_open") uint32_t cd_ws_open_url(const char *url, uint32_t len); /* -> socket id */
CD_NET_IMPORT("ws_send") void cd_ws_send(uint32_t id, const void *p, uint32_t len); /* one binary message */
CD_NET_IMPORT("ws_close") void cd_ws_close_id(uint32_t id);
/* One request; the answer is cd_http_done(token, status, ptr, len), status 0 when it failed. */
CD_NET_IMPORT("http") void cd_http(uint32_t token, const char *method, uint32_t mlen, const char *url, uint32_t ulen,
                                   const void *body, uint32_t blen);

/* The app exports these (export_name). */
void *cd_net_buf(uint32_t n);                               /* room for n incoming bytes */
void cd_ws_open(uint32_t id);
void cd_ws_msg(uint32_t id, const uint8_t *p, uint32_t len);
void cd_ws_close(uint32_t id, uint32_t code);
void cd_http_done(uint32_t token, uint32_t status, const uint8_t *p, uint32_t len);
#endif
