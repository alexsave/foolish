// tune - the tuner's local endpoint: /sounds edits tuning.tsv through it, by ear.
//
// A DEV TOOL, never shipped and never run by a build. `npm run tune` (make -C
// tools/soundgen tune) starts it beside `npm run dev`; docs/AUDIO.md "How to
// tune" is the guide. It listens on 127.0.0.1 only (TUNE_PORT, default 3101)
// and answers the page on localhost, nothing else:
//
//   GET  /tuning            tuning.tsv as it is now
//   GET  /tuning/committed  tuning.tsv as git HEAD has it: what "changed" is measured from
//   POST /tuning            a whole new tuning.tsv: written, then tune.sh rebuilds every
//                           take, the take table and the kernel links that carry it.
//                           200 and the log, or 422 and the log with the old file put back
//
// It is the one writer of the file besides an editor: it never reads or keeps a
// value of its own, and soundgen is what decides whether a file is good (a
// line it cannot read stops tune.sh before anything is written).
//
// One request at a time, in order: a rebuild is a few seconds and the page
// sends the next only when the last has answered.

#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#define FILE_NAME "tuning.tsv"
#define MAX_BODY  (256 * 1024)
#define MAX_HEAD  8192

typedef struct { char *p; size_t n, cap; } Buf;
static void buf_add(Buf *b, const char *s, size_t n) {
    if (b->n + n + 1 > b->cap) {
        b->cap = (b->n + n + 1) * 2;
        b->p = realloc(b->p, b->cap);
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}

// Everything a shell command prints (stderr too), and its exit status.
static int run(const char *cmd, Buf *out) {
    FILE *p = popen(cmd, "r");
    if (!p) { buf_add(out, "tune: cannot run a command\n", 27); return -1; }
    char chunk[4096];
    size_t got;
    while ((got = fread(chunk, 1, sizeof chunk, p)) > 0) buf_add(out, chunk, got);
    const int st = pclose(p);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static int read_file(const char *path, Buf *out) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    char chunk[4096];
    size_t got;
    while ((got = fread(chunk, 1, sizeof chunk, f)) > 0) buf_add(out, chunk, got);
    fclose(f);
    return 0;
}

static int write_file(const char *path, const char *s, size_t n) {
    char tmp[256];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return -1;
    const size_t put = fwrite(s, 1, n, f);
    if (fclose(f) != 0 || put != n) { remove(tmp); return -1; }
    return rename(tmp, path);
}

static void send_all(int fd, const char *s, size_t n) {
    while (n > 0) {
        const ssize_t k = send(fd, s, n, 0);
        if (k <= 0) { if (k < 0 && errno == EINTR) continue; return; }
        s += k; n -= (size_t)k;
    }
}

static void respond_as(int fd, int code, const char *type, const char *origin, const char *body, size_t n);
static void respond(int fd, int code, const char *origin, const char *body, size_t n) {
    respond_as(fd, code, "text/plain; charset=utf-8", origin, body, n);
}
static void respond_as(int fd, int code, const char *type, const char *origin, const char *body, size_t n) {
    const char *what = code == 200 ? "OK" : code == 204 ? "No Content" : code == 403 ? "Forbidden"
                     : code == 404 ? "Not Found" : code == 413 ? "Payload Too Large" : code == 422 ? "Unprocessable Entity"
                     : "Bad Request";
    char head[1024];
    const int h = snprintf(head, sizeof head,
                           "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                           "Cache-Control: no-store\r\nConnection: close\r\n"
                           "Access-Control-Allow-Origin: %s\r\nAccess-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
                           "Access-Control-Allow-Headers: Content-Type\r\n\r\n",
                           code, what, type, n, origin && *origin ? origin : "null");
    send_all(fd, head, (size_t)h);
    send_all(fd, body, n);
}

// The page this answers: the dev server on this machine, at any port. A page
// from anywhere else gets nothing (the endpoint rewrites a file and runs a build).
static int local_origin(const char *o) {
    return !*o || !strncmp(o, "http://localhost:", 17) || !strncmp(o, "http://127.0.0.1:", 17);
}

// The value of header `name` in `head`, into out (empty when there is none).
static void header(const char *head, const char *name, char *out, size_t cap) {
    out[0] = 0;
    const size_t len = strlen(name);
    for (const char *l = strstr(head, "\r\n"); l; l = strstr(l + 2, "\r\n")) {
        const char *s = l + 2;
        if (!strncasecmp(s, name, len) && s[len] == ':') {
            s += len + 1;
            while (*s == ' ') s++;
            const size_t n = strcspn(s, "\r\n");
            snprintf(out, cap, "%.*s", (int)(n < cap ? n : cap - 1), s);
            return;
        }
    }
}

static void serve(int fd) {
    char head[MAX_HEAD + 1];
    size_t got = 0;
    char *end = NULL;
    while (got < MAX_HEAD && !(end = strstr(head, "\r\n\r\n"))) {
        const ssize_t k = recv(fd, head + got, MAX_HEAD - got, 0);
        if (k <= 0) return;
        got += (size_t)k;
        head[got] = 0;
    }
    if (!end) return;
    *end = 0;
    char method[8] = "", path[128] = "", origin[256], length[32];
    sscanf(head, "%7s %127s", method, path);
    header(head, "Origin", origin, sizeof origin);
    header(head, "Content-Length", length, sizeof length);
    if (!local_origin(origin)) { respond(fd, 403, NULL, "not from this machine\n", 22); return; }
    if (!strcmp(method, "OPTIONS")) { respond(fd, 204, origin, "", 0); return; }

    Buf out = { 0 };
    if (!strcmp(method, "GET") && !strcmp(path, "/tuning")) {
        if (read_file(FILE_NAME, &out) != 0) respond(fd, 404, origin, "no tuning.tsv\n", 14);
        else respond(fd, 200, origin, out.p, out.n);
    } else if (!strcmp(method, "GET") && !strcmp(path, "/tuning/committed")) {
        const int rc = run("git show HEAD:./" FILE_NAME " 2>/dev/null", &out);
        if (rc != 0) respond(fd, 404, origin, "", 0);
        else respond(fd, 200, origin, out.p ? out.p : "", out.n);
    } else if (!strcmp(method, "GET") && !strncmp(path, "/solo/", 6)) {
        // GET /solo/<cue>/<role>/<part>/<take>/<strokes>: one part of one take alone, as a WAV at
        // the game's level (soundgen solo), for the tuner's per-part Play. Names are
        // checked to [a-z0-9_] before they reach a shell.
        char cue[32], role[16], part[32];
        int take = -1, strokes = -1, used = 0;
        const int ok = sscanf(path + 6, "%31[a-z0-9_]/%15[a-z]/%31[a-z0-9_]/%d/%d%n", cue, role, part, &take, &strokes, &used) == 5
                       && path[6 + used] == 0 && take >= 0 && take < 64 && strokes >= 0 && strokes <= 9;
        if (!ok) { respond(fd, 400, origin, "want /solo/<cue>/<role>/<part>/<take>/<strokes>\n", 49); free(out.p); return; }
        char cmd[256];
        snprintf(cmd, sizeof cmd, "SOUNDGEN_CACHE=build/cache ./build/soundgen solo %s %s %s %d build/solo.wav %d 2>&1", cue, role, part, take, strokes);
        if (run(cmd, &out) != 0) respond(fd, 422, origin, out.p ? out.p : "", out.n);
        else {
            Buf wav = { 0 };
            if (read_file("build/solo.wav", &wav) != 0) respond(fd, 404, origin, "", 0);
            else respond_as(fd, 200, "audio/wav", origin, wav.p, wav.n);
            free(wav.p);
        }
    } else if (!strcmp(method, "POST") && !strcmp(path, "/tuning")) {
        const long n = strtol(length, NULL, 10);
        if (n <= 0 || n > MAX_BODY) { respond(fd, 413, origin, "want a tuning.tsv\n", 18); free(out.p); return; }
        Buf body = { 0 };
        const char *after = end + 4;
        buf_add(&body, after, got - (size_t)(after - head));
        while ((long)body.n < n) {
            char chunk[8192];
            const ssize_t k = recv(fd, chunk, sizeof chunk, 0);
            if (k <= 0) break;
            buf_add(&body, chunk, (size_t)k);
        }
        Buf old = { 0 };
        if ((long)body.n != n || read_file(FILE_NAME, &old) != 0) {
            respond(fd, 400, origin, "short body\n", 11);
        } else if (old.n == body.n && !memcmp(old.p, body.p, body.n)) {
            respond(fd, 200, origin, "unchanged\n", 10);
        } else if (write_file(FILE_NAME, body.p, body.n) != 0) {
            respond(fd, 400, origin, "cannot write tuning.tsv\n", 24);
        } else {
            fprintf(stderr, "tune: rebuilding\n");
            const int rc = run("bash ./tune.sh 2>&1", &out);
            if (rc != 0) write_file(FILE_NAME, old.p, old.n);
            fprintf(stderr, "%s", out.p ? out.p : "");
            respond(fd, rc == 0 ? 200 : 422, origin, out.p ? out.p : "", out.n);
        }
        free(body.p); free(old.p);
    } else {
        respond(fd, 404, origin, "GET /tuning, GET /tuning/committed, POST /tuning\n", 49);
    }
    free(out.p);
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    const char *p = getenv("TUNE_PORT");
    const int port = p && *p ? atoi(p) : 3101;
    const int s = socket(AF_INET, SOCK_STREAM, 0);
    const int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (s < 0 || bind(s, (struct sockaddr *)&a, sizeof a) != 0 || listen(s, 8) != 0) {
        fprintf(stderr, "tune: cannot listen on 127.0.0.1:%d\n", port);
        return 1;
    }
    fprintf(stderr, "tune: http://127.0.0.1:%d - open http://localhost:3000/sounds (npm run dev) to tune\n", port);
    for (;;) {
        const int c = accept(s, NULL, NULL);
        if (c < 0) { if (errno == EINTR) continue; return 1; }
        serve(c);
        close(c);
    }
}
