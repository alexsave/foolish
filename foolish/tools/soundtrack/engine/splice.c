// splice: assembles one song from stretches of existing recordings, by an edit list written as text.
//   splice out.wav edit.txt [sections_out.txt]
// Every join is declared, so a song stays editable: move a cut, change an overlap, re-run (a second or two).
// Edit list, one statement per line ('#' starts a comment):
//   SRC <name> <path.wav>              a 48 kHz wav (decode mp3 first: ffmpeg -i x.mp3 -ar 48000 -ac 2 x.wav)
//   LEVEL <lufs>                       every clip after this line is brought to this integrated loudness, measured on
//                                      the clip's own stretch (default -16; "LEVEL off" keeps source levels)
//   CLIP <name> <in_s> <out_s|end> [key value]...   placed after the previous clip; keys:
//       OVER s    start s seconds before the previous clip ends (a crossfade; default 0 = butt join)
//       GAP s     start s seconds after the previous clip ends (silence, or the previous clip's WASH ringing)
//       FIN s     fade-in length (default: OVER, or 0.005); FOUT s  fade-out length (default: the next clip's OVER, or 0.005)
//       GAIN db   added after LEVEL
//       WASH s    the last 1.5 s of the clip also feed a hall of this reverb time, which rings past the clip's end
//       LPOUT s   over the last s seconds a low-pass closes from 18 kHz to 250 Hz (a DJ filter fade)
//       HPIN s    over the first s seconds a high-pass opens from 1.2 kHz down to 20 Hz
//       LABEL txt a name for the sections file (one word; use _ for spaces)
//   MASTER <lufs> <true_peak_db>       final loudness and ceiling (default -16 -1.3); gain only, the limiter runs only if
//                                      the peak would pass the ceiling, and it says so
// Fades are equal power (sin/cos). Nothing here is random.
#include "mix.h"

typedef struct { char name[32]; float *L, *R; long n; } Src;
typedef struct { int src; double in, out, over, gap, fin, fout, gain, wash, lpout, hpin; double level; int fin_set, fout_set; char label[64]; long at, len; } Clip;
static Src g_src[64]; static int g_nsrc; static Clip g_clip[256]; static int g_nclip;

static int src_find(const char *n) { for (int i = 0; i < g_nsrc; i++) if (!strcmp(g_src[i].name, n)) return i; fprintf(stderr, "splice: no SRC %s\n", n); exit(1); }

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: splice out.wav edit.txt [sections.txt]\n"); return 2; }
    FILE *f = fopen(argv[2], "r"); if (!f) { perror(argv[2]); return 1; }
    char line[2048]; double level = -16, m_lufs = -16, m_tp = -1.3; int level_on = 1;
    while (fgets(line, sizeof line, f)) {
        char *h = strchr(line, '#'); if (h) *h = 0;
        char *tok[64]; int nt = 0; for (char *p = strtok(line, " \t\r\n"); p && nt < 64; p = strtok(0, " \t\r\n")) tok[nt++] = p;
        if (!nt) continue;
        if (!strcmp(tok[0], "SRC") && nt >= 3) {
            Src *s = &g_src[g_nsrc++]; snprintf(s->name, sizeof s->name, "%s", tok[1]); s->n = wav_read(tok[2], &s->L, &s->R);
            if (g_wav_sr != SR) { fprintf(stderr, "splice: %s is %u Hz, need %d\n", tok[2], g_wav_sr, SR); return 1; }
        } else if (!strcmp(tok[0], "LEVEL") && nt >= 2) { if (!strcmp(tok[1], "off")) level_on = 0; else { level_on = 1; level = atof(tok[1]); } }
        else if (!strcmp(tok[0], "MASTER") && nt >= 3) { m_lufs = atof(tok[1]); m_tp = atof(tok[2]); }
        else if (!strcmp(tok[0], "CLIP") && nt >= 4) {
            Clip *c = &g_clip[g_nclip++]; memset(c, 0, sizeof *c); c->src = src_find(tok[1]); c->in = atof(tok[2]);
            c->out = !strcmp(tok[3], "end") ? (double)g_src[c->src].n / SR : atof(tok[3]); c->level = level_on ? level : 1e9;
            snprintf(c->label, sizeof c->label, "%s_%s", tok[1], tok[2]);
            for (int i = 4; i + 1 < nt; i += 2) { double v = atof(tok[i + 1]); const char *k = tok[i];
                if (!strcmp(k, "OVER")) c->over = v; else if (!strcmp(k, "GAP")) c->gap = v; else if (!strcmp(k, "FIN")) { c->fin = v; c->fin_set = 1; }
                else if (!strcmp(k, "FOUT")) { c->fout = v; c->fout_set = 1; } else if (!strcmp(k, "GAIN")) c->gain = v; else if (!strcmp(k, "WASH")) c->wash = v;
                else if (!strcmp(k, "LPOUT")) c->lpout = v; else if (!strcmp(k, "HPIN")) c->hpin = v; else if (!strcmp(k, "LABEL")) snprintf(c->label, sizeof c->label, "%s", tok[i + 1]);
                else { fprintf(stderr, "splice: unknown key %s\n", k); return 1; } }
            if (c->out > (double)g_src[c->src].n / SR + 1e-6 || c->in < 0 || c->out <= c->in) { fprintf(stderr, "splice: clip %s %.2f-%.2f is outside its source (%.2f s)\n", tok[1], c->in, c->out, (double)g_src[c->src].n / SR); return 1; }
        } else { fprintf(stderr, "splice: cannot read line starting %s\n", tok[0]); return 1; }
    }
    fclose(f); if (!g_nclip) { fprintf(stderr, "splice: no clips\n"); return 1; }
    // place the clips
    long cur = 0, total = 0;
    for (int i = 0; i < g_nclip; i++) { Clip *c = &g_clip[i]; c->len = lround((c->out - c->in) * SR);
        c->at = cur - lround(c->over * SR) + lround(c->gap * SR); if (c->at < 0) c->at = 0;
        if (!c->fin_set) c->fin = c->over > 0 ? c->over : 0.005;
        if (!c->fout_set) c->fout = (i + 1 < g_nclip && g_clip[i + 1].over > 0) ? g_clip[i + 1].over : 0.005;
        cur = c->at + c->len; long end = cur + (long)((c->wash > 0 ? c->wash * 2.5 + 1 : 0) * SR); if (end > total) total = end; }
    float *OL = calloc(total + SR, sizeof(float)), *OR = calloc(total + SR, sizeof(float));
    FILE *sf = argc > 3 ? fopen(argv[3], "w") : 0;
    for (int i = 0; i < g_nclip; i++) { Clip *c = &g_clip[i]; Src *s = &g_src[c->src]; long a = lround(c->in * SR), n = c->len;
        float *xl = malloc(n * sizeof(float)), *xr = malloc(n * sizeof(float));
        for (long k = 0; k < n; k++) { long j = a + k; xl[k] = j < s->n ? s->L[j] : 0; xr[k] = j < s->n ? s->R[j] : 0; }
        double I = lufs(xl, xr, n, 0, 0, 0), g = db2a(c->gain); if (c->level < 1e8 && I > -60) g *= db2a(c->level - I);
        long nfi = lround(c->fin * SR), nfo = lround(c->fout * SR), nlp = lround(c->lpout * SR), nhp = lround(c->hpin * SR), nw = (long)(1.5 * SR);
        Svf lpl = {0}, lpr = {0}, hpl = {0}, hpr = {0}; Rev rv; int wash = c->wash > 0;
        if (wash) rev_init(&rv, (RevCfg){c->wash, c->wash * 0.5, 10, 1.4, 200, 6000, 0.14, 1.0, 0.2, 0.25});
        long ntail = wash ? (long)(c->wash * 2.5 * SR) : 0;
        for (long k = 0; k < n + ntail; k++) {
            double l = k < n ? xl[k] * g : 0, r = k < n ? xr[k] * g : 0, bp, hp;
            if (nhp > 0 && k < nhp) { double fc = 1200 * pow(20.0 / 1200, (double)k / nhp); svf(&hpl, l, fc, 0.7, &bp, &hp); l = hp; svf(&hpr, r, fc, 0.7, &bp, &hp); r = hp; }
            if (nlp > 0 && k >= n - nlp && k < n) { double fc = 18000 * pow(250.0 / 18000, (double)(k - (n - nlp)) / nlp); l = svf(&lpl, l, fc, 0.7, &bp, &hp); r = svf(&lpr, r, fc, 0.7, &bp, &hp); }
            double e = 1; if (k < nfi) e = sin(0.5 * M_PI * k / nfi); if (k >= n - nfo && k < n) e *= cos(0.5 * M_PI * (double)(k - (n - nfo)) / nfo);
            double wl = 0, wr = 0;
            if (wash) { double send = (k >= n - nw && k < n) ? smoothstep((double)(k - (n - nw)) / (0.3 * SR)) * 0.5 : 0; rev_tick(&rv, l * send, r * send, &wl, &wr); }
            long j = c->at + k; if (j < total + SR) { OL[j] += (float)(l * e + wl); OR[j] += (float)(r * e + wr); }
        }
        fprintf(stderr, "%7.2f - %7.2f  %-28s src %6.2f-%6.2f  %6.1f LUFS  gain %+5.1f dB  over %.2f gap %.2f%s\n", (double)c->at / SR, (double)(c->at + n) / SR, c->label, c->in, c->out, I, a2db(g), c->over, c->gap, wash ? "  wash" : "");
        if (sf) fprintf(sf, "%.2f\t%.2f\t%s\n", (double)c->at / SR, (double)(c->at + n) / SR, c->label);
        free(xl); free(xr);
    }
    if (sf) fclose(sf);
    // trim trailing silence under -70 dBFS, keep 0.3 s
    long n = total + SR; while (n > 1 && fabsf(OL[n - 1]) < 3e-4 && fabsf(OR[n - 1]) < 3e-4) n--; n += (long)(0.3 * SR); if (n > total + SR) n = total + SR;
    double I = lufs(OL, OR, n, 0, 0, 0), g = db2a(m_lufs - I); for (long i = 0; i < n; i++) { OL[i] *= (float)g; OR[i] *= (float)g; }
    double tp = a2db(true_peak(OL, OR, n)); int limited = 0;
    if (tp > m_tp) { limit(OL, OR, n, m_tp, 0.12); limited = 1; double I2 = lufs(OL, OR, n, 0, 0, 0); fprintf(stderr, "limiter ran: peak was %.2f dBTP, loudness after %.2f LUFS\n", tp, I2); tp = a2db(true_peak(OL, OR, n)); }
    wav_write24(argv[1], OL, OR, n);
    fprintf(stderr, "wrote %s  %.2f s  %.2f LUFS  %.2f dBTP%s\n", argv[1], (double)n / SR, lufs(OL, OR, n, 0, 0, 0), tp, limited ? "  (limited)" : "");
    return 0;
}
