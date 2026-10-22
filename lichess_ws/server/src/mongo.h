// mongo.h - a minimal MongoDB client: OP_MSG over a blocking TCP socket, one connection per
// request-pool thread, `find` with a filter and a projection, limit 1. That is every read the
// gateway makes at a socket's connect (SCOPE.md 1.1: security, user4, game5). No auth: lichess's
// local stack runs MongoDB without it, and so does the race.
#ifndef LWS_MONGO_H
#define LWS_MONGO_H

#include "bson.h"
#include "util/str.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    char host[128];
    int  port;
    char db[64];
    int  timeout_ms;   // connect, send and receive
} mongo_cfg;

typedef struct mongo_conn mongo_conn;

// Connects (blocking, within cfg->timeout_ms). CB_OK, or CB_E_IO.
int  mongo_connect(mongo_conn **out, const mongo_cfg *cfg);
void mongo_close(mongo_conn *c);

// Runs {find: coll, filter, projection, limit: 1, singleBatch: true}. The first document of
// the reply is copied into `doc` (CB_OK), or CB_E_NOT_FOUND when the batch is empty, or
// CB_E_IO when the connection failed (it is then unusable: close it and connect again).
// `proj` may be NULL.
int  mongo_find_one(mongo_conn *c, const char *coll, const cb_buf *filter, const cb_buf *proj, cb_buf *doc);
// Runs {insert: coll, documents: [doc]} (the tests and the bench seed with it). CB_OK, CB_E_CONFLICT
// for a duplicate key, CB_E_IO otherwise.
int  mongo_insert_one(mongo_conn *c, const char *coll, const cb_buf *doc);

// The calling thread's connection, made on first use and remade after an error, from the
// configuration given once at startup (mongo_thread_configure). NULL when it cannot connect.
void        mongo_thread_configure(const mongo_cfg *cfg);
mongo_conn *mongo_thread_conn(void);
// After CB_E_IO from the thread's connection: drops it so the next mongo_thread_conn reconnects.
void        mongo_thread_drop(void);

#endif
