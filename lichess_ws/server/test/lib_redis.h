// lib_redis.h - a blocking Redis client for the tests: it plays lila's side of the pub/sub.
#ifndef LWS_TEST_REDIS_H
#define LWS_TEST_REDIS_H

#include "util/str.h"

#include <stddef.h>

// A connection to 127.0.0.1:port, or -1.
int  tr_connect(int port);
// PUBLISH chan msg; 0 or -1.
int  tr_publish(int fd, const char *chan, const char *msg);
// PSUBSCRIBE pattern (waits for the confirmation); 0 or -1.
int  tr_psubscribe(int fd, const char *pattern);
// Reads the next pmessage within timeout_ms: the channel into chan, the payload into msg.
// 0, or -1 on timeout / error.
int  tr_next(int fd, int timeout_ms, char *chan, size_t ccap, cb_buf *msg);
// Reads pmessages until one whose channel equals `chan` and whose payload starts with `prefix`
// arrives (within timeout_ms, total); that payload into msg. 0 or -1. Messages skipped on the way
// are dropped.
int  tr_wait(int fd, int timeout_ms, const char *chan, const char *prefix, cb_buf *msg);
// Like tr_wait but matches any channel starting with chan_prefix (the round lanes "r-in:").
int  tr_wait_prefix(int fd, int timeout_ms, const char *chan_prefix, const char *prefix, char *chan, size_t ccap, cb_buf *msg);

#endif
