/* geomdump: plain text dump of the ALT-CELLO geometry for the renderers.
 *   geomdump mesh [frame=cello|room] [key=value ...]   triangles, parts, points, boxes
 *   geomdump sections                                  voxelisation slices (geom_cell)
 * Pose keys: bow_string bow_beta bow_angle bow_tilt bow_u finger_string finger_dist
 *            finger2_string finger2_dist finger2_lift mic_x mic_y mic_z
 * No JSON: lines of text, first word is the record type. */
#include "geom.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int room;
static void tf(const double in[3], double out[3]) {
  if (room) geom_to_room(in, out);
  else memcpy(out, in, 3 * sizeof(double));
}

static void set_key(GeomPose *p, const char *k, double v) {
  if (!strcmp(k, "bow_string")) p->bow_string = (int)v;
  else if (!strcmp(k, "bow_beta")) p->bow_beta = v;
  else if (!strcmp(k, "bow_angle")) p->bow_angle = v;
  else if (!strcmp(k, "bow_tilt")) p->bow_tilt = v;
  else if (!strcmp(k, "bow_u")) p->bow_u = v;
  else if (!strcmp(k, "finger_string")) p->finger_string = (int)v;
  else if (!strcmp(k, "finger_dist")) p->finger_dist = v;
  else if (!strcmp(k, "finger2_string")) p->finger2_string = (int)v;
  else if (!strcmp(k, "finger2_dist")) p->finger2_dist = v;
  else if (!strcmp(k, "finger2_lift")) p->finger2_lift = v;
  else if (!strcmp(k, "mic_x")) p->mic[0] = v;
  else if (!strcmp(k, "mic_y")) p->mic[1] = v;
  else if (!strcmp(k, "mic_z")) p->mic[2] = v;
  else { fprintf(stderr, "geomdump: unknown key %s\n", k); exit(2); }
}

static void pt(const char *name, double x, double y, double z) {
  double a[3] = {x, y, z}, b[3];
  tf(a, b);
  printf("pt %s %.5f %.5f %.5f\n", name, b[0], b[1], b[2]);
}
static void box(const char *name, double x0, double y0, double z0, double x1, double y1, double z1, const char *label) {
  printf("box %s %.4f %.4f %.4f %.4f %.4f %.4f %s\n", name, x0, y0, z0, x1, y1, z1, label);
}

static int mesh(const GeomPose *p) {
  GeomMesh m;
  geom_mesh_build(&m, p);
  printf("# altcello geomdump v1 text; units metres\n");
  printf("frame %s\n", room ? "room" : "cello");
  for (int i = 1; i < GEOM_NPARTS; i++) printf("part %d %s\n", i, geom_part_name(i));
  printf("ntri %d\n", m.n);
  for (int t = 0; t < m.n; t++) {
    double v[9];
    for (int k = 0; k < 3; k++) {
      double a[3] = {m.v[t * 9 + k * 3], m.v[t * 9 + k * 3 + 1], m.v[t * 9 + k * 3 + 2]}, b[3];
      tf(a, b);
      memcpy(v + k * 3, b, sizeof b);
    }
    printf("tri %.5f %.5f %.5f %.5f %.5f %.5f %.5f %.5f %.5f %d\n", v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7],
           v[8], m.part[t]);
  }
  /* anchor points for labels */
  double nut[3], br[3], c[3];
  pt("bridge", 0, GEOM_BRIDGE_Y, geom_top_z(0, GEOM_BRIDGE_Y) + GEOM_BRIDGE_H * 0.55);
  pt("fhole", GEOM_FH_CX, GEOM_FH_CY, geom_top_z(GEOM_FH_CX, GEOM_FH_CY));
  const char *sn[4] = {"string_C", "string_G", "string_D", "string_A"};
  for (int i = 0; i < 4; i++) {
    geom_string_endpoints(i, nut, br);
    double t = 0.30;
    pt(sn[i], nut[0] + (br[0] - nut[0]) * t, nut[1] + (br[1] - nut[1]) * t, nut[2] + (br[2] - nut[2]) * t);
    char nm[32];
    snprintf(nm, sizeof nm, "%s_nut", sn[i]);
    pt(nm, nut[0], nut[1], nut[2]);
    snprintf(nm, sizeof nm, "%s_bridge", sn[i]);
    pt(nm, br[0], br[1], br[2]);
  }
  geom_bow_contact(p, c);
  pt("bow_contact", c[0], c[1], c[2]);
  geom_finger_point(p, c);
  pt("finger", c[0], c[1], c[2]);
  pt("mic", p->mic[0], p->mic[1], p->mic[2]);
  printf("param bow_beta %.4f\nparam bow_string %d\nparam finger_dist %.5f\nparam finger_string %d\n", p->bow_beta,
         p->bow_string, p->finger_dist, p->finger_string);
  printf("param string_length %.4f\nparam body_length %.4f\nparam w_upper %.4f\nparam w_waist %.4f\nparam w_lower %.4f\n",
         GEOM_STRING_L, GEOM_BODY_LENGTH, GEOM_W_UPPER, GEOM_W_WAIST, GEOM_W_LOWER);
  double mc[3] = {p->mic[0], p->mic[1], p->mic[2]}, ctr[3] = {0, GEOM_BRIDGE_Y, 0.0};
  printf("param mic_distance %.4f\n", sqrt(pow(mc[0] - ctr[0], 2) + pow(mc[1] - ctr[1], 2) + pow(mc[2] - ctr[2], 2)));
  if (room) {
    box("room", -GEOM_ROOM_X / 2, -GEOM_ROOM_Y / 2, 0, GEOM_ROOM_X / 2, GEOM_ROOM_Y / 2, GEOM_ROOM_Z, "Room 6 x 5 x 3.2 m");
    double h = GEOM_FDTD_SIZE / 2;
    box("fdtd", -h, GEOM_ROOM_CELLO_Y - h, 0, h, GEOM_ROOM_CELLO_Y + h, GEOM_FDTD_SIZE,
        "3D FDTD domain (dx 2.5 cm)");
  }
  geom_mesh_free(&m);
  return 0;
}

/* region code per cell: 0 outside air, 1 cavity air, 2 f-hole opening, 3+part solid */
static int code(double x, double y, double z, double dx) {
  int p = geom_cell(x, y, z, dx);
  if (p) return 3 + p;
  if (geom_cell_fhole(x, y, z, dx)) return 2;
  return geom_cavity(x, y, z) ? 1 : 0;
}

/* axis: 0 x, 1 y, 2 z.  a and b are the two varying axes, f the fixed axis. */
static void slice(const char *name, int fa, double fv, int aa, double a0, double a1, int ba, double b0, double b1, double dx) {
  int na = (int)lround((a1 - a0) / dx), nb = (int)lround((b1 - b0) / dx);
  printf("slice %s %d %.4f %d %.4f %.5f %d %d %.4f %.5f %d %.5f\n", name, fa, fv, aa, a0, dx, na, ba, b0, dx, nb, dx);
  for (int j = 0; j < nb; j++) {
    for (int i = 0; i < na; i++) {
      double c[3];
      c[fa] = fv;
      c[aa] = a0 + (i + 0.5) * dx;
      c[ba] = b0 + (j + 0.5) * dx;
      printf("%d%c", code(c[0], c[1], c[2], dx), i + 1 < na ? ' ' : '\n');
    }
  }
}

static int sections(void) {
  printf("# altcello geomdump sections v1 text\n");
  for (int i = 1; i < GEOM_NPARTS; i++) printf("part %d %s\n", i, geom_part_name(i));
  printf("param dx %.4f\n", GEOM_FDTD_DX);
  double d = GEOM_FDTD_DX;
  /* sagittal x = 0.0125 (axes y,z), cross section y = 0.37 (x,z), plans z = -0.06 and 0.012 (x,y) */
  slice("sagittal", 0, 0.0125, 1, -0.30, 1.50, 2, -0.20, 0.30, d);
  slice("cross", 1, GEOM_FH_CY, 0, -0.30, 0.30, 2, -0.18, 0.12, d);
  slice("plan_mid", 2, -0.06, 0, -0.30, 0.30, 1, -0.05, 0.80, d);
  slice("plan", 2, 0.012, 0, -0.30, 0.30, 1, -0.05, 0.80, d);
  /* 5 mm inset around the right f-hole: plan view */
  slice("plan_inset", 2, 0.012, 0, 0.02, 0.13, 1, 0.29, 0.45, 0.005);
  slice("cross_inset", 1, GEOM_FH_CY, 0, 0.02, 0.13, 2, -0.02, 0.06, 0.005);
  return 0;
}

int main(int argc, char **argv) {
  if (argc < 2) { fprintf(stderr, "usage: geomdump mesh|sections [frame=room] [key=value...]\n"); return 2; }
  GeomPose p;
  geom_pose_default(&p);
  for (int i = 2; i < argc; i++) {
    char *eq = strchr(argv[i], '=');
    if (!eq) { fprintf(stderr, "bad arg %s\n", argv[i]); return 2; }
    *eq = 0;
    if (!strcmp(argv[i], "frame")) room = !strcmp(eq + 1, "room");
    else set_key(&p, argv[i], atof(eq + 1));
  }
  if (!strcmp(argv[1], "mesh")) return mesh(&p);
  if (!strcmp(argv[1], "sections")) return sections();
  fprintf(stderr, "unknown mode %s\n", argv[1]);
  return 2;
}
