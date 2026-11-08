// m4a.c - see m4a.h.
#include "m4a.h"

#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

extern char **environ;

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint64_t be64(const uint8_t *p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }

// Walk the boxes in [off, end), descending into the containers on the way to
// the audio track's elst and mdhd (there is one track).
static void walk(const uint8_t *b, size_t off, size_t end, M4aInfo *m) {
    while (off + 8 <= end) {
        uint64_t size = be32(b + off);
        const char *t = (const char *)b + off + 4;
        size_t hdr = 8;
        if (size == 1 && off + 16 <= end) { size = be64(b + off + 8); hdr = 16; }
        if (size == 0) size = end - off;
        if (size < hdr || off + size > end) return;
        const uint8_t *p = b + off + hdr;
        if (!memcmp(t, "moov", 4) || !memcmp(t, "trak", 4) || !memcmp(t, "mdia", 4) || !memcmp(t, "edts", 4)) {
            walk(b, off + hdr, off + size, m);
        } else if (!memcmp(t, "mvhd", 4)) {
            m->movie_timescale = p[0] == 1 ? be32(p + 20) : be32(p + 12);
        } else if (!memcmp(t, "mdhd", 4)) {
            if (p[0] == 1) { m->media_timescale = be32(p + 20); m->media_duration = (int64_t)be64(p + 24); }
            else { m->media_timescale = be32(p + 12); m->media_duration = be32(p + 16); }
        } else if (!memcmp(t, "elst", 4)) {
            m->n_edits = (int)be32(p + 4);
            if (m->n_edits >= 1) {
                if (p[0] == 1) { m->edit_duration = (int64_t)be64(p + 8); m->edit_media_time = (int64_t)be64(p + 16); }
                else { m->edit_duration = be32(p + 8); m->edit_media_time = (int32_t)be32(p + 12); }
            }
        }
        off += (size_t)size;
    }
}

int m4a_info(const char *path, M4aInfo *m) {
    memset(m, 0, sizeof *m);
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)(n > 0 ? n : 1));
    const int ok = n > 0 && fread(b, 1, (size_t)n, f) == (size_t)n;
    fclose(f);
    if (ok) walk(b, 0, (size_t)n, m);
    free(b);
    m->bytes = n;
    return ok && m->media_timescale > 0 ? 0 : -1;
}

int m4a_encode(const char *wav, const char *m4a, int rate, int kbps) {
    char br[16], ts[16];
    snprintf(br, sizeof br, "%dk", kbps);
    snprintf(ts, sizeof ts, "%d", rate);
    // The movie's timescale is the sample rate, so the edit list says the take's
    // length to the sample (at ffmpeg's default 1000 it is rounded to the ms).
    // bitexact: no encoder version string, so one ffmpeg writes one file.
    char *const argv[] = {
        "ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error", "-y", "-i", (char *)wav,
        "-c:a", "aac", "-aac_coder", "fast", "-b:a", br, "-ac", "1",
        "-movie_timescale", ts, "-map_metadata", "-1", "-fflags", "+bitexact", "-flags:a", "+bitexact",
        (char *)m4a, NULL,
    };
    pid_t pid;
    if (posix_spawnp(&pid, "ffmpeg", NULL, NULL, argv, environ) != 0) return -1;
    return (int)pid;
}

int m4a_wait(int pid) {
    int st = 0;
    if (waitpid((pid_t)pid, &st, 0) < 0) return -1;
    return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : -1;
}
