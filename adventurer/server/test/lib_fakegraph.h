// lib_fakegraph.h - a synthetic road graph for the tests: a street grid around a point, built
// through the real builder (graph.h) and written to a temporary file, so every test reads the
// file format the server reads. Linked into every test program.
//
// THE GRID: `cols` by `rows` blocks of `block_m` metres, streets along every grid line, the
// south-west corner at (lat0, lon0). Each street between two crossings is one OSM way (so every
// crossing is a node), with a shape point every `shape_m` metres. Two regions split the grid
// down the middle ("West" and "East") unless `regions` is false. All streets allow bike and foot
// unless the caller marks some otherwise through fg_opts.
#ifndef ADV_LIB_FAKEGRAPH_H
#define ADV_LIB_FAKEGRAPH_H

#include "graph.h"

#include <stdbool.h>

typedef struct {
    int    cols, rows;        // blocks (so cols+1 north-south streets, rows+1 east-west ones)
    double block_m;           // 0: 120
    double shape_m;           // 0: 40 (a shape point this often; 0 with no interior points: set < 0)
    adv_pt sw;                // 0,0: a default in a flat part of the world (lat 41.8, lon -71.4)
    bool   regions;           // two regions down the middle
    bool   foot_only_rows;    // every other east-west street is foot-only (a footpath)
} fg_opts;

// Builds the grid into `path` (the caller's temporary file; fg_tmp_path makes one) and opens it
// into g. 0, or -1 having printed why.
int fg_build(const fg_opts *o, const char *path, adv_graph *g);
// A fresh temporary path under the scratch directory ($TMPDIR or /tmp), into out.
void fg_tmp_path(char *out, size_t cap, const char *tag);

// Where the grid's crossing (i, j) is (i east, j north, 0-based) under the same options.
adv_pt fg_crossing(const fg_opts *o, int i, int j);

#endif
