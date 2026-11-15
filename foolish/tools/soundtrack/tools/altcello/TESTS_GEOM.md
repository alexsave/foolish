# ALT-CELLO geometry tests

Run `./build_geom.sh test` (builds geomdump and test_geom, then runs the test).
Render the images with `/Users/alex/Dev/soundtrack-tools/venv/bin/python -I charts/render_geometry.py`.

## What test_geom.c asserts

- The four strings have vibrating length 0.69 m within 1 mm (measured: 0.69000 m each, the nut y is solved so the length is exact).
- The outline widths at y = 0.17, 0.38, 0.60 match 0.440, 0.235, 0.345 within 2 mm (measured 0.4400, 0.2350, 0.3450), and the waist is a minimum.
- The body centre is in the cavity, the top plate centre is BODY_TOP, 5 cm above the top plate centre is air, and the bridge interior is BRIDGE.
- Both f-hole centres are air and the top plate 3 cm beside each is BODY_TOP.
- The cavity volume by 5 mm voxel counting is printed and range checked: 0.0311 m^3 (31.1 litres, 249100 cells).
  The brief expected 0.06 to 0.09 m^3, but 0.755 m x about 0.30 m x about 0.14 m cannot hold that much: a real cello air volume is roughly 0.02 to 0.03 m^3.
  The test therefore asserts 0.018 to 0.035 m^3 and the discrepancy is reported to the dispatcher.
- The bow contact is beta * 0.69 = 0.069 m from the bridge along the D string, and the hair and the mic capsule are found by geom_inside_pose.

## Mutation checks

Each mutation was applied to geom.c, the test run, then geom.c restored and the test re-run green.

1. In `in_slot_one`, replace the capsule test by `return 0` (no f-hole opens).
   Failing lines: `FAIL f-hole centre (-0.075,0.37) is air` and `FAIL f-hole centre (+0.075,0.37) is air`.
2. In `in_slot_one`, replace the capsule test by `return 1` (the whole top plate is open).
   Failing lines: `FAIL top plate centre solid (part 0)` and `FAIL top plate 3 cm beside the f-hole (-0.045) is solid` (and +0.045).
3. In `nut_pt`, use `GEOM_STRING_L * 0.99` in the nut solve.
   Failing lines: `FAIL string 0 length 0.68310 m (want 0.69 +- 0.001)` for all four strings.
4. In `geom_outline_halfwidth`, scale the upper bout by 1.05.
   Failing line: `FAIL upper bout 0.3622 (want 0.345 +- 0.002)`.

## Neck angle (added)

- The neck projection (board plane extended to the bridge, above the plate there) is asserted in 0.080-0.0835 m (measured 0.0831).
- The board end underside over the top plate is asserted in 0.025-0.035 m (measured 0.0306).
