// resp.h - a Redis pub/sub client over the loop (net/loop.h): RESP2, two connections, one that
// subscribes and receives pushes, one that publishes. Loop thread only. Both reconnect by
// themselves (every second) and the subscriptions are replayed, as Lettuce does for lila-ws.
#ifndef LWS_RESP_H
#define LWS_RESP_H

#include "net/loop.h"

#include <stdbool.h>
#include <stddef.h>

typedef struct resp_client resp_client;

// A message published on a channel this client subscribed to. The bytes live until it returns.
typedef void (*resp_msg_fn)(const char *chan, size_t clen, const char *msg, size_t mlen, void *ud);
// Both connections are up and every subscription confirmed (true), or one went down (false).
typedef void (*resp_state_fn)(bool up, void *ud);

resp_client *resp_new(cb_loop *l, const char *host, int port, resp_msg_fn on_msg, resp_state_fn on_state, void *ud);
// After the loop has stopped (cb_loop_run has returned, so a queued publish flush has run).
void         resp_free(resp_client *c);
// Starts connecting; on_state(true) follows once up. Called once.
void         resp_start(resp_client *c);
// Subscribes (now, and again after every reconnect). Before resp_start it is just remembered.
int          resp_subscribe(resp_client *c, const char *chan);
// Queues PUBLISH chan msg. CB_OK; CB_E_STATE when the publishing connection is down (the
// message is dropped: the caller decides what to buffer, as lila-ws does for critical lines).
int          resp_publish(resp_client *c, const char *chan, const char *msg, size_t n);
bool         resp_up(const resp_client *c);
// Counters: messages published and received so far.
void         resp_stats(const resp_client *c, long *published, long *received);

#endif
