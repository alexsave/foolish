// lib_store_backend.c - SQLite or a fresh DynamoDB Local table (lib_store_backend.h), on c_base's
// own (providers/aws/test/lib_dynamo_local.h), with the schema's bots seeded as SQLite seeds them.
#include "lib_store_backend.h"

#include "lib_dynamo_local.h"
#include "store/store_import.h"
#include "util/err.h"

#include <stdio.h>

bool tsb_dynamo(void) { return tdl_enabled(); }

int tsb_create_table(const char *name) { return tdl_create_table(name); }

const char *tsb_table(void) { return tdl_table(); }

cb_store *tsb_open_fresh(const char *sqlite_path) {
    cb_store *s = tdl_open_fresh(sqlite_path);
    if (!s || !tdl_enabled()) return s;
    int n = cb_import_seed_bots(s);
    if (n < 0) {
        fprintf(stderr, "tsb: seeding the bots into %s failed: %s\n", tdl_table(), cb_strerror(n));
        cb_store_close(s);
        return NULL;
    }
    return s;
}
