// sim.h - THE FAKE HUMANS: players that sign up, sit down at tables with each other and with the
// server's bots, and play through the real protocol, each on its own /ws socket, choosing moves
// with the kernel's own client table and brain. A few threads own the players between them; a
// table's humans all belong to one thread, so no player ever waits on another thread.
#ifndef BE_SIM_H
#define BE_SIM_H

#include <stddef.h>
#include <stdint.h>

#define BE_SIM_MAX_PLAYERS 2048
#define BE_SIM_MAX_THREADS 32

typedef struct {
    char     server[128];   // host:port of the game server
    int      players;
    int      tables;        // kept live at once, across the threads
    int      threads;
    int      think_min_ms, think_max_ms;
    uint64_t seed;
    int      bot_share;     // percent of tables with a server bot
    int      podkidnoy;     // percent of tables under podkidnoy
    int      hostile_every; // one move in N; 0 never
    int      rematch;       // percent of finished games that rematch
} be_sim_opts;

// Signs the players up and starts the threads. CB_OK, or a CB_E_* with a sentence in err.
int  be_sim_start(const be_sim_opts *o, char *err, size_t cap);
// Stops the threads (every player leaves its table first, as far as a few seconds allow).
void be_sim_stop(void);

// Writes the sim's own gauges into the registry (be_metrics.h): the players, the seats, the bots,
// the threads (gauges.h writes the rest). Called before each report (the ops timer, once a second).
void be_sim_gauges(void);

#endif
