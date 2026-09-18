// Rust port of c/src/cordite_sim.c's exact bitboard endgame solver —
// sim_solve_rec + sim_gen_moves + the transposition table at the SHIPPED
// configuration (TT12, 2-way, packed 8-byte entries) — octogen's measured
// hot path (36% + 20% of instructions at 2 players). Safe Rust throughout;
// the TT entry is a hand-packed u64 (Rust has no bitfields), and the C
// solver's "clone everything except the dead deck[] tail" per-node memcpy
// becomes `child.c = s.c` on the split SimCore.
//
// Deterministic: the checksum covers value, aborted flag, and nodes expanded,
// so this port must search the identical tree as the C solver.
use rustpoc::sim::*;
use rustpoc::*;

const SOLVE_BUDGET: i64 = 200_000;
const MAX_ENDGAME_CARDS: u32 = 16;

const CD_TT_BITS: u32 = 12;
const CD_TT_SIZE: usize = 1 << CD_TT_BITS;
const CD_TT_MASK: u64 = (CD_TT_SIZE as u64) - 1;
const CD_SIM_SOLVE_MAX_DEPTH: usize = 48;
const CD_SOLVE_MOVES_CAP: usize = 96;
const CD_SIM_SOLVE_REC_SLOTS: usize = 100;

// CD_TT_PACK8 entry as a hand-packed u64:
//   bits 0..39  key tag   (fingerprint >> CD_TT_BITS, low 40 bits)
//   bits 40..51 value     (signed 12-bit, |v| <= 1000)
//   bits 52..57 depth     (<= 48)
//   bit  58     valid
#[inline] fn tt_tag(key: u64) -> u64 { (key >> CD_TT_BITS) & 0xFF_FFFF_FFFF }
#[inline] fn e_tag(e: u64) -> u64 { e & 0xFF_FFFF_FFFF }
#[inline] fn e_value(e: u64) -> i32 { (((e >> 40) & 0xFFF) as i32) << 20 >> 20 }
#[inline] fn e_depth(e: u64) -> i32 { ((e >> 52) & 0x3F) as i32 }
#[inline] fn e_valid(e: u64) -> bool { e >> 58 & 1 != 0 }
#[inline] fn e_pack(tag: u64, value: i32, depth: i32) -> u64 {
    tag | (((value as u32 as u64) & 0xFFF) << 40) | ((depth as u64) << 52) | (1u64 << 58)
}

// Mirrors sim_fingerprint (cordite_sim.c:980).
fn sim_fingerprint(s: &SimState, a: usize, b: usize) -> u64 {
    let mut h = s.c.hand[a].wrapping_mul(0x9E3779B97F4A7C15);
    h ^= s.c.hand[b].wrapping_add(0x7F4A7C15).wrapping_mul(0xC2B2AE3D27D4EB4F);
    let mut t: u64 = 0;
    for i in 0..s.c.num_battles as usize {
        let mut cell = s.c.atk[i] as u64;
        if s.c.covered_mask & (1u64 << i) != 0 {
            cell |= ((s.c.def[i] as u64) << 8) | (1u64 << 16);
        }
        t = t.wrapping_mul(1099511628211).wrapping_add(cell + 1);
    }
    h ^= t.wrapping_mul(0xFF51AFD7ED558CCD);
    h ^= (s.c.defender as u64) << 1;
    h ^= (s.c.first_attacker as u64) << 9;
    h ^= ((s.c.good_mask & 0xff) as u64) << 17;
    h ^= (s.c.num_battles as u64) << 25;
    h ^= 0x94D049BB133111EB;
    h ^= h >> 31;
    if h == 0 { 1 } else { h }
}

#[derive(Clone, Copy)]
struct SolMove {
    typ: MvType,
    n: u8,
    cards: [u8; 8],
    battle: [u8; 8],
}
const SOL_ZERO: SolMove = SolMove { typ: MvType::Good, n: 0, cards: [0; 8], battle: [0; 8] };

// ---- move enumeration (mirrors sim_gen_* in cordite_sim.c:1136-1335) ----

fn enum_subsets(group: u64, cap_lo: i32, cap_hi: i32, buf: &mut [SolMove], n: &mut usize,
                max_n: usize, typ: MvType) {
    let mut ids = [0i32; 8];
    let mut gn = 0usize;
    let mut g = group;
    while g != 0 {
        ids[gn] = g.trailing_zeros() as i32;
        gn += 1;
        g &= g - 1;
    }
    let total = 1i32 << gn;
    let mut mask = 1i32;
    while mask < total && *n < max_n {
        let k = mask.count_ones() as i32;
        if k < cap_lo || k > cap_hi { mask += 1; continue; }
        let m = &mut buf[*n];
        *n += 1;
        m.typ = typ;
        m.n = k as u8;
        let mut c = 0usize;
        for i in 0..gn {
            if mask & (1 << i) != 0 {
                m.cards[c] = ids[i] as u8;
                c += 1;
            }
        }
        mask += 1;
    }
}

fn sim_gen_first_attack(s: &SimState, p: usize, buf: &mut [SolMove], max_n: usize,
                        masks: &Masks) -> usize {
    let mut n = 0usize;
    let defcap = sim_hand_count(s, s.c.defender as usize) as i32;
    let h = s.c.hand[p];
    let mut v = 1;
    while v <= 13 && n < max_n {
        let group = h & masks.value[v as usize];
        if group != 0 {
            enum_subsets(group, 1, defcap, buf, &mut n, max_n, MvType::Attack);
        }
        v += 1;
    }
    n
}

fn sim_gen_regular_attack(s: &SimState, p: usize, buf: &mut [SolMove], max_n: usize) -> usize {
    let tv = sim_table_value_mask(s);
    let h = s.c.hand[p] & tv;
    if h == 0 { return 0; }
    let uncovered = sim_count_uncovered(s) as i32;
    let mut defcap = sim_hand_count(s, s.c.defender as usize) as i32 - uncovered;
    if defcap <= 0 { return 0; }
    let mut n = 0usize;
    let mut ids = [0i32; 64]; // C uses int ids[16], unchecked; 64 is the safe superset
    let mut hn = 0usize;
    let mut hh = h;
    while hh != 0 {
        ids[hn] = hh.trailing_zeros() as i32;
        hn += 1;
        hh &= hh - 1;
    }
    if defcap > hn as i32 { defcap = hn as i32; }
    let total = 1i32 << hn;
    let mut mask = 1i32;
    while mask < total && n < max_n {
        let k = mask.count_ones() as i32;
        if k > defcap { mask += 1; continue; }
        let m = &mut buf[n];
        n += 1;
        m.typ = MvType::Attack;
        m.n = k as u8;
        let mut c = 0usize;
        for i in 0..hn {
            if mask & (1 << i) != 0 {
                m.cards[c] = ids[i] as u8;
                c += 1;
            }
        }
        mask += 1;
    }
    n
}

fn sim_gen_pass(s: &SimState, p: usize, buf: &mut [SolMove], max_n: usize,
                masks: &Masks) -> usize {
    if s.c.num_battles == 0 { return 0; }
    if s.c.covered_mask != 0 { return 0; }
    let v0 = id_value(s.c.atk[0] as u32);
    for i in 1..s.c.num_battles as usize {
        if id_value(s.c.atk[i] as u32) != v0 { return 0; }
    }
    let matching = s.c.hand[p] & masks.value[v0 as usize];
    if matching == 0 { return 0; }
    let next = sim_next_player(s, s.c.defender as i32);
    let kmax = sim_hand_count(s, next as usize) as i32 - s.c.num_battles as i32;
    if kmax < 1 { return 0; }
    let mut n = 0usize;
    enum_subsets(matching, 1, kmax, buf, &mut n, max_n, MvType::Pass);
    n
}

fn cover_assign(s: &SimState, power: u32, bidx: &[i32], pn: usize, depth: usize,
                chosen_card: &mut [u8; 8], used: u64, buf: &mut [SolMove], n: &mut usize,
                max_n: usize) {
    if depth == pn {
        if *n >= max_n { return; }
        let m = &mut buf[*n];
        *n += 1;
        m.typ = MvType::Cover;
        m.n = pn as u8;
        for i in 0..pn {
            m.cards[i] = chosen_card[i];
            m.battle[i] = bidx[i] as u8;
        }
        return;
    }
    let atk = s.c.atk[bidx[depth] as usize] as u32;
    let avail = s.c.hand[s.c.defender as usize] & !used;
    let mut a = avail;
    while a != 0 && *n < max_n {
        let id = a.trailing_zeros();
        a &= a - 1;
        if !id_can_cover(atk, id, power) { continue; }
        chosen_card[depth] = id as u8;
        cover_assign(s, power, bidx, pn, depth + 1, chosen_card, used | (1u64 << id), buf, n, max_n);
    }
}

fn sim_gen_cover(s: &SimState, power: u32, buf: &mut [SolMove], max_n: usize) -> usize {
    let mut ubat = [0i32; SIM_MAX_BATTLES];
    let mut nub = 0usize;
    for i in 0..s.c.num_battles as usize {
        if s.c.covered_mask & (1u64 << i) == 0 {
            ubat[nub] = i as i32;
            nub += 1;
        }
    }
    if nub == 0 { return 0; }
    let mut n = 0usize;
    let mut bidx = [0i32; SIM_MAX_BATTLES];
    let mut chosen = [0u8; 8];
    let mut k = 1usize;
    while k <= nub && n < max_n {
        let mut comb = [0i32; SIM_MAX_BATTLES];
        for i in 0..k { comb[i] = i as i32; }
        while n < max_n {
            for i in 0..k { bidx[i] = ubat[comb[i] as usize]; }
            cover_assign(s, power, &bidx, k, 0, &mut chosen, 0, buf, &mut n, max_n);
            let mut i = k as i32 - 1;
            while i >= 0 && comb[i as usize] == (nub - k) as i32 + i { i -= 1; }
            if i < 0 { break; }
            comb[i as usize] += 1;
            for j in i as usize + 1..k { comb[j] = comb[j - 1] + 1; }
        }
        k += 1;
    }
    n
}

fn sim_apply_sol(s: &mut SimState, p: usize, m: &SolMove, masks: &Masks, rng: &mut Lcg) {
    match m.typ {
        MvType::Attack => sim_apply_attack(s, p, &m.cards, m.n as usize, masks),
        MvType::Pass => sim_apply_pass(s, p, &m.cards, m.n as usize, masks),
        MvType::Pickup => sim_apply_pickup(s, p, rng),
        MvType::Good => sim_apply_good(s, p, rng),
        MvType::Cover => {
            let mut bi = [0usize; 8];
            for i in 0..m.n as usize { bi[i] = m.battle[i] as usize; }
            sim_apply_cover(s, p, &m.cards, &bi, m.n as usize, masks, rng);
        }
    }
}

fn sim_gen_moves(s: &SimState, actor: usize, buf: &mut [SolMove], max_n: usize,
                 masks: &Masks) -> usize {
    let power = s.c.power_suit as u32;
    let first_attack = s.c.num_battles == 0;
    let is_def = actor as i8 == s.c.defender;
    if first_attack && actor as i8 == s.c.first_attacker {
        sim_gen_first_attack(s, actor, buf, max_n, masks)
    } else if is_def && s.c.num_battles > 0 {
        let mut n = sim_gen_cover(s, power, buf, max_n);
        if !sim_all_covered(s) && n < max_n {
            buf[n].typ = MvType::Pickup;
            buf[n].n = 0;
            n += 1;
        }
        n += sim_gen_pass(s, actor, &mut buf[n..], max_n - n, masks);
        n
    } else if !is_def && s.c.num_battles > 0 {
        if s.c.good_mask & (1u32 << actor) != 0 { return 0; }
        let mut n = sim_gen_regular_attack(s, actor, buf, max_n);
        if n < max_n {
            buf[n].typ = MvType::Good;
            buf[n].n = 0;
            n += 1;
        }
        n
    } else {
        0
    }
}

struct Solver<'a> {
    budget: i64,
    aborted: bool,
    me: usize,
    tt: Vec<u64>,
    masks: &'a Masks,
}

fn sim_solve_rec(sv: &mut Solver, s: &SimState, kids: &mut [SimState],
                 mvs: &mut [[SolMove; CD_SIM_SOLVE_REC_SLOTS]],
                 mut alpha: i32, mut beta: i32, depth: i32) -> i32 {
    let loser = sim_done(s);
    if loser >= 0 {
        return if loser == sv.me as i32 { -(1000 - depth) } else { 1000 - depth };
    }
    if sim_in_count(s) == 0 { return 0; }
    if depth >= CD_SIM_SOLVE_MAX_DEPTH as i32 { sv.aborted = true; return 0; }
    sv.budget -= 1;
    if sv.budget <= 0 { sv.aborted = true; return 0; }

    let mut actor: i32 = -1;
    if sim_should_act(s, s.c.defender as usize) {
        actor = s.c.defender as i32;
    } else {
        for i in 0..s.c.num_players as usize {
            if sim_should_act(s, i) { actor = i as i32; break; }
        }
    }
    if actor < 0 { return 0; }
    let actor = actor as usize;

    let mut a: i32 = -1;
    let mut b: i32 = -1;
    for i in 0..s.c.num_players as usize {
        if s.c.status_p[i] == PLAYER_STATUS_IN {
            if a < 0 { a = i as i32; } else { b = i as i32; }
        }
    }

    let mut key: u64 = 0;
    let mut bkt_base: usize = 0;
    if b >= 0 {
        key = sim_fingerprint(s, a as usize, b as usize);
        bkt_base = (key & CD_TT_MASK & !1u64) as usize;
        // 2-way probe: hit on either half of the bucket.
        let e0 = sv.tt[bkt_base];
        let e1 = sv.tt[bkt_base + 1];
        let hit = if e_valid(e0) && e_tag(e0) == tt_tag(key) { Some(e0) }
                  else if e_valid(e1) && e_tag(e1) == tt_tag(key) { Some(e1) }
                  else { None };
        if let Some(e) = hit {
            let mut v = e_value(e);
            if v > 0 { v = v - (1000 - e_depth(e)) + (1000 - depth); }
            else if v < 0 { v = v + (1000 - e_depth(e)) - (1000 - depth); }
            return v;
        }
    }

    let (mybuf, mvs_rest) = mvs.split_first_mut().unwrap();
    let nm = sim_gen_moves(s, actor, mybuf, CD_SIM_SOLVE_REC_SLOTS, sv.masks);
    if nm == 0 { return 0; }
    if nm > CD_SOLVE_MOVES_CAP { sv.aborted = true; return 0; }

    let alpha0 = alpha;
    let beta0 = beta;
    let maximizing = actor == sv.me;
    let mut best = if maximizing { -2000 } else { 2000 };
    let mut applied = false;
    let (child, kids_rest) = kids.split_first_mut().unwrap();
    for i in 0..nm {
        // C: memcpy(child, s, offsetof(SimState, deck)) — skip the dead deck[]
        // tail. Here: copy the SimCore only; child.deck keeps stale bytes that
        // are never read (deck_n == 0 on every solver path).
        child.c = s.c;
        // Dummy RNG: the apply handlers thread one through, but a deck-empty
        // solve never draws (mirrors the C solver, which shares the engine LCG
        // and likewise never consumes it here).
        let mut rng = Lcg(1);
        sim_apply_sol(child, actor, &mybuf[i], sv.masks, &mut rng);
        applied = true;
        let v = sim_solve_rec(sv, &*child, kids_rest, mvs_rest, alpha, beta, depth + 1);
        if sv.aborted { return 0; }
        if maximizing {
            if v > best { best = v; }
            if best > alpha { alpha = best; }
        } else {
            if v < best { best = v; }
            if best < beta { beta = best; }
        }
        if alpha >= beta { break; }
    }
    if !applied || best == -2000 || best == 2000 { return 0; }

    // Store EXACT values only (fail-soft results outside the original window
    // are bounds, not values). 2-way victim: same-key, else empty, else the
    // deeper-ply entry. Mirrors cordite_sim.c:1630-1684.
    if b >= 0 && key != 0 && best > alpha0 && best < beta0 {
        let e0 = sv.tt[bkt_base];
        let e1 = sv.tt[bkt_base + 1];
        let slot = if e_valid(e0) && e_tag(e0) == tt_tag(key) { bkt_base }
                   else if e_valid(e1) && e_tag(e1) == tt_tag(key) { bkt_base + 1 }
                   else if !e_valid(e0) { bkt_base }
                   else if !e_valid(e1) { bkt_base + 1 }
                   else if e_depth(e0) >= e_depth(e1) { bkt_base }
                   else { bkt_base + 1 };
        sv.tt[slot] = e_pack(tt_tag(key), best, depth);
    }
    best
}

fn cd_sim_solve_d(sv: &mut Solver, s: &SimState, kids: &mut [SimState],
                  mvs: &mut [[SolMove; CD_SIM_SOLVE_REC_SLOTS]], alpha: i32, beta: i32,
                  budget: &mut i64, depth0: i32) -> (i32, bool) {
    sv.budget = *budget;
    sv.aborted = false;
    let v = sim_solve_rec(sv, s, kids, mvs, alpha, beta, depth0);
    *budget = sv.budget;
    (v, sv.aborted)
}

fn is_endgame(s: &PocState) -> bool {
    if s.status != GAME_STATUS_PLAYING { return false; }
    if s.deck_count != 0 || s.has_flipped != 0 { return false; }
    let mut in_count = 0;
    let mut cards: u32 = 0;
    for p in 0..s.num_players as usize {
        if s.pstatus[p] == PLAYER_STATUS_IN {
            in_count += 1;
            cards += s.hand_count[p] as u32;
        }
    }
    if in_count != 2 { return false; }
    cards += s.num_battles as u32;
    cards <= MAX_ENDGAME_CARDS
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let path = args.get(1).map(|s| s.as_str()).unwrap_or("states.bin");
    let reps: usize = args.get(2).and_then(|s| s.parse().ok()).unwrap_or(20);
    let states = load_states(path);
    let masks = build_masks();

    let mut tmpl: Vec<SimState> = Vec::new();
    let mut me: Vec<usize> = Vec::new();
    for s in &states {
        if !is_endgame(s) { continue; }
        tmpl.push(state_to_sim(s, &masks));
        me.push(s.actor as usize);
    }
    let n = tmpl.len();
    assert!(n > 0, "no endgame states in dump");

    let mut sv = Solver {
        budget: 0,
        aborted: false,
        me: 0,
        tt: vec![0u64; CD_TT_SIZE],
        masks: &masks,
    };
    // Recursion scratch, allocated once — mirrors the C solver's depth-indexed
    // _Thread_local BSS arrays (sim_rec_moves / sim_rec_child, M7a).
    let mut kids = vec![SimState::zeroed(); CD_SIM_SOLVE_MAX_DEPTH];
    let mut mvs = vec![[SOL_ZERO; CD_SIM_SOLVE_REC_SLOTS]; CD_SIM_SOLVE_MAX_DEPTH];

    let mut sum = FNV_INIT;
    let mut total_nodes: i64 = 0;
    let mut best_t = f64::INFINITY;
    let mut t_total = 0.0;
    for _ in 0..reps {
        let mut rep_sum = FNV_INIT;
        let mut rep_nodes: i64 = 0;
        let t0 = std::time::Instant::now();
        for i in 0..n {
            let s = tmpl[i];
            sv.tt.fill(0); // cd_sim_solve_reset
            sv.me = me[i];
            let mut budget = SOLVE_BUDGET;
            let (v, aborted) = cd_sim_solve_d(&mut sv, &s, &mut kids, &mut mvs,
                                              -1000, 1000, &mut budget, 0);
            rep_nodes += SOLVE_BUDGET - budget;
            rep_sum = fnv1a_u32(rep_sum, v as u32);
            rep_sum = fnv1a_u32(rep_sum, aborted as u32);
            rep_sum = fnv1a_u32(rep_sum, (SOLVE_BUDGET - budget) as u32);
        }
        let dt = t0.elapsed().as_secs_f64();
        t_total += dt;
        if dt < best_t { best_t = dt; }
        sum = rep_sum;
        total_nodes = rep_nodes;
    }

    println!("bench=solve impl=rust states={n} reps={reps} nodes_per_pass={total_nodes} checksum={sum:016x}");
    println!("bench=solve impl=rust best_ms={:.3} mean_ms={:.3} us_per_solve={:.2} mnodes_per_s={:.2} peak_rss_kb={}",
             best_t * 1e3, t_total / reps as f64 * 1e3,
             best_t * 1e6 / n as f64, total_nodes as f64 / best_t / 1e6, peak_rss_kb());
}
