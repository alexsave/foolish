// New York - old-school / 90s NY hip hop suite (round 3). Score text by Fable (score.txt); sound by the C engine.
// Drums: Big Rusty Drums (CC0) close+overhead, through an SP-1200-style 26.04 kHz / 12-bit stage, MPC swing.
// Melodic "samples" re-performed with real instruments (Salamander piano, VSCO horns/strings/flute, VCSL vibes and
// TX81Z FM piano, D. Smolken double bass) and made dusty (low-pass, crush, vinyl), as the research describes.
#include "../engine/render.h"
#include "../engine/scoretxt.h"
#define SM "../samples/manifests/"
static int KICK, KICKOH, SNARE, SNAREOH, HAT, OHAT, RIM, CLAP, SHK, COWB, CRASH, RHODES, RHODESFM, PIANO, TPT, TBN, HRN, VLN, VLA, VC,
           VIBES, FLUTE, GUITAR, BASS, SUB, SYNTH, SCRATCH, VOX, VINYL, B808, STUT, BRAKE, SPIN;
static void syn_none(Track *tr, const Note *n, float *L, float *R, long nf) { (void)tr; (void)n; (void)L; (void)R; (void)nf; }
#define SP(t, ...) do { double v_[] = {__VA_ARGS__}; memcpy(g_tr[t].sp, v_, sizeof v_); } while (0)
static int smp(const char *name, const char *man, double rel, int loop, int grp) { int t = smp_track(name, man); g_tr[t].sp[0] = rel; g_tr[t].sp[10] = loop; g_tr[t].group = grp; return t; }
static void dusty(int t, double lp, double sr) { eq(t, 1, lp, 0.7, 0); eq(t, 2, 90, 0.7, 0); g_tr[t].crush_sr = sr; g_tr[t].crush_bits = 12; }

void city_setup(void) {
    g_on_done = sections_out;
    // ---- drums: close + overhead layers, SP-1200 crush on the kit, MPC swing (8ths ~57% on kick/snare, hats looser) ----
    KICK = smp("kick", SM "brd_kick_cl.txt", 0.15, 0, 1);   g_tr[KICK].gain_db = 2; eq(KICK, 3, 60, 1, 3); eq(KICK, 3, 160, 1.2, -5);
    KICKOH = smp("kickoh", SM "brd_kick_oh.txt", 0.2, 0, 1); g_tr[KICKOH].gain_db = -6;
    SNARE = smp("snare", SM "brd_snare_top.txt", 0.2, 0, 1); g_tr[SNARE].gain_db = 10; g_tr[SNARE].delay_ms = 8; eq(SNARE, 3, 200, 1, 2); eq(SNARE, 3, 4500, 1, 2);
    SNAREOH = smp("snareoh", SM "brd_snare_oh.txt", 0.3, 0, 1); g_tr[SNAREOH].gain_db = 2; g_tr[SNAREOH].delay_ms = 8;
    HAT = smp("hat", SM "brd_hat_tc.txt", 0.08, 0, 1);      g_tr[HAT].gain_db = 7; g_tr[HAT].pan = 0.25; g_tr[HAT].human_ms = 3; g_tr[HAT].human_vel = 0.1;
    OHAT = smp("ohat", SM "brd_hat_open.txt", 0.3, 0, 1);   g_tr[OHAT].gain_db = -2; g_tr[OHAT].pan = 0.25;
    RIM = smp("rim", SM "brd_rim.txt", 0.1, 0, 1);          g_tr[RIM].gain_db = 0; g_tr[RIM].pan = -0.2;
    CLAP = syn_track("clap", syn_clap); SP(CLAP, 1100, 1.0, 0.1, 9, 4, 600); g_tr[CLAP].gain_db = -8; g_tr[CLAP].group = 1; eq(CLAP, 1, 7000, 0.7, 0);
    SHK = smp("shaker", SM "vcsl_shaker.txt", 0.08, 0, 1);  g_tr[SHK].gain_db = 4; g_tr[SHK].pan = -0.35; g_tr[SHK].human_ms = 4;
    COWB = smp("cowbell", SM "vcsl_cowbell.txt", 0.1, 0, 1); g_tr[COWB].gain_db = -6; g_tr[COWB].pan = 0.3;
    CRASH = smp("crash", SM "crash_oh.txt", 1.5, 0, 0);     g_tr[CRASH].gain_db = -8; g_tr[CRASH].pan = -0.3; eq(CRASH, 1, 7000, 0.7, 0);
    int sw8[] = {KICK, KICKOH, SNARE, SNAREOH, RIM, CLAP}; for (unsigned i = 0; i < 6; i++) g_tr[sw8[i]].swing8 = 0.14;
    int swh[] = {HAT, OHAT, SHK, COWB}; for (unsigned i = 0; i < 4; i++) { g_tr[swh[i]].swing8 = 0.24; g_tr[swh[i]].swing = 0.24; }
    g_grp[1] = (Group){.drive = 0.5, .cthr = -16, .cratio = 4, .catt = 0.012, .crel = 0.1, .parallel = 0.6, .gain_db = 1};
    for (int t = KICK; t <= COWB; t++) { g_tr[t].crush_sr = 26040; g_tr[t].crush_bits = 12; }
    // ---- the "sample": real instruments, dusty ----
    RHODES = syn_track("rhodes", syn_epiano); SP(RHODES, 1.8, 0.4, 0.2, 2.6, 0.3, 0.3, 4.2, 0.6);
    g_tr[RHODES].gain_db = -8;
    RHODESFM = smp("rhodesfm", SM "fmpiano.txt", 0.3, 0, 0); g_tr[RHODESFM].gain_db = -6; g_tr[RHODESFM].chorus_mix = 0.4; g_tr[RHODESFM].chorus_ms = 7; g_tr[RHODESFM].chorus_depth_ms = 1.5; dusty(RHODESFM, 7000, 32000);
    PIANO = smp("piano", SM "salamander.txt", 0.25, 0, 0); g_tr[PIANO].gain_db = -1; g_tr[PIANO].human_ms = 5; dusty(PIANO, 5200, 26040); g_tr[PIANO].send[0] = 0.15;
    TPT = smp("trumpets", SM "tpt_sus.txt", 0.15, 1, 3);   g_tr[TPT].pan = 0.2; g_tr[TPT].human_ms = 6; g_tr[TPT].delay_ms = -10;
    TBN = smp("trombones", SM "tbn_sus.txt", 0.15, 1, 3);  g_tr[TBN].pan = -0.2; g_tr[TBN].human_ms = 6; g_tr[TBN].delay_ms = -10;
    HRN = smp("horns", SM "hn_sus.txt", 0.2, 1, 3);        g_tr[HRN].pan = 0.0; g_tr[HRN].human_ms = 6; g_tr[HRN].gain_db = -3; g_tr[HRN].delay_ms = -15;
    g_grp[3] = (Group){.drive = 0.3, .cthr = -18, .cratio = 3, .catt = 0.005, .crel = 0.1, .gain_db = -2, .eq = {{1, 6500, 0.7, 0}, {2, 120, 0.7, 0}}, .neq = 2};
    VLN = smp("violins", SM "vln_sus.txt", 0.35, 1, 0); g_tr[VLN].pan = -0.4; g_tr[VLN].delay_ms = -30; g_tr[VLN].gain_db = -2; dusty(VLN, 7000, 32000); g_tr[VLN].send[0] = 0.25;
    VLA = smp("violas", SM "vla_sus.txt", 0.35, 1, 0);  g_tr[VLA].pan = 0.1; g_tr[VLA].delay_ms = -30; g_tr[VLA].gain_db = -4; dusty(VLA, 7000, 32000); g_tr[VLA].send[0] = 0.25;
    VC = smp("cellos", SM "vc_sus.txt", 0.35, 1, 0);    g_tr[VC].pan = 0.4; g_tr[VC].delay_ms = -30; g_tr[VC].gain_db = -4; dusty(VC, 6000, 32000); g_tr[VC].send[0] = 0.25;
    VIBES = smp("vibes", SM "vibes.txt", 1.0, 0, 0);    g_tr[VIBES].gain_db = -3; g_tr[VIBES].pan = 0.3; dusty(VIBES, 8000, 32000); g_tr[VIBES].send[0] = 0.2;
    FLUTE = smp("flute", SM "fl_vib.txt", 0.2, 1, 0);   g_tr[FLUTE].gain_db = -4; g_tr[FLUTE].pan = -0.25; dusty(FLUTE, 7500, 32000); g_tr[FLUTE].send[0] = 0.25;
    GUITAR = syn_track("guitar", syn_guitar); SP(GUITAR, 0.996, 0.55, 0.16, 0.6, 0.2); g_tr[GUITAR].gain_db = -3; g_tr[GUITAR].pan = -0.35; g_tr[GUITAR].send[0] = 0.2; dusty(GUITAR, 6000, 32000);
    g_tr[GUITAR].chorus_mix = 0.25; g_tr[GUITAR].chorus_ms = 6; g_tr[GUITAR].chorus_depth_ms = 1;
    BASS = smp("bass", SM "dbass_pizz.txt", 0.12, 0, 0); g_tr[BASS].gain_db = 4; g_tr[BASS].delay_ms = -5; eq(BASS, 3, 90, 1, 3); eq(BASS, 1, 3000, 0.7, 0); g_tr[BASS].cthr = -20; g_tr[BASS].cratio = 4; g_tr[BASS].catt = 0.01; g_tr[BASS].crel = 0.1;
    SUB = syn_track("sub", syn_sub); SP(SUB, 0.01, 0.1, 0.1, 0.3, 0); g_tr[SUB].gain_db = -15; eq(SUB, 1, 100, 0.7, 0);
    SYNTH = syn_track("synth", syn_supersaw); SP(SYNTH, 5, 18, 0.5, 2.2, 6, 0.25, 0.01, 0, 1, 0.12, 0.4, 0.8, 0.7, 1.2, 600);
    g_tr[SYNTH].gain_db = -4; g_tr[SYNTH].send[0] = 0.15; g_tr[SYNTH].chorus_mix = 0.4; g_tr[SYNTH].chorus_ms = 8; g_tr[SYNTH].chorus_depth_ms = 2; g_tr[SYNTH].chorus_hz = 0.3;
    // round 4: the scratches work the playing record. SCRATCH/STUTTER/BRAKE/SPINBACK are not sounds; they become turntable
    // events on the record (the drums, group 1; the sample, group 2; the horns, group 3), so the beat itself is cut and warped
    SCRATCH = syn_track("scratch", syn_none); g_tr[SCRATCH].mute = 1;
    STUT = syn_track("stutter", syn_none); g_tr[STUT].mute = 1; BRAKE = syn_track("brake", syn_none); g_tr[BRAKE].mute = 1; SPIN = syn_track("spinback", syn_none); g_tr[SPIN].mute = 1;
    // the 808: a long sine boom with a click, a little drive, under the upright
    B808 = syn_track("808", syn_808); SP(B808, 0, 1.4, 3, 0.12, 0.25); g_tr[B808].gain_db = -6; eq(B808, 2, 28, 0.7, 0); eq(B808, 1, 2500, 0.7, 0);
    g_tr[B808].cthr = -18; g_tr[B808].cratio = 3; g_tr[B808].catt = 0.005; g_tr[B808].crel = 0.12;
    VOX = syn_track("vox", syn_voice); SP(VOX, 2, 1.0, 5, 10, 5.5, 20, 0.08, 0.02, 0.15, 0.5, 0, 0, 0.2, 0.05, 0.02, 2, 6000);
    g_tr[VOX].gain_db = -9; g_tr[VOX].send[0] = 0.3; dusty(VOX, 4500, 22050);
    VINYL = syn_track("vinyl", syn_vinyl); SP(VINYL, 0.004, 9, 0.5, 0.003); g_tr[VINYL].gain_db = -6;
    { int rec[] = {RHODES, RHODESFM, PIANO, VLN, VLA, VC, VIBES, FLUTE, GUITAR, VOX}; for (unsigned i = 0; i < 10; i++) g_tr[rec[i]].group = 2; }
    g_grp[2] = (Group){.cthr = -20, .cratio = 2, .catt = 0.01, .crel = 0.15, .gain_db = 0};
    g_bus[0] = (Bus){.type = 1, .rev = {1.1, 0.5, 15, 0.8, 300, 6000, 0.15, 1.0, 0.2, 0.2}, .gain_db = -8, .duck_src = -1};
    g_master.eq[0] = (EqBand){2, 30, 0.7, 0}; g_master.eq[1] = (EqBand){5, 10000, 0, 1}; g_master.neq = 2;
    g_master.cthr = -14; g_master.cratio = 2; g_master.catt = 0.01; g_master.crel = 0.15; g_master.width_lo_hz = 120;
}
enum { CHORD_HORNS = 1000, CHORD_STRINGS };
static int map(const char *p, double *tp, double *vs, double *dp) {
    *tp = 0; *vs = 1;
    if (!strcmp(p, "KICK")) return KICK;   if (!strcmp(p, "SNARE")) return SNARE;  if (!strcmp(p, "HAT")) return HAT;
    if (!strcmp(p, "OHAT")) return OHAT;   if (!strcmp(p, "RIM")) return RIM;      if (!strcmp(p, "CLAP")) return CLAP;
    if (!strcmp(p, "SHAKER")) return SHK;  if (!strcmp(p, "COWBELL")) return COWB; if (!strcmp(p, "CRASH")) { *dp = -60; return CRASH; }
    if (!strcmp(p, "RHODES")) return RHODES; if (!strcmp(p, "PIANO")) return PIANO; if (!strcmp(p, "HORNS")) return TPT;
    if (!strcmp(p, "STRINGS")) return VLN; if (!strcmp(p, "VIBES")) return VIBES;  if (!strcmp(p, "FLUTE")) return FLUTE;
    if (!strcmp(p, "GUITAR")) return GUITAR; if (!strcmp(p, "BASS")) return BASS;  if (!strcmp(p, "SUB")) return SUB;
    if (!strcmp(p, "SYNTH")) return SYNTH; if (!strcmp(p, "SCRATCH")) return SCRATCH; if (!strcmp(p, "VOX")) return VOX;
    if (!strcmp(p, "808")) return B808; if (!strcmp(p, "STUTTER")) { *dp = -60; return STUT; } if (!strcmp(p, "BRAKE")) { *dp = -60; return BRAKE; } if (!strcmp(p, "SPINBACK")) { *dp = -60; return SPIN; }
    return -1;
}
// split chords written on one track across a section, low to high
static void split_chords(int n0, int src, const int *dst, int nd) {
    int n1 = g_nnote;
    for (int i = n0; i < n1; i++) { if (g_note[i].tr != src || g_note[i].vel <= 0) continue;
        int idx[8], c = 0; for (int j = i; j < n1 && c < 8; j++) if (g_note[j].tr == src && fabs(g_note[j].beat - g_note[i].beat) < 1e-6) idx[c++] = j;
        for (int a = 0; a < c; a++) for (int b = a + 1; b < c; b++) if (g_note[idx[b]].pitch < g_note[idx[a]].pitch) { int x = idx[a]; idx[a] = idx[b]; idx[b] = x; }
        for (int a = 0; a < c; a++) { int k = c == 1 ? nd - 1 : (int)lround((double)a * (nd - 1) / (c - 1)); g_note[idx[a]].tr = dst[k]; g_note[idx[a]].tr = g_note[idx[a]].tr == src ? -src - 2 : g_note[idx[a]].tr; }
    }
    for (int i = n0; i < n1; i++) if (g_note[i].tr < -1) g_note[i].tr = -g_note[i].tr - 2;
}
static void post(int n0) {
    // horns: low notes to trombones, the middle to horns, the top to trumpets; strings to cellos/violas/violins
    int hd[3] = {TBN, HRN, TPT}; split_chords(n0, TPT, hd, 3);
    int sd[3] = {VC, VLA, VLN}; split_chords(n0, VLN, sd, 3);
    int n1 = g_nnote;
    for (int i = n0; i < n1; i++) { Note n = g_note[i];
        if (n.tr == KICK) note(KICKOH, n.beat, n.len, 60, n.vel * 0.9);
        if (n.tr == SNARE) note(SNAREOH, n.beat, n.len, 60, n.vel * 0.9);
        if (n.tr == RHODES) note(RHODESFM, n.beat, n.len, n.pitch, n.vel * 0.85);                          // FM e-piano layered on the Rhodes
        if (n.tr == BASS) note(SUB, n.beat, n.len, n.pitch < 36 ? n.pitch + 12 : n.pitch, n.vel * 0.8);   // a sine under the upright
        if (n.tr == GUITAR || n.tr == VIBES) g_note[i].p[3] = 0;
    }
    // the turntable: baby and chirp cut the sample only (the drums keep the time under the DJ), transformer, stutter, brake and
    // backspin take the whole record, drums included
    for (int i = n0; i < n1; i++) { Note n = g_note[i]; int ty = -1; double sp = 2, dep = 0.11;
        if (n.tr == SCRATCH) { int k = (int)n.p[0]; ty = k == 0 ? 1 : k == 1 ? 2 : 3; if (ty == 3) sp = 4; }
        else if (n.tr == STUT) { ty = 4; sp = 4; } else if (n.tr == BRAKE) ty = 5; else if (n.tr == SPIN) ty = 6;
        if (ty < 0) continue;
        double len = n.tr == SCRATCH ? n.len / 0.95 : n.len;
        turntable(2, n.beat, len, ty, sp, dep); turntable(3, n.beat, len, ty, sp, dep);
        if (ty >= 3) turntable(1, n.beat, len, ty, sp, dep);
    }
    // vinyl under everything
    note(VINYL, 0, score_total_bars() * 4.0, 60, 1);
}
static void automation(void) {
    // the sample loop's filter moves, as Fable's FX lines ask (low-pass, Hz)
    int loop[] = {VLN, VLA, VC, RHODES, RHODESFM, PIANO, VIBES, BASS};
    for (int i = 0; i < 8; i++) { sec_lp(loop[i], "Flash: Needle drop", 1, 4, 300, 6000, 0); sec_lp(loop[i], "Cypher: Needle", 3, 4, 6000, 300, 1); }
    int close[] = {VLN, VLA, VC, RHODES, RHODESFM, BASS}; for (int i = 0; i < 6; i++) sec_lp(close[i], "Flash: Hand-off", 1, 4, 6000, 400, 0);
    sec_lp(RHODES, "Dusty: Loop", 1, 4, 800, 12000, 0); sec_lp(RHODESFM, "Dusty: Loop", 1, 4, 800, 12000, 0);
    sec_lp(RHODES, "Dusty: Walkdown", 1, 4, 4000, 1200, 0); sec_lp(RHODESFM, "Dusty: Walkdown", 1, 4, 4000, 1200, 0);
    sec_lp(PIANO, "Queensbridge: Loop", 1, 4, 900, 900, 0);
    sec_lp(PIANO, "Queensbridge: Vanish", 5, 8, 2000, 2000, 0);
}
double city_loop(void) { int n0 = g_nnote; g_nsec = 0; score_txt("score.txt", map, 0); post(n0); automation(); return score_total_bars() * 4.0; }
void city_full(void) { city_loop(); }
