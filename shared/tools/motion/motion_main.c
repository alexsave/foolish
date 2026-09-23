/* motion - the ruler finder and the ride scorer, from the command line.
 *
 *   motion find --size WxH --times FILE [--scale S] < frames.rgb > take.tbl
 *       frames.rgb: packed rgb24 frames, one after another (ffmpeg -f rawvideo
 *       -pix_fmt rgb24 -fps_mode passthrough); FILE: one presentation time per
 *       frame (ffprobe). S: pixels per point (default 3 if H >= 2000 else 2).
 *
 *   motion score [--name N] [--span S] [--snap P] [--whole] [--side FILE]
 *                [--anchor MARK=red|green|mid|none]... take.tbl...
 *       Every mark against its anchor, averaged over the takes (the largest
 *       snap is the largest of any take). --side FILE: "h side" lines, the
 *       product's board side for a drawer height, so a board mark off the
 *       centre is scored against the board's scale (motion.h).
 *
 *   motion pace --size WxH --times FILE --box X,Y,W,H [--lum L] < frames.rgb
 *       How often the box's pixels change (a recording keeps a frame only
 *       when the screen changed, so these are the frames our content drew)
 *       and how much ink each one laid: a "t ink changed" row per frame,
 *       then a "pace" line - frames, fps, the largest gap (ms), the largest
 *       one-frame share of the ink, and judder (motion.h MtPace).
 *
 * maxstep is the largest change of a mark's offset from its anchor in one
 * frame, maxsnap the largest of those above --snap (ride.py's). Output is
 * fixed-layout text. motion_take.sh films the pipe for a movie. */
#include "motion.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int usage(void) {
    fprintf(stderr, "usage: motion find --size WxH --times FILE [--scale S] < rgb\n"
                    "       motion score [--name N] [--span S] [--snap P] [--whole] [--side FILE]"
                    " [--anchor MARK=red|green|mid|none] take.tbl...\n"
                    "       motion pace --size WxH --times FILE --box X,Y,W,H [--lum L] < rgb\n");
    return 2;
}

static int find_main(int argc, char **argv) {
    int32_t W = 0, H = 0;
    double scale = 0;
    const char *times = NULL;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--size") && i + 1 < argc) sscanf(argv[++i], "%dx%d", &W, &H);
        else if (!strcmp(argv[i], "--times") && i + 1 < argc) times = argv[++i];
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) scale = atof(argv[++i]);
        else return usage();
    }
    if (W <= 0 || H <= 0 || !times) return usage();
    if (scale <= 0) scale = H >= 2000 ? 3.0 : 2.0;
    FILE *tf = fopen(times, "r");
    if (!tf) { perror(times); return 1; }
    size_t sz = (size_t)W * (size_t)H * 3;
    uint8_t *buf = malloc(sz);
    MtRow row;
    mt_write_header(stdout);
    double t;
    int32_t n = 0;
    while (fread(buf, 1, sz, stdin) == sz) {
        if (fscanf(tf, "%lf", &t) != 1) { fprintf(stderr, "motion: more frames than times (%d)\n", n); break; }
        row.t = t;
        mt_find(buf, W, H, scale, &row);
        mt_write_row(stdout, &row);
        n++;
    }
    fclose(tf);
    free(buf);
    fprintf(stderr, "motion: %d frames\n", n);
    return n > 0 ? 0 : 1;
}

static int pace_main(int argc, char **argv) {
    int32_t W = 0, H = 0, lum = 150;
    MtBox b = {0, 0, 0, 0};
    const char *times = NULL;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--size") && i + 1 < argc) sscanf(argv[++i], "%dx%d", &W, &H);
        else if (!strcmp(argv[i], "--times") && i + 1 < argc) times = argv[++i];
        else if (!strcmp(argv[i], "--box") && i + 1 < argc)
            sscanf(argv[++i], "%d,%d,%d,%d", &b.x, &b.y, &b.w, &b.h);
        else if (!strcmp(argv[i], "--lum") && i + 1 < argc) lum = atoi(argv[++i]);
        else return usage();
    }
    if (W <= 0 || H <= 0 || !times || b.w <= 0 || b.h <= 0) return usage();
    FILE *tf = fopen(times, "r");
    if (!tf) { perror(times); return 1; }
    size_t sz = (size_t)W * (size_t)H * 3;
    uint8_t *buf = malloc(sz);
    int32_t cap = 1 << 16, n = 0;
    double *t = malloc(sizeof *t * cap);
    int32_t *ink = malloc(sizeof *ink * cap);
    uint64_t *sum = malloc(sizeof *sum * cap);
    printf("t ink changed\n");
    while (n < cap && fread(buf, 1, sz, stdin) == sz) {
        if (fscanf(tf, "%lf", &t[n]) != 1) break;
        ink[n] = mt_box_ink(buf, W, H, b, lum, &sum[n]);
        printf("%.6f %d %d\n", t[n], ink[n], n > 0 && sum[n] != sum[n - 1]);
        n++;
    }
    fclose(tf);
    MtPace p;
    mt_pace(t, ink, sum, n, &p);
    printf("pace frames %d fps %.1f maxgap_ms %.0f maxstep %.3f rough %.2f span_ms %.0f\n",
           p.frames, p.fps, p.maxgap * 1000, p.maxstep, p.rough, (p.t1 - p.t0) * 1000);
    free(buf); free(t); free(ink); free(sum);
    return n > 0 ? 0 : 1;
}

static const char *ANAME[] = {"red", "green", "mid", "none"};

static int score_main(int argc, char **argv) {
    MtScoreOpts o;
    mt_default_opts(&o);
    const char *name = "take", *side = NULL;
    const char *files[512];
    int32_t nf = 0;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--span") && i + 1 < argc) o.span = atof(argv[++i]);
        else if (!strcmp(argv[i], "--snap") && i + 1 < argc) o.snap = atof(argv[++i]);
        else if (!strcmp(argv[i], "--whole")) o.whole = 1;
        else if (!strcmp(argv[i], "--name") && i + 1 < argc) name = argv[++i];
        else if (!strcmp(argv[i], "--side") && i + 1 < argc) side = argv[++i];
        else if (!strcmp(argv[i], "--anchor") && i + 1 < argc) {
            char nm[32], an[16];
            if (sscanf(argv[++i], "%31[^=]=%15s", nm, an) != 2) return usage();
            int32_t a = -1;
            for (int32_t k = 0; k < 4; k++) if (!strcmp(an, ANAME[k])) a = k;
            if (a < 0) return usage();
            /* a base ink name covers its quadrants too */
            int32_t hit = 0;
            char b[32];
            for (int32_t m = 0; m < MT_MARKS; m++) {
                mt_mark_name(m, b, sizeof b);
                size_t L = strlen(nm);
                if (!strcmp(b, nm) || (!strncmp(b, nm, L) && b[L] == '_')) {
                    o.anchor[m] = a;
                    o.scaled[m] = a == MT_ANCHOR_MID && m != mt_mark(0, MT_Q_ONE);
                    hit = 1;
                }
            }
            if (!hit) { fprintf(stderr, "motion: no mark %s\n", nm); return 2; }
        } else if (argv[i][0] == '-') return usage();
        else if (nf < 512) files[nf++] = argv[i];
    }
    if (!nf) return usage();
    static double sides[4096];
    if (side) {
        FILE *f = fopen(side, "r");
        if (!f) { perror(side); return 1; }
        int32_t h, h0 = -1, h1 = -1;
        double sd;
        while (fscanf(f, "%d %lf%*[^\n]", &h, &sd) == 2) {
            if (h0 < 0) h0 = h;
            if (h - h0 < 4096) { sides[h - h0] = sd; h1 = h; }
        }
        fclose(f);
        o.side = sides; o.side_h0 = h0; o.side_h1 = h1;
    }
    static MtRow rows[20000];
    MtScore acc[MT_MARKS], s[MT_MARKS];
    int32_t takes[MT_MARKS] = {0};
    double jf[MT_MARKS] = {0};
    memset(acc, 0, sizeof acc);
    int32_t used = 0;
    for (int32_t f = 0; f < nf; f++) {
        int32_t n = mt_read_table(files[f], rows, 20000);
        if (n < 0) { perror(files[f]); return 1; }
        if (!mt_score(rows, n, &o, s)) { fprintf(stderr, "motion: %s: nothing moved\n", files[f]); continue; }
        used++;
        for (int32_t m = 0; m < MT_MARKS; m++) {
            if (!s[m].seen) continue;
            takes[m]++;
            acc[m].snaps += s[m].snaps; acc[m].late += s[m].late; acc[m].miss += s[m].miss;
            if (s[m].maxsnap > acc[m].maxsnap) acc[m].maxsnap = s[m].maxsnap;
            if (s[m].maxstep > acc[m].maxstep) acc[m].maxstep = s[m].maxstep;
            acc[m].rough += s[m].rough; acc[m].jerk += s[m].jerk; acc[m].floor += s[m].floor;
            acc[m].stray += s[m].stray; acc[m].travel += s[m].travel;
            jf[m] += s[m].floor > 1 ? s[m].jerk / s[m].floor : (s[m].jerk < 1 ? 0 : 99);
        }
    }
    printf("# motion score v1: %s, %d takes, snap > %.1fpt, span %.2fs%s\n", name, used, o.snap,
           o.span, o.whole ? " (whole take)" : "");
    printf("%-10s %-6s %5s %8s %8s %6s %5s %5s %9s %9s %8s %6s %9s %7s\n", "mark", "anchor", "takes",
           "maxstep", "maxsnap", "snaps", "late", "miss", "rough", "jerk", "floor", "jf", "stray", "travel");
    char b[32];
    for (int32_t m = 0; m < MT_MARKS; m++) {
        if (!takes[m]) continue;
        double k = takes[m];
        printf("%-10s %-6s %5d %8.1f %8.1f %6.1f %5.1f %5.1f %9.0f %9.0f %8.0f %6.2f %9.0f %7.1f\n",
               mt_mark_name(m, b, sizeof b), ANAME[o.anchor[m]], takes[m], acc[m].maxstep, acc[m].maxsnap,
               acc[m].snaps / k, acc[m].late / k, acc[m].miss / k, acc[m].rough / k,
               acc[m].jerk / k, acc[m].floor / k, jf[m] / k, acc[m].stray / k, acc[m].travel / k);
    }
    return used ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc < 2) return usage();
    if (!strcmp(argv[1], "find")) return find_main(argc - 2, argv + 2);
    if (!strcmp(argv[1], "score")) return score_main(argc - 2, argv + 2);
    if (!strcmp(argv[1], "pace")) return pace_main(argc - 2, argv + 2);
    return usage();
}
