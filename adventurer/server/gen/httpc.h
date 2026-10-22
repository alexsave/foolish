// httpc.h - the generator's HTTP/1.1 client: one blocking keep-alive TCP connection, POST with a
// Content-Length body, the status and the body back. No library: the server answers every
// request with a Content-Length (or a 204) on the same connection, which is all this reads.
//
// A connection that the server closed (keep-alive timeout, a restart) is reconnected once and
// the request sent again, which is safe only because nothing had been read from it: a request
// the server answered is never repeated. Every socket wait is bounded by HTTPC_TIMEOUT_S.
#ifndef ADV_HTTPC_H
#define ADV_HTTPC_H

#include <stddef.h>
#include <stdint.h>

#define HTTPC_TIMEOUT_S 5
#define HTTPC_HOST 128
#define HTTPC_HDR_CAP 8192   // the response head; the server's is a few hundred bytes

typedef struct {
    char     host[HTTPC_HOST];
    char     port[8];
    int      fd;             // -1 when not connected
    uint64_t connects;       // how often a socket was opened (reconnects show here)
} httpc;

// "host:port", ":port" (127.0.0.1) or "host" (port 80). CB_OK or CB_E_INVALID. Nothing is
// opened: the first request connects.
int  httpc_init(httpc *c, const char *host_port);
void httpc_close(httpc *c);
// POST body to path. *status is the HTTP status; the body goes into resp (up to cap, *resp_len
// its length; a longer one is CB_E_FULL and the connection is dropped). CB_OK when a status was
// read, CB_E_IO when the connection failed or timed out (the socket is closed; the next call
// reconnects), CB_E_INVALID for a response this reader does not understand.
int httpc_post(httpc *c, const char *path, const uint8_t *body, size_t len, int *status,
               uint8_t *resp, size_t cap, size_t *resp_len);

#endif
