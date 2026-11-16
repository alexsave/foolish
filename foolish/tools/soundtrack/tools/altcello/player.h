/* The player (stage 4): turns a note list into dense control contours (control.h PlayTracks) for the simulator.
 *
 * NOTE LIST (plain text, one note per line, # comments):
 *   start_s dur_s midi vel [string] [vib] [depth_c] [rate_hz] [slide]
 *   string C G D A or - (auto); vib 1, 0 or - (default); depth and rate - for the defaults; slide 1 = audible portamento into this note.
 *
 * Two styles. EXPERT_LEGATO (default): lowest position, finger landing and scoop, vibrato as finger motion, a bow that arches and drifts
 * inside the playability table of the instrument, Guettler attack, bow changes through zero speed, force-first release with the left
 * hand damping. NOVICE: abrupt start, force high in the band, fixed beta, no drift, constant vibrato or none, release by stopping the bow
 * with the force on. Every contour is sampled at ctrl_rate per second of LOGICAL time and every random draw comes from a stream seeded by
 * (ALTCELLO_PLAYER_SEED, run seed, note, purpose): the number of draws per note depends on the note's length only, never on the audio
 * sample rate. */
#ifndef ALTCELLO_PLAYER_H
#define ALTCELLO_PLAYER_H
#include "control.h"
#include "instrument.h"
#include <stdint.h>

typedef enum { STYLE_EXPERT = 0, STYLE_NOVICE = 1 } PlayStyle;

typedef struct {
    double start, dur;
    int midi;
    double vel;
    int string;  /* -1: auto, else 0..nstr-1 */
    int vib;     /* -1: default (on), 0 off, 1 on */
    double depth_c, rate_hz; /* < 0: default */
    int slide;
} Note;

/* 0 on success; *out malloc'ed (caller frees) */
int notes_read(const char *path, Note **out, int *n);

typedef struct {
    const Instrument *inst;
    PlayStyle style;
    uint64_t seed;      /* run seed, mixed into ALTCELLO_PLAYER_SEED (--seed N) */
    double ctrl_rate;   /* contour samples per logical second */
    int vib_all_off;    /* --vib off */
    double damp_press;  /* finger press left on the string after the bow lift (the damping contact) */
    double vb_min, vb_span;     /* plateau bow speed = vb_min + vb_span * vel, clamped to 0.06 - 0.18 m/s */
    double bc_lo, bc_hi;        /* length of the bow change through zero speed, s (drawn per change), 0.03 - 0.05 */
    double swap_dip;            /* press lost at the middle of a finger swap on the same string, 0.8 (the old finger lifts as the new one lands) */
    int slur;                   /* diagnosis: 1 = no direction reversal at bow changes (one long bow) */
    double dip;                 /* force at the middle of a bow change as a fraction of the target, 0.75 */
    double rock;                /* amplitude of the finger press rocking with the vibrato (press 1 - 2 rock .. 1), 0.05 */
    double acc_lo, acc_hi;      /* the Guettler acceleration time of the bow, s (drawn per note), 0.06 - 0.12 */
    double beta_base;           /* phrase beta (relative to the sounding length) at vel 0.5, 0.105; loud notes lower, soft higher */
    double frac_min, frac_span; /* aimed log-band position = frac_min + frac_span * vel (0.35 - 0.60) */
    /* the expert attack of a note that starts from rest (onset work, ONSET.md): the bow force lands 0.02 s before the motion, the bow
       accelerates at att_acc (m/s^2) to att_v_frac of the note's plateau speed with the force at the log-band position att_frac (0.4),
       then speed and band position swell together to the plateau over a time drawn in [sw_lo, sw_hi] s (raised cosine). The force
       is always taken from the playability table at the INSTANTANEOUS speed and stays inside 0.2 - 0.8 of its log band. */
    int att_on;
    double att_acc, att_v_frac, att_frac, sw_lo, sw_hi;
} Player;

#define PLAYER_CTRL_RATE 1000.0
void player_init(Player *p, const Instrument *inst, PlayStyle style, uint64_t seed);

/* what the player decided for one note (for tests, the trace and the charts) */
typedef struct {
    double t_s, t_e;          /* nominal start and end */
    int string, semis;        /* string index, semitones above the open string */
    int linked_prev, linked_next; /* legato bow change into / out of this note */
    int vib_on;
    double depth_c, rate_hz;  /* drawn vibrato depth (peak, cents) and base rate */
    double t_vib0;            /* vibrato start, s */
    double beta0, vB0, frac0; /* phrase beta, plateau speed, log-band fraction aimed at */
    double t_sus0, t_sus1;    /* the sustained part where the force must sit in the band (after the attack, before the lift, outside bow changes) */
    double scoop_c, T_acc;    /* T_acc: the time from the start of the motion to the end of the attack (the plateau) */
    double ratio;             /* sounding pitch over the open string's pitch: the bow sits at beta0 of the SOUNDING length, so the simulator's beta is beta / ratio */
} NotePlan;

typedef struct {
    PlayTracks trk;
    double *beta_rel;         /* bow position as a fraction of the sounding string length (the playability table's coordinate); trk.beta = beta_rel / ratio */
    double *cents;            /* intended pitch offset of the bowed string's finger (scoop, drift, vibrato), cents re the nominal */
    NotePlan *plan;
    int nnotes;
    double dur;               /* seconds covered (last end + ring-out) */
    long ndraws;              /* random draws made in total */
} Played;

/* build the contours for notes[0..n-1] (sorted by start); 0 on success */
int player_play(const Player *p, const Note *notes, int n, Played *out);
void played_free(Played *p);

/* the playability table at (beta, vb): bilinear in log force between the grid nodes (clamped outside the grid); false when the string
   has no band anywhere */
int player_band(const Instrument *I, int str, double beta, double vb, double *fmin, double *fmax);
/* the log-band position of force F at (beta, vb): 0 = F_min, 1 = F_max */
double player_band_frac(const Instrument *I, int str, double beta, double vb, double F);

/* the per-sample trace (plain text header OUT.txt plus raw float32 OUT.f32, columns listed in the header) */
int played_write_trace(const Played *pl, const Instrument *I, const char *prefix);
#endif
