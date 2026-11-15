# ALT-CELLO stage 1 tests

Run `make test`.
Each line below is one mutation check: the test, the mutation of the code it covers, and the assertion that went red.
Mutations were applied in a scratch copy of the directory (another agent builds in this directory), then the copy was discarded.

- test_rng: `rng.c` `s[3] = rotl(s[3], 45)` changed to 44 -> `FAIL test_rng.c:15: v == ref[i] -- output 3 = 9daac4484c964f3a, want e1fd7d5fe2aba6d4`
- test_string_free (T1 partials): `altstring.c` biharmonic stencil `-4.0 * y1[l + 1]` changed to `-3.9` -> `FAIL test_string_free.c:104: fabs(cd) < 1.0 -- partial 1: measured 148.740 Hz vs discrete eigenfrequency 146.835 Hz = 22.312 cents`
- test_string_free (T2 lossless energy): `string_energy` kinetic factor 0.5 changed to 0.6 -> `FAIL test_string_free.c:66: rel < 1e-10 -- energy not conserved: relative spread 0.102433`
- test_string_free (T2 lossy energy): `altstring.c` `s->s0 = sp->sigma0` changed to `-sp->sigma0` (anti-damping) -> `FAIL test_string_free.c:69: worst_up < 1e-12 -- energy increased by relative 1.62398e-06 in one step`
- test_helmholtz (T3): `bow.h` `BOW_PHI_STATIC` 1.2 changed to 0.3 (wrong stick criterion) -> `FAIL test_helmholtz.c:99: nok > 0 -- no force in 0.2..3 N gives Helmholtz motion` (then `h.ok_one_per_period -- slip intervals per period min 0 max 1`, `mean slip fraction 0.0046 vs beta 0.10`)
- test_schelleng (T4): `bow.c` friction curve replaced by Coulomb `p = 1.0` -> `FAIL test_schelleng.c:61: nh > 0 -- beta 0.10: no Helmholtz point on the grid`
- test_finger (T5): `finger.c` `if (a <= 0.0)` changed to `if (1)` (finger never applied) -> `FAIL test_finger.c:107: F > 0 -- no bow force in the list gives a periodic Helmholtz note on the stopped A3`
- test_determinism (T6 draw schedule): `bow.c` draw schedule `n * BOW_NOISE_RATE >= idx_next * ifs` changed to `n >= idx_next * 24` (sample based, not seconds based) -> `FAIL test_determinism.c:48: 0 -- draw 2 made at 0.000250000 s (96k) 0.000500000 s (48k), want 0.000250000 s` (also :36 and :41)
- test_determinism (T6 bit identity): `sim.c` a one-off `+1e-9` on the 5000th step counted across runs -> `FAIL test_determinism.c:26: same -- two runs with the same seed differ`
- test_rng_ban (T7): `sim.c` gained `static int bad_mutation(void) { return rand(); }` -> `FAIL test_rng_ban: banned random/time symbol outside rng.c/rng.h:`

## Known issues (reported, not hidden)

- T4 as written in the brief asks for ONE contiguous Helmholtz band.
  This model gives, at beta 0.1, Helmholtz at F_B = 0.159, 0.206, 0.345, 0.447, 0.579 N with a 4-slips-per-period pocket at 0.266 N inside the span.
  The test asserts: a Helmholtz point exists at beta 0.1, only multiple slipping below it, only raucous above it, no raucous point inside the span.
  Contiguity is printed as "NOT contiguous", not asserted.
  At beta 0.05 no point on the grid classifies as Helmholtz (the nearest are 0.75 and 0.97 N with 1.10 and 1.17 slip intervals per period), at beta 0.2 exactly one (0.345 N).
- T3 as written wants the check "after 0.5 s".
  The string-only model (sigma0 = 1.2 /s, no body or bridge loss) needs seconds to settle: at 0.5 s with F_B = 1.0 N the window shows 1..5 slips per period and correlation 0.33.
  The checks run on a window starting at 3.0 s, and F_B = 1.0 N does not give Helmholtz even then (raucous); the sweep finds 0.638, 0.775, 0.940 N.
- A "slip interval" ignores runs of |v_rel| > 1e-4 shorter than 0.15 ms: these ripple micro-slips (4 to 11 samples at 96 kHz, peak below 0.35 m/s) occur in the stick phase of the Helmholtz motion, real slips last about 0.9 ms and peak near 1.1 m/s.

## Stage 2 (body, four strings, wav)

The coupling is one conservative leapfrog of the whole instrument.
The strings' bridge end value y[N] is the bridge displacement z = sum_k c_k q_k.
The force on the bridge is minus dV/dz of the discrete string potential (`string_bridge_force` with the end value included), the body modes use the state at time n like the strings do.
There is no solve and no lag, and the energy balance is exact (T8).

- test_wav: `wav.c` decimator cutoff 0.45 changed to 0.95 (no anti-alias) -> `FAIL test_wav.c:35: rej < -60.0 -- 30 kHz tone only -0.0 dB down`
- test_body_energy (T8 lossless): `altstring.c` the bridge force's stiffness term scaled by 1.1 (force no longer the potential gradient) -> `FAIL test_body_energy.c:43: rel < 1e-9 -- coupled energy not conserved: relative spread 0.0019413`
- test_body_energy (T8 lossy): `body.c` damping sign flipped (anti-damped body modes) -> `FAIL test_body_energy.c:47: worst_up < 1e-12 -- energy increased by relative inf in one step`
- test_admittance (passivity): `body.c` `Gu` sign flipped -> `FAIL test_admittance.c:49: worst >= -1e-9 * maxY -- Re Y_uu = -0.15686 < 0 at 1720.8 Hz (passivity)`
- test_admittance (peaks): `body.c` stiffness term scaled by 1.05 (modes 2.5 percent off) -> `FAIL test_admittance.c:64: fabs(err) < 1.0 -- A0 peak at 104.00 Hz is 1.965% from 102.0 Hz`
- test_bridge_loss (T10): `sim.c` body driven with zero force (rigid in effect) -> `FAIL test_bridge_loss.c:73: tb < tr_ -- partial 1: body-on T60 5.214 s is not shorter than rigid 5.214 s`
- test_sympathetic (T11): `sim.c` the strings' end value forced to 0 (the bridge does not move them) -> `FAIL test_sympathetic.c:51: db >= 20.0 -- peak at 290.13 Hz is only 0.0 dB above the 270 Hz level`
- test_body_helmholtz (T12): `bow.h` `BOW_PHI_STATIC` 1.2 changed to 0.3 -> `FAIL test_body_helmholtz.c:100: nh1 > 0 -- no Helmholtz point at beta 0.1 with the body on`
- test_wolf (T13): no assertions (report only), so there is nothing to mutate.

Stage 2 notes:
- T9 asserts the A0 peak in Y_ww and the others in Y_uu (the direction of larger participation).
  In Y_uu the A0 peak sits 1.27 percent low because its c_u is only 0.3 and the skirts of CBR and B1- pull it.
- T10 fits the dB envelope between 0.05 s and 1 s: the coupled string loses most of its energy to the body in the first 0.3 s and then beats slowly, so a fit from 0.3 s on does not see the loss.

## Stage 2 iteration 2 (wound-string EI, torsion, body mass, radiativity)

String stiffness is now `EI` per string (string_params.c): G2 3.03e-4 N m^2 measured, A3 D3 C2 chosen for B = 2.5e-5, 3.5e-5, 8.0e-5 (assumed).
Every string carries a torsional coordinate on its own grid (torsion.c) and the bow contact velocity is u_t + r_s theta_t (bow.c derivation).
Existing tests whose numbers moved (all expected values come from the physics, no tolerance was loosened):
- T1 (test_string_free): B of D3 7.72e-5 -> 3.50e-5; the partials still match the discrete eigenfrequencies to 0.03 cents (they are still printed against the continuous formula, within 3 cents up to partial 10).
- T3 (test_helmholtz): D3 F 1.0 N no longer gives Helmholtz motion, the forces that work are 0.526 / 0.638 / 0.940 N (3 of 15, best correlation 0.526 N, 0.49 cents, slip fraction 0.1105); iteration 1 was 0.638 N, -0.94 cents, 0.1254.
- T4 (test_schelleng): the band at beta 0.1 is 0.447-1.26 N (contiguous, iteration 1: 0.159-0.579 N with a hole); at 10 N the last grid point now classifies as "none" (the string is choked, 0.35 slips per period) instead of "raucous", so the assertion above the band accepts "none" at the last grid point only.
- T8 (test_body_energy): extended to the torsion (see below); E0 now includes 33 percent torsion energy.
- T9 (test_admittance): unchanged assertions; the statistical modes are 3x heavier, max |Y| 0.158 -> 0.091 s/kg (-4.8 dB), peaks of the table modes unchanged to within 0.4 percent.
- T12 (test_body_helmholtz): see the report; the body-on band at beta 0.1 is 0.583-0.766 N, and the C2 body-on sweep is written to out/sweep_body_c2.txt.

New and changed tests, each mutation-checked (mutation applied in the working tree, then the saved text written back; make re-run green afterwards):

- test_helmholtz (T3, C2 added, asserts a Helmholtz point at beta 0.1 for D3 and C2): `string_params.c` C2 `EI_B(8.0e-5, 135.0)` changed to `EI_B(4.0e-4, 135.0)` (the old solid-core stiffness) -> `FAIL test_helmholtz.c:105: nok2 > 0 -- C2 beta 0.1: no force in 0.2..3 N gives Helmholtz motion`
- test_schelleng (T4, the "none" at the last grid point): the allowance `i == NF - 1` changed to `0` -> `FAIL test_schelleng.c:71: cls[i] == CLS_RAUCOUS || (cls[i] == CLS_NONE && 0) -- beta 0.10: F 10.000 above the band is none`
- test_body_energy (T8 with torsion plucked in every string, torsion must hold more than 5 percent of the energy): `torsion.c` `a1 = ct^2 k^2` scaled by 1.001 -> `FAIL test_body_energy.c:50: rel < 1e-9 -- coupled energy not conserved: relative spread 0.000219146`
- test_torsion (T14a partials vs the discrete torsion eigenfrequencies): `torsion.c` `a1` scaled by 1.02 -> `FAIL test_torsion.c:49: fabs(cents(fm, fd)) < 0.5 -- torsional partial 1: 719.421 Hz vs discrete 734.149 Hz = -35.083 cents`
- test_torsion (T14b damping): `torsion.c` sigma_t doubled -> `FAIL test_torsion.c:80: fabs(rate / (2.0 * sig) - 1.0) < 0.03 -- decay rate 184.51 vs 2 sigma_t 92.26`
- test_torsion (T14c surface impedance 3 Z0): `string_params.c` `string_Ip` scaled by 1.5 -> `FAIL test_torsion.c:106: fabs(Zmeas / (6.0 * Z0) - 1.0) < 0.05 -- F/v = 6.4177 vs 6 Z0 = 4.1456`
- test_torsion (T14d the bow solve with torsion): `sim.c` torque on theta halved -> `FAIL test_torsion.c:142: c.worst < 1e-9 -- contact velocity from the updated states differs from vB + v_rel by 0.0431072 m/s`; torque on theta removed -> `... by 0.0858101 m/s`; torsional admittance dropped from G (`G = F * G / (2.0 * k)`) -> `... by 0.0862817 m/s`
- test_torsion (T14e torsion changes the instrument): the torsional contact switched off in the solve (`if (0)`) -> `FAIL test_torsion.c:142: c.worst < 1e-9 -- ... by 1.34513 m/s` and `FAIL test_torsion.c:166: fabs(d[1] - d[0]) > 1.0 -- torsion changes the ripple hump by only 0.00 dB`
- test_radiation (T15, p(t) uses the curve): `body_params.c` every `radiativity_gain(...)` replaced by 1.0 -> `FAIL test_radiation.c:69: fabs(with - flat) > 3.0 -- the 2-4 kHz level changes by only 0.00 dB when R is flat: p(t) does not use the curve` (the same assertion failed on the flat table before the calibration was run)
- test_radiation (smoothness): `radiativity_table.c` the 1250 Hz point set to +10 dB -> `FAIL test_radiation.c:51: step <= 12.0 -- step of 33.70 dB between 800 and 1250 Hz (limit 12)`
- test_radiation (anchor): the 200 Hz point set to 3 dB -> `FAIL test_radiation.c:65: fabs(radiativity_eval_db(200.0)) < 1e-9 -- R(200 Hz) = 3 dB, want the 0 dB anchor`
- test_radiation (log-frequency linear interpolation): `radiativity.c` interpolation `x` squared -> `FAIL test_radiation.c:53: fabs(mid - 0.5 * (...)) < 1e-9 -- not linear in dB against log frequency between 80 and 125 Hz`
- test_body_helmholtz (T12, C2 with the body on, asserts a Helmholtz point at beta 0.1): `bow.h` `BOW_PHI_STATIC` 1.2 changed to 0.3 -> `FAIL test_body_helmholtz.c:125: nc2 > 0 -- no Helmholtz point at beta 0.1 for C2 with the body on` (and `:102 nh1 > 0` for D3)
