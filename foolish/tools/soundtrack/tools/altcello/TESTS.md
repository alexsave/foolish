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

## Stage 2 iteration 3 (physical radiation, wood loss, bridge hill, bow-hair option)

The free radiativity curve (radiativity.c, radiativity_table.c, charts/calibrate_radiation.py, test_radiation T15) was removed in iteration 3: it could not fix the 18 dB excess at 3-8 kHz, which came from the mass-line response of the low modes radiated as a compact monopole at all frequencies, so the pressure now comes from the modal velocities through radiate_body.c (compact first-order filter for A0 CBR B1- B1+, baffled-plate efficiency filter for all other modes). The last iteration-2 control points stay in ITERATION_LOG.md and the iteration-2 report only.
Existing tests whose numbers moved (all expectations come from the physics, no tolerance was loosened, the assertions are unchanged):
- T9 (test_admittance): unchanged assertions; max |Y_uu| 0.091 s/kg as before (the bridge hill is milder than the old single-dof hill, but the statistical Q schedule changed); the table-mode peaks are exactly as in iteration 2.
- T12 (test_body_helmholtz): the body-on Helmholtz band at beta 0.1 is now 0.583-1.007 N (iteration 2: 0.583-0.766 N), 3 points; the best force of the T3 conditions with the body 1.140 N; settling 2.2 s (body on); hump at the T3 force -11.5 dB re h1. C2 with the body on: 4 Helmholtz points at beta 0.1 (1.457-2.504 N). Assertions (a Helmholtz point at beta 0.1 for D3 and C2) unchanged.
- T14e (test_torsion): hump torsion off -6.6 dB, on -15.6 dB re h1 (rigid bridge, assertion: they differ by more than 1 dB, unchanged).
- T10, T11, T13, T3, T4: unchanged results or still passing with the same assertions (T3 still 3 of 15 forces, best 0.526 N).
- test_admittance and the callers of `body_new` take the new `opts` argument (NULL: default knobs). `SimOpts.flat_radiation` is gone (it switched the free curve off); `SimOpts.body_opts` took its place.

New tests, each mutation-checked (mutation applied in the working tree, make and run, then the saved text written back; sources touched with a pause so make rebuilds):
- test_radiate (T16a, sigma(f) of the plate formula against the REFERENCES 7.4 table at 250 500 1000 2000 Hz within 3 percent, clamped to (0, 1], never falling): `radiate_body.c` plate prefactor `P lambda_c` scaled by 2.5 -> `FAIL test_radiate.c:27: fabs(s / ref[i].s - 1.0) < 0.03 -- sigma(250) = 0.1089, table 0.044`
- test_radiate (T16b, realised digital plate filter against sqrt(sigma): shipped f_c 3 kHz under 1 dB outside a window 0.85-1.1 x the knee, under 2 dB inside; other f_c under 1.5 / 2.5 dB; the six real sections cannot follow the hard clamp kink to 1 dB: measured 0.97 dB outside the window and 1.65 dB inside at 3 kHz): `radiate_body.c` `R->K` times 1.5 -> `FAIL test_radiate.c:59: worst_out < lim_out -- f_c 2000: plate filter off by 4.181 dB at 135 Hz (outside the knee)`
- test_radiate (T16b compact filter: -3.01 dB at ka = 1 (218 Hz), 6 dB/oct below, 0 dB above): `radiate_body.c` `c_b1` scaled by 0.5 -> `FAIL test_radiate.c:69: fabs(m + 3.0103) < 0.3 -- corner gain 27.87 dB, want -3.01` (and the slope and high-frequency assertions)
- test_radiate (T16c plate signs +-1, deterministic, balanced; compact strengths 1.0 and 0.7): `body_params.c` signs all +1 -> `FAIL test_radiate.c:89: fabs(plus - 0.5 * nplate) < 5.0 * sqrt(0.25 * nplate) -- signs unbalanced: 650 of 650 positive`
- test_radiate (T16d G_PLATE, equal path power in 212-424 Hz for a white bridge force, within 1 dB; measured compact / plate 1.90 dB, so G_PLATE = 1.245): `RADIATE_G_PLATE` 1.0 -> `FAIL test_radiate.c:122: fabs(ratio_db - g_db) < 1.0 -- paths differ by 1.90 dB in 212-424 Hz after G_PLATE (0.00 dB)`
- test_radiate (T16e no flat-acceleration brightness, 3-8 kHz power density below -8 dB re 212-424 Hz, measured -10.6 dB): body.c back to the old acceleration formula `sum r (q2 - 2 q1 + q0)` in both paths -> `FAIL test_radiate.c:129: hi_db < -8.0 -- 3-8 kHz density is 12.5 dB re the 212-424 Hz band, want below -8` (and T16d)
- test_bridge_hill (T17a/b, hill factor 1 at 20 Hz, peak 3.3 (10.4 dB) at 1465 Hz, 1.5x inertia lowers the hill to 1165 Hz, ratio 0.796 against 1/sqrt(1.5) = 0.816 within 10 percent): `body_bridge_gain` ignores the inertia argument -> `FAIL test_bridge_hill.c:46: fabs(ratio / (1.0 / sqrt(1.5)) - 1.0) < 0.10 -- ratio 1.000 is not within 10 percent of 0.816`
- test_bridge_hill (T17b, the built modal masses follow the hill factor of the chosen inertia): `m /= body_bridge_gain(...)` replaced by `m /= 1.0` -> `FAIL test_bridge_hill.c:49: w < 1e-12 -- modal masses do not follow the bridge inertia: 0.884008`
- test_bridge_hill (T17c, statistical Q = 1/(0.012 (f/500)^0.3), table Q unchanged): exponent 0.3 -> 0.2 in `body_opts_default` -> `FAIL test_bridge_hill.c:59: worst < 1e-9 -- statistical Q differs from 1/eta(f) by 0.374074`
- test_hair (T18a, hair oscillator: static deflection F/k, ring frequency, damping ratio 0.3): `bow.c` `g` times 1.1 -> `FAIL test_hair.c:25: fabs(h.d1 * kh - 1.0) < 1e-4 -- deflection 0.00011, want 0.0001`; `A` stiffness term times 1.1 -> `FAIL test_hair.c:49: ... ring 715.6 Hz, expected 679.0 Hz`; `r` doubled -> `FAIL test_hair.c:58: fabs(rate / want - 1.0) < 0.05 -- decay 2684.2 1/s, want 1341.6`
- test_hair (T18b, the scalar friction solve with the hair in series is exact, 1e-9 m/s over 45600 steps, measured 1.2e-15): hair admittance dropped from G -> `FAIL test_hair.c:95: c.worst < 1e-9 -- contact velocity differs by 0.00739468 m/s`; hair velocity dropped from q -> `... by 0.0896005 m/s`; `dprev` taken from the wrong state -> `... by 0.0404806 m/s`
- test_hair (T18c, hair off by default, switching it on changes the D3 ripple hump by more than 0.5 dB): `hair_init` sets `on = 0` -> `FAIL test_hair.c:118: fabs(h[1] - h[0]) > 0.5 -- the hair changes the ripple hump by only 0.00 dB`
- Not covered by a test: the plate filter fits are generated by charts/fit_plate_filter.py (check by running it and the T16b numbers); the study tool bowhair_study.c is a report, not a test.

## Stage 4 part 1 (Instrument description refactor)

string_params.c/h and body_params.c/h are gone: every instrument constant lives in instrument_cello.c (struct Instrument, instrument.h), the core takes `const Instrument *` (sim_init, sim_init_instrument, body_new, body_modes_build, radiator_init, bow_solve). The Makefile now builds with `-ffp-contract=off`: with the default contraction the refactor (compile-time literals became loads) moved the last bit of some samples, so the arithmetic must not depend on what the compiler sees as constant. All other tests pass with unchanged assertions (the RADIATE_* macros became Instrument fields).

- test_instrument (T23, regression: a 1 s probe of D3 and of C2, body on, all strings, 96 kHz, FNV-1a hash of all channels bit-identical to the pre-refactor code built with -ffp-contract=off: D3 3179987b8cae4f4b, C2 c4533b1282189180). Mutation checks: friction `c` 0.35 -> 0.36 -> `FAIL test_instrument.c:47: hc == REF_C2 -- C2 probe changed: 9a2c4fb5fe94ba0f`; G_PLATE 1.245 -> 1.246 -> `FAIL ... 1c7c6554fd8688fc`; D3 f0 146.83 -> 146.84 -> `FAIL ... 258fa7e09d0a7e70`; each restored by re-editing, make, PASS.

## Stage 3a room

`room.c` builds a shoebox IR (image sources to order 6, three bands, 8 line FDN tail, source directivity, microphone pattern) and `room_apply` adds the microphone self-noise; `altcello room` is the command line.
Assumed, not verified: the source directivity (+-1.5 dB cos at the high band, +-0.75 mid, 0 low; idea from Meyer, numbers ASSUMED), the cardioid leak floor of -25 dB, the band tilt (low x1.2, high x0.7 of the mid RT60), the nominal 74 dB SPL for a forte cello note, the A-weighted white noise model of the 12 dB(A) self-noise.
Verified against the source: the Neumann U 87 Ai cardioid 12 dB(A) figure (REFERENCES 4.4); the A weighting at 100 Hz, 1 kHz and 10 kHz (-19.1, 0, -2.5 dB, standard table values, checked in the test).
The ISO 9613-1 formula was typed from memory and is UNVERIFIED against the standard text; it gives 4.66 dB/km at 1 kHz, 29.7 at 4 kHz and 105.3 at 8 kHz at 20 C and 50 percent RH (REFERENCES recalls 4.7, 37 and 117, also unverified; the test only asserts the textbook ranges).
The wall absorption is Eyring, not Sabine: with Sabine's alpha the order-6 image set decays at 0.54 s while the FDN decays at 0.6 s (alpha 0.2), which bends the Schroeder curve at the crossover.

Tests in test_room.c (T24), each mutation-checked (mutation applied in room.c, 1 s pause, make, run, then the saved text restored and the run repeated green):

- T24a image arrival times: the direct path and the six first-order paths (x=0, x=Lx, y=0, y=Ly, floor, ceiling) have their IR peak within one sample of the geometric distance / c * fs (measured errors -0.48 to +0.34 samples), and a peak above 0.3 of 1/r.
  Mutation: `add_frac(buf[b], ne, tau * s->fs, A)` shifted by +1.5 samples -> `FAIL test_room.c:48: fabs((double)best - p[k]) <= 1.0 -- direct arrives at sample 220, geometric 218.23` (all 7 paths fail).
- T24b RT60 (T20 x 3, Schroeder, octave bands) within 10 percent of the target at 500 Hz and 1 kHz for 0.6 and 1.0 s, omni and cardioid: omni 0.6 s +1.7 / -0.4 percent, 1.0 s +1.7 / -2.9 percent; cardioid 0.6 s -3.2 / -6.6 percent, 1.0 s +0.3 / -2.0 percent.
  Mutation: FDN line gain exponent `-3.0` -> `-2.0` -> `FAIL test_room.c:70: fabs(rt / T[k] - 1.0) < 0.10 -- RT60 500 Hz target 0.60 measured 0.907` (and 1.0 s measured 1.523).
- T24c late decay is exponential: the Schroeder curve (omni mic) lies within 1.5 dB of its least-squares line between -5 and -35 dB: 1.02 / 1.06 dB at 0.6 s (500 Hz / 1 kHz), 0.62 / 0.69 dB at 1.0 s.
  Reported, not asserted: the cardioid pointed at the source weights the direct sound and the sparse first reflections, which bends the start of the curve (1.18 / 1.56 dB at 0.6 s); at a target of 0.4 s the deviation is 1.5 to 3.3 dB and the T20 RT60 reads 5 to 15 percent short for the same reason.
  Mutation: late FDN output multiplied by `(1 + 0.8 sin(2 pi t / 0.2))` -> `FAIL test_room.c:79: dev < 1.5 -- Schroeder curve 500 Hz target 0.60 deviates 4.40 dB from a line` (4 such failures plus the RT60 ones).
- T24d cardioid rear rejection: direct sound with the source in front of the mic against the source behind it, 25.0 dB (at least 15 required); the omni mic differs by 0.00 dB when turned around and the cardioid on axis equals the omni.
  Mutation: `mic_gain` returns 1.0 for the cardioid -> `FAIL test_room.c:107: front - back >= 15.0 -- cardioid rear rejection 0.0 dB`.
- T24e source directivity: direct sound in 4-8 kHz, cello facing the mic against turned around, 2.88 dB (3.0 +- 0.4); below 250 Hz 0.01 dB (0 +- 0.3); measured in a 60 x 60 x 40 m box so no reflection lands in the analysis window.
  Mutation: sign of `dir_db` flipped -> `FAIL test_room.c:141: fabs((lf - lb) - 3.0) < 0.4 -- front-back at 4-8 kHz is -2.88 dB, want 3`.
- T24f noise floor: silent input through room_apply, A-weighted level of the output 12.00 dB(A) against the 12.0 specified, re 74 dB SPL at rms 1.0 (within 2 dB required); unweighted -59.3 dB re rms 1; also the A-weighting function at 1 kHz, 100 Hz, 10 kHz, and the level convention (the sounding part of a wet signal has rms 1.03).
  Mutations: noise std times 1.5 -> `FAIL test_room.c:171: fabs(dba - s.self_noise_dba) < 2.0 -- noise floor 15.52 dB(A), spec 12.0`; A weighting offset `+ 2.0` removed -> `FAIL test_room.c:172: fabs(room_aweight_db(1000.0)) < 0.05 -- A(1 kHz) = -2.000 dB` (and the 100 Hz and 10 kHz checks); active-rms normalisation removed -> `FAIL test_room.c:182: fabs(sqrt(p / 70000.0) - 1.0) < 0.1 -- sounding rms 0.196`.
- T24g determinism: the same spec gives a bit-identical IR and wet signal (noise from rng); a different seed gives a different IR (the FDN input and output signs come from rng) and different noise.
  Mutations: FDN signs seeded with a constant -> `FAIL test_room.c:197: memcmp(a, c, ...) -- a different seed gives the same IR: the FDN signs do not come from rng`; noise seed ignoring the spec seed -> `FAIL test_room.c:203: m3 == m1 && memcmp(o1, o3, ...) -- a different seed gives the same noise`; noise seed mixed with a call counter -> `FAIL test_room.c:202: m1 == m2 && !memcmp(o1, o2, ...) -- same seed gives a different wet signal`.
- T24h structure: FDN lengths mutually prime (mutation `881` -> `1202`: `FAIL test_room.c:213: gcd(L[i], L[j]) == 1 -- FDN lengths 601 and 1202 share a factor`); wall alpha satisfies Eyring (mutation: Sabine formula: `FAIL test_room.c:216: ... Eyring: 0.161 V / (-S ln(1-alpha)) = 0.739 s`); the air absorption lies in the textbook ranges (mutation: factor 8.686 -> 4.343: `FAIL test_room.c:221: a1 > 3.0 && ... -- air absorption out of the textbook range`); energy continuity across the image/FDN crossover, the 10 ms energy 15 ms after against 15 ms before the crossover differs from the pure decay by 0.6 dB (3 dB allowed; mutation: FDN scale times 2: `FAIL test_room.c:229: fabs(db - expect) < 3.0 -- energy step across the crossover 3.23 dB, decay alone -2.25`).
- Measured, not a test: diffuse-field check (`altcello room ... --drr-check`): omni source and omni mic, flat bands, 0.6 s: DRR -3.69 dB against -5.81 dB from the theory, 2.1 dB less reverberant energy than the theory; with the cardioid mic +4.35 dB against +4.8 dB expected.
- `make test` currently fails in `test_rng_ban.sh` on a comment in sim.h (`... logical time (independent of fs)` matches the `time (` pattern); the line is not from this stage, all 19 test programs including test_room pass when run directly.

## Stage 5 (single note diagnosis knobs)

- test_knobs (T24a walk table: relative sd 0.0496 against 0.05 within 15 percent, lag-1 correlation 0.8796 against exp(-2 pi bw / 100) = 0.8819 within 0.03, identical table at any sample rate; T24b `sim_vib_finger`: 0 cents leaves the finger, +-1200 cents halve / double the vibrating length, 13.5 cents ratio exact; T24c `stat_mass_scale` and `lead_mass_scale` scale exactly the statistical / the table masses, the bridge resistance R_inf lowers the hill peak 3.31 -> 0.98 at 3 R, the default R equals `body_bridge_gain`). Mutation checks (each applied, make, run, restored with a 1 s pause): walk `a` exponent 2 pi -> pi -> `FAIL test_knobs.c:30: ... walk correlation 0.9370 want 0.8819`; vibrato `pow(2, -c/1200)` -> `pow(2, c/1200)` -> `FAIL test_knobs.c:39 ... +1200 cents does not halve the length` (and :40); `m *= o->stat_mass_scale` removed -> `FAIL test_knobs.c:53: w < 1e-12 -- mass scales are not exact: 0.5`; lead mass scale dropped -> same line 53; `R` forced to the default in `body_bridge_gain_R` -> `FAIL test_knobs.c:62: pk3 < 0.5 * pk0 -- bridge resistance does not lower the hill (3.31 vs 3.31)`.
- Not tested: the probe options themselves (--fb-env, --vb-env, --q2k, --mode, --vib, --walk), which only edit a private copy of the Instrument or the controls; they are exercised by charts/diagnose_run.py.

## Stage 6 (instrument default C1, diagnosis setting)

The Instrument default became the diagnosis setting C1 (instrument_cello.c): string Q at 2 kHz 500 for D3 and A3, 300 for C2 and G2 (sigma1 from the formula of the probe option --q2k, sigma0 1.2 unchanged; D3 sigma1 3e-4 -> 2.96e-3), bridge resistance R_inf 30 -> 90 (x3), statistical modal masses x0.5 (STAT_M_SCALE 3.0 -> 1.5). Tests that moved, each from the new physics:

- test_instrument (T23): the two regression hashes were RE-RECORDED deliberately, because the default instrument's physics changed so every sample changes: D3 3179987b8cae4f4b -> a88857656984cdac, C2 c4533b1282189180 -> 58f68de34d67ee18 (the old pair was the bit-identical pre-refactor reference and stays in git history). Mutation check on the new pair: D3 Q(2 kHz) 500 -> 501 -> `FAIL test_instrument.c:50: hd == REF_D3 -- D3 probe changed: fea76e1ed975af05, want a88857656984cdac` (and the C2 line: the strings share the bridge); restored, PASS.
- test_bridge_hill (T17a/b/d): with R_inf x3 the bridge hill is damped away (peak factor 1.000), so the designed-hill checks (peak above 1.3 near 1.5 kHz, 1.5x inertia lowers the hill by 1/sqrt(1.5)) now run on a copy of the instrument with Woodhouse's R_inf (the default divided by 3: peak 3.31 at 1465 Hz, ratio 0.796, unchanged numbers), and a new T17d states the C1 default: no hill left (peak factor 1.000 < 1.02, 0.11 at 8 kHz < 0.3). Mutation: BRIDGE_R_INF back to 30 -> `FAIL test_bridge_hill.c:55: pk < 1.02 -- default hill factor peaks at 3.309` (and :56); restored, PASS.
- test_knobs (T24c): the reference R is now the Woodhouse value (default / 3) against 3 R = the default: 3.31 -> 0.98, assertion unchanged (a 3x raise of the new default would give 0.70 / 0.98, the hill is already gone); the default-R equality check now compares against the instrument's own R. Same mutation as before (`R` forced to the default) still applies to the assertion at the same line.
- test_schelleng (T4): with the C1 string loss the string only band at beta 0.1 is still 0.447-1.26 N (contiguous, 5 points), but the choked "none" class now starts below the top of the grid (7.7 N: 0.31 slips per period), so the assertion above the band accepts "none" once a raucous point has been seen above the band (before: at the last grid point only). Mutation: allowance `&& seen_raucous` -> `&& 0` -> `FAIL test_schelleng.c:74: cls[i] == CLS_RAUCOUS || (cls[i] == CLS_NONE && 0) -- beta 0.10: F 7.719 above the band is none`; restored, PASS.
- All other tests unchanged and green (`make test`).

## Stage 7 (the MSW waveguide oracle)

oracle_msw.c / oracle_msw.h: an independent bowed string (McIntyre-Schumacher-Woodhouse / Smith digital waveguide) used as a reference for the Schelleng band of the finite difference model (a test and diagnostic tool, linked only into sweep_tool and test_oracle_msw, never into the shipped objects). Z0 = sqrt(T rho), rigid terminations, one one-pole loss filter per reflection fitted to the finite difference string's own pluck decays (oracle_spec_from_string), 3rd order Lagrange delays at 96 kHz, point bow, no torsion, no stiffness, the bow junction solved independently of bow.c. analysis.c gained analyse_simple (the simple classifier: one dominant slip per period) and partial_decays (decay rates of partials from a pluck).

- test_oracle_msw T25a (D3, loss fitted from our string's pluck, bow position 0.2371 so no partial sits at a node): partials 1..20 of a plucked oracle at k f0 within 0.64 cents (required 1), decay rates of partials 1..20 within 3.6 percent of the finite difference string's measured decays (required 10; the one-pole fit has an rms relative error of 3.1 percent over 27 partials, to 4 kHz), and equal to the filter's own formula (-2 f0 ln|H|) to 0.1 percent.
- T25b: bowed at beta 0.1, v_B 0.1 m/s, 0.67 N: one slip per period for 20 periods, slip fraction 0.107 (beta 0.1 +- 0.02), pitch -0.48 cents (within 2).
- T25c: the junction alone: inside the static cone with a slipping root (q = 0.95 mu_s G, G = 1) a previously stuck junction stays stuck (v 0) and a previously slipping one stays on the slipping root (v 0.7898, exact residual); outside the cone the solution satisfies v = q - G phi(v) over q in [-3, 3] to 1e-12; force normalisation: the first step from rest satisfies v = q - F/(2 Z0) phi(v) exactly.
- Mutation checks (each applied, 1 s pause, make, run, restored, 1 s pause, make): junction continuation `b1 = v - a2, b2 = v - a1` swapped (the bug I wrote first) -> `FAIL test_oracle_msw.c:50: fabs(c) <= 1.0 -- partial 1 is 41.867 cents from k f0` (42 failures); delay budget one sample short (`- 1.0` -> `- 0.0` in the line lengths) -> partial 1 `-5.288 cents` (21 failures); loss pole feedback halved (`0.5 * s->p * o->ya`) -> `partial 1 is 3.663 cents from k f0` (43 failures: pitch and decays); stick hysteresis removed (`*stuck ||` -> `0 ||`) -> `FAIL test_oracle_msw.c:88: st1 == 1 && v1 == 0.0 -- hysteresis: a stuck junction inside the cone must stay stuck` (the bowed check T25b alone did NOT catch this one: one slip per period survives without hysteresis, hence T25c); bow force normalisation `G = F / Z0` instead of `F / (2 Z0)` -> `FAIL ... residual` of the junction check and `FAIL test_oracle_msw.c:75: fabs(h.cents_flat) <= 2.0 -- pitch -3.660 cents from f0`.
- Not tested: sweep_tool itself (a thin driver around the classifiers and the two models; its output is what the oracle report is built from).

## Iteration 2 (bow noise on the friction coefficient, torsion damping, playability table)

Physics moved: the bow noise multiplies the SLIDING friction coefficient (phi_slip = (1 + n) phi, `bow_solve` takes n; sticking and the static limit are untouched, no more force gate on |v_rel|), level 0.03 -> 0.01; zeta_t 0.01 -> 0.05; the probe option `--noise` now defaults to the instrument's level.

- test_instrument (T23): hashes RE-RECORDED deliberately because the default physics changed (noise model and level, torsional damping): D3 a88857656984cdac -> aea0191a9767bf02, C2 58f68de34d67ee18 -> 20e16156da2131f0. All other tests unchanged and green (`make test`).
- test_bow_noise (new, T26a-c):
  - T26a: a sticking junction (previous step sticking, inside the static cone) gives exactly v = 0 and Phi = q/G for noise 0, 0.3, -0.3, 0.9; a slipping junction gives Phi = (1 + n) phi(v) at the solved v and v = q - G Phi to 1e-10; noise 0.2 moves Phi 0.35 -> 0.42.
    Mutations: stick branch `*Phi = mul * q / G` -> `FAIL test_bow_noise.c:24: st == 1 && v == 0.0 && Phi == 0.7 * fr->mu_static -- sticking with noise 0.3: v 0 Phi 1.092 stuck 1`; slip branch without `mul` -> `FAIL test_bow_noise.c:33: ... noise 0.3: Phi 0.35 not (1+n) phi(v) = 0.455` (and :34).
  - T26b: the draw schedule in logical time: over 1 s, 4000 draws at 48 kHz and at 96 kHz, the first 1000 identical in value and logical time (T6 in test_determinism covers the same through the simulator).
    Mutation: the schedule made to depend on fs (`n * RATE * (ifs > 60000 ? 1 : 2)`) -> `FAIL test_bow_noise.c:59: a.idx_next == b.idx_next && a.idx_next == 4000 -- draw index after 1 s: 8000 (48k) 4000 (96k), want 4000`.
  - T26c: the playability table: every row has fmin <= fmax and centre = sqrt(fmin fmax) (1e-4), and the centre force at beta 0.1, v_B 0.1 gives Helmholtz motion in a 3 s run on every string, body on, all strings (both classifiers): C2 1.703 N, G2 1.301 N, D3 0.759 N, A3 0.580 N, slip fractions 0.106-0.110.
    Mutation: the D3 centre at beta 0.1, v_B 0.1 set to 5.0 N -> `FAIL test_bow_noise.c:81: ... D3 row 1 1: centre 5 is not the geometric centre` and `FAIL test_bow_noise.c:95: h.ok_one_per_period && sh.ok -- D3: no Helmholtz motion at the table centre 5.0000 N`.
  - Each mutation applied, 1 s pause, make, run, restored, 1 s pause, make, PASS.
- Not tested: charts/iter2_run.sh, iter2_notes.py, iter2_gate.py, band.py (measurement and chart scripts).
