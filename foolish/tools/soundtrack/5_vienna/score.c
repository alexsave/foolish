// Vienna - a requiem suite in Mozart's manner, angry (round 3). Score text by Fable (score.txt): Dies irae, Rex tremendae,
// fugato, Recordare, Lacrimosa, and the Dies irae returning into an Amen. Sampled orchestra (VSCO 2 CE strings, brass, winds;
// VCSL timpani and pipe organ). The choir is the one place no free sampled library fits: each voice is the GM choir (Apple
// DLS "Choir Aahs") layered with the formant ensemble (eight singers per part, own detune, vibrato and scatter), and the
// trombones double it as Mozart's do.
// Mockup rules: string notes of an eighth or shorter switch to the spiccato samples (the hammered wrath), longer ones play
// the sustain samples with an early start; every brass part is a doubled section; choir starts 60-90 ms early.
#include "../engine/orch.h"
#define SP(t, ...) do { double v_[] = {__VA_ARGS__}; memcpy(g_tr[t].sp, v_, sizeof v_); } while (0)

static int SUS[8], SPIC[8], NSTR, ORG;
static int choir(const char *gname, const char *fname, int vt, double pan, double gain) {
    int g = gm_track(gname, 52, 0); g_tr[g].pan = pan; g_tr[g].delay_ms = -70; g_tr[g].human_ms = 5; g_tr[g].gain_db = gain; g_tr[g].send[0] = 0.45;
    eq(g, 2, 90, 0.7, 0); eq(g, 3, 280, 1, -2);
    int f = syn_track(fname, syn_voice);
    //    type fsh size det vibHz vibc breath att rel tilt scoop scoopS vibdel glide scatter singer maxHz
    SP(f, vt, 1.0, 8, 14, 5.2, 28, 0.16, 0.07, 0.25, -3.0, 0, 0, 0.25, 0.02, 0.025, 1, 9500);   // darker: the brighter setting buzzed like an electric guitar on exposed entries (2:10, fugato)
    g_tr[f].pan = pan; g_tr[f].delay_ms = -60; g_tr[f].human_ms = 4; g_tr[f].gain_db = gain - 9; g_tr[f].send[0] = 0.5; g_tr[f].width = 0.8;
    eq(f, 2, 90, 0.7, 0); eq(f, 3, 280, 1, -2);
    return g * 1000 + f;
}

void city_setup(void) {
    g_on_done = sections_out;
    // strings, sustain + spiccato pairs; seating: firsts far left, seconds left, violas centre-right, cellos right, basses far right
    int v1 = o_smp("violins1", OM "vln_sus.txt", 0.3, 1, -0.55, -30, 0.35, 7), v1b = o_double(v1, "violins1b", 0.12); o_hi(v1, "violins1 hi", 88);
    int v1s = o_smp("violins1 spic", OM "vln_spic.txt", 0.12, 0, -0.55, -6, 0.3, 3);
    int v2 = o_smp("violins2", OM "vln_sus.txt", 0.3, 1, -0.25, -30, 0.35, 0), v2s = o_smp("violins2 spic", OM "vln_spic.txt", 0.12, 0, -0.25, -6, 0.3, -1);
    int v2t = o_smp("violins2 trem", OM "vln_trem.txt", 0.3, 1, -0.25, -20, 0.35, -5);
    int va = o_smp("violas", OM "vla_sus.txt", 0.3, 1, 0.15, -30, 0.35, 0), vas = o_smp("violas spic", OM "vla_spic.txt", 0.12, 0, 0.15, -6, 0.3, -1);
    int vc = o_smp("cellos", OM "vc_sus.txt", 0.3, 1, 0.4, -30, 0.3, 2), vc2 = o_double(vc, "cellos2", -0.12), vcs = o_smp("cellos spic", OM "vc_spic.txt", 0.12, 0, 0.4, -6, 0.3, 1);
    int cb = o_smp("basses", OM "cb_sus.txt", 0.35, 1, 0.55, -35, 0.25, 9), cb2 = o_double(cb, "basses2", -0.12), cbs = o_smp("basses spic", OM "cb_spic.txt", 0.15, 0, 0.55, -6, 0.25, 8);
    int cbp = o_smp("basses pizz", OM "cb_pizz.txt", 0.6, 0, 0.55, 0, 0.3, 10);
    int s0[] = {v1, v2, va, vc, cb}, s1[] = {v1s, v2s, vas, vcs, cbs}; NSTR = 5; memcpy(SUS, s0, sizeof s0); memcpy(SPIC, s1, sizeof s1);
    // winds (basset horns are clarinets here), brass sections, timpani, organ
    int cl = o_smp("bassethorns", OM "cl_sus.txt", 0.25, 1, -0.1, -20, 0.4, -1), bn = o_smp("bassoons", OM "bn_sus.txt", 0.25, 1, 0.15, -20, 0.35, -1);
    int tp = o_smp("trumpets", OM "tpt_sus.txt", 0.2, 1, 0.1, -20, 0.25, -4), tp2 = o_double(tp, "trumpets2", 0.12);
    int tb = o_smp("trombones", OM "tbn_sus.txt", 0.25, 1, 0.3, -30, 0.28, 4), tb2 = o_double(tb, "trombones2", 0.15);
    int ti = o_smp("timpani", OM "timpani.txt", 1.2, 0, 0.0, 0, 0.22, 6);
    int og = ORG = o_smp("organ", OM "organ_loud.txt", 0.6, 1, 0.0, -20, 0.5, 0); eq(og, 3, 250, 1, -3); eq(og, 1, 6000, 0.7, 0);
    // the piano at the centre (round 4: "Where's the piano? Is this Vienna or Tchaikovsky?"): Salamander, slightly forward,
    // dry enough to cut through the tutti; the right hand leads the left by a few ms
    int prh = o_smp("piano rh", OM "salamander.txt", 0.3, 0, -0.05, -8, 0.2, -3), plh = o_smp("piano lh", OM "salamander.txt", 0.3, 0, 0.05, 0, 0.2, -4);
    for (int t = prh; t <= plh; t++) { g_tr[t].sp[1] = 0.6; g_tr[t].human_ms = 4; g_tr[t].width = 0.6; eq(t, 3, 2800, 1, 1.5); }
    o_part("PIANO RH", prh, -1, 0, 0, 0); o_part("PIANO LH", plh, -1, 0, 0, 0);
    // the choir: GM + formant per voice, sopranos and altos left of centre, tenors and basses right
    int S = choir("sopranos", "sopranos f", 3, -0.3, 0), A = choir("altos", "altos f", 2, -0.1, -1);
    int T = choir("tenors", "tenors f", 1, 0.1, -1), B = choir("basschoir", "basschoir f", 0, 0.3, 0);
    o_part("SOPRANO", S / 1000, S % 1000, 0, 0, 0); o_part("ALTO", A / 1000, A % 1000, 0, 0, 0);
    o_part("TENOR", T / 1000, T % 1000, 0, 0, 0); o_part("BASS", B / 1000, B % 1000, 0, 0, 0);
    o_part("VIOLINS1", v1, v1b, 0, 0, 0); o_part("VIOLINS2", v2, -1, 0, 0, 0); o_part("VIOLINS2 TREM", v2t, -1, 0, 0, 0);
    o_part("VIOLAS", va, -1, 0, 0, 0); o_part("CELLOS", vc, vc2, 0, 0, 0); o_part("BASSES", cb, cb2, 0, 0, 0); o_part("BASSES PIZZ", cbp, -1, 0, 0, 0);
    o_part("CLARINETS", cl, -1, 0, 0, 0); o_part("BASSOONS", bn, -1, 0, 0, 0);
    o_part("TRUMPETS", tp, tp2, 0, 0, 0); o_part("TROMBONES", tb, tb2, 0, 0, 0); o_part("TIMPANI", ti, -1, 0, 0, 0); o_part("ORGAN", og, -1, 0, 0, 0);
    int sus[] = {v1, v1b, v2, va, vc, vc2, cb, cb2, cl, bn, tp, tp2, tb, tb2, og}; for (unsigned i = 0; i < sizeof sus / sizeof *sus; i++) g_o_sustain[g_o_nsus++] = sus[i];
    eq(v1, 3, 250, 1, -2); eq(v1, 5, 4000, 0, 1.5); eq(tb, 3, 3000, 1, -1.5);
    o_hall(2.6, 22);
    g_dyn_depth = 1.0;
}
double city_loop(void) {
    int n0 = g_nnote; g_nsec = 0; score_txt("score.txt", o_map, 0);
    sec_rit(0.08, 2, 0, 0);   // an 8% breath into every section end
    // the hammered notes: an eighth or shorter goes to the spiccato samples of the same section (before o_post, so no doubles)
    for (int i = n0; i < g_nnote; i++) for (int k = 0; k < NSTR; k++)
        if (g_note[i].tr == SUS[k] && g_note[i].len <= 0.5 * 0.95 + 1e-6) { g_note[i].tr = SPIC[k]; g_note[i].vel = fmin(1, g_note[i].vel * 1.05); break; }
    // the organ's pedal: the lowest note of each organ chord an octave down (a 16' stop), the low D under the wrath
    for (int i = n0, n1 = g_nnote; i < n1; i++) if (g_note[i].tr == ORG) { int low = 1;
        for (int j = n0; j < n1; j++) if (j != i && g_note[j].tr == ORG && fabs(g_note[j].beat - g_note[i].beat) < 1e-6 && g_note[j].pitch < g_note[i].pitch) low = 0;
        if (low && g_note[i].pitch - 12 >= 24) note(ORG, g_note[i].beat, g_note[i].len, g_note[i].pitch - 12, g_note[i].vel * 0.9); }
    o_post(n0);
    return score_total_bars() * 4.0;
}
void city_full(void) { city_loop(); }
