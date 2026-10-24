/* engine.c - see engine.h. */
#include "engine.h"

#include "util/err.h"
#include "util/log.h"
#include "util/clock.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __linux__
#include <sched.h>
#endif

#define MAX_ENGINES 64
#define LINE_MAX_ 4096

typedef struct job {
    struct job   *next;
    cc_engine_ask ask;
    ch_move      *moves;
    cc_engine_fn  fn;
    void         *ud;
    int           tries;
    int64_t       queued_us;
} job;

typedef struct {
    pid_t   pid;
    int     in, out;           /* we write `in`, read `out` */
    char    buf[LINE_MAX_];
    size_t  len;
    job    *busy;              /* the search it runs */
    int64_t started_us;
    cc_engine_answer last;     /* the deepest info seen of the running search */
    bool    ready;
} engine;

static struct {
    char            path[1024];
    int             n, hash_mb, queue_max;
    engine          e[MAX_ENGINES];
    pthread_t       thread;
    pthread_mutex_t mu;
    job            *head, *tail;
    int             queued;
    int             wake[2];
    bool            running, stop;
    uint64_t        searches, nodes;
} E = { .mu = PTHREAD_MUTEX_INITIALIZER, .wake = { -1, -1 } };

static int env_int(const char *name, int dflt, int lo, int hi) {
    const char *v = getenv(name);
    if (!v || !*v) return dflt;
    int x = atoi(v);
    return x < lo ? lo : x > hi ? hi : x;
}

static void say(engine *g, const char *s) {
    size_t n = strlen(s);
    while (n) {
        ssize_t k = write(g->in, s, n);
        if (k < 0) { if (errno == EINTR) continue; return; }
        s += k;
        n -= (size_t)k;
    }
}

/* Starts one engine process: stdin and stdout piped, background priority, the rest of its setup by
 * UCI (answered later on the manager thread). */
static int spawn(engine *g) {
    int to[2], from[2];
    if (pipe(to) != 0) return -1;
    if (pipe(from) != 0) { close(to[0]); close(to[1]); return -1; }
    pid_t pid = fork();
    if (pid < 0) { close(to[0]); close(to[1]); close(from[0]); close(from[1]); return -1; }
    if (pid == 0) {
        dup2(to[0], 0);
        dup2(from[1], 1);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) dup2(devnull, 2);
        for (int fd = 3; fd < 1024; fd++) close(fd);
        setpriority(PRIO_PROCESS, 0, 19);   /* the lowest: the move path never waits for an engine */
#ifdef __linux__
        const char *cpus = getenv("CC_ENGINE_CPUS");   /* "4-7": the background CPU set */
        if (cpus && *cpus) {
            cpu_set_t set;
            CPU_ZERO(&set);
            int a = 0, b = 0;
            if (sscanf(cpus, "%d-%d", &a, &b) == 2) for (int c = a; c <= b; c++) CPU_SET(c, &set);
            else CPU_SET(atoi(cpus), &set);
            sched_setaffinity(0, sizeof set, &set);
        }
#endif
        execl(E.path, E.path, (char *)NULL);
        _exit(127);
    }
    close(to[0]);
    close(from[1]);
    fcntl(from[0], F_SETFL, fcntl(from[0], F_GETFL) | O_NONBLOCK);
    fcntl(to[1], F_SETFD, FD_CLOEXEC);
    fcntl(from[0], F_SETFD, FD_CLOEXEC);
    g->pid = pid;
    g->in = to[1];
    g->out = from[0];
    g->len = 0;
    g->ready = false;
    g->busy = NULL;
    char opts[256];
    snprintf(opts, sizeof opts, "uci\nsetoption name Threads value 1\nsetoption name Hash value %d\nisready\n", E.hash_mb);
    say(g, opts);
    return 0;
}

static void reap(engine *g) {
    if (g->pid > 0) {
        kill(g->pid, SIGKILL);
        waitpid(g->pid, NULL, 0);
    }
    if (g->in >= 0) close(g->in);
    if (g->out >= 0) close(g->out);
    g->pid = -1;
    g->in = g->out = -1;
}

static void uci_square(char *o, int sq) { o[0] = (char)('a' + sq % 8); o[1] = (char)('1' + sq / 8); }

static void begin(engine *g, job *j) {
    g->busy = j;
    g->started_us = cb_mono_us();
    memset(&g->last, 0, sizeof g->last);
    size_t cap = 160 + (size_t)j->ask.n * 6;
    char *cmd = malloc(cap);
    if (!cmd) return;
    int o = snprintf(cmd, cap, "setoption name Skill Level value %d\nposition startpos%s", j->ask.skill, j->ask.n ? " moves" : "");
    for (int i = 0; i < j->ask.n; i++) {
        ch_move m = j->moves[i];
        cmd[o++] = ' ';
        uci_square(cmd + o, CH_FROM(m)); o += 2;
        uci_square(cmd + o, CH_TO(m)); o += 2;
        if (CH_PROMO(m)) cmd[o++] = "?nbrq"[CH_PROMO(m)];
    }
    o += snprintf(cmd + o, cap - (size_t)o, "\ngo");
    if (j->ask.movetime_ms) o += snprintf(cmd + o, cap - (size_t)o, " movetime %d", j->ask.movetime_ms);
    if (j->ask.depth) o += snprintf(cmd + o, cap - (size_t)o, " depth %d", j->ask.depth);
    if (j->ask.nodes) o += snprintf(cmd + o, cap - (size_t)o, " nodes %llu", (unsigned long long)j->ask.nodes);
    snprintf(cmd + o, cap - (size_t)o, "\n");
    say(g, cmd);
    free(cmd);
}

static ch_move parse_uci_move(const char *s) {
    if (strlen(s) < 4 || s[0] < 'a' || s[0] > 'h' || s[2] < 'a' || s[2] > 'h' || s[1] < '1' || s[1] > '8' || s[3] < '1' || s[3] > '8')
        return CH_MOVE_NONE;
    int from = (s[1] - '1') * 8 + (s[0] - 'a'), to = (s[3] - '1') * 8 + (s[2] - 'a'), pr = 0;
    switch (s[4]) { case 'n': pr = CH_KNIGHT; break; case 'b': pr = CH_BISHOP; break; case 'r': pr = CH_ROOK; break; case 'q': pr = CH_QUEEN; break; default: break; }
    return CH_MOVE(from, to, pr);
}

static void finish(engine *g, ch_move best) {
    job *j = g->busy;
    g->busy = NULL;
    if (!j) return;
    cc_engine_answer a = g->last;
    a.best = best;
    a.time_ms = (int)((cb_mono_us() - g->started_us) / 1000);
    pthread_mutex_lock(&E.mu);
    E.searches++;
    E.nodes += a.nodes;
    pthread_mutex_unlock(&E.mu);
    j->fn(j->ud, &a);
    free(j->moves);
    free(j);
}

static void line(engine *g, char *s) {
    if (!g->ready) {
        if (strcmp(s, "readyok") == 0) g->ready = true;
        return;
    }
    if (!g->busy) return;
    if (strncmp(s, "info ", 5) == 0) {
        char *t = strstr(s, " depth ");
        if (t) g->last.depth = atoi(t + 7);
        if ((t = strstr(s, " score cp "))) g->last.score_cp = atoi(t + 10);
        else if ((t = strstr(s, " score mate "))) { int m = atoi(t + 12); g->last.score_cp = m > 0 ? 30000 - 2 * m : -30000 - 2 * m; }
        if ((t = strstr(s, " nodes "))) g->last.nodes = strtoull(t + 7, NULL, 10);
    } else if (strncmp(s, "bestmove ", 9) == 0) {
        char mv[8] = { 0 };
        sscanf(s + 9, "%7s", mv);
        finish(g, strcmp(mv, "(none)") == 0 ? CH_MOVE_NONE : parse_uci_move(mv));
    }
}

static void requeue_or_fail(job *j) {
    if (!j) return;
    if (++j->tries < 2) {
        pthread_mutex_lock(&E.mu);
        j->next = E.head;
        E.head = j;
        if (!E.tail) E.tail = j;
        E.queued++;
        pthread_mutex_unlock(&E.mu);
        return;
    }
    cc_engine_answer a = { 0 };
    j->fn(j->ud, &a);
    free(j->moves);
    free(j);
}

static void *manager(void *ud) {
    (void)ud;
    struct pollfd p[MAX_ENGINES + 1];
    while (!E.stop) {
        /* idle, ready engines take queued searches */
        for (int i = 0; i < E.n; i++) {
            engine *g = &E.e[i];
            if (g->pid <= 0 || !g->ready || g->busy) continue;
            pthread_mutex_lock(&E.mu);
            job *j = E.head;
            if (j) { E.head = j->next; if (!E.head) E.tail = NULL; E.queued--; }
            pthread_mutex_unlock(&E.mu);
            if (!j) break;
            begin(g, j);
        }
        p[0] = (struct pollfd){ E.wake[0], POLLIN, 0 };
        for (int i = 0; i < E.n; i++) p[1 + i] = (struct pollfd){ E.e[i].out, POLLIN, 0 };
        int r = poll(p, (nfds_t)(E.n + 1), 1000);
        if (r < 0 && errno != EINTR) break;
        if (p[0].revents & POLLIN) { char b[64]; while (read(E.wake[0], b, sizeof b) > 0) {} }
        for (int i = 0; i < E.n; i++) {
            engine *g = &E.e[i];
            if (!(p[1 + i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            for (;;) {
                ssize_t k = read(g->out, g->buf + g->len, sizeof g->buf - 1 - g->len);
                if (k < 0 && (errno == EAGAIN || errno == EINTR)) break;
                if (k <= 0) {   /* the engine died: a new one, and its search again once */
                    CB_WARN("cc", "engine %d (pid %d) exited; restarting it", i, (int)g->pid);
                    job *j = g->busy;
                    g->busy = NULL;
                    reap(g);
                    requeue_or_fail(j);
                    if (!E.stop) spawn(g);
                    break;
                }
                g->len += (size_t)k;
                g->buf[g->len] = 0;
                char *s = g->buf, *nl;
                while ((nl = memchr(s, '\n', g->len - (size_t)(s - g->buf)))) {
                    *nl = 0;
                    if (nl > s && nl[-1] == '\r') nl[-1] = 0;
                    line(g, s);
                    s = nl + 1;
                }
                g->len -= (size_t)(s - g->buf);
                memmove(g->buf, s, g->len);
                if (g->len == sizeof g->buf - 1) g->len = 0;   /* a line longer than the buffer: dropped */
            }
        }
    }
    return NULL;
}

int cc_engine_start(void) {
    const char *path = getenv("CC_ENGINE");
    if (!path || !*path) return CB_OK;
    snprintf(E.path, sizeof E.path, "%s", path);
    if (access(E.path, X_OK) != 0) { CB_ERROR("cc", "engine: %s is not an executable", E.path); return CB_E_INVALID; }
    E.n = env_int("CC_ENGINES", 2, 1, MAX_ENGINES);
    E.hash_mb = env_int("CC_ENGINE_HASH_MB", 16, 1, 4096);
    E.queue_max = env_int("CC_ENGINE_QUEUE", 256, 1, 1 << 20);
    if (pipe(E.wake) != 0) return CB_E_NOMEM;
    fcntl(E.wake[0], F_SETFL, O_NONBLOCK);
    fcntl(E.wake[1], F_SETFL, O_NONBLOCK);
    for (int i = 0; i < E.n; i++) {
        E.e[i].in = E.e[i].out = -1;
        if (spawn(&E.e[i]) != 0) { CB_ERROR("cc", "engine: could not start %s", E.path); return CB_E_NOMEM; }
    }
    E.stop = false;
    if (pthread_create(&E.thread, NULL, manager, NULL) != 0) return CB_E_NOMEM;
    E.running = true;
    CB_INFO("cc", "engine: %d x %s, hash %d MB each, queue %d", E.n, E.path, E.hash_mb, E.queue_max);
    return CB_OK;
}

bool cc_engine_available(void) { return E.running; }

void cc_engine_stop(void) {
    if (!E.running) return;
    E.stop = true;
    if (write(E.wake[1], "x", 1) < 0) {}
    pthread_join(E.thread, NULL);
    for (int i = 0; i < E.n; i++) {
        if (E.e[i].busy) { job *j = E.e[i].busy; E.e[i].busy = NULL; free(j->moves); free(j); }
        reap(&E.e[i]);
    }
    for (job *j = E.head, *nx; j; j = nx) { nx = j->next; free(j->moves); free(j); }
    E.head = E.tail = NULL;
    E.queued = 0;
    close(E.wake[0]);
    close(E.wake[1]);
    E.running = false;
}

int cc_engine_search(const cc_engine_ask *ask, cc_engine_fn fn, void *ud) {
    if (!E.running) return CB_E_STATE;
    job *j = calloc(1, sizeof *j);
    if (!j) return CB_E_NOMEM;
    j->ask = *ask;
    j->moves = malloc(sizeof *j->moves * (size_t)(ask->n ? ask->n : 1));
    if (!j->moves) { free(j); return CB_E_NOMEM; }
    memcpy(j->moves, ask->moves, sizeof *j->moves * (size_t)ask->n);
    j->ask.moves = j->moves;
    j->fn = fn;
    j->ud = ud;
    j->queued_us = cb_mono_us();
    pthread_mutex_lock(&E.mu);
    if (E.queued >= E.queue_max) { pthread_mutex_unlock(&E.mu); free(j->moves); free(j); return CB_E_FULL; }
    if (E.tail) E.tail->next = j; else E.head = j;
    E.tail = j;
    E.queued++;
    pthread_mutex_unlock(&E.mu);
    if (write(E.wake[1], "x", 1) < 0) {}
    return CB_OK;
}

void cc_engine_counts(uint64_t *searches, uint64_t *nodes, uint32_t *queued, uint32_t *busy) {
    pthread_mutex_lock(&E.mu);
    *searches = E.searches;
    *nodes = E.nodes;
    *queued = (uint32_t)E.queued;
    pthread_mutex_unlock(&E.mu);
    uint32_t b = 0;
    for (int i = 0; i < E.n; i++) b += E.e[i].busy != NULL;
    *busy = b;
}
