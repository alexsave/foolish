// play.c: the generic replica player. One program for every replica directory: run it from inside the directory,
//   ./play full OUTDIR        (SOLO=drums renders one part alone, unnormalised, scaled by MGAIN dB if set)
// It reads score.txt (TEMPO, SECTION and "EVENTS PART: beat,pitch,len,vel ..." lines, see engine/scoretxt.h) and parts.txt from the
// current directory. parts.txt is plain text, one line per part, lines starting with # ignored:
//   PART <name> sampler <manifest> gain_db <x> release_s <x> group <drums|bass|other> [exact 0|1] [roots spaced]
// "sampler" may be repeated: the k-th manifest answers events whose pitch is 128k + root (extract.py --split-roots writes them).
// "roots spaced" (extract.py --roots spaced): one manifest whose roots are 100 i + 50 and an event's pitch is root + shift in semitones;
// the pitch >= 127.5 routing above does not apply to such a part.
// "exact" is optional (default 1): 1 sets the sampler's sp[15] so no random detune or gain variation touches a note; 0 is for tests.
// Optional on a sampler part: attack_ms <x> (sampler sp[2], a linear attack fade) and release_lin_ms <x> (sampler sp[16]); both default 0.
// A synth part: PART <name> synth <supersaw|saw|808|sub|kick|clap|hat|snare> sp <comma separated numbers> gain_db <x> group <g> [eq <type>:<f>:<q>:<gain> ...]
//   [duck <part> <db> <att_s> <rel_s>] [send <bus> <level>]   (eq type 1 lp 2 hp 3 peak 4 lshelf 5 hshelf; duck: this part ducks under the
//   named part's note onsets with the engine's apply_duck; send: only bus 0, the reverb of the BUS line, exists)
// One optional line: BUS reverb <decay_s> <gain_db> (bus 0, an FDN reverb: rt60 decay, high rt60 0.6 of it, 5 ms predelay, size 0.6, hp 300, lp 9000, er 0.15).
// Every part is a sampler track with velocity exponent 1, no loop, no pan, no sends, no EQ, no compression, no humanise, no swing.
// The master has no EQ, compression, tape, room noise, width processing or top filter; loudness normalisation to -16 LUFS and the
// limiter of the engine stay. Track names are the part names lower-cased, so SOLO=<part> works. An unknown keyword, an unknown part
// in the score or a malformed line stops the run.
// Build: clang -O2 -w -o play play.c   Run (from a replica directory): SAMPLES=<samples dir> ENGINE=<engine dir> ./play full out
#include "../engine/synth.h"
#include "../engine/render.h"
#include "../engine/scoretxt.h"
#include <strings.h>

#define MAXPART 16
#define MAXSUB 16
typedef struct { char name[48]; int ntr, tr[MAXSUB]; char group[16]; int spaced; char duck[48]; double duck_db, duck_att, duck_rel; } Part;
static int g_bus_on; static double g_bus_decay, g_bus_gain;
static int g_send_used;
static Part g_part[MAXPART]; static int g_npart;

static void die(const char *file, int line, const char *msg, const char *what) { fprintf(stderr, "play: %s:%d: %s '%s'\n", file, line, msg, what); exit(1); }

void city_setup(void) {
    FILE *f = fopen("parts.txt", "r"); if (!f) { perror("parts.txt"); exit(1); }
    char line[2048]; int ln = 0;
    while (fgets(line, sizeof line, f)) {
        ln++; char *s = line; while (*s == ' ' || *s == '\t') s++;
        if (!*s || *s == '#' || *s == '\n' || *s == '\r') continue;
        char *tok[64]; int nt = 0; for (char *p = strtok(s, " \t\r\n"); p && nt < 64; p = strtok(0, " \t\r\n")) tok[nt++] = p;
        if (!strcmp(tok[0], "BUS")) {
            if (nt != 4 || strcmp(tok[1], "reverb") || g_bus_on) die("parts.txt", ln, "the only BUS line is: BUS reverb <decay_s> <gain_db>, got", tok[nt > 1 ? 1 : 0]);
            g_bus_on = 1; g_bus_decay = atof(tok[2]); g_bus_gain = atof(tok[3]); continue; }
        if (nt < 2 || strcmp(tok[0], "PART")) die("parts.txt", ln, "a line must start with PART <name>, got", tok[0]);
        if (g_npart >= MAXPART) die("parts.txt", ln, "too many parts at", tok[1]);
        Part *pt = &g_part[g_npart]; memset(pt, 0, sizeof *pt);
        snprintf(pt->name, sizeof pt->name, "%s", tok[1]); for (char *c = pt->name; *c; c++) *c = tolower((unsigned char)*c);
        char *man[MAXSUB]; int nm = 0; double gain = 0, rel = 0.012; int have_gain = 0, have_rel = 0, exact = 1, spaced = 0;
        SynthFn sfn = 0; double sp[24] = {0}; int have_sp = 0; EqBand eqb[8]; int neq = 0; double send0 = 0; double att_ms = 0, rlin_ms = 0;
        for (int i = 2; i < nt; ) {
            const char *k = tok[i];
            int ar = !strcmp(k, "duck") ? 4 : !strcmp(k, "send") ? 2 : 1;
            if (i + ar >= nt) die("parts.txt", ln, "keyword without its value(s)", tok[i]);
            if (!strcmp(k, "sampler")) { if (nm >= MAXSUB) die("parts.txt", ln, "too many samplers at", tok[i + 1]); man[nm++] = tok[i + 1]; }
            else if (!strcmp(k, "gain_db")) { gain = atof(tok[i + 1]); have_gain = 1; }
            else if (!strcmp(k, "release_s")) { rel = atof(tok[i + 1]); have_rel = 1; }
            else if (!strcmp(k, "group")) { if (strcmp(tok[i + 1], "drums") && strcmp(tok[i + 1], "bass") && strcmp(tok[i + 1], "other")) die("parts.txt", ln, "group must be drums, bass or other, got", tok[i + 1]); snprintf(pt->group, sizeof pt->group, "%s", tok[i + 1]); }
            else if (!strcmp(k, "roots")) { if (strcmp(tok[i + 1], "spaced")) die("parts.txt", ln, "roots must be 'spaced', got", tok[i + 1]); spaced = 1; }
            else if (!strcmp(k, "exact")) exact = atoi(tok[i + 1]) != 0;
            else if (!strcmp(k, "attack_ms")) att_ms = atof(tok[i + 1]);
            else if (!strcmp(k, "release_lin_ms")) rlin_ms = atof(tok[i + 1]);
            else if (!strcmp(k, "synth")) { const char *v = tok[i + 1];
                sfn = !strcmp(v, "supersaw") ? syn_supersaw : !strcmp(v, "saw") ? syn_saw : !strcmp(v, "808") ? syn_808 : !strcmp(v, "sub") ? syn_sub
                    : !strcmp(v, "kick") ? syn_kick : !strcmp(v, "clap") ? syn_clap : !strcmp(v, "hat") ? syn_hat : !strcmp(v, "snare") ? syn_snare : 0;
                if (!sfn) die("parts.txt", ln, "synth must be supersaw, saw, 808, sub, kick, clap, hat or snare, got", v); }
            else if (!strcmp(k, "sp")) { char *c = tok[i + 1]; int n = 0;
                while (*c) { char *e; double v = strtod(c, &e); if (e == c) die("parts.txt", ln, "bad number in sp at", c); if (n >= 24) die("parts.txt", ln, "sp has more than 24 numbers:", tok[1]); sp[n++] = v; c = e; if (*c == ',') c++; else if (*c) die("parts.txt", ln, "bad separator in sp at", c); }
                have_sp = 1; }
            else if (!strcmp(k, "eq")) { if (neq >= 8) die("parts.txt", ln, "more than 8 eq bands:", tok[1]); int ty; double f, q, g; char junk;
                if (sscanf(tok[i + 1], "%d:%lf:%lf:%lf%c", &ty, &f, &q, &g, &junk) != 4 || ty < 1 || ty > 5) die("parts.txt", ln, "eq must be type:f:q:gain with type 1..5, got", tok[i + 1]);
                eqb[neq++] = (EqBand){ty, f, q, g}; }
            else if (!strcmp(k, "duck")) { snprintf(pt->duck, sizeof pt->duck, "%s", tok[i + 1]); for (char *c = pt->duck; *c; c++) *c = tolower((unsigned char)*c);
                pt->duck_db = atof(tok[i + 2]); pt->duck_att = atof(tok[i + 3]); pt->duck_rel = atof(tok[i + 4]); }
            else if (!strcmp(k, "send")) { if (atoi(tok[i + 1]) != 0) die("parts.txt", ln, "only send bus 0 exists, got", tok[i + 1]); send0 = atof(tok[i + 2]); g_send_used = 1; }
            else die("parts.txt", ln, "unknown keyword", tok[i]);
            i += ar + 1;
        }
        if (sfn) {
            if (nm || have_rel || spaced || att_ms || rlin_ms || !have_sp || !have_gain || !pt->group[0]) die("parts.txt", ln, "a synth part needs synth, sp, gain_db, group and no sampler keywords:", tok[1]);
            int t = syn_track(pt->name, sfn); Track *tr = &g_tr[t]; memcpy(tr->sp, sp, sizeof sp);
            pt->tr[pt->ntr++] = t;
        } else {
            if (!nm || !have_gain || !have_rel || !pt->group[0]) die("parts.txt", ln, "the part lacks sampler, gain_db, release_s or group:", tok[1]);
            if (have_sp) die("parts.txt", ln, "sp belongs to a synth part:", tok[1]);
            if (spaced && nm != 1) die("parts.txt", ln, "a part with roots spaced has exactly one sampler:", tok[1]);
            pt->spaced = spaced;
            for (int k2 = 0; k2 < nm; k2++) {
                int t = syn_track(pt->name, syn_sampler); Track *tr = &g_tr[t]; tr->user = ss_load(man[k2]);
                tr->sp[0] = rel; tr->sp[1] = 1; tr->sp[2] = att_ms; tr->sp[3] = 127; tr->sp[10] = 0; tr->sp[15] = exact; tr->sp[16] = rlin_ms;
                pt->tr[pt->ntr++] = t;
            }
        }
        for (int k2 = 0; k2 < pt->ntr; k2++) {
            Track *tr = &g_tr[pt->tr[k2]];
            tr->gain_db = gain; tr->pan = 0; tr->width = 1; tr->group = 0; tr->neq = 0; tr->cthr = 0; tr->drive = 0; tr->chorus_mix = 0; tr->haas_ms = 0; tr->crush_sr = 0;
            tr->human_ms = 0; tr->human_vel = 0; tr->swing = 0; tr->swing8 = 0; tr->delay_ms = 0; tr->duck_src = -1; for (int b = 0; b < 4; b++) tr->send[b] = 0;
            for (int e = 0; e < neq; e++) tr->eq[tr->neq++] = eqb[e];
            tr->send[0] = send0;
        }
        g_npart++;
    }
    fclose(f);
    if (!g_npart) { fprintf(stderr, "play: parts.txt lists no part\n"); exit(1); }
    // master: nothing but the engine's loudness normalisation and limiter
    g_master.neq = 0; g_master.cthr = 0; g_master.tape = 0; g_master.room_db = 0; g_master.top_hz = 0; g_master.width_lo_hz = 0; g_master.side = 0;
    for (int b = 0; b < 4; b++) g_bus[b].type = 0;
    if (g_send_used && !g_bus_on) { fprintf(stderr, "play: a part sends to bus 0 but parts.txt has no BUS line\n"); exit(1); }
    if (g_bus_on) g_bus[0] = (Bus){.type = 1, .rev = {g_bus_decay, g_bus_decay * 0.6, 5, 0.6, 300, 9000, 0.15, 1.0, 0, 0}, .gain_db = g_bus_gain, .duck_src = -1};
    for (int p = 0; p < g_npart; p++) if (g_part[p].duck[0]) {
        int src = -1; for (int q = 0; q < g_npart; q++) if (!strcmp(g_part[q].name, g_part[p].duck)) src = g_part[q].tr[0];
        if (src < 0) { fprintf(stderr, "play: part %s ducks under '%s', which parts.txt does not list\n", g_part[p].name, g_part[p].duck); exit(1); }
        for (int k = 0; k < g_part[p].ntr; k++) { Track *tr = &g_tr[g_part[p].tr[k]]; tr->duck_src = src; tr->duck_db = g_part[p].duck_db; tr->duck_att = g_part[p].duck_att; tr->duck_rel = g_part[p].duck_rel; } }
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
        for (int p = 0; p < g_npart; p++) if (n->tr == g_part[p].tr[0]) { if (g_part[p].spaced) break; int k = (int)((n->pitch + 0.5) / 128);
            if (k >= g_part[p].ntr) { fprintf(stderr, "play: pitch %.1f of part %s needs manifest %d, the part has %d\n", n->pitch, g_part[p].name, k + 1, g_part[p].ntr); exit(1); }
            n->tr = g_part[p].tr[k]; n->pitch -= 128 * k; break; } }
    return score_total_bars() * 4.0;
}
void city_full(void) { city_loop(); }
