/* anim_run.c - see anim_run.h. The capture is table.c's (table_snap, scope_open, table_push); the
 * read is the web's (src/state/pushSequence.ts over client_push_next). */
#include "anim_run.h"
#include "evwire.h"

/* The operation's snapshots: the Game up to its log, as table.c keeps them (TABLE_SNAP_BYTES). */
enum { SNAP_BYTES = offsetof(Game, num_logs) };
static struct { _Alignas(8) unsigned char bytes[SNAP_BYTES]; } snap_slot[MAX_SNAPS];
static int snap_tag[MAX_SNAPS], snap_aux[MAX_SNAPS], n_snaps, capturing, log_start;
static void (*prev_hook)(const Game *, int, int); /* whatever hook was there, put back after */

static void snap(const Game *g, int tag, int aux) {
  if (!capturing || n_snaps >= MAX_SNAPS) return; /* MAX_SNAPS holds the widest operation (game.h) */
  __builtin_memcpy(snap_slot[n_snaps].bytes, g, SNAP_BYTES);
  snap_tag[n_snaps] = tag;
  snap_aux[n_snaps] = aux;
  n_snaps++;
}

void run_capture_begin(const Game *g) {
  n_snaps = 0;
  log_start = g->num_logs;
  capturing = 1;
  prev_hook = engine_snap_hook;
  engine_snap_hook = snap;
}

/* The stream as bytes (a deal's, the largest, is about 4 KB at two seats), and the slot it is
 * read back through. */
static unsigned char wire[32 * 1024];
static ClientSlot read_slot;
static ClientTable reader;

int run_capture_end(const Game *g, int viewer, int actor, int ended, RunStep *steps, int cap) {
  engine_snap_hook = prev_hook;
  capturing = 0;
  EvSnap refs[MAX_SNAPS];
  for (int i = 0; i < n_snaps; i++) {
    refs[i].g = (const Game *)(const void *)snap_slot[i].bytes;
    refs[i].tag = snap_tag[i];
    refs[i].aux = snap_aux[i];
  }
  int len = evwire_serialize(refs, n_snaps, g->logs + log_start, g->num_logs - log_start, g, viewer, actor, ended,
                             wire, (int)sizeof wire);
  if (len < 0) return -1;
  static int inited;
  if (!inited) { client_init(&reader, &read_slot); inited = 1; }
  if (client_push_open(&reader, wire, len, 0, 0, 0, 0) != CLIENT_OK) return -1;
  int n = 0, rc;
  while ((rc = client_push_next(&reader)) == 1) {
    if (n >= cap) return -1;
    steps[n].ev = reader.event;
    steps[n].board = reader.view;
    n++;
  }
  if (rc != 0 || client_push_final(&reader) != CLIENT_OK) return -1;
  return n;
}

const TableView *run_capture_final(void) { return &reader.view; }
