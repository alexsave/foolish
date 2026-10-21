// replay_v11.c - `cbase-replay-v11`: the stored replays, transcoded once from format 10 to 11.
//
//   build/cbase-replay-v11 --dry-run /var/lib/cbase/cbase.sqlite
//   build/cbase-replay-v11 --backup /var/lib/cbase/pre_v11.sqlite /var/lib/cbase/cbase.sqlite
//
// THROWAWAY. It exists for one production run and is deleted with its unit test
// (foolish/server/test/unit_replay_v11.c), its Makefile line and its case in
// build_bootstrap.sh in the PR after that run. It is the ONLY place the v10 trump
// alphabet is still known: the kernel reads and writes 11 alone (replay.h).
//
// WHY A TRANSCODE. A replay row outlives its session log (table.h: only a code may
// retire the log), so a stored game cannot be re-encoded from its moves; and a
// rebuild from the seed needs the log too. The code itself is the only record, so
// it is rewritten symbol for symbol. That is exact and needs no v10 decoder: the
// coder is mixed-radix rANS (replay.c), and the header's first four symbols are
// uniform - version (16), pass mode (2), seats - 2 (7), trump (the trump alphabet)
// - so the code integer is
//
//     X = version + 16 * (pass + 2 * (seats - 2 + 7 * (trump + A * rest)))
//
// where `rest` is everything after the trump, which every later decode step reads
// from the same integer whatever came before. v10 coded the trump over the deck
// without its Aces (A = 48 at 52 cards, 32 at 36); v11 over the whole deck (52,
// 36). So the transcode pops the four symbols with v10's radices, maps the trump's
// index to the same card's index in the whole deck, and pushes them back with
// v11's radices and version 11. `rest` is never touched.
//
// THE GATE, per row, by the shipped (v11-only) kernel this is linked with: the new
// code decodes (replay_decode_atoms_v6) to version 11, the same seat count, pass
// mode and trump card the v10 header named, the fool the row's `fool` column
// holds (when it holds one), and it rebuilds whole through replay_steps_v6. A row
// that fails any of it fails the run, and nothing is written.
//
// IDEMPOTENT: a row already at 11 is checked and left; a second run writes
// nothing. A row at any other version (5..9 were refused before this; junk) is
// listed and left, as unreadable as it was. One transaction (BEGIN IMMEDIATE:
// `cbase serve` in WAL mode waits on its busy timeout for the few milliseconds it
// holds the write lock), so the file is either all transcoded or unchanged.
//
// --backup PATH: before anything is written, `VACUUM INTO PATH` (refused if PATH
// exists), then PATH must pass PRAGMA integrity_check and hold as many replays.
// Required unless --dry-run. --dry-run: the same reads, transcodes and gates, then
// ROLLBACK.
//
// Exit 0: every v10 row transcoded and gated (or none to do). 1: a row failed its
// gate or a check failed (the file is unchanged). 2: usage or I/O.
#include "replay.h"
#include "replay_steps.h"
#include "card.h"
#include "view.h"

#include <sqlite3.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define V10 10
#define V11 REPLAY_FORMAT_VERSION_V11
_Static_assert(REPLAY_FORMAT_VERSION_V11 == 11, "this tool writes format 11 and knows format 10's trump alphabet");
#define VERSION_RADIX REPLAY_VERSION_ALPHABET
#define SEATS_RADIX 7      // seats 2..8, replay.c's header

// ---- a big-endian byte integer: the code itself --------------------------------------------------

typedef struct {
    unsigned char b[REPLAY_MAX_INT_BYTES + 8];
    int off, n;   // the number is b[off .. off+n), most significant first; n == 0 is zero
} BeInt;

static void be_load(BeInt *x, const unsigned char *in, int len) {
    x->off = 8;
    x->n = len;
    memcpy(x->b + x->off, in, (size_t)len);
    while (x->n > 0 && x->b[x->off] == 0) { x->off++; x->n--; }
}

// x = x / d, the remainder returned (d >= 1).
static uint32_t be_divmod(BeInt *x, uint32_t d) {
    uint32_t rem = 0;
    for (int i = 0; i < x->n; i++) {
        uint32_t cur = (rem << 8) | x->b[x->off + i];
        x->b[x->off + i] = (unsigned char)(cur / d);
        rem = cur % d;
    }
    while (x->n > 0 && x->b[x->off] == 0) { x->off++; x->n--; }
    return rem;
}

// x = x * m + a (m, a < 2^16); false when it would not fit (it always fits: the
// header grows by under two bytes, and there are eight in front).
static bool be_mul_add(BeInt *x, uint32_t m, uint32_t a) {
    uint32_t carry = a;
    for (int i = x->off + x->n - 1; i >= x->off; i--) {
        uint32_t t = (uint32_t)x->b[i] * m + carry;
        x->b[i] = (unsigned char)t;
        carry = t >> 8;
    }
    while (carry) {
        if (x->off == 0) return false;
        x->b[--x->off] = (unsigned char)carry;
        x->n++;
        carry >>= 8;
    }
    return true;
}

// What the four header symbols said.
typedef struct {
    int version, pass, seats, trump_id;
} V10Head;

// The version symbol alone: x mod 16 (a code's lowest symbol).
static int code_version(const unsigned char *in, int len) {
    static BeInt x;
    be_load(&x, in, len);
    return (int)be_divmod(&x, VERSION_RADIX);
}

// THE TRANSCODE. A v10 code in, its v11 self out (the minimal big-endian bytes,
// as replay.c writes them); the header it read in *h. The length, or -1 for a
// code that is not version 10 or does not fit `cap`.
static int replay_v10_to_v11(const unsigned char *in, int len, unsigned char *out, int cap, V10Head *h) {
    static BeInt x;
    if (len <= 0 || len > REPLAY_MAX_INT_BYTES) return -1;
    be_load(&x, in, len);
    h->version = (int)be_divmod(&x, VERSION_RADIX);
    if (h->version != V10) return -1;
    h->pass = (int)be_divmod(&x, 2);
    h->seats = (int)be_divmod(&x, SEATS_RADIX) + 2;
    const int min_v = min_value_for(h->seats);
    const int per_suit10 = ACE_VALUE - min_v;        // v10: min_v .. King
    const int per_suit11 = ACE_VALUE - min_v + 1;    // v11: min_v .. Ace
    const int t10 = (int)be_divmod(&x, (uint32_t)(4 * per_suit10));
    const int suit = t10 / per_suit10, value = min_v + t10 % per_suit10;
    h->trump_id = suit * 13 + (value - 1);
    const int t11 = suit * per_suit11 + (value - min_v);
    if (!be_mul_add(&x, (uint32_t)(4 * per_suit11), (uint32_t)t11) || !be_mul_add(&x, SEATS_RADIX, (uint32_t)(h->seats - 2))
        || !be_mul_add(&x, 2, (uint32_t)h->pass) || !be_mul_add(&x, VERSION_RADIX, V11))
        return -1;
    if (x.n > cap) return -1;
    memcpy(out, x.b + x.off, (size_t)x.n);
    return x.n;
}

// ---- the gate ------------------------------------------------------------------------------------

static void no_events(void *ctx, const EvwEvent *ev) { (void)ctx; (void)ev; }

// NULL when the v11 code is the game the v10 header named, else why not.
static const char *gate(const unsigned char *code, int len, const V10Head *h, bool has_fool, int fool) {
    ReplayHeader hdr;
    memset(&hdr, 0, sizeof hdr);
    if (replay_decode_atoms_v6(code, len, &hdr, NULL, NULL) != REPLAY_EOK) return "does not decode as v11";
    if (hdr.version != V11) return "is not version 11";
    if (hdr.n != h->seats || hdr.pass_allowed != h->pass || hdr.trump_id != h->trump_id) return "names another header";
    if (has_fool && hdr.fool != fool) return "names another fool than the row";
    if (replay_steps_v6(code, len, VIEW_SPECTATOR, NULL, no_events, NULL) != REPLAY_EOK) return "does not rebuild through replay_steps";
    return NULL;
}

// ---- the run -------------------------------------------------------------------------------------

typedef struct {
    long rows, transcoded, already, other, failed;
} Counts;

static int exec_sql(sqlite3 *db, const char *sql) {
    char *err = NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "cbase-replay-v11: %s: %s\n", sql, err ? err : "?");
        sqlite3_free(err);
        return -1;
    }
    return 0;
}

static long count_replays(sqlite3 *db) {
    sqlite3_stmt *s = NULL;
    long n = -1;
    if (sqlite3_prepare_v2(db, "SELECT count(*) FROM replays", -1, &s, NULL) == SQLITE_OK && sqlite3_step(s) == SQLITE_ROW)
        n = (long)sqlite3_column_int64(s, 0);
    sqlite3_finalize(s);
    return n;
}

static void print_id(FILE *f, const void *id, int n) {
    for (int i = 0; i < n; i++) fprintf(f, "%02x", ((const unsigned char *)id)[i]);
}

// The backup: VACUUM INTO a new file, then that file is checked on its own.
static int backup(sqlite3 *db, const char *path, long want) {
    struct stat sb;
    if (stat(path, &sb) == 0) { fprintf(stderr, "cbase-replay-v11: %s exists; name a new backup\n", path); return -1; }
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db, "VACUUM INTO ?1", -1, &s, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(s, 1, path, -1, SQLITE_STATIC);
    int rc = sqlite3_step(s);
    sqlite3_finalize(s);
    if (rc != SQLITE_DONE) { fprintf(stderr, "cbase-replay-v11: VACUUM INTO %s: %s\n", path, sqlite3_errmsg(db)); return -1; }
    sqlite3 *b = NULL;
    bool ok = false;
    if (sqlite3_open_v2(path, &b, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK) {
        sqlite3_stmt *q = NULL;
        ok = sqlite3_prepare_v2(b, "PRAGMA integrity_check", -1, &q, NULL) == SQLITE_OK && sqlite3_step(q) == SQLITE_ROW
             && strcmp((const char *)sqlite3_column_text(q, 0), "ok") == 0;
        sqlite3_finalize(q);
        ok = ok && count_replays(b) == want;
    }
    sqlite3_close(b);
    if (!ok) { fprintf(stderr, "cbase-replay-v11: the backup %s does not check out\n", path); return -1; }
    printf("backup: %s (integrity ok, %ld replays)\n", path, want);
    return 0;
}

int cb_replay_v11_run(int argc, char **argv) {
    bool dry = false;
    const char *backup_path = NULL, *path = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--dry-run") == 0) dry = true;
        else if (strcmp(argv[i], "--backup") == 0 && i + 1 < argc) backup_path = argv[++i];
        else if (argv[i][0] != '-' && !path) path = argv[i];
        else path = NULL, i = argc;
    }
    if (!path || (!dry && !backup_path)) {
        fprintf(stderr, "usage: cbase-replay-v11 --dry-run <db>\n"
                        "       cbase-replay-v11 --backup <new backup file> <db>\n");
        return 2;
    }
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK) {
        fprintf(stderr, "cbase-replay-v11: cannot open %s: %s\n", path, db ? sqlite3_errmsg(db) : "?");
        sqlite3_close(db);
        return 2;
    }
    sqlite3_busy_timeout(db, 30000);
    const long before = count_replays(db);
    if (before < 0) { fprintf(stderr, "cbase-replay-v11: %s has no replays table\n", path); sqlite3_close(db); return 2; }
    if (!dry && backup(db, backup_path, before) != 0) { sqlite3_close(db); return 2; }

    if (exec_sql(db, "BEGIN IMMEDIATE") != 0) { sqlite3_close(db); return 2; }
    Counts c = {0};
    sqlite3_stmt *sel = NULL, *upd = NULL;
    int exit_code = 0;
    if (sqlite3_prepare_v2(db, "SELECT id, moves, fool FROM replays ORDER BY created_at, id", -1, &sel, NULL) != SQLITE_OK
        || sqlite3_prepare_v2(db, "UPDATE replays SET moves = ?1 WHERE id = ?2", -1, &upd, NULL) != SQLITE_OK) {
        fprintf(stderr, "cbase-replay-v11: %s\n", sqlite3_errmsg(db));
        exit_code = 2;
    }
    static unsigned char out[REPLAY_MAX_INT_BYTES + 8];
    int rc;
    while (exit_code == 0 && (rc = sqlite3_step(sel)) == SQLITE_ROW) {
        c.rows++;
        const void *id = sqlite3_column_blob(sel, 0);
        const int id_len = sqlite3_column_bytes(sel, 0);
        const unsigned char *moves = sqlite3_column_blob(sel, 1);
        const int len = sqlite3_column_bytes(sel, 1);
        const bool has_fool = sqlite3_column_type(sel, 2) != SQLITE_NULL;
        const int fool = sqlite3_column_int(sel, 2);
        const int v = len > 0 && len <= REPLAY_MAX_INT_BYTES ? code_version(moves, len) : -1;
        if (v == V11) {
            ReplayHeader hdr;
            if (replay_decode_atoms_v6(moves, len, &hdr, NULL, NULL) == REPLAY_EOK) { c.already++; continue; }
            printf("replay "); print_id(stdout, id, id_len); printf(": version 11 and does not decode\n");
            c.failed++;
            continue;
        }
        if (v != V10) {
            printf("replay "); print_id(stdout, id, id_len); printf(": version %d, left as it is\n", v);
            c.other++;
            continue;
        }
        V10Head h;
        const int n = replay_v10_to_v11(moves, len, out, (int)sizeof out, &h);
        const char *why = n > 0 ? gate(out, n, &h, has_fool, fool) : "does not transcode";
        if (why) {
            printf("replay "); print_id(stdout, id, id_len); printf(": %s\n", why);
            c.failed++;
            continue;
        }
        sqlite3_reset(upd);
        sqlite3_bind_blob(upd, 1, out, n, SQLITE_TRANSIENT);
        sqlite3_bind_blob(upd, 2, id, id_len, SQLITE_TRANSIENT);
        if (sqlite3_step(upd) != SQLITE_DONE || sqlite3_changes(db) != 1) {
            fprintf(stderr, "cbase-replay-v11: update: %s\n", sqlite3_errmsg(db));
            exit_code = 2;
            break;
        }
        c.transcoded++;
    }
    sqlite3_finalize(sel);
    sqlite3_finalize(upd);
    if (exit_code == 0 && c.failed) exit_code = 1;
    if (exit_code == 0 && c.rows != before) { fprintf(stderr, "cbase-replay-v11: read %ld rows of %ld\n", c.rows, before); exit_code = 1; }
    printf("%ld replays: %ld transcoded 10 -> 11, %ld already 11, %ld at another version (left), %ld failed\n",
           c.rows, c.transcoded, c.already, c.other, c.failed);
    if (exit_code != 0 || dry) {
        exec_sql(db, "ROLLBACK");
        printf(exit_code ? "nothing written\n" : "dry run: nothing written\n");
    } else if (exec_sql(db, "COMMIT") != 0) {
        exit_code = 2;
    } else {
        printf("committed\n");
    }
    sqlite3_close(db);
    return exit_code;
}

#ifndef CB_REPLAY_V11_NO_MAIN
int main(int argc, char **argv) { return cb_replay_v11_run(argc, argv); }
#endif
