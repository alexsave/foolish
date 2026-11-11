// Moscow - the army that cannot be stopped (round 3). Score text by Fable (score.txt): steppe, the march growing out of a
// snare ostinato (Shostakovich 7 growth), a soldiers' chant, a lament, the army, and the return. A real military band:
// every brass part is a doubled section (VSCO trumpets, horns, trombones, tuba; the euphonium is a darkened trombone
// section), field drum, march snare and bass drum (VCSL), clash cymbals, timpani; strings from VSCO 2 CE; the men's choir is
// the GM choir layered with the formant ensemble. No bells: Fable's BELL strokes are low piano octaves with the pedal down
// (Rachmaninoff's bell imitation) and the tam-tam is the clash cymbal an octave down, rung long.
#include "../engine/orch.h"
#define SP(t, ...) do { double v_[] = {__VA_ARGS__}; memcpy(g_tr[t].sp, v_, sizeof v_); } while (0)

static int SUS[8], SPIC[8], NSTR, BELLT;
static int choir(const char *gname, const char *fname, int vt, double pan, double gain) {
    int g = gm_track(gname, 52, 0); g_tr[g].pan = pan; g_tr[g].delay_ms = -80; g_tr[g].human_ms = 5; g_tr[g].gain_db = gain; g_tr[g].send[0] = 0.4;
    eq(g, 2, 50, 0.7, 0); eq(g, 3, 300, 1, -1.5);
    int f = syn_track(fname, syn_voice);
    //    type fsh size det vibHz vibc breath att rel tilt scoop scoopS vibdel glide scatter singer maxHz
    SP(f, vt, 1.0, 10, 12, 5.0, 10, 0.16, 0.05, 0.25, -3.0, 0, 0, 0.5, 0.02, 0.02, 1, 9500);   // "no vibrato": 10 cents, late
    g_tr[f].pan = pan; g_tr[f].delay_ms = -60; g_tr[f].human_ms = 4; g_tr[f].gain_db = gain - 12; g_tr[f].send[0] = 0.45; g_tr[f].width = 0.8;
    eq(f, 2, 50, 0.7, 0); eq(f, 3, 300, 1, -1.5);
    return g * 1000 + f;
}

void city_setup(void) {
    g_on_done = sections_out;
    int v1 = o_smp("violins", OM "vln_sus.txt", 0.3, 1, -0.5, -30, 0.35, 5), v1b = o_double(v1, "violins2", 0.15);
    int v1s = o_smp("violins spic", OM "vln_spic.txt", 0.12, 0, -0.5, -6, 0.3, 3);
    int v2 = o_smp("violins second", OM "vln_sus.txt", 0.3, 1, -0.25, -30, 0.35, 3);
    o_hi(v1, "violins hi", 88); o_hi(v2, "violins second hi", 88);
    int va = o_smp("violas", OM "vla_sus.txt", 0.3, 1, 0.15, -30, 0.35, 1), vas = o_smp("violas spic", OM "vla_spic.txt", 0.12, 0, 0.15, -6, 0.3, 0);
    int vc = o_smp("cellos", OM "vc_sus.txt", 0.3, 1, 0.4, -30, 0.3, 2), vc2 = o_double(vc, "cellos2", -0.12), vcs = o_smp("cellos spic", OM "vc_spic.txt", 0.12, 0, 0.4, -6, 0.3, 2);
    int cb = o_smp("basses", OM "cb_sus.txt", 0.4, 1, 0.55, -35, 0.25, 9), cb2 = o_double(cb, "basses2", -0.12), cbs = o_smp("basses spic", OM "cb_spic.txt", 0.15, 0, 0.55, -6, 0.25, 8);
    int s0[] = {v1, va, vc, cb, v2}, s1[] = {v1s, vas, vcs, cbs, v1s}; NSTR = 5; memcpy(SUS, s0, sizeof s0); memcpy(SPIC, s1, sizeof s1);
    int bal = o_smp("balalaika", OM "vla_pizz.txt", 0.3, 0, -0.2, 0, 0.3, 6); eq(bal, 3, 2500, 1.2, 3); eq(bal, 2, 150, 0.7, 0);
    // the band
    int tp = o_smp("trumpets", OM "tpt_sus.txt", 0.2, 1, 0.1, -20, 0.25, -2), tp2 = o_double(tp, "trumpets2", 0.12);
    int hn = o_smp("horns", OM "hn_sus.txt", 0.3, 1, -0.2, -35, 0.3, 1), hn2 = o_double(hn, "horns2", 0.15);
    int tb = o_smp("trombones", OM "tbn_sus.txt", 0.25, 1, 0.3, -30, 0.25, 13), tb2 = o_double(tb, "trombones2", 0.12);
    int eu = o_smp("euphonium", OM "tbn_sus.txt", 0.3, 1, 0.0, -35, 0.25, 11), eu2 = o_double(eu, "euphonium2", -0.12);
    int tu = o_smp("tuba", OM "tuba_sus.txt", 0.3, 1, 0.2, -40, 0.2, 6), tu2 = o_double(tu, "tuba2", 0.1);
    int fl = o_smp("flutes", OM "fl_vib.txt", 0.25, 1, -0.1, -20, 0.4, -1), pc = o_smp("piccolo", OM "fl_vib.txt", 0.2, 1, -0.05, -20, 0.4, -6);
    int cl = o_smp("clarinets", OM "cl_sus.txt", 0.25, 1, 0.05, -20, 0.4, 1), cl2 = o_double(cl, "clarinets2", -0.1);
    int bn = o_smp("bassoons", OM "bn_sus.txt", 0.25, 1, 0.2, -20, 0.35, 0);
    int ti = o_smp("timpani", OM "timpani.txt", 1.2, 0, 0.0, 0, 0.22, 4);
    int sn = o_smp("snare", OM "marchsnare.txt", 0.3, 0, -0.1, 0, 0.15, 2), fd = o_smp("fielddrum", OM "fielddrum.txt", 0.4, 0, 0.15, 0, 0.2, 7);
    int bd = o_smp("bassdrum", OM "bassdrum.txt", 1.0, 0, 0.05, 0, 0.18, 3);
    int cy = o_smp("cymbals", OM "clash.txt", 2.0, 0, 0.2, 0, 0.25, -4);
    int tt = o_smp("tamtam", OM "clash.txt", 4.0, 0, -0.2, 0, 0.35, -2); g_tr[tt].sp[6] = -1100; eq(tt, 1, 3500, 0.7, 0);
    BELLT = o_smp("bell piano", OM "salamander.txt", 2.0, 0, -0.1, 0, 0.45, -4); g_tr[BELLT].sp[9] = 1;
    int S = choir("tenorchoir", "tenorchoir f", 1, -0.2, 0), B = choir("basschoir", "basschoir f", 0, 0.2, 1);
    eq(eu, 1, 2500, 0.7, 0); eq(eu2, 1, 2500, 0.7, 0);   // the euphonium: a trombone with its edge taken off
    o_part("VIOLINS", v1, v1b, 0, 0, 0); o_part("VIOLINS2", v2, -1, 0, 0, 0); o_part("VIOLAS", va, -1, 0, 0, 0); o_part("CELLOS", vc, vc2, 0, 0, 0); o_part("BASSES", cb, cb2, 0, 0, 0);
    o_part("BALALAIKA", bal, -1, 0, 0, 0);
    o_part("TRUMPETS", tp, tp2, 0, 0, 0); o_part("HORNS", hn, hn2, 0, 0, 0); o_part("TROMBONES", tb, tb2, 0, 0, 0);
    o_part("EUPHONIUM", eu, eu2, 0, 0, 0); o_part("TUBA", tu, tu2, 0, 0, 0);
    o_part("FLUTES", fl, -1, 0, 0, 0); o_part("PICCOLO", pc, -1, 0, 0, 0); o_part("CLARINETS", cl, cl2, 0, 0, 0); o_part("BASSOONS", bn, -1, 0, 0, 0);
    o_part("TIMPANI", ti, -1, 0, 0, 0); o_part("SNARE", sn, -1, 0, 1, 0); o_part("FIELDDRUM", fd, -1, 0, 1, 0); o_part("BASSDRUM", bd, -1, 0, 1, 0);
    o_part("CYMBALS", cy, -1, 0, 1, 0); o_part("TAMTAM", tt, -1, 0, 1, 0); o_part("BELL", BELLT, -1, 0, 0, 0);
    o_part("CHOIR", S / 1000, S % 1000, 0, 0, 0); o_part("CHOIR BASSES", B / 1000, B % 1000, 0, 0, 0);   // the second is filled from CHOIR in city_loop
    int sus[] = {v1, v1b, v2, va, vc, vc2, cb, cb2, tp, tp2, hn, hn2, tb, tb2, eu, eu2, tu, tu2, fl, pc, cl, cl2, bn}; for (unsigned i = 0; i < sizeof sus / sizeof *sus; i++) g_o_sustain[g_o_nsus++] = sus[i];
    eq(v1, 3, 250, 1, -2); eq(hn, 3, 300, 1, -1.5); eq(tb, 3, 3000, 1, -1.5);
    o_hall(2.4, 20);
    // against the USSR anthem recording: 6-10 dB dark in presence and air, 7 dB short in sub
    g_master.eq[4] = (EqBand){5, 3000, 0, 5}; g_master.eq[5] = (EqBand){4, 55, 0, 3}; g_master.neq = 6;
    g_dyn_depth = 0.8;
}
double city_loop(void) {
    int n0 = g_nnote; g_nsec = 0; score_txt("score.txt", o_map, 0);
    // the choir chords: notes under C3 go to the bass section (GM + formant), the rest stay with the tenors
    int tg = -1, bg = -1;
    for (int t = 0; t < g_ntr; t++) { if (!strcmp(g_tr[t].name, "tenorchoir")) tg = t; if (!strcmp(g_tr[t].name, "basschoir")) bg = t; }
    for (int i = n0; i < g_nnote; i++) if (g_note[i].tr == tg && g_note[i].pitch < 48) g_note[i].tr = bg;
    // hammered string notes (an eighth or shorter) to the spiccato samples
    for (int i = n0; i < g_nnote; i++) for (int k = 0; k < NSTR; k++)
        if (g_note[i].tr == SUS[k] && g_note[i].len <= 0.5 * 0.95 + 1e-6) { g_note[i].tr = SPIC[k]; break; }
    int n1 = g_nnote; o_post(n0);
    // the bell strokes: low octaves on the piano, pedal held through the stroke
    for (int i = n0; i < n1; i++) if (g_note[i].tr == BELLT) { Note a = g_note[i]; note(BELLT, a.beat, a.len, a.pitch - 12, a.vel); cc(BELLT, a.beat - 0.05, 64, 127); cc(BELLT, a.beat + a.len - 0.1, 64, 0); }
    return score_total_bars() * 4.0;
}
void city_full(void) { city_loop(); }
