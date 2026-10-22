// gauges.h - THE NUMBERS EVERY MODE WRITES (be_metrics.h gauges): the world's tables, the sockets,
// and the game server's resident set and CPU, read through its pid (shared/c/proc.h).
//
// The CPU is a WINDOW of the last BE_CPU_WINDOW seconds, not the last second: one second swings
// with whether a bot's turn or a SQLite checkpoint happened to land in it (13% and 1% of a core
// read a minute apart under the same load), and `ps -o %cpu`, which a reader checks it against,
// is itself a decaying average.
#ifndef BE_GAUGES_H
#define BE_GAUGES_H

#include <stdint.h>

#define BE_CPU_WINDOW 10

typedef struct {
    uint64_t cpu_us[BE_CPU_WINDOW + 1], at_us[BE_CPU_WINDOW + 1];
    int      n, head;
} be_cpu_window;

// One reading (a process's CPU time so far, and when, both in microseconds) into the window. The
// CPU over the window in hundredths of a percent of one core, or -1 while the window holds one
// reading. A reading below the last (another process behind the pid: the server restarted)
// starts the window over.
int64_t be_cpu_window_add(be_cpu_window *w, uint64_t cpu_us, uint64_t at_us);

// The world's and the sockets' gauges, and the server's RSS and CPU when server_pid is known.
// Once a second, on the ops timer.
void be_gauges_common(int server_pid);

#endif
