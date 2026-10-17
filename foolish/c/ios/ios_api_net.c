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
int fio_net_session_read(int status, const uint8_t *body, int len) { return nc_session_read(status, body, len); }
const void *fio_net_error(void) { return nc_error(); }
int fio_net_retry_ms(int method, int attempt, int status, const uint8_t *body, int len) { return nc_retry_ms(method, attempt, status, body, len); }
int fio_net_status_read(int status, const uint8_t *body, int len) { return nc_status_read(status, body, len); }
const void *fio_net_action_read(int status, const uint8_t *body, int len) { return nc_action_read(status, body, len); }
const void *fio_net_view_read(int status, const uint8_t *body, int len) { return nc_view_read(status, body, len); }
const void *fio_net_req_signup(const char *name, int name_len, const char *pw, int pw_len) { return nc_req_signup(name, name_len, pw, pw_len); }
const void *fio_net_req_signin(const char *name, int name_len, const char *pw, int pw_len) { return nc_req_signin(name, name_len, pw, pw_len); }
const void *fio_net_req_refresh(void) { return nc_req_refresh(); }
const void *fio_net_req_signout(int global, int64_t now_ms) { return nc_req_signout(global, now_ms); }
const void *fio_net_req_delete_account(int64_t now_ms) { return nc_req_delete_account(now_ms); }
const void *fio_net_req_create(int64_t now_ms) { return nc_req_create(now_ms); }
const void *fio_net_req_action(const char *gid, int gid_len, const uint8_t *wire, int wire_len, int64_t intent, int64_t now_ms) {
    return nc_req_action(gid, gid_len, wire, wire_len, intent, now_ms);
}
const void *fio_net_req_meta(const uint8_t *ask, int ask_len, int64_t now_ms) { return nc_req_meta(ask, ask_len, now_ms); }
const void *fio_net_req_game(const char *gid, int gid_len, int64_t now_ms) { return nc_req_game(gid, gid_len, now_ms); }
const void *fio_net_req_stream(void) { return nc_req_stream(); }
int fio_net_stream_sub(int what, const char *gid, int gid_len) { return nc_stream_sub(what, gid, gid_len); }
int fio_net_stream_unsub(int what, const char *gid, int gid_len) { return nc_stream_unsub(what, gid, gid_len); }
void fio_net_stream_opened(int64_t now_ms) { nc_stream_opened(now_ms); }
int fio_net_stream_closed(int64_t now_ms) { return nc_stream_closed(now_ms); }
const void *fio_net_stream_in(const uint8_t *frame, int len, int is_text, int64_t now_ms) { return nc_stream_in(frame, len, is_text, now_ms); }
const uint8_t *fio_net_event_payload(void) { return nc_event_payload(); }
const void *fio_net_stream_next(int64_t now_ms) { return nc_stream_next(now_ms); }
int fio_net_stream_due_ms(int64_t now_ms) { return nc_stream_due_ms(now_ms); }
void fio_net_gate_reset(const char *gid, int gid_len) { nc_gate_reset(gid, gid_len); }
