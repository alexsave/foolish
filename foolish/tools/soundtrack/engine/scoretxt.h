// scoretxt.h: reads a through-composed score written as text (TEMPO, SECTION ... BARS n KEY k, and
// "PART: bar | bar | ..." lines; % repeats the previous bar, %N repeats bar N of the part in the section;
// drums use x / x@0.6 hits; chop notes may carry a vowel letter after the octave, e.g. E5a:0.25).
// The city maps part names to tracks with a callback; sections are kept for automation by name.
#pragma once
#include "score.h"

typedef struct { char name[48], key[16]; int bar0, bars; int ndyn; double dyn_pos[24], dyn_val[24]; } Section;
static Section g_sec[128]; static int g_nsec; static double g_score_bpm;
// the city answers: track index (or -1 to ignore), transposition, velocity scale, and whether it's a drum part (hit pitch)
typedef int (*PartMap)(const char *part, double *transpose, double *vscale, double *drum_pitch);
static int sec_find(const char *name) { for (int i = 0; i < g_nsec; i++) if (!strcmp(g_sec[i].name, name)) return i; return -1; }

static int vowel_of(char c) { return c == 'a' ? 0 : c == 'e' ? 1 : c == 'i' ? 2 : c == 'o' ? 3 : c == 'u' ? 4 : -1; }
// parse one bar's tokens at beat b; returns notes written
static int bar_tokens(int tr, double b, const char *src, double tp, double vs, double drum_pitch, double gate) {
    const char *s = src; double beat = b; int cnt = 0;
    // a bar holding one token with no length ("F3+Ab3+C4") is a whole-bar note
    int ntok = 0, inw = 0; for (const char *q = src; *q; q++) { if (*q != ' ' && *q != '\t') { if (!inw) ntok++; inw = 1; } else inw = 0; }
    double deflen = (ntok == 1 && !strchr(src, ':')) ? 4 : 1;
    while (*s) {
        while (*s == ' ' || *s == '\t') s++;
        if (!*s) break;
        double ps[8]; int np = 0, rest = 0, hit = 0, vow = -1; double vel = 0.8, len = deflen;
        if (*s == 'r' || *s == 'R') { rest = 1; s++; }
        else if (*s == 'x' || *s == 'X') { hit = 1; s++; if (isdigit((unsigned char)*s)) hit = (int)strtol(s, (char **)&s, 10); }   // x8 / x16 / x32: a roll at that note rate
        else for (;;) { ps[np++] = pname(&s); if (vowel_of(*s) >= 0 && (s[1] == ':' || s[1] == '+')) { vow = vowel_of(*s); s++; }
            else if ((*s == 'b' || *s == 'c' || *s == 't') && (s[1] == ':' || s[1] == '+')) { vow = *s == 'b' ? 0 : *s == 'c' ? 1 : 2; s++; }   // scratch: baby / chirp / transformer
            if (*s == '+') { s++; continue; } break; }
        for (int k = 0; k < 2; k++) {   // ":len" and "@vel" in either order
            if (*s == ':') { s++; len = strtod(s, (char **)&s); }
            else if (*s == '@') { s++; vel = strtod(s, (char **)&s); } }
        while (*s && *s != ' ' && *s != '\t') s++;
        if (!rest && tr >= 0) {
            if (hit == 1) { note(tr, beat, drum_pitch < 0 ? len : fmin(len, 0.5), fabs(drum_pitch), vel * vs); cnt++; }   // a negative drum pitch keeps the token length (risers)
            else if (hit > 1) { double step = 4.0 / hit; int k = (int)lround(len / step); for (int i = 0; i < k; i++) { note(tr, beat + i * step, step * 0.5, drum_pitch, vel * vs * (0.55 + 0.45 * i / fmax(1, k - 1))); cnt++; } }
            else for (int i = 0; i < np; i++) { Note *n = note(tr, beat, len * gate, ps[i] + tp, vel * vs); if (vow >= 0) { n->p[0] = vow; n->p[1] = -1; } cnt++; }
        }
        beat += len;
    }
    if (fabs(beat - b - 4) > 1e-6 && tr >= 0) fprintf(stderr, "warning: bar at beat %.2f sums to %.3f\n", b, beat - b);
    return cnt;
}

// DYN lines: marks (ppp..fff, sfz/sffz) become anchors (position in bars from the section start, level). A mark followed by
// "bar N" / "bars N-M" holds over those bars; a range written before the mark in the same clause ("bar 8 subito p") applies
// to it; "to X" / "-> X" after a preceding range ramps over that range ("cresc bars 5-7 to f"); "from bar N" is a step there;
// "over bars N-M" ramps; unranged "to"/"->" marks spread evenly to the section's end; the first unranged mark sits at 0.
static const char *g_dmk[] = {"ppp", "pp", "p", "mp", "mf", "f", "ff", "fff", "sfz", "sffz"};
static const double g_dmv[] = {0.25, 0.35, 0.45, 0.56, 0.68, 0.8, 0.92, 1.0, 1.0, 1.0};
static void dyn_add(Section *x, double pos, double v) { if (x->ndyn < 24) { x->dyn_pos[x->ndyn] = pos; x->dyn_val[x->ndyn++] = v; } }
static void dyn_parse(const char *txt, Section *x) {
    char w[64][24]; int nw = 0, cl[64]; int clause = 0; const char *q = txt;
    while (*q && nw < 64) {   // words, numbers, "-" between numbers, "->" ; clause breaks at , ; (
        if (*q == ',' || *q == ';' || *q == '(' || *q == '.') { clause++; q++; continue; }
        if (q[0] == '-' && q[1] == '>') { strcpy(w[nw], "to"); cl[nw++] = clause; q += 2; continue; }
        if (*q == '-') { strcpy(w[nw], "-"); cl[nw++] = clause; q++; continue; }
        if (isalnum((unsigned char)*q)) { int L = 0; int dig = isdigit((unsigned char)*q); while (*q && (dig ? isdigit((unsigned char)*q) : isalpha((unsigned char)*q)) && L < 23) w[nw][L++] = *q++; w[nw][L] = 0; cl[nw++] = clause; continue; }
        q++;
    }
    int nunr = 0, unr[24]; double prev = -1;
    for (int i = 0; i < nw; i++) {
        int mk = -1; for (int k = 0; k < 10; k++) if (!strcmp(w[i], g_dmk[k])) mk = k;
        if (mk < 0) continue; double v = g_dmv[mk];
        if (i > 0 && cl[i - 1] == cl[i] && isupper((unsigned char)w[i - 1][0]) && isupper((unsigned char)w[i - 1][1])) continue;   // "LEAD mp": a part's own level, not the section's
        int is_to = i > 0 && !strcmp(w[i - 1], "to") && cl[i - 1] == cl[i];
        int a = -1, b = -1, kind = 0;   // kind: 0 hold, 1 from (step), 2 over (ramp)
        for (int j = i + 1; j < nw && cl[j] == cl[i]; j++) {   // a following range
            int m2 = 0; for (int k = 0; k < 10; k++) if (!strcmp(w[j], g_dmk[k])) m2 = 1; if (m2) break;
            if ((!strcmp(w[j], "bar") || !strcmp(w[j], "bars")) && j + 1 < nw && isdigit((unsigned char)w[j + 1][0])) {
                a = b = atoi(w[j + 1]); if (j + 3 < nw && !strcmp(w[j + 2], "-") && isdigit((unsigned char)w[j + 3][0])) b = atoi(w[j + 3]);
                for (int k = i + 1; k < j; k++) { if (!strcmp(w[k], "from")) kind = 1; if (!strcmp(w[k], "over")) kind = 2; }
                break; }
        }
        int pre = 0;
        if (a < 0) for (int j = i - 1; j >= 0 && cl[j] == cl[i]; j--)   // a range earlier in the clause
            if ((!strcmp(w[j], "bar") || !strcmp(w[j], "bars")) && j + 1 < nw && isdigit((unsigned char)w[j + 1][0])) {
                a = b = atoi(w[j + 1]); if (j + 3 < nw && !strcmp(w[j + 2], "-") && isdigit((unsigned char)w[j + 3][0])) b = atoi(w[j + 3]); pre = 1; break; }
        if (a > 0) {
            if (kind == 1) { if (prev >= 0) dyn_add(x, a - 1, prev); dyn_add(x, a - 1, v); }
            else if (kind == 2 || (is_to && pre)) { if (prev >= 0) dyn_add(x, a - 1, prev); dyn_add(x, b, v); }
            else { dyn_add(x, a - 1, v); dyn_add(x, b, v); }
        } else if (x->ndyn == 0 && nunr == 0 && !is_to) dyn_add(x, 0, v);
        else if (nunr < 24) { unr[nunr++] = x->ndyn; dyn_add(x, -1, v); }
        prev = v;
    }
    // unranged trailing marks: spread evenly from the anchor before them to the section's end
    for (int u = 0; u < nunr; ) {
        int u1 = u; while (u1 + 1 < nunr && unr[u1 + 1] == unr[u1] + 1) u1++;
        double p0 = 0; for (int k = unr[u] - 1; k >= 0; k--) if (x->dyn_pos[k] >= 0) { p0 = x->dyn_pos[k]; break; }
        double p1 = x->bars; for (int k = unr[u1] + 1; k < x->ndyn; k++) if (x->dyn_pos[k] >= 0) { p1 = x->dyn_pos[k]; break; }
        int cnt = u1 - u + 1; for (int k = 0; k < cnt; k++) x->dyn_pos[unr[u + k]] = p0 + (p1 - p0) * (k + 1) / cnt;
        u = u1 + 1;
    }
    // stable sort by position
    for (int i = 1; i < x->ndyn; i++) for (int j = i; j > 0 && x->dyn_pos[j - 1] > x->dyn_pos[j]; j--) {
        double t = x->dyn_pos[j]; x->dyn_pos[j] = x->dyn_pos[j - 1]; x->dyn_pos[j - 1] = t; t = x->dyn_val[j]; x->dyn_val[j] = x->dyn_val[j - 1]; x->dyn_val[j - 1] = t; }
}
static double dyn_at(const Section *x, double pos) {
    if (pos < x->dyn_pos[0]) return x->dyn_val[0];
    int j = 0; for (int k = 0; k < x->ndyn; k++) if (x->dyn_pos[k] <= pos + 1e-9) j = k;
    if (j + 1 >= x->ndyn || x->dyn_pos[j + 1] <= x->dyn_pos[j]) return x->dyn_val[j];
    double f = (pos - x->dyn_pos[j]) / (x->dyn_pos[j + 1] - x->dyn_pos[j]); return x->dyn_val[j] + (x->dyn_val[j + 1] - x->dyn_val[j]) * f;
}
// the city may pre-process each part's bar (gate, articulation) through this hook; default gate 0.95
static double g_txt_gate = 0.95;
static double g_dyn_depth = 1.0;   // how strongly DYN marks scale velocities (0 = ignore)
static void score_txt(const char *path, PartMap map, double bar_offset) {
    int n_start = g_nnote;
    FILE *f = fopen(path, "r"); if (!f) { perror(path); exit(1); }
    static char line[65536]; int bar0 = (int)bar_offset, cur = -1, pend_next = 0;
    while (fgets(line, sizeof line, f)) {
        char *e = line + strlen(line); while (e > line && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ')) *--e = 0;
        char *s = line; while (*s == ' ' || *s == '\t' || *s == '`') s++;
        if (!*s || *s == '#') continue;
        if (!strncmp(s, "TEMPO", 5)) {   // at the start of the next section (or the current one if it has no parts yet); optional RAMP <beats>
            double bpm = atof(s + 5), ramp = 0; char *r = strstr(s, "RAMP"); if (r) ramp = atof(r + 4);
            double at = cur < 0 ? bar_offset * 4 : (g_sec[cur].bar0 + (pend_next ? g_sec[cur].bars : 0)) * 4.0;
            if (cur >= 0 && !pend_next) at = g_sec[cur].bar0 * 4.0;
            { char *a2 = strstr(s, " AT "); if (a2) at += atof(a2 + 4); }   // "AT -8": start the change 8 beats early (a ritardando or accelerando into the boundary)
            if (ramp > 0 && g_score_bpm > 0) { tempo(at, g_score_bpm); tempo(at + ramp, bpm); } else tempo(at, bpm);
            g_score_bpm = bpm; continue; }
        if (!strncmp(s, "SECTION", 7)) {
            Section *x = &g_sec[g_nsec]; memset(x, 0, sizeof *x); char *b = strstr(s, " BARS "), *k = strstr(s, " KEY ");
            if (!b) continue;
            int nl = (int)(b - (s + 8)); snprintf(x->name, sizeof x->name, "%.*s", nl > 47 ? 47 : nl, s + 8);
            x->bars = atoi(b + 6); if (k) sscanf(k + 5, "%15s", x->key);
            if (cur >= 0) bar0 = g_sec[cur].bar0 + g_sec[cur].bars;
            x->bar0 = bar0; cur = g_nsec++; pend_next = 0; continue;
        }
        if (cur < 0) continue;
        char *colon = strchr(s, ':'); if (!colon) continue;
        char part[48]; int pl = (int)(colon - s); if (pl <= 0 || pl > 40) continue; snprintf(part, sizeof part, "%.*s", pl, s);
        { int L = (int)strlen(part); while (L > 0 && part[L - 1] == ' ') part[--L] = 0; }   // keep suffixes like "VIOLINS SPIC"
        if (!strcmp(part, "DYN")) { dyn_parse(colon + 1, &g_sec[cur]); continue; }
        if (!strcmp(part, "FX") || !strcmp(part, "TRANSITION") || !strcmp(part, "NOTE")) continue;
        pend_next = 1;
        double tp = 0, vs = 1, dp = 60; int tr = map(part, &tp, &vs, &dp);
        if (tr < 0) { fprintf(stderr, "score: part %s ignored\n", part); continue; }
        // split bars on |
        char *bars[512]; int nb = 0; char *p = colon + 1;
        while (p && nb < 512) { char *bar = p; char *bar_end = strchr(p, '|'); if (bar_end) { *bar_end = 0; p = bar_end + 1; } else p = 0; bars[nb++] = bar; }
        char *resolved[512];
        for (int i = 0; i < nb; i++) {
            char *t = bars[i]; while (*t == ' ') t++;
            char *te = t + strlen(t); while (te > t && te[-1] == ' ') *--te = 0;
            if (t[0] == '%') { int ref = t[1] ? atoi(t + 1) - 1 : i - 1; resolved[i] = (ref >= 0 && ref < i) ? resolved[ref] : t; }
            else resolved[i] = t;
            if (resolved[i][0] == '%') continue;
            bar_tokens(tr, (g_sec[cur].bar0 + i) * 4.0, resolved[i], tp, vs, dp, g_txt_gate);
        }
        if (nb != g_sec[cur].bars) fprintf(stderr, "score: %s in %s has %d bars (section has %d)\n", part, g_sec[cur].name, nb, g_sec[cur].bars);
    }
    fclose(f);
    // apply section dynamics to the notes written by this call (velocity scaled around mf = 1.0)
    for (int i = n_start; i < g_nnote; i++) { Note *n = &g_note[i];
        for (int k = 0; k < g_nsec; k++) { Section *x = &g_sec[k]; if (!x->ndyn) continue; double b0 = x->bar0 * 4.0, b1 = (x->bar0 + x->bars) * 4.0;
            if (n->beat >= b0 - 1e-6 && n->beat < b1 - 1e-6) { double d = dyn_at(x, (n->beat - b0) / 4.0); n->vel = clampd(n->vel * d / 0.68 * g_dyn_depth + n->vel * (1 - g_dyn_depth), 0.03, 1); break; } } }
}
static int score_total_bars(void) { return g_nsec ? g_sec[g_nsec - 1].bar0 + g_sec[g_nsec - 1].bars : 0; }

// writes outdir/sections.txt (start seconds and names) and outdir/sections_csv.txt (start seconds, comma separated)
static void sections_out(const char *outdir) {
    char p[1024]; snprintf(p, sizeof p, "%s/sections.txt", outdir); FILE *f = fopen(p, "w");
    snprintf(p, sizeof p, "%s/sections_csv.txt", outdir); FILE *c = fopen(p, "w");
    for (int i = 0; i < g_nsec; i++) { double t = b2t(g_sec[i].bar0 * 4.0); fprintf(f, "%8.2f  bar %4d  %s (%s)\n", t, g_sec[i].bar0 + 1, g_sec[i].name, g_sec[i].key); if (i) fprintf(c, "%s%.2f", i > 1 ? "," : "", t); }
    fprintf(c, "\n"); fclose(f); fclose(c);
}

// low-pass automation inside a named section: bars are 1-based within the section, inclusive of the start, the sweep ends at the
// start of bar "to"+1; the filter opens fully (19 kHz) right after unless keep_after
static void sec_lp(int tr, const char *sec, int from, int to, double f0, double f1, int keep_after) {
    int s = sec_find(sec); if (s < 0) { fprintf(stderr, "sec_lp: no section %s\n", sec); return; }
    double b0 = (g_sec[s].bar0 + from - 1) * 4.0, b1 = (g_sec[s].bar0 + to) * 4.0;
    // the sweep starts by stepping to f0 (whatever held before stays until then), ramps to f1, then either holds f1 until the
    // track's next sweep or steps open; holding never drifts toward a later point (that drift broke a loop's seam)
    autolp_step(tr, b0, f0); autolp(tr, b1, f1); if (!keep_after) autolp_step(tr, b1 + 0.01, 19000);
}

// rubato for piano and orchestral cities: a ritardando of frac over the last `beats` beats of every section (a breath at each
// cadence) and of pfrac at the end of every `pbars`-bar phrase inside it; the tempo returns at the next downbeat. Skipped
// where the score already changes tempo near that point (its own RAMP / AT lines own those).
static double bpm_at_pts(const TempoPt *p, int n, double beat) {
    if (beat <= p[0].beat) return p[0].bpm;
    for (int i = 0; i + 1 < n; i++) if (beat < p[i + 1].beat) { double x = (beat - p[i].beat) / (p[i + 1].beat - p[i].beat + 1e-12); return p[i].bpm + (p[i + 1].bpm - p[i].bpm) * x; }
    return p[n - 1].bpm;
}
static void rit_at(const TempoPt *o, int n, double E, double beats, double f) {
    double B = E - beats; if (f <= 0 || B < 0) return;
    for (int i = 0; i < n; i++) if (o[i].beat > B - 4 && o[i].beat < E + 4) return;   // the score's own tempo change owns this spot
    double v = bpm_at_pts(o, n, B), v2 = bpm_at_pts(o, n, E);
    tempo(B, v); tempo(E - 0.02, v * (1 - f)); tempo(E, v2);
}
static void sec_rit(double frac, double beats, int pbars, double pfrac) {
    if (g_k || !g_ntempo) return;
    int n = g_ntempo; TempoPt *o = malloc(n * sizeof *o); memcpy(o, g_tempo, n * sizeof *o); qsort(o, n, sizeof *o, tcmp);
    for (int s = 0; s < g_nsec; s++) {
        int nb = g_sec[s].bars; double b0 = g_sec[s].bar0 * 4.0;
        if (pbars > 0) for (int bar = pbars; bar < nb; bar += pbars) rit_at(o, n, b0 + bar * 4.0, beats * 0.5, pfrac);
        rit_at(o, n, b0 + nb * 4.0, beats, frac);
    }
    free(o);
}
