// play.c: the generic replica player. One program for every replica directory: run it from inside the directory,
//   ./play full OUTDIR        (SOLO=drums renders one part alone, unnormalised, scaled by MGAIN dB if set)
// It reads score.txt (TEMPO, SECTION and "EVENTS PART: beat,pitch,len,vel ..." lines, see engine/scoretxt.h) and parts.txt from the
// current directory. parts.txt is plain text, one line per part, lines starting with # ignored:
//   PART <name> sampler <manifest> gain_db <x> release_s <x> group <drums|bass|other> [exact 0|1]
// "sampler" may be repeated: the k-th manifest answers events whose pitch is 128k + root (extract.py --split-roots writes them).
// "exact" is optional (default 1): 1 sets the sampler's sp[15] so no random detune or gain variation touches a note; 0 is for tests.
// Every part is a sampler track with velocity exponent 1, no loop, no pan, no sends, no EQ, no compression, no humanise, no swing.
// The master has no EQ, compression, tape, room noise, width processing or top filter; loudness normalisation to -16 LUFS and the
// limiter of the engine stay. Track names are the part names lower-cased, so SOLO=<part> works. An unknown keyword, an unknown part
// in the score or a malformed line stops the run.
// Build: clang -O2 -w -o play play.c   Run (from a replica directory): SAMPLES=<samples dir> ENGINE=<engine dir> ./play full out
#include "../engine/render.h"
#include "../engine/scoretxt.h"
#include <strings.h>

#define MAXPART 16
#define MAXSUB 16
typedef struct { char name[48]; int ntr, tr[MAXSUB]; char group[16]; } Part;
static Part g_part[MAXPART]; static int g_npart;

static void die(const char *file, int line, const char *msg, const char *what) { fprintf(stderr, "play: %s:%d: %s '%s'\n", file, line, msg, what); exit(1); }

void city_setup(void) {
    FILE *f = fopen("parts.txt", "r"); if (!f) { perror("parts.txt"); exit(1); }
    char line[2048]; int ln = 0;
    while (fgets(line, sizeof line, f)) {
        ln++; char *s = line; while (*s == ' ' || *s == '\t') s++;
        if (!*s || *s == '#' || *s == '\n' || *s == '\r') continue;
        char *tok[64]; int nt = 0; for (char *p = strtok(s, " \t\r\n"); p && nt < 64; p = strtok(0, " \t\r\n")) tok[nt++] = p;
        if (nt < 2 || strcmp(tok[0], "PART")) die("parts.txt", ln, "a line must start with PART <name>, got", tok[0]);
        if (g_npart >= MAXPART) die("parts.txt", ln, "too many parts at", tok[1]);
        Part *pt = &g_part[g_npart]; memset(pt, 0, sizeof *pt);
        snprintf(pt->name, sizeof pt->name, "%s", tok[1]); for (char *c = pt->name; *c; c++) *c = tolower((unsigned char)*c);
        char *man[MAXSUB]; int nm = 0; double gain = 0, rel = 0.012; int have_gain = 0, have_rel = 0, exact = 1;
        for (int i = 2; i < nt; i += 2) {
            if (i + 1 >= nt) die("parts.txt", ln, "keyword without a value", tok[i]);
            if (!strcmp(tok[i], "sampler")) { if (nm >= MAXSUB) die("parts.txt", ln, "too many samplers at", tok[i + 1]); man[nm++] = tok[i + 1]; }
            else if (!strcmp(tok[i], "gain_db")) { gain = atof(tok[i + 1]); have_gain = 1; }
            else if (!strcmp(tok[i], "release_s")) { rel = atof(tok[i + 1]); have_rel = 1; }
            else if (!strcmp(tok[i], "group")) { if (strcmp(tok[i + 1], "drums") && strcmp(tok[i + 1], "bass") && strcmp(tok[i + 1], "other")) die("parts.txt", ln, "group must be drums, bass or other, got", tok[i + 1]); snprintf(pt->group, sizeof pt->group, "%s", tok[i + 1]); }
            else if (!strcmp(tok[i], "exact")) exact = atoi(tok[i + 1]) != 0;
            else die("parts.txt", ln, "unknown keyword", tok[i]);
        }
        if (!nm || !have_gain || !have_rel || !pt->group[0]) die("parts.txt", ln, "the part lacks sampler, gain_db, release_s or group:", tok[1]);
        for (int k = 0; k < nm; k++) {
            int t = syn_track(pt->name, syn_sampler); Track *tr = &g_tr[t]; tr->user = ss_load(man[k]);
            tr->sp[0] = rel; tr->sp[1] = 1; tr->sp[3] = 127; tr->sp[10] = 0; tr->sp[15] = exact;
            tr->gain_db = gain; tr->pan = 0; tr->width = 1; tr->group = 0; tr->neq = 0; tr->cthr = 0; tr->drive = 0; tr->chorus_mix = 0; tr->haas_ms = 0; tr->crush_sr = 0;
            tr->human_ms = 0; tr->human_vel = 0; tr->swing = 0; tr->swing8 = 0; tr->delay_ms = 0; tr->duck_src = -1; for (int b = 0; b < 4; b++) tr->send[b] = 0;
            pt->tr[pt->ntr++] = t;
        }
        g_npart++;
    }
    fclose(f);
    if (!g_npart) { fprintf(stderr, "play: parts.txt lists no part\n"); exit(1); }
    // master: nothing but the engine's loudness normalisation and limiter
    g_master.neq = 0; g_master.cthr = 0; g_master.tape = 0; g_master.room_db = 0; g_master.top_hz = 0; g_master.width_lo_hz = 0; g_master.side = 0;
    for (int b = 0; b < 4; b++) g_bus[b].type = 0;
    fprintf(stderr, "play: engine sample rate %d Hz\n", SR);
}
static int map(const char *part, double *tp, double *vs, double *dp) {
    *tp = 0; *vs = 1; *dp = 60;
    for (int i = 0; i < g_npart; i++) if (!strcasecmp(part, g_part[i].name)) return g_part[i].tr[0];
    fprintf(stderr, "play: score names the part '%s', parts.txt does not\n", part); exit(1);
}
double city_loop(void) {
    int n0 = g_nnote; g_nsec = 0; score_txt("score.txt", map, 0);
    // pitch 128k + root belongs to the part's k-th manifest
    for (int i = n0; i < g_nnote; i++) { Note *n = &g_note[i]; if (n->pitch < 127.5) continue;
        for (int p = 0; p < g_npart; p++) if (n->tr == g_part[p].tr[0]) { int k = (int)((n->pitch + 0.5) / 128);
            if (k >= g_part[p].ntr) { fprintf(stderr, "play: pitch %.1f of part %s needs manifest %d, the part has %d\n", n->pitch, g_part[p].name, k + 1, g_part[p].ntr); exit(1); }
            n->tr = g_part[p].tr[k]; n->pitch -= 128 * k; break; } }
    return score_total_bars() * 4.0;
}
void city_full(void) { city_loop(); }
