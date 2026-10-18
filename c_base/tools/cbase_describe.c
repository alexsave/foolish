// cbase_describe.c - `cbase-describe`: what the linked app needs from a
// provider to be deployed there (docs/PLATFORM.md 8.5), written to stdout.
//
//   cbase-describe --provider aws <skeleton>   the CloudFormation template
//                                              (providers/aws/ops/app.yaml.in)
//   cbase-describe --provider local            nothing: local deploys no
//                                              infrastructure (a SQLite file)
//   cbase-describe --settings                  the manifest's settings lines
//                                              (describe.h cb_aws_settings)
//
// The packager (tools/cbase) runs it; the Makefile runs it twice and compares
// the two outputs, so the template is a pure function of the build.
#include "cbase/app.h"
#include "describe.h"
#include "util/err.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int usage(void) {
    fprintf(stderr, "usage: cbase-describe --provider aws <skeleton> | --provider local | --settings\n");
    return 2;
}

static int put(cb_buf *b) {
    if (fwrite(b->p, 1, b->len, stdout) != b->len || fflush(stdout) != 0) {
        perror("cbase-describe: stdout");
        return 1;
    }
    return 0;
}

static char *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    cb_buf b;
    cb_buf_init(&b);
    char chunk[8192];
    size_t k;
    while ((k = fread(chunk, 1, sizeof chunk, f)) > 0) cb_buf_append(&b, chunk, k);
    int bad = ferror(f) || b.oom;
    fclose(f);
    if (bad) {
        cb_buf_free(&b);
        return NULL;
    }
    return (char *)cb_buf_take(&b, n);
}

int main(int argc, char **argv) {
    char err[256] = "";
    cb_buf out;
    cb_buf_init(&out);
    int rc;
    if (argc == 2 && strcmp(argv[1], "--settings") == 0) {
        rc = cb_aws_settings(cb_main_app, &out, err, sizeof err);
    } else if (argc == 3 && strcmp(argv[1], "--provider") == 0 && strcmp(argv[2], "local") == 0) {
        rc = CB_OK;
    } else if (argc == 4 && strcmp(argv[1], "--provider") == 0 && strcmp(argv[2], "aws") == 0) {
        size_t n = 0;
        char *skel = slurp(argv[3], &n);
        if (!skel) {
            perror(argv[3]);
            return 1;
        }
        rc = cb_aws_describe(cb_main_app, skel, n, &out, err, sizeof err);
        free(skel);
    } else {
        return usage();
    }
    if (rc == CB_OK && out.oom) {
        rc = CB_E_NOMEM;
        snprintf(err, sizeof err, "out of memory");
    }
    if (rc != CB_OK) {
        fprintf(stderr, "cbase-describe: %s\n", err);
        cb_buf_free(&out);
        return 1;
    }
    int w = put(&out);
    cb_buf_free(&out);
    return w;
}
