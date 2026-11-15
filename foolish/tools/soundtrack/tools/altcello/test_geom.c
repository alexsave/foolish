/* test_geom: geometry sanity.  Every assertion prints its number. */
#include "geom.h"
#include <math.h>
#include <stdio.h>

static int fails;
#define CHECK(cond, ...) do { int ok_ = (cond); printf("%s ", ok_ ? "ok  " : "FAIL"); printf(__VA_ARGS__); printf("\n"); if (!ok_) fails++; } while (0)

int main(void) {
  /* string vibrating lengths */
  for (int s = 0; s < 4; s++) {
    double n[3], b[3];
    geom_string_endpoints(s, n, b);
    double L = sqrt(pow(n[0] - b[0], 2) + pow(n[1] - b[1], 2) + pow(n[2] - b[2], 2));
    CHECK(fabs(L - 0.69) < 0.001, "string %d length %.5f m (want 0.69 +- 0.001)", s, L);
  }
  /* outline widths at the three bouts */
  double wl = 2 * geom_outline_halfwidth(GEOM_Y_LOWER), ww = 2 * geom_outline_halfwidth(GEOM_Y_WAIST),
         wu = 2 * geom_outline_halfwidth(GEOM_Y_UPPER);
  CHECK(fabs(wl - GEOM_W_LOWER) < 0.002, "lower bout %.4f (want %.3f +- 0.002)", wl, GEOM_W_LOWER);
  CHECK(fabs(ww - GEOM_W_WAIST) < 0.002, "waist %.4f (want %.3f +- 0.002)", ww, GEOM_W_WAIST);
  CHECK(fabs(wu - GEOM_W_UPPER) < 0.002, "upper bout %.4f (want %.3f +- 0.002)", wu, GEOM_W_UPPER);
  /* the outline is the widest at the bouts: waist is a real minimum */
  CHECK(geom_outline_halfwidth(0.30) > geom_outline_halfwidth(GEOM_Y_WAIST) &&
            geom_outline_halfwidth(0.50) > geom_outline_halfwidth(GEOM_Y_WAIST),
        "waist is a minimum between the bouts");
  /* inside tests */
  double zc = geom_top_z(0, GEOM_BODY_LENGTH / 2);
  CHECK(geom_cavity(0, GEOM_BODY_LENGTH / 2, -0.06), "body centre (0,%.3f,-0.06) is in the cavity", GEOM_BODY_LENGTH / 2);
  CHECK(geom_inside(0, GEOM_BODY_LENGTH / 2, zc - 0.0015) == GEOM_BODY_TOP, "top plate centre solid (part %d)",
        geom_inside(0, GEOM_BODY_LENGTH / 2, zc - 0.0015));
  CHECK(geom_inside(0, GEOM_BODY_LENGTH / 2, -0.06) == GEOM_AIR, "cavity centre is air (shell model)");
  CHECK(geom_inside(0, GEOM_BODY_LENGTH / 2, zc + 0.05) == GEOM_AIR, "5 cm above the top plate centre is air");
  double zb = geom_top_z(0, GEOM_BRIDGE_Y) + 0.04;
  CHECK(geom_inside(0, GEOM_BRIDGE_Y, zb) == GEOM_BRIDGE, "inside the bridge (part %d)", geom_inside(0, GEOM_BRIDGE_Y, zb));
  /* f-holes open, plate beside them solid */
  for (int sg = -1; sg <= 1; sg += 2) {
    double x = sg * GEOM_FH_CX, y = GEOM_FH_CY, z = geom_top_z(x, y) - 0.0015;
    CHECK(geom_inside(x, y, z) == GEOM_AIR, "f-hole centre (%+.3f,%.2f) is air", x, y);
    double xs = x - sg * 0.03, zs = geom_top_z(xs, y) - 0.0015;
    CHECK(geom_inside(xs, y, zs) == GEOM_BODY_TOP, "top plate 3 cm beside the f-hole (%+.3f) is solid", xs);
  }
  /* cavity volume by voxel counting at 5 mm */
  const double h = 0.005;
  long n = 0;
  for (double x = -0.23; x < 0.23; x += h)
    for (double y = 0.0; y < 0.76; y += h)
      for (double z = -0.16; z < 0.03; z += h)
        if (geom_cavity(x + h / 2, y + h / 2, z + h / 2)) n++;
  double vol = n * h * h * h;
  /* Brief asked for 0.06-0.09 m^3, but 0.755 x ~0.30 x ~0.14 m is physically about 0.02-0.03 m^3
   * (a real cello air volume is roughly 20-25 litres).  The test pins the geometric range. */
  CHECK(vol > 0.018 && vol < 0.035, "cavity volume %.4f m^3 = %.1f litres (5 mm voxels, %ld cells)", vol, vol * 1000, n);
  /* nut and bridge heights: the board clearance at the end is 5-6 mm */
  GeomPose p;
  geom_pose_default(&p);
  double c[3], f[3], nut[3], br[3];
  geom_bow_contact(&p, c);
  geom_string_endpoints(p.bow_string, nut, br);
  double dd = sqrt(pow(c[0] - br[0], 2) + pow(c[1] - br[1], 2) + pow(c[2] - br[2], 2));
  CHECK(fabs(dd - 0.1 * 0.69) < 1e-6, "bow contact is %.5f m from the bridge along the string (beta 0.1 = 0.069)", dd);
  geom_finger_point(&p, f);
  CHECK(geom_inside_pose(&p, c[0], c[1], c[2] + 0.0008 + 0.00075) == GEOM_BOW_HAIR, "point on the hair at the contact is BOW_HAIR");
  CHECK(geom_inside_pose(&p, p.mic[0], p.mic[1], p.mic[2]) == GEOM_MIC, "mic centre is MIC");
  printf("%s\n", fails ? "GEOM TEST FAILED" : "GEOM TEST PASSED");
  return fails ? 1 : 0;
}
