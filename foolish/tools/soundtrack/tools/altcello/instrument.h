/* The instrument description (stage 4): everything that is specific to one instrument lives in one struct Instrument.
 * The core (strings, bow, finger, body, radiation, controller) takes a `const Instrument *` and has no instrument
 * constants of its own, so a violin or a haegeum is a new instrument_<name>.c that fills the same struct.
 * instrument_cello.c holds the values with their citation comments (sources: REFERENCES.md of the project). */
#ifndef ALTCELLO_INSTRUMENT_H
#define ALTCELLO_INSTRUMENT_H

#define INSTR_MAXSTR 4
#define INSTR_MAXLEAD 16
#define INSTR_MAXSEMI 15 /* stopped-pitch correction table: 0..14 semitones above the open string */

typedef struct {
    const char *name;
    double f0;     /* open string pitch, Hz */
    double T;      /* tension, N */
    double r;      /* outer (surface) radius of the wound string, m: where the bow touches (torsion lever arm) */
    double L;      /* vibrating length nut to bridge, m */
    double EI;     /* bending stiffness, N m^2 (a wound string: the winding adds mass, not stiffness) */
    double sigma0; /* frequency independent loss, 1/s */
    double sigma1; /* frequency dependent loss, m^2/s */
    double ct_ratio; /* torsional wave speed / transverse wave speed */
    double zeta_t;   /* torsional damping ratio at the torsional fundamental (sigma_t = zeta_t w_t1); 0 = lossless */
    double zt_ratio; /* torsional characteristic impedance at the surface / Z0 = sqrt(T rho) */
} StringSpec;

/* friction curve phi(v) = a1 exp(-|v|/v1) + a2 exp(-|v|/v2) + c (odd), phi(0+) = mu_static */
typedef struct {
    double a1, v1, a2, v2, c;
    double mu_static;
} Friction;

/* thermal elasto-plastic friction, van Walstijn et al. 2026 (friction_thermal.h): Table 1 of the paper unless noted */
typedef struct {
    double mu_s, mu_d;      /* static and dynamic friction coefficients (Table 1: 1.05, 0.3) */
    double sigma0;          /* bristle stiffness, 1/m (1e6) */
    double tau_g;           /* glass transition temperature above ambient, K (25) */
    double ya, xi, chi;     /* asymptotic temperature factor (0.4), transition steepness (2), break-away fraction (0.5) */
    double a_tau, b_tau, c_tau; /* heat constants: absorption m/K (1e-6), velocity dependent conduction (0.22), conduction (1e-4) */
    int nsub;               /* substeps of the friction state per audio sample */
    double v_slip;          /* m/s: the bow noise acts on slips above this relative speed (full above twice it) */
} ThermalSpec;

typedef enum { BOW_CURVE = 0, BOW_THERMAL = 1 } BowModel;

typedef struct {
    double width;       /* bow hair contact width, m */
    double noise_level; /* std of the raw bow-noise draws */
    double hair_length; /* usable hair, m (the controller's bow length budget) */
    Friction friction;
    BowModel model;     /* BOW_CURVE: friction curve (bow.h); BOW_THERMAL: friction_thermal.h */
    ThermalSpec thermal;
} BowSpec;

typedef struct {
    double width; /* pad width, m */
    double K;     /* N/m^2 */
    double R;     /* N s/m^2 */
} FingerSpec;

typedef struct {
    const char *name;
    double f, Q, m, cu, cw; /* Hz, -, kg (effective mass at the bridge), participation in u and w */
} LeadMode;

typedef struct {
    int nlead;
    LeadMode lead[INSTR_MAXLEAD]; /* the named table modes */
    int ncompact;                 /* the first ncompact table modes radiate as compact monopoles */
    double compact_r[INSTR_MAXLEAD]; /* their monopole strengths */
    /* statistical modes above the table */
    double stat_f0, stat_f1;      /* Hz, range */
    double stat_gap_mean;         /* Hz, exponential gaps */
    double stat_m500;             /* kg at 500 Hz (mass law m = m500 sqrt(f / 500)) times the scale already applied */
    double stat_m_sd_db;          /* spread of the masses, dB */
    double eta0, eta_exp;         /* loss factor eta(f) = eta0 (f / 500 Hz)^eta_exp */
    /* Woodhouse bridge model: bridge rocking inertia, resistance R_inf, hill frequency (sets the stiffness) */
    double bridge_inertia;        /* kg m^2 */
    double bridge_R;              /* rad/(m s N) */
    double bridge_f0;             /* Hz */
} BodySpec;

typedef struct {
    double a_eff;   /* m, equivalent radius of the compact sources */
    double fc;      /* Hz, plate coincidence frequency (a value of radiate_plate_fits) */
    double area;    /* m^2, plate area */
    double perim;   /* m, plate perimeter */
    double g_plate; /* relative level of the plate path (measured once by test_radiate T16d) */
} RadiationSpec;

#define PLAY_NBETA 3
#define PLAY_NVB 3
/* one playability entry: the contiguous Helmholtz band of bow force (N) for one string at one beta and v_B; all zero when no
   band was found (generated, see instrument_cello.c) */
typedef struct {
    double fmin, fmax, fcentre; /* N; fcentre = sqrt(fmin fmax) */
} PlayBand;

typedef struct {
    const char *name;
    int nstr;
    StringSpec str[INSTR_MAXSTR];
    int midi_open[INSTR_MAXSTR];     /* open string MIDI notes */
    BowSpec bow;
    FingerSpec finger;
    BodySpec body;
    RadiationSpec radiation;
    /* playability anchors: the bow force (N) in the middle of the Helmholtz band at beta 0.1, v_B 0.1 m/s, per string */
    double helmholtz_force[INSTR_MAXSTR];
    /* playability table (generated by charts/playability_table.py): per string, beta (play_beta) and bow speed (play_vb), the
       longest contiguous Helmholtz band. The player controller stays inside it with margin. */
    double play_beta[PLAY_NBETA], play_vb[PLAY_NVB];
    PlayBand playability[INSTR_MAXSTR][PLAY_NBETA][PLAY_NVB];
    /* stopped-pitch correction, cents to ADD to the ideal pitch when computing the finger position, per string and semitone
       (generated by charts/stopped_pitch.py, see instrument_cello.c) */
    double stop_cents[INSTR_MAXSTR][INSTR_MAXSEMI];
    /* the Guettler-wedge attack of the expert player (player.h), per string: the bow acceleration att_acc (m/s^2) and the wedge slope att_k (N per
       m/s^2): during the acceleration the force is att_k * att_acc (generated by charts/wedge_table.py, see instrument_cello.c) */
    double att_acc[INSTR_MAXSTR], att_k[INSTR_MAXSTR];
    /* playing defaults of the player */
    double vib_depth_c, vib_rate_hz;
} Instrument;

const Instrument *instrument_cello(void);
/* DIAGNOSTIC HOOK (knobs.h): the tools install a modified copy of the cello (one factor changed) and every later instrument_cello()
   returns it; NULL restores the shipped values. The shipped renders never call it. */
void instrument_cello_override(const Instrument *I);
/* NULL if unknown */
const StringSpec *instrument_string(const Instrument *I, const char *name);
int instrument_string_index(const Instrument *I, const char *name); /* -1 if unknown */

double string_rho(const StringSpec *sp);         /* T / (2 L f0)^2, kg/m */
double string_B(const StringSpec *sp);           /* pi^2 EI / (T L^2) */
double string_EI_for_B(double B, double T, double L); /* EI = B T L^2 / pi^2 */
double string_ct(const StringSpec *sp);          /* torsional wave speed, m/s */
double string_Ip(const StringSpec *sp);          /* torsional inertia per length, kg m: Z_t,surface = Ip c_t / r^2 = zt_ratio Z0 */

#endif
