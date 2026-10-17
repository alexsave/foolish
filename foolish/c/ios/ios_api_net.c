// ios_api_net.c - THE BACKEND'S CLIENT (see ios/include/ios_api.h).
//
// One line per entry: each is netclient.h's own function under the bridge's
// name, so Swift reaches the client through the one module it imports and the
// header that module shows. Nothing here decides anything.

#include "ios_api.h"

#include "netclient.h"

void fio_net_reset(void) { nc_reset(); }
int fio_net_session_load(const uint8_t *p, int len) { return nc_session_load(p, len); }
int fio_net_session_save(uint8_t *out, int cap) { return nc_session_save(out, cap); }
const void *fio_net_session_info(void) { return nc_session_info(); }
const void *fio_net_error(void) { return nc_error(); }
uint32_t fio_net_do(const void *intent, int64_t now_ms) { return nc_do((const NcIntent *)intent, now_ms); }
const void *fio_net_next_request(int64_t now_ms) { return nc_next_request(now_ms); }
const void *fio_net_on_response(uint32_t id, int status, const uint8_t *body, int len, int64_t now_ms) {
    return nc_on_response(id, status, body, len, now_ms);
}
const void *fio_net_next_done(void) { return nc_next_done(); }
const void *fio_net_row(int kind, const uint8_t *body, int len, int i) { return nc_row(kind, body, len, i); }
int fio_net_due_ms(int64_t now_ms) { return nc_due_ms(now_ms); }
const void *fio_net_stream_request(void) { return nc_req_stream(); }
int fio_net_stream_sub(int what, const char *gid, int gid_len) { return nc_stream_sub(what, gid, gid_len); }
int fio_net_stream_unsub(int what, const char *gid, int gid_len) { return nc_stream_unsub(what, gid, gid_len); }
void fio_net_stream_opened(int64_t now_ms) { nc_stream_opened(now_ms); }
int fio_net_stream_closed(int64_t now_ms) { return nc_stream_closed(now_ms); }
int fio_net_stream_in(const uint8_t *frame, int len, int is_text, int64_t now_ms) { return nc_stream_in(frame, len, is_text, now_ms); }
const void *fio_net_event(int i) { return nc_event(i); }
const uint8_t *fio_net_event_payload(int i) { return nc_event_payload(i); }
const void *fio_net_stream_next(int64_t now_ms) { return nc_stream_next(now_ms); }
