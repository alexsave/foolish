// adventurer_app.c - adventurer as the platform's app (cbase/app.h): skeleton.
#include "cbase/app.h"
#include "store/store.h"

extern const char adv_schema_sql[];

static const cb_store_app ADV_STORE = {
    .sql_schema = adv_schema_sql,
};

static const cb_app ADV_APP = {
    .name = "adventurer",
    .store = &ADV_STORE,
};

const cb_app *const cb_main_app = &ADV_APP;
