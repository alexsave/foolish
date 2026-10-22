-- adventurer's tables: THE JOURNAL. SQLite is not read on any request path; it is the
-- append-only record of each result, and the state is rebuilt from it at start (world.h). Applied
-- after the platform's schema in the same transaction, at every open (store.h cb_store_app).

-- The roster, insert or replace at every generator start.
CREATE TABLE IF NOT EXISTS adv_users (
    id       INTEGER PRIMARY KEY,
    kind     INTEGER NOT NULL,
    name     TEXT NOT NULL,
    home_lat INTEGER NOT NULL,
    home_lon INTEGER NOT NULL
);

-- One accepted activity: what the matcher found (hits: n x adv_hit, 8 bytes each) and the
-- numbers the feed showed. Replayed in id order at start through the same apply the live path used.
CREATE TABLE IF NOT EXISTS adv_activities (
    id        INTEGER PRIMARY KEY,
    user      INTEGER NOT NULL,
    type      INTEGER NOT NULL,
    flags     INTEGER NOT NULL,
    start_ms  INTEGER NOT NULL,
    gen_id    INTEGER NOT NULL,
    n_points  INTEGER NOT NULL,
    matched_m REAL NOT NULL,
    accuracy  REAL NOT NULL,
    cpu_us    INTEGER NOT NULL,
    hits      BLOB NOT NULL
);
