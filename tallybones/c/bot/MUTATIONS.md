# tb_bot_test mutation checks

Each row is one break in `tb_bot.c`, run with `build/tb_bot_test 200000`, then restored (the file compared byte-for-byte with a saved copy afterwards).
Every row went red (exit 1); the column says which assertions failed.
The group that owns the break is listed first.

| # | Break | Red assertions |
|---|---|---|
| M1 | drop raw outcome 0 from every reroll distribution | G1 entry count, counts sum to 6^r, sums to 1, raw enumeration; G2 five/four/three alike; G3 every hand-computed value; G4; G5 first roll; G6 |
| M2 | a rerolled die never shows a six (`x % 5`) | G1 entry count, raw enumeration; G2 all six category counts; G3 every value |
| M3 | the policy scores greedily (highest points now) instead of the argmax over EV | G4 move reaches the brute-force best; G7 simulated mean (-338 SE) |
| M4 | the induction drops the future term (greedy for both) | G4 score-now, ev, move; G6 both; G7 |
| M5 | crossing 63 earns no bonus | G3 Sixes at 33, Ones at 60; G4; G6 both; G7 |
| M6 | the best keep ignores the smaller keeps (no lattice max) | G3 every value; G4 ev, move, table; G5 first roll; G6; G7 |
| M7 | keeping by position is inverted (`!(mask >> i & 1)`) | G1 sums, raw enumeration; G2 all six; G4; G5 first roll; G7 (-2045 SE) |
| M8 | the induction runs in decreasing order of remaining categories | G5 first roll; G6 both; G7 |
| M9 | the first roll is valued with one reroll left, not two | G3 every value; G5 first roll; G6 both; G7 |
| M10 | the simulated total leaves out the bonus | G7 simulated mean (-323 SE) only |
| M11 | a worker seeds its games by its own index | G7 same tallies for any thread count only |
| M12 | stopping is sent as KEEP(31) | G4 illegal kind; G8 moves the kernel refused |
| M13 | every category adds to the numbers total | G5 independence of the upper total; G4; G6 both; G7 |
| M14 | the bonus only for landing on exactly 63 | G5 monotone in the upper total; G3 Ones at 60; G4; G6 both; G7 |
