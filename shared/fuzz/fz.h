/* The harness the product byte-decoder fuzz targets share (see Makefile).
 *
 * A target is fz_<name>.c: it defines LLVMFuzzerTestOneInput and, for the seed
 * writer, fz_seeds(const char *dir) built from the product's REAL encoders.
 * Compiled with -DFZ_SEEDS the file is an ordinary program that writes the
 * seeds (main below); otherwise libFuzzer supplies main.
 */
#ifndef FZ_H
#define FZ_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

/* Write one seed file <dir>/<name>. */
static inline void fz_put(const char *dir, const char *name, const void *p, int n)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f || (n > 0 && fwrite(p, 1, (size_t)n, f) != (size_t)n)) { perror(path); exit(1); }
    fclose(f);
}

/* A NUL-terminated copy of the input, for the text readers. */
static inline char *fz_text(const uint8_t *d, size_t n, char *buf, size_t cap)
{
    if (n >= cap) n = cap - 1;
    memcpy(buf, d, n);
    buf[n] = 0;
    return buf;
}

#ifdef FZ_SEEDS
void fz_seeds(const char *dir);
int main(int argc, char **argv)
{
    if (argc != 2) { fprintf(stderr, "usage: %s <corpus dir>\n", argv[0]); return 2; }
    fz_seeds(argv[1]);
    return 0;
}
#endif

#endif
