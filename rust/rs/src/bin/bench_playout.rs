// Rust port of c/src/cordite_sim.c's rollout core (see rs/src/sim.rs),
// benchmarked over the same dumped states as the C harness. The engine LCG is
// ported too, so playout results must be bit-identical with the C harness.
use rustpoc::sim::*;
use rustpoc::*;

fn elim_bytes(e: &[i8; 8]) -> [u8; 8] {
    let mut out = [0u8; 8];
    for i in 0..8 { out[i] = e[i] as u8; }
    out
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let path = args.get(1).map(|s| s.as_str()).unwrap_or("states.bin");
    let reps: usize = args.get(2).and_then(|s| s.parse().ok()).unwrap_or(40);
    let max_states: usize = args.get(3).and_then(|s| s.parse().ok()).unwrap_or(600);
    let states = load_states(path);
    let masks = build_masks();
    let n = states.len().min(max_states);
    let tmpl: Vec<SimState> = states[..n].iter().map(|s| state_to_sim(s, &masks)).collect();
    let actors: Vec<usize> = states[..n].iter().map(|s| s.actor as usize).collect();

    let mut rng = Lcg(1237);
    let mut sum = FNV_INIT;
    let mut best = f64::INFINITY;
    let mut t_total = 0.0;
    for r in 0..reps {
        let mut rep_sum = FNV_INIT;
        let t0 = std::time::Instant::now();
        for i in 0..n {
            let mut s = tmpl[i];
            let seed = 0x9E3779B9u32 ^ (i as u32).wrapping_mul(2654435761) ^ (r as u32).wrapping_mul(40503);
            rng.set_seed(seed);
            let fp = cd_sim_playout(&mut s, actors[i], 2000, false, &masks, &mut rng);
            rep_sum = fnv1a_u32(rep_sum, fp as u32);
            rep_sum = fnv1a_u32(rep_sum, s.c.in_mask);
            rep_sum = fnv1a_u32(rep_sum, s.c.out_mask);
            rep_sum = fnv1a_u32(rep_sum, s.c.num_eliminated as u32);
            rep_sum = fnv1a(rep_sum, &elim_bytes(&s.c.elim_order));
            rep_sum = fnv1a_u32(rep_sum, s.c.discard_pile_length as u32);
        }
        let dt = t0.elapsed().as_secs_f64();
        t_total += dt;
        if dt < best { best = dt; }
        sum ^= rep_sum;
    }

    println!("bench=playout impl=rust states={n} reps={reps} checksum={sum:016x}");
    println!("bench=playout impl=rust best_ms={:.3} mean_ms={:.3} us_per_playout={:.3} peak_rss_kb={} sizeof_SimState={}",
             best * 1e3, t_total / reps as f64 * 1e3,
             best * 1e6 / n as f64, peak_rss_kb(), std::mem::size_of::<SimState>());
}
