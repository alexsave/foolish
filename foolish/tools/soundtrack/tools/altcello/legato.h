/* Finger-change and bow-change experiments on one string with one continuous bow (the onset work, ONSET.md): library of onset_tool fing
 * and of the legato test. See onset_tool.c for the scenarios. */
#ifndef ALTCELLO_LEGATO_H
#define ALTCELLO_LEGATO_H
#include "instrument.h"
#include "sim.h"
#include <stdint.h>

#define FS96 96000.0
typedef struct {
    long n;
    double *vrel, *bridge; /* 96 kHz */
    double *aud;           /* the sound at audfs */
    long naud;
    double audfs;
} Rec;
void rec_free(Rec *r);
/* run an initialised sim for secs, record vrel, bridge force and the radiated pressure at 48 kHz */
void rec_sim(Sim *S, double secs, Rec *r);
/* finger position (m from the nut) of the note `semis` semitones above the open string `si`, with the stopped-pitch correction */
double finger_for(const Instrument *I, int si, int semis);

typedef struct {
    const char *scen;           /* slur, lift or bow */
    Instrument ic;              /* a private copy: the finger multipliers act on ic.finger */
    int si, s0, s1;             /* string index; semitones above the open string before and after */
    double ramp, dip, tsw, F, vB, fdip, fw, bw, bdip;
    uint64_t seed;              /* 0: the instrument's bow noise stream */
    int only_bowed;
} LegatoArgs;
typedef struct {
    int pre_ok;
    double f_fin, cents_nominal, t_pitch, t_pitch_ramp, t_reper, dip_c, lev_dip_db;
    long ntrack;
    double *tc, *fq;
} LegatoOut;
/* 0 on success; 1 for an unknown string or scenario */
int legato_default(LegatoArgs *a, const char *scen, const char *string);
int legato_run(const LegatoArgs *a, LegatoOut *out);
void legato_out_free(LegatoOut *o);
#endif
