/* net_test.c - the network imports, proved against a local echo server
 * (tests/net_test.mjs): a binary WebSocket message goes out and comes back, then
 * an HTTP POST does. The verdict is written into the page. */
#include "cdom.h"
#include "cdom_net.h"

#define EXPORT(n) __attribute__((export_name(n)))

static uint8_t inbox[4096];
static const uint8_t MSG[] = {0, 1, 2, 3, 0x7f, 0x80, 0xfe, 0xff};
static cd_id status_txt;
static int ws_ok;

static int same(const uint8_t *a, const uint8_t *b, uint32_t n) {
  for (uint32_t i = 0; i < n; i++) if (a[i] != b[i]) return 0;
  return 1;
}
static void say(const char *s) { cd_set_text(status_txt, cd_internz(s)); }

EXPORT("cd_net_buf") void *cd_net_buf(uint32_t n) { return n <= sizeof inbox ? inbox : 0; }
EXPORT("cd_ws_open") void cd_ws_open(uint32_t id) { cd_ws_send(id, MSG, sizeof MSG); }
EXPORT("cd_ws_msg") void cd_ws_msg(uint32_t id, const uint8_t *p, uint32_t len) {
  ws_ok = len == sizeof MSG && same(p, MSG, len);
  say(ws_ok ? "ws ok" : "ws wrong bytes");
  cd_ws_close_id(id);
  static const char url[] = "/echo";
  cd_http(7, "POST", 4, url, sizeof url - 1, MSG, sizeof MSG);
  cd_flush();
}
EXPORT("cd_ws_close") void cd_ws_close(uint32_t id, uint32_t code) { (void)id; (void)code; }
EXPORT("cd_http_done") void cd_http_done(uint32_t token, uint32_t status, const uint8_t *p, uint32_t len) {
  /* The server answers the body reversed, so an echo of the request object itself cannot pass. */
  int ok = token == 7 && status == 200 && len == sizeof MSG;
  for (uint32_t i = 0; ok && i < len; i++) ok = p[i] == MSG[len - 1 - i];
  say(!ws_ok ? "ws failed" : ok ? "ws ok, http ok" : "http wrong");
  cd_flush();
}
EXPORT("cd_timer") void cd_timer(uint32_t t) { (void)t; }

void cd_on_event(const cd_event_rec *e) { (void)e; }
void cd_main(void) {
  cd_at(CD_MOUNT, 0);
  cd_open(cd_internz("p"));
  cd_attr(cd_internz("id"), cd_internz("status"));
  cd_text(cd_internz("waiting"));
  cd_name(status_txt = cd_id_new());
  cd_close();
  static const char url[] = "ws://127.0.0.1:8720/ws";
  cd_ws_open_url(url, sizeof url - 1);
}
