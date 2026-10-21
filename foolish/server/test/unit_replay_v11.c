// unit_replay_v11.c - the one-time replay transcode (foolish/server/migrate/replay_v11.c), 10 -> 11.
//
// THROWAWAY, with the tool: deleted in the PR after the production run.
//
// The fixtures are the same seeded games cut twice: the v10 code by the kernel
// before the Ace-trump change (main a7bab4e1a) and the v11 code by this one, both
// played by the handwritten bot (2, 5, 6 and 8 seats, perevodnoy and podkidnoy,
// forced openings among them). The transcode must give the new kernel's own code
// BYTE FOR BYTE: that is the proof that it changes the header's radices and
// nothing of the game.
//
// Then the tool end to end on a SQLite file with the store's schema: a dry run
// writes nothing; a run without --backup is refused; a run transcodes every v10
// row, leaves a v11 row and a retired version alone, and leaves a backup holding
// the v10 rows; a second run writes nothing; and a row whose fool disagrees with
// its code fails the run with the file unchanged.
//
// MUTATION-CHECKED: the v11 radix back to v10's (per_suit11 = ACE_VALUE - min_v)
// fails "transcodes to the new kernel's own code"; dropping the fool comparison
// from gate() fails "a row whose fool disagrees fails the run".
#define CB_REPLAY_V11_NO_MAIN
#include "../migrate/replay_v11.c"

#include "t.h"
#include "store/store.h"
#include "util/err.h"
#include "../../../shared/c/hex/hex.h"

#include <stdlib.h>
#include <unistd.h>

typedef struct {
    int seats, no_pass, open, fool;
    const char *v10, *v11;
} Fixture;

static const Fixture FX[] = {
    { 2, 0, 0, 1,
      "03ccb1b3e910e34a3eeceb1a4a0471c6ab0e2f992c5e251d699ccde92262ba",
      "044647ea6632ffb386ca887d9344ffff806ff58c51e9e9c116d067a646aebb" },
    { 2, 1, -1, 0,
      "012b4b439df471625ff3a04ae15ce5bff0b14399dc321c2f076a",
      "0150b4ac11b2ff8eabf214543d888277eec76c0d17b85fb4e7cb" },
    { 5, 0, 2, 2,
      "1e0a45ab451d00c6050a1f84b96e5a062f5fc58f6bd9dd5deb6695b23afa9a",
      "21cb8e60adc0a0dec5ab6375509c2546f54bbe4159551909a8d3686882591b" },
    { 6, 0, -1, 5,
      "6314a5d64ba512ccfd8c8a1cb619c44cd1b19ccc56994b9dcbca4bbdcbff678f119cbb258b1dc365375b747bab26ef177567a1bea7cc73778321d9fba005c9e2093fd35aec906d0519e38dce90fa",
      "6b565e52d1f2d45e12ad959f1a9be9fde32b1488087b674047707cb847aa0585a869cabe01603e58514dbe30a414d85969daef3935c827c178ba017b42b0f00a34afcfa2804720c586b6844a71fb" },
    { 8, 0, 2, 5,
      "022769974d46f57e20ce0d384902129e3a5293c8d348a116ae8b3d9a14d058b58cc46566d41b70bb920dad058c6d4b86f800df9a94d8cfbc1cc196924babdeebc8518c73528d893a",
      "02555d0e93b789f34e348e524f17942b69d9756ee4e403d8926c2d66ebe1b56f587f6dda10730f75de397b70ad7667278cab9ce776958bb6747c631e7ca4dc2a190302d24419543b" },
    { 8, 1, -1, 2,
      "2273a2fd3c472b03986595e2b71cd4a22ed9af263c4f45f7590be1d766d36dae41fd69f8d709b8c885c23d2050dc47d76b936ad7c7dbb07a21587081b9897cf0a9d22a",
      "25529b3d014d1943e518b7b59bb4910508167dbec155e1214b2234a95a0fb6d21cd28822e8f532d93b926ce30243f87eb48a5e69c32dff2ef975248c88ff9caf62a38b" },
};
#define N_FX ((int)(sizeof FX / sizeof FX[0]))

static char g_dir[64];

static int unhex(const char *h, unsigned char *out) {
    const size_t n = strlen(h) / 2;
    return hex_decode(h, strlen(h), out, n) == (int)n ? (int)n : -1;
}

static void transcodes_to_the_new_kernels_own_code(void) {
    unsigned char a[512], b[512], out[600];
    for (int i = 0; i < N_FX; i++) {
        const int na = unhex(FX[i].v10, a), nb = unhex(FX[i].v11, b);
        T_CHECK(na > 0 && nb > 0);
        V10Head h;
        const int n = replay_v10_to_v11(a, na, out, (int)sizeof out, &h);
        T_EQ_INT(h.version, 10);
        T_EQ_INT(h.seats, FX[i].seats);
        T_EQ_INT(h.pass, !FX[i].no_pass);
        // transcodes to the new kernel's own code
        T_EQ_MEM(out, n, b, nb);
        T_CHECK(gate(out, n, &h, true, FX[i].fool) == NULL);

        // The shipped kernel reads 11 alone: the v10 code is refused by version.
        ReplayHeader hdr;
        T_EQ_INT(replay_decode_atoms_v6(a, na, &hdr, NULL, NULL), -REPLAY_EVERSION);
        T_EQ_INT(replay_last_error_detail(), 10);
        // And a v11 code is not transcoded again.
        T_EQ_INT(replay_v10_to_v11(b, nb, out, (int)sizeof out, &h), -1);
    }
}

// ---- the tool on a file --------------------------------------------------------------------------

static void insert(sqlite3 *db, int k, const unsigned char *moves, int n, int fool) {
    sqlite3_stmt *s = NULL;
    T_EQ_INT(sqlite3_prepare_v2(db, "INSERT INTO replays (id, game_id, players, moves, extras, created_at, fool, dur)"
                                    " VALUES (?1, NULL, ?2, ?3, NULL, ?4, ?5, NULL)", -1, &s, NULL), SQLITE_OK);
    unsigned char id[16] = {0}, players[16] = {0};
    id[15] = (unsigned char)k;
    sqlite3_bind_blob(s, 1, id, 16, SQLITE_TRANSIENT);
    sqlite3_bind_blob(s, 2, players, 16, SQLITE_TRANSIENT);
    sqlite3_bind_blob(s, 3, moves, n, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s, 4, 1000 + k);
    if (fool >= 0) sqlite3_bind_int(s, 5, fool); else sqlite3_bind_null(s, 5);
    T_EQ_INT(sqlite3_step(s), SQLITE_DONE);
    sqlite3_finalize(s);
}

// Row k's moves into out; its length, or -1.
static int moves_of(const char *path, int k, unsigned char *out) {
    sqlite3 *db = NULL;
    sqlite3_stmt *s = NULL;
    int n = -1;
    if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK
        && sqlite3_prepare_v2(db, "SELECT moves FROM replays WHERE id = ?1", -1, &s, NULL) == SQLITE_OK) {
        unsigned char id[16] = {0};
        id[15] = (unsigned char)k;
        sqlite3_bind_blob(s, 1, id, 16, SQLITE_TRANSIENT);
        if (sqlite3_step(s) == SQLITE_ROW) {
            n = sqlite3_column_bytes(s, 0);
            memcpy(out, sqlite3_column_blob(s, 0), (size_t)n);
        }
    }
    sqlite3_finalize(s);
    sqlite3_close(db);
    return n;
}

static int run(const char *a1, const char *a2, const char *a3) {
    char *argv[] = { "cbase-replay-v11", (char *)a1, (char *)a2, (char *)a3, NULL };
    int argc = 1 + (a1 != NULL) + (a2 != NULL) + (a3 != NULL);
    fflush(stdout);
    return cb_replay_v11_run(argc, argv);
}

// The file: every fixture's v10 code (rows 0..N_FX-1), the first fixture's v11
// code (row 50), and a retired version-7 code (row 51, the single byte 7).
static void make_db(const char *path, int bad_fool_row) {
    cb_store *st = NULL;
    T_EQ_INT(cb_store_open_sqlite(path, &st), CB_OK);   // the store's own schema
    cb_store_close(st);
    sqlite3 *db = NULL;
    T_EQ_INT(sqlite3_open(path, &db), SQLITE_OK);
    unsigned char m[512];
    for (int i = 0; i < N_FX; i++)
        insert(db, i, m, unhex(FX[i].v10, m), i == bad_fool_row ? (FX[i].fool + 1) % FX[i].seats : FX[i].fool);
    insert(db, 50, m, unhex(FX[0].v11, m), FX[0].fool);
    const unsigned char retired = 7;
    insert(db, 51, &retired, 1, -1);
    sqlite3_close(db);
}

static void the_tool_transcodes_a_file_once(void) {
    char path[128], bak[128];
    snprintf(path, sizeof path, "%s/a.sqlite", g_dir);
    snprintf(bak, sizeof bak, "%s/a_pre_v11.sqlite", g_dir);
    make_db(path, -1);
    unsigned char got[512], want[512];

    T_EQ_INT(run("--dry-run", path, NULL), 0);
    for (int i = 0; i < N_FX; i++) T_EQ_MEM(got, moves_of(path, i, got), want, unhex(FX[i].v10, want));
    T_EQ_INT(run(path, NULL, NULL), 2);   // a write without a backup is refused

    T_EQ_INT(run("--backup", bak, path), 0);
    for (int i = 0; i < N_FX; i++) {
        T_EQ_MEM(got, moves_of(path, i, got), want, unhex(FX[i].v11, want));
        T_EQ_MEM(got, moves_of(bak, i, got), want, unhex(FX[i].v10, want));
    }
    T_EQ_MEM(got, moves_of(path, 50, got), want, unhex(FX[0].v11, want));
    T_EQ_INT(moves_of(path, 51, got), 1);
    T_EQ_INT(got[0], 7);

    // A second run writes nothing (and its backup must be a new file).
    T_EQ_INT(run("--backup", bak, path), 2);
    snprintf(bak, sizeof bak, "%s/a_pre_v11_2.sqlite", g_dir);
    T_EQ_INT(run("--backup", bak, path), 0);
    for (int i = 0; i < N_FX; i++) T_EQ_MEM(got, moves_of(path, i, got), want, unhex(FX[i].v11, want));
}

static void a_row_whose_fool_disagrees_fails_the_run(void) {
    char path[128], bak[128];
    snprintf(path, sizeof path, "%s/b.sqlite", g_dir);
    snprintf(bak, sizeof bak, "%s/b_pre_v11.sqlite", g_dir);
    make_db(path, 3);
    T_EQ_INT(run("--backup", bak, path), 1);
    unsigned char got[512], want[512];
    for (int i = 0; i < N_FX; i++) T_EQ_MEM(got, moves_of(path, i, got), want, unhex(FX[i].v10, want));
}

int main(void) {
    snprintf(g_dir, sizeof g_dir, "/tmp/cbase-unit-replay-v11-XXXXXX");
    if (!mkdtemp(g_dir)) { perror("mkdtemp"); return 1; }
    T_RUN(transcodes_to_the_new_kernels_own_code);
    T_RUN(the_tool_transcodes_a_file_once);
    T_RUN(a_row_whose_fool_disagrees_fails_the_run);
    char cmd[128];
    snprintf(cmd, sizeof cmd, "rm -rf %s", g_dir);
    if (system(cmd) != 0) fprintf(stderr, "could not remove %s\n", g_dir);
    return T_DONE();
}
