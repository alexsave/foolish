/* ALT-CELLO geometry: one description of the cello, used for 3D FDTD voxels
 * (geom_inside, geom_cell) and for pictures (geom_mesh_build).
 *
 * Units metres.  Cello frame: x across the instrument (bass side negative),
 * y along the strings from the endpin end (y = 0 at the bottom of the body)
 * toward the scroll, z out of the top plate (top plate surface z = 0 at the
 * outline, arched up to +GEOM_ARCH_TOP; back plate at z = -GEOM_RIB_H).
 * Room frame (geom_to_room): X = x, Y = ROOM_CELLO_Y - z, Z = y + endpin
 * length, so the cello stands upright on its endpin and the top plate faces -Y.
 *
 * All typical full-size cello values come from common luthier practice; the
 * research agent will refine them.  Every number is a named constant here. */
#ifndef ALTCELLO_GEOM_H
#define ALTCELLO_GEOM_H
#include <stdint.h>

/* ---- body (typical full-size cello, luthier values) ---- */
#define GEOM_BODY_LENGTH 0.755 /* m, stop length of the body (typical 0.75-0.76) */
#define GEOM_W_UPPER 0.345     /* m, upper bout width (typical 0.340-0.350) */
#define GEOM_W_WAIST 0.235     /* m, C-bout (waist) width (typical 0.230-0.240) */
#define GEOM_W_LOWER 0.440     /* m, lower bout width (typical 0.435-0.445) */
#define GEOM_Y_LOWER 0.17      /* m, y of the lower bout maximum */
#define GEOM_Y_WAIST 0.38      /* m, y of the waist minimum */
#define GEOM_Y_UPPER 0.60      /* m, y of the upper bout maximum */
#define GEOM_RIB_H 0.120       /* m, rib height (constant here, real: 0.115-0.125) */
#define GEOM_ARCH_TOP 0.025    /* m, top plate arching at the centre */
#define GEOM_ARCH_BACK 0.025   /* m, back plate arching at the centre */
#define GEOM_T_TOP 0.003       /* m, top plate thickness (typical 3 mm at the thick middle) */
#define GEOM_T_BACK 0.004      /* m, back plate thickness */
#define GEOM_T_RIB 0.002       /* m, rib thickness */
/* f-holes: capsule slots */
#define GEOM_FH_LEN 0.125 /* m, f-hole length */
#define GEOM_FH_W 0.010   /* m, f-hole width (a simplified straight slot) */
#define GEOM_FH_CX 0.075  /* m, |x| of the f-hole centres */
#define GEOM_FH_CY 0.37   /* m, y of the f-hole centres */
#define GEOM_FH_ANG 0.12  /* rad, slant, upper ends tilted toward the centreline */
/* bridge */
#define GEOM_BRIDGE_Y 0.40     /* m, between the f-hole notches */
#define GEOM_BRIDGE_H 0.090    /* m, height above the top plate at the centre */
#define GEOM_BRIDGE_W 0.090    /* m, width at the feet */
#define GEOM_BRIDGE_WTOP 0.075 /* m, width at the top corners (tapered) */
#define GEOM_BRIDGE_T 0.005    /* m, thickness along the strings */
/* neck, fingerboard, strings */
#define GEOM_STRING_L 0.69        /* m, vibrating length nut to bridge (along the string) */
#define GEOM_NECK_ANGLE 0.015     /* rad, rise of the string line along the neck */
#define GEOM_FB_LEN 0.58          /* m, fingerboard length from the nut */
#define GEOM_SP_BRIDGE 0.0115     /* m, string spacing at the bridge */
#define GEOM_SP_NUT 0.0065        /* m, string spacing at the nut */
#define GEOM_PEGBOX_LEN 0.17      /* m, pegbox beyond the nut */
#define GEOM_ENDPIN_LEN 0.20      /* m, endpin below the body (room floor at y = -this) */
#define GEOM_NSTRINGS 4
/* bow */
#define GEOM_HAIR_W 0.010 /* m, hair ribbon width */
#define GEOM_HAIR_L 0.62  /* m, hair length */
/* mic */
#define GEOM_MIC_R 0.010 /* m, capsule radius (0.02 diameter) */
#define GEOM_MIC_L 0.050 /* m, capsule length */
/* room */
#define GEOM_ROOM_X 6.0
#define GEOM_ROOM_Y 5.0
#define GEOM_ROOM_Z 3.2
#define GEOM_ROOM_CELLO_Y 0.5 /* m, room Y of the cello centre plane */
#define GEOM_FDTD_SIZE 3.0    /* m, cube side of the FDTD domain */
#define GEOM_FDTD_DX 0.025    /* m, FDTD cell size */

enum {
  GEOM_AIR = 0,
  GEOM_BODY_TOP,
  GEOM_BODY_BACK,
  GEOM_RIBS,
  GEOM_FHOLE,
  GEOM_BRIDGE,
  GEOM_NECK,
  GEOM_FINGERBOARD,
  GEOM_SCROLL,
  GEOM_STRING_C,
  GEOM_STRING_G,
  GEOM_STRING_D,
  GEOM_STRING_A,
  GEOM_BOW_STICK,
  GEOM_BOW_HAIR,
  GEOM_FINGER,
  GEOM_MIC,
  GEOM_TAILPIECE,
  GEOM_ENDPIN,
  GEOM_NPARTS
};

typedef struct {
  int bow_string;      /* 0..3 = C G D A; the string the bow hair lies on */
  double bow_beta;     /* distance from the bridge as a fraction of the string length */
  double bow_angle;    /* rad, hair deviation from perpendicular to the string (toward the nut +) */
  double bow_tilt;     /* rad, stick lean toward the fingerboard */
  double bow_u;        /* 0..1, contact position along the hair (0 frog, 1 tip) */
  int finger_string;   /* 0..3, or -1 for none */
  double finger_dist;  /* m from the nut along the string (default A string, E4) */
  int finger2_string;  /* second, lifted finger; -1 for none */
  double finger2_dist; /* m from the nut */
  double finger2_lift; /* m, lift height of the pad above the string */
  double mic[3];       /* capsule centre, cello frame */
} GeomPose;

typedef struct {
  float *v;       /* 9 floats per triangle (x1 y1 z1 x2 y2 z2 x3 y3 z3), cello frame */
  uint8_t *part;  /* one GEOM_* tag per triangle */
  int n, cap;
} GeomMesh;

void geom_pose_default(GeomPose *p);
void geom_mesh_build(GeomMesh *m, const GeomPose *p);
void geom_mesh_free(GeomMesh *m);

/* Which static solid part a point is inside (GEOM_AIR = 0 for air).  The
 * instrument only: body shell, bridge, neck, strings, tailpiece, endpin.  The
 * f-hole openings are air. */
int geom_inside(double x, double y, double z);
/* Same plus the pose dependent parts (bow hair, finger, mic capsule).  The bow
 * stick is not tested (it is far from everything the FDTD resolves). */
int geom_inside_pose(const GeomPose *p, double x, double y, double z);
/* 1 if the point is in the enclosed body cavity (the air volume of the cello). */
int geom_cavity(double x, double y, double z);
/* FDTD cell classifier for a cell of side dx centred at c: supersample (about 2 mm),
 * any solid part wins (thin plates are thickened to one cell, conservative),
 * except that a cell mostly over an f-hole is open (returns GEOM_AIR). */
int geom_cell(double cx, double cy, double cz, double dx);
/* 1 if geom_cell is air only because of the f-hole opening rule (an open f-hole cell). */
int geom_cell_fhole(double cx, double cy, double cz, double dx);

int geom_fhole_xy(double x, double y);   /* 1 if (x,y) is over an f-hole slot */
double geom_outline_halfwidth(double y); /* half width of the body outline at y */
double geom_top_z(double x, double y);   /* top plate outer surface */
void geom_string_endpoints(int s, double nut[3], double bridge[3]);
void geom_bow_contact(const GeomPose *p, double out[3]);  /* on the string, cello frame */
void geom_finger_point(const GeomPose *p, double out[3]); /* pressed point of finger 1 */
void geom_to_room(const double in[3], double out[3]);
const char *geom_part_name(int part);

#endif
