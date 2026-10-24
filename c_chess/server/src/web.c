/* web.c - the site itself, from the same process: every file of the built site (CC_WEB_DIR, the
 * Next.js static export) is read into memory at start, with its gzip twin when the build made one
 * (name.gz), and answered from memory on the loop thread. A page is asked for without its .html
 * ("/game" is game.html). Hashed assets (/_next/static/) are cached for a year; pages are
 * revalidated every time. */
#include "web.h"

#include "net/http.h"
#include "util/log.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

typedef struct {
    char    *path;          /* "/index.html" */
    uint8_t *body, *gz;
    size_t   n, gz_n;
    const char *type;
} wfile;

static wfile *FILES;
static size_t n_files, cap_files;

static const char *type_of(const char *path) {
    const char *e = strrchr(path, '.');
    if (!e) return "application/octet-stream";
    static const char *const T[][2] = {
        { ".html", "text/html; charset=utf-8" }, { ".js", "text/javascript; charset=utf-8" }, { ".css", "text/css; charset=utf-8" },
        { ".wasm", "application/wasm" }, { ".svg", "image/svg+xml" }, { ".png", "image/png" }, { ".ico", "image/x-icon" },
        { ".json", "application/json" }, { ".txt", "text/plain; charset=utf-8" }, { ".woff2", "font/woff2" }, { ".webmanifest", "application/manifest+json" },
    };
    for (size_t i = 0; i < sizeof T / sizeof *T; i++)
        if (strcmp(e, T[i][0]) == 0) return T[i][1];
    return "application/octet-stream";
}

static uint8_t *slurp(const char *file, size_t *n) {
    FILE *f = fopen(file, "rb");
    if (!f) return NULL;
    struct stat st;
    if (fstat(fileno(f), &st) != 0 || st.st_size < 0 || st.st_size > (64 << 20)) { fclose(f); return NULL; }
    uint8_t *p = malloc((size_t)st.st_size + 1);
    if (p && fread(p, 1, (size_t)st.st_size, f) != (size_t)st.st_size) { free(p); p = NULL; }
    fclose(f);
    *n = (size_t)st.st_size;
    return p;
}

static wfile *find(const char *path, size_t len) {
    for (size_t i = 0; i < n_files; i++)
        if (strlen(FILES[i].path) == len && memcmp(FILES[i].path, path, len) == 0) return &FILES[i];
    return NULL;
}

static void walk(const char *root, const char *rel) {
    char dir[1024];
    snprintf(dir, sizeof dir, "%s%s", root, rel);
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char r[1024], full[2048];
        snprintf(r, sizeof r, "%s/%s", rel, e->d_name);
        snprintf(full, sizeof full, "%s%s", root, r);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) { walk(root, r); continue; }
        size_t len = strlen(r);
        if (len > 3 && strcmp(r + len - 3, ".gz") == 0) continue;   /* a twin, read with its file */
        if (n_files == cap_files) {
            size_t c = cap_files ? cap_files * 2 : 64;
            wfile *nf = realloc(FILES, c * sizeof *nf);
            if (!nf) break;
            FILES = nf; cap_files = c;
        }
        wfile *w = &FILES[n_files];
        memset(w, 0, sizeof *w);
        if (!(w->body = slurp(full, &w->n))) continue;
        char gz[2100];
        snprintf(gz, sizeof gz, "%s.gz", full);
        w->gz = slurp(gz, &w->gz_n);
        w->path = strdup(r);
        w->type = type_of(r);
        n_files++;
    }
    closedir(d);
}

int cc_web_load(const char *dir) {
    if (!dir || !*dir) return 0;
    walk(dir, "");
    size_t bytes = 0;
    for (size_t i = 0; i < n_files; i++) bytes += FILES[i].n + FILES[i].gz_n;
    CB_INFO("cc", "web: %zu files (%zu KB) from %s", n_files, bytes >> 10, dir);
    return (int)n_files;
}

void cc_web_free(void) {
    for (size_t i = 0; i < n_files; i++) { free(FILES[i].path); free(FILES[i].body); free(FILES[i].gz); }
    free(FILES);
    FILES = NULL;
    n_files = cap_files = 0;
}

static bool accepts_gzip(const cb_http_req *req) {
    const char *ae = cb_http_header_get(req, "accept-encoding");
    return ae && strstr(ae, "gzip");
}

/* A ".." path segment (a way out of the site's tree). Two dots inside a name are fine: the
 * framework's chunk names have them ("0sf..t3v.psx1.js"). The paths only ever match the files read
 * at start, so this is a belt beside the table, not the only guard. */
static bool dot_segment(const char *p, size_t len) {
    for (size_t i = 0; i + 1 < len; i++)
        if (p[i] == '.' && p[i + 1] == '.' && (i == 0 || p[i - 1] == '/') && (i + 2 == len || p[i + 2] == '/')) return true;
    return false;
}

static void h_web(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    (void)ctx;
    const char *p = req->path;
    size_t len = strcspn(p, "?#");
    if (len == 0 || p[0] != '/' || dot_segment(p, len)) { cb_res_text(res, 400, "bad path\n"); return; }
    wfile *w = NULL;
    if (len == 1) w = find("/index.html", 11);
    else if (!(w = find(p, len))) {
        char page[600];
        if (len < 500) {
            memcpy(page, p, len);
            memcpy(page + len, ".html", 6);
            w = find(page, len + 5);
        }
    }
    if (!w) w = find("/404.html", 9);
    if (!w) { cb_res_text(res, 404, "not found\n"); return; }
    bool gz = w->gz && accepts_gzip(req);
    cb_res_send(res, strcmp(w->path, "/404.html") == 0 && len > 1 && strncmp(p, "/404", 4) != 0 ? 404 : 200, w->type,
                gz ? w->gz : w->body, gz ? w->gz_n : w->n);
    if (gz) cb_res_header(res, "Content-Encoding", "gzip");
    cb_res_header(res, "Vary", "Accept-Encoding");
    cb_res_header(res, "Cache-Control", strncmp(w->path, "/_next/static/", 14) == 0 ? "public, max-age=31536000, immutable" : "no-cache");
}

static const cb_route ROUTES[] = {
    { "GET", "/*", CB_ROUTE_POOL | CB_ROUTE_INLINE | CB_ROUTE_HOME, h_web, 300 },
};
const cb_route_list cc_web_routes = { ROUTES, 1 };
