/* cdom_net: WebSocket and fetch, bytes straight into C memory. Link src/cdom_net.c to use it.
 * Incoming bytes are copied into a buffer cdom owns and handed to the app's hook as (p, len),
 * valid until the hook returns: no strings and no JSON cross. Every answer arrives from the
 * event loop through an export that flushes on its way out. */
#ifndef CDOM_NET_H
#define CDOM_NET_H
#include <stdint.h>

uint32_t cd_ws_open(const char *url, uint32_t len);          /* -> socket id; binary messages */
void cd_ws_send(uint32_t id, const void *p, uint32_t len);   /* one binary message */
void cd_ws_close(uint32_t id);
/* One request; the answer is cd_on_http(token, status, p, len), status 0 when it failed. */
void cd_http(uint32_t token, const char *method, uint32_t mlen, const char *url, uint32_t ulen,
             const void *body, uint32_t blen);

/* The app's hooks (empty weak defaults in cdom_net.c). */
void cd_on_ws_open(uint32_t id);
void cd_on_ws_msg(uint32_t id, const uint8_t *p, uint32_t len);
void cd_on_ws_close(uint32_t id, uint32_t code);
void cd_on_http(uint32_t token, uint32_t status, const uint8_t *p, uint32_t len);
#endif
