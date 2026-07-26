// Rust port of c/src/cordite_sim.c's rollout core: SimState bitboards, the
// handwritten rollout policy, and cd_sim_playout — the Monte-Carlo hot loop
// of the shipped cordite/octogen bots. Safe Rust, faithful line-for-line.
// The engine LCG is ported too, so playout results must be bit-identical
// with the C harness (same seeds, same states).
use rustpoc::*;

const MAX_PLAYERS: usize = 8;
const SIM_MAX_BATTLES: usize = 64;
const MAX_DECK: usize = 64;
const CARDS_PER_PLAYER: u32 = 6;
const GAME_STATUS_PLAYING: u8 = 1;
const PLAYER_STATUS_IN: u8 = 2;
const PLAYER_STATUS_OUT: u8 = 3;

#[inline] fn id_suit(id: u32) -> u32 { id / 13 }
#[inline] fn id_value(id: u32) -> u32 { id % 13 + 1 }

struct Masks {
    value: [u64; 14],
    suit: [u64; 4],
    higher: [u64; 52],
}

fn build_masks() -> Masks {
    let mut m = Masks { value: [0; 14], suit: [0; 4], higher: [0; 52] };
    for s in 0..4u32 {
        for v in 1..=13u32 {
            let id = s * 13 + (v - 1);
            m.suit[s as usize] |= 1u64 << id;
            m.value[v as usize] |= 1u64 << id;
        }
    }
    for s in 0..4u32 {
        for v in 1..=13u32 {
            let id = (s * 13 + (v - 1)) as usize;
            for w in v + 1..=13 {
                m.higher[id] |= 1u64 << (s * 13 + (w - 1));
            }
        }
    }
    m
}

#[inline] fn id_score(id: u32, power: u32) -> i32 {
    (id_value(id) + if id_suit(id) == power { 1000 } else { 0 }) as i32
}

#[derive(Clone, Copy)]
struct SimState {
    num_players: u8,
    power_suit: u8,
    defender: i8,
    first_attacker: i8,
    status: u8,
    num_battles: u8,
    discard_pile_length: i16,
    has_flipped: u8,
    flipped_id: u8,
    good_mask: u32,
    num_eliminated: u8,
    hand: [u64; MAX_PLAYERS],
    status_p: [u8; MAX_PLAYERS],
    in_mask: u32,
    out_mask: u32,
    elim_order: [i8; MAX_PLAYERS],
    atk: [u8; SIM_MAX_BATTLES],
    def: [u8; SIM_MAX_BATTLES],
    covered_mask: u64,
    table_vmask: u64,
    deck_n: i16,
    deck: [u8; MAX_DECK],
}

#[derive(Clone, Copy, PartialEq)]
enum MvType { Attack, Cover, Pass, Pickup, Good }

struct SimMove {
    typ: MvType,
    n: usize,
    cards: [u8; SIM_MAX_BATTLES],
    battle: [usize; SIM_MAX_BATTLES],
}

impl SimMove {
    fn new() -> SimMove {
        SimMove { typ: MvType::Good, n: 0, cards: [0; SIM_MAX_BATTLES], battle: [0; SIM_MAX_BATTLES] }
    }
}

#[inline] fn sim_hand_count(s: &SimState, p: usize) -> u32 { s.hand[p].count_ones() }
#[inline] fn sim_in_count(s: &SimState) -> u32 { s.in_mask.count_ones() }

#[inline]
fn sim_next_player(s: &SimState, cur: i32) -> i32 {
    if s.in_mask.count_ones() <= 1 { return cur; }
    let notout = !s.out_mask & ((1u32 << s.num_players) - 1);
    let hi = notout & !((2u32 << cur) - 1);
    (if hi != 0 { hi } else { notout }).trailing_zeros() as i32
}

#[inline]
fn sim_done(s: &SimState) -> i32 {
    if s.in_mask.count_ones() != 1 { return -1; }
    if s.out_mask.count_ones() != s.num_players as u32 - 1 { return -1; }
    s.in_mask.trailing_zeros() as i32
}

#[inline] fn sim_no_cards_left(s: &SimState) -> bool { s.deck_n == 0 && s.has_flipped == 0 }
#[inline] fn sim_count_uncovered(s: &SimState) -> u32 { s.num_battles as u32 - s.covered_mask.count_ones() }
#[inline] fn sim_all_covered(s: &SimState) -> bool {
    s.num_battles > 0 && s.covered_mask.count_ones() == s.num_battles as u32
}
#[inline] fn sim_table_value_mask(s: &SimState) -> u64 { s.table_vmask }

// sim_draw without the forced-draw queue (empty in this workload, exactly as
// in the C harness).
fn sim_draw(s: &mut SimState, rng: &mut Lcg) -> Option<u32> {
    if s.deck_n == 0 {
        if s.has_flipped == 0 { return None; }
        let out = s.flipped_id as u32;
        s.has_flipped = 0;
        return Some(out);
    }
    let mut idx = (rng.random() * s.deck_n as f64) as i32;
    if idx < 0 { idx = 0; }
    if idx >= s.deck_n as i32 { idx = s.deck_n as i32 - 1; }
    let out = s.deck[idx as usize] as u32;
    for i in idx as usize + 1..s.deck_n as usize {
        s.deck[i - 1] = s.deck[i];
    }
    s.deck_n -= 1;
    Some(out)
}

fn sim_eliminate(s: &mut SimState, p: usize) {
    s.status_p[p] = PLAYER_STATUS_OUT;
    s.in_mask &= !(1u32 << p);
    s.out_mask |= 1u32 << p;
    s.elim_order[s.num_eliminated as usize] = p as i8;
    s.num_eliminated += 1;
}

fn sim_refill(s: &mut SimState, rng: &mut Lcg) {
    if sim_no_cards_left(s) {
        for i in 0..s.num_players as usize {
            if s.in_mask >> i & 1 != 0 && sim_hand_count(s, i) == 0 {
                sim_eliminate(s, i);
            }
        }
        return;
    }
    let defender = s.defender as usize;
    if sim_hand_count(s, defender) == 0 {
        while sim_hand_count(s, defender) < CARDS_PER_PLAYER {
            let Some(c) = sim_draw(s, rng) else { break };
            s.hand[defender] |= 1u64 << c;
        }
    }
    let mut p_idx = s.first_attacker as i32;
    let mut visited = 0u32;
    loop {
        if visited & (1 << p_idx) != 0 { break; }
        visited |= 1 << p_idx;
        while sim_hand_count(s, p_idx as usize) < CARDS_PER_PLAYER {
            let Some(c) = sim_draw(s, rng) else { break };
            s.hand[p_idx as usize] |= 1u64 << c;
        }
        if sim_hand_count(s, p_idx as usize) == 0 && s.in_mask >> p_idx & 1 != 0 {
            sim_eliminate(s, p_idx as usize);
        }
        p_idx = sim_next_player(s, p_idx);
        if p_idx == s.first_attacker as i32 { break; }
    }
}

fn sim_apply_attack(s: &mut SimState, p_idx: usize, ids: &[u8], n: usize, masks: &Masks) {
    for i in 0..n {
        s.hand[p_idx] &= !(1u64 << ids[i]);
        let b = s.num_battles as usize;
        s.num_battles += 1;
        s.atk[b] = ids[i];
        s.covered_mask &= !(1u64 << b);
        s.table_vmask |= masks.value[id_value(ids[i] as u32) as usize];
    }
    s.good_mask = 0;
    if sim_hand_count(s, p_idx) == 0 && sim_no_cards_left(s) {
        sim_eliminate(s, p_idx);
    }
}

fn sim_apply_cover(s: &mut SimState, p_idx: usize, covers: &[u8], battle_idx: &[usize], n: usize,
                   masks: &Masks, rng: &mut Lcg) {
    for i in 0..n {
        let b = battle_idx[i];
        s.def[b] = covers[i];
        s.covered_mask |= 1u64 << b;
        s.hand[p_idx] &= !(1u64 << covers[i]);
        s.table_vmask |= masks.value[id_value(covers[i] as u32) as usize];
    }

    if sim_hand_count(s, p_idx) == 0 {
        s.discard_pile_length += s.num_battles as i16 * 2;
        s.num_battles = 0;
        s.covered_mask = 0;
        s.table_vmask = 0;
        sim_refill(s, rng);
        s.first_attacker = s.defender;
        s.good_mask = 0;
        if sim_hand_count(s, s.first_attacker as usize) == 0 {
            let fa = s.first_attacker as usize;
            let was_in = s.in_mask >> fa & 1 != 0;
            if was_in {
                sim_eliminate(s, fa);
            } else {
                s.status_p[fa] = PLAYER_STATUS_OUT;
                s.out_mask |= 1u32 << fa;
            }
            s.first_attacker = sim_next_player(s, fa as i32) as i8;
        }
        s.defender = sim_next_player(s, s.first_attacker as i32) as i8;
        return;
    }
    s.good_mask = 0;
}

fn sim_apply_pass(s: &mut SimState, p_idx: usize, ids: &[u8], n: usize, masks: &Masks) {
    let next = sim_next_player(s, s.defender as i32);
    for i in 0..n {
        s.hand[p_idx] &= !(1u64 << ids[i]);
        let b = s.num_battles as usize;
        s.num_battles += 1;
        s.atk[b] = ids[i];
        s.covered_mask &= !(1u64 << b);
        s.table_vmask |= masks.value[id_value(ids[i] as u32) as usize];
    }
    s.good_mask = 0;
    if sim_no_cards_left(s) && sim_hand_count(s, p_idx) == 0 {
        sim_eliminate(s, p_idx);
    }
    s.defender = next as i8;
}

fn sim_apply_pickup(s: &mut SimState, p_idx: usize, rng: &mut Lcg) {
    for i in 0..s.num_battles as usize {
        s.hand[p_idx] |= 1u64 << s.atk[i];
        if s.covered_mask & (1u64 << i) != 0 {
            s.hand[p_idx] |= 1u64 << s.def[i];
        }
    }
    s.num_battles = 0;
    s.covered_mask = 0;
    s.table_vmask = 0;
    sim_refill(s, rng);
    s.first_attacker = sim_next_player(s, s.defender as i32) as i8;
    s.defender = sim_next_player(s, s.first_attacker as i32) as i8;
    s.good_mask = 0;
}

fn sim_round_transition(s: &mut SimState, rng: &mut Lcg) {
    s.discard_pile_length += s.num_battles as i16 * 2;
    s.num_battles = 0;
    s.covered_mask = 0;
    s.table_vmask = 0;
    sim_refill(s, rng);
    s.first_attacker = s.defender;
    s.defender = sim_next_player(s, s.first_attacker as i32) as i8;
    s.good_mask = 0;
}

fn sim_apply_good(s: &mut SimState, p_idx: usize, rng: &mut Lcg) {
    s.good_mask |= 1u32 << p_idx;
    let attackers = s.in_mask & !(1u32 << s.defender);
    let all_good = attackers != 0 && (s.good_mask & attackers) == attackers;
    if all_good && sim_all_covered(s) {
        sim_round_transition(s, rng);
    }
}

fn sim_trump_attack_prob(s: &SimState) -> f64 {
    if s.deck_n > 0 || s.has_flipped != 0 { return 0.02; }
    let mut table = 0i32;
    for i in 0..s.num_battles as usize {
        table += 1 + if s.covered_mask & (1u64 << i) != 0 { 1 } else { 0 };
    }
    let mut hands = 0i32;
    for i in 0..s.num_players as usize { hands += sim_hand_count(s, i) as i32; }
    let mut total = s.deck_n as i32 + s.discard_pile_length as i32 + table + hands
        + if s.has_flipped != 0 { 1 } else { 0 };
    if total < 1 { total = 1; }
    let mut ratio = s.discard_pile_length as f64 / total as f64;
    if ratio < 0.0 { ratio = 0.0; }
    if ratio > 1.0 { ratio = 1.0; }
    let mut p = 0.65 + 0.35 * ratio;
    if p < 0.5 { p = 0.5; }
    if p > 0.95 { p = 0.95; }
    p
}

fn sim_first_attack_group(s: &SimState, p: usize, power: u32, non_trump_only: bool,
                          out: &mut [u8], masks: &Masks) -> usize {
    let mut h = s.hand[p];
    if non_trump_only { h &= !masks.suit[power as usize]; }
    let defcap = sim_hand_count(s, s.defender as usize) as i32;
    if defcap <= 0 || h == 0 { return 0; }
    let mut best_v: i32 = -1;
    let mut best_eff: i32 = 0;
    let mut hh = h;
    while hh != 0 {
        let v = id_value(hh.trailing_zeros());
        let g = h & masks.value[v as usize];
        hh = (hh & (hh - 1)) & !g;
        let sz = g.count_ones() as i32;
        let eff = sz.min(defcap);
        if eff > best_eff || (eff == best_eff && (v as i32) < best_v) {
            best_eff = eff;
            best_v = v as i32;
        }
    }
    if best_v < 0 { return 0; }
    let mut g = h & masks.value[best_v as usize];
    let mut n = 0usize;
    while g != 0 && n < best_eff as usize {
        out[n] = g.trailing_zeros() as u8;
        n += 1;
        g &= g - 1;
    }
    n
}

fn sim_attack_group_core(h: u64, defcap: i32, power: u32, out: &mut [u8]) -> usize {
    if h == 0 || defcap <= 0 { return 0; }
    let mut ids = [0u8; 64];
    let mut m = 0usize;
    let mut hh = h;
    while hh != 0 {
        ids[m] = hh.trailing_zeros() as u8;
        m += 1;
        hh &= hh - 1;
    }
    if m <= defcap as usize {
        out[..m].copy_from_slice(&ids[..m]);
        return m;
    }
    for i in 1..m {
        let key = ids[i];
        let ks = id_score(key as u32, power);
        let mut j = i as i32 - 1;
        while j >= 0 && id_score(ids[j as usize] as u32, power) > ks {
            ids[j as usize + 1] = ids[j as usize];
            j -= 1;
        }
        ids[(j + 1) as usize] = key;
    }
    out[..defcap as usize].copy_from_slice(&ids[..defcap as usize]);
    defcap as usize
}

fn sim_greedy_full_cover(s: &SimState, p: usize, power: u32, out: &mut SimMove, masks: &Masks) -> bool {
    let mut avail = s.hand[p];
    let mut n = 0usize;
    for i in 0..s.num_battles as usize {
        if s.covered_mask & (1u64 << i) != 0 { continue; }
        let atk = s.atk[i] as u32;
        let mut best: i32 = -1;
        let same = masks.higher[atk as usize] & avail;
        if same != 0 {
            best = same.trailing_zeros() as i32;
        } else if id_suit(atk) != power {
            let tr = masks.suit[power as usize] & avail;
            if tr != 0 { best = tr.trailing_zeros() as i32; }
        }
        if best < 0 { return false; }
        avail &= !(1u64 << best);
        out.cards[n] = best as u8;
        out.battle[n] = i;
        n += 1;
    }
    out.typ = MvType::Cover;
    out.n = n;
    true
}

fn sim_pass_move(s: &SimState, p: usize, power: u32, out: &mut SimMove, masks: &Masks) -> bool {
    if s.num_battles == 0 { return false; }
    if s.covered_mask != 0 { return false; }
    let v0 = id_value(s.atk[0] as u32);
    for i in 1..s.num_battles as usize {
        if id_value(s.atk[i] as u32) != v0 { return false; }
    }
    let matching = s.hand[p] & masks.value[v0 as usize];
    if matching == 0 { return false; }
    let next = sim_next_player(s, s.defender as i32);
    let next_cards = sim_hand_count(s, next as usize) as i32;
    let mn = matching.count_ones() as i32;
    let mut kmax = next_cards - s.num_battles as i32;
    if kmax < 1 { return false; }
    if kmax > mn { kmax = mn; }
    let _ = kmax;
    let nt = matching & !masks.suit[power as usize];
    let best = (if nt != 0 { nt } else { matching }).trailing_zeros();
    out.typ = MvType::Pass;
    out.n = 1;
    out.cards[0] = best as u8;
    true
}

fn sim_handwritten_move(s: &mut SimState, p: usize, out: &mut SimMove, masks: &Masks,
                        rng: &mut Lcg) -> bool {
    let power = s.power_suit as u32;
    let first_attack = s.num_battles == 0;
    let is_def = p as i8 == s.defender;

    let can_attack = if first_attack {
        p as i8 == s.first_attacker
    } else {
        !is_def && s.good_mask & (1u32 << p) == 0
    };

    let mut h_tab = 0u64;
    let mut defcap = 0i32;
    if can_attack && !first_attack {
        h_tab = s.hand[p] & sim_table_value_mask(s);
        defcap = sim_hand_count(s, s.defender as usize) as i32 - sim_count_uncovered(s) as i32;
    }

    if can_attack {
        let mut buf = [0u8; 64];
        let n_nt = if first_attack {
            sim_first_attack_group(s, p, power, true, &mut buf, masks)
        } else {
            sim_attack_group_core(h_tab & !masks.suit[power as usize], defcap, power, &mut buf)
        };
        if n_nt > 0 {
            out.typ = MvType::Attack;
            out.n = n_nt;
            out.cards[..n_nt].copy_from_slice(&buf[..n_nt]);
            return true;
        }
        let mut tbuf = [0u8; 64];
        let n_tr = if first_attack {
            sim_first_attack_group(s, p, power, false, &mut tbuf, masks)
        } else {
            sim_attack_group_core(h_tab, defcap, power, &mut tbuf)
        };
        if n_tr > 0 {
            if rng.random() < sim_trump_attack_prob(s) {
                out.typ = MvType::Attack;
                out.n = n_tr;
                out.cards[..n_tr].copy_from_slice(&tbuf[..n_tr]);
                return true;
            }
            if !first_attack {
                out.typ = MvType::Good;
                out.n = 0;
                return true;
            }
        }
    }

    if is_def && s.num_battles > 0 {
        let mut pm = SimMove::new();
        if sim_pass_move(s, p, power, &mut pm, masks) {
            out.typ = pm.typ; out.n = pm.n;
            out.cards = pm.cards; out.battle = pm.battle;
            return true;
        }
        let mut cm = SimMove::new();
        if sim_greedy_full_cover(s, p, power, &mut cm, masks) {
            out.typ = cm.typ; out.n = cm.n;
            out.cards = cm.cards; out.battle = cm.battle;
            return true;
        }
        out.typ = MvType::Pickup;
        out.n = 0;
        return true;
    }

    if !is_def && s.num_battles > 0 && s.good_mask & (1u32 << p) == 0 {
        let _ = rng.random(); // mirror: the struct engine consumes a draw here
        out.typ = MvType::Good;
        out.n = 0;
        return true;
    }

    if can_attack {
        let mut buf = [0u8; 64];
        if s.deck_n > 0 || s.has_flipped != 0 {
            let n = if first_attack {
                sim_first_attack_group(s, p, power, true, &mut buf, masks)
            } else {
                sim_attack_group_core(h_tab & !masks.suit[power as usize], defcap, power, &mut buf)
            };
            if n > 0 {
                out.typ = MvType::Attack;
                out.n = n;
                out.cards[..n].copy_from_slice(&buf[..n]);
                return true;
            }
            if !first_attack {
                out.typ = MvType::Good;
                out.n = 0;
                return true;
            }
        }
        let n = if first_attack {
            sim_first_attack_group(s, p, power, false, &mut buf, masks)
        } else {
            sim_attack_group_core(h_tab, defcap, power, &mut buf)
        };
        if n > 0 {
            out.typ = MvType::Attack;
            out.n = n;
            out.cards[..n].copy_from_slice(&buf[..n]);
            return true;
        }
    }

    false
}

fn sim_apply(s: &mut SimState, p: usize, m: &SimMove, masks: &Masks, rng: &mut Lcg) {
    match m.typ {
        MvType::Attack => sim_apply_attack(s, p, &m.cards, m.n, masks),
        MvType::Cover => sim_apply_cover(s, p, &m.cards, &m.battle, m.n, masks, rng),
        MvType::Pass => sim_apply_pass(s, p, &m.cards, m.n, masks),
        MvType::Pickup => sim_apply_pickup(s, p, rng),
        MvType::Good => sim_apply_good(s, p, rng),
    }
}

fn cd_sim_playout(s: &mut SimState, my_idx: usize, max_turns: i32, early_exit: bool,
                  masks: &Masks, rng: &mut Lcg) -> i32 {
    let mut turns = 0;
    while sim_done(s) < 0 {
        turns += 1;
        if turns > max_turns { break; }
        if early_exit && s.in_mask >> my_idx & 1 == 0 {
            for i in 0..s.num_eliminated as usize {
                if s.elim_order[i] == my_idx as i8 { return i as i32 + 1; }
            }
            break;
        }
        let mut elig = 0u32;
        if s.status == GAME_STATUS_PLAYING {
            if s.num_battles == 0 {
                elig = s.in_mask & (1u32 << s.first_attacker);
            } else {
                elig = s.in_mask & !s.good_mask & !(1u32 << s.defender);
                if !sim_all_covered(s) {
                    elig |= s.in_mask & (1u32 << s.defender);
                }
            }
        }
        let mut acted = false;
        let mut m = elig;
        let mut mv = SimMove::new();
        while m != 0 {
            let pi = m.trailing_zeros() as usize;
            if sim_handwritten_move(s, pi, &mut mv, masks, rng) {
                sim_apply(s, pi, &mv, masks, rng);
                acted = true;
                break;
            }
            m &= m - 1;
        }
        if !acted { break; }
    }
    if sim_done(s) < 0 { return 0; }
    for i in 0..s.num_eliminated as usize {
        if s.elim_order[i] == my_idx as i8 { return i as i32 + 1; }
    }
    s.num_players as i32
}

// cd_sim_from_game equivalent, built from the portable dump.
fn state_to_sim(st: &PocState, masks: &Masks) -> SimState {
    let mut s = SimState {
        num_players: 0, power_suit: 0, defender: 0, first_attacker: 0, status: 0,
        num_battles: 0, discard_pile_length: 0, has_flipped: 0, flipped_id: 0,
        good_mask: 0, num_eliminated: 0, hand: [0; MAX_PLAYERS],
        status_p: [0; MAX_PLAYERS], in_mask: 0, out_mask: 0,
        elim_order: [0; MAX_PLAYERS], atk: [0; SIM_MAX_BATTLES],
        def: [0; SIM_MAX_BATTLES], covered_mask: 0, table_vmask: 0,
        deck_n: 0, deck: [0; MAX_DECK],
    };
    s.num_players = st.num_players;
    s.power_suit = st.power_suit;
    s.defender = st.defender as i8;
    s.first_attacker = st.first_attacker as i8;
    s.status = st.status;
    s.num_battles = st.num_battles;
    s.discard_pile_length = st.discard_len as i16;
    s.has_flipped = st.has_flipped;
    s.flipped_id = if st.has_flipped != 0 { st.flipped_id } else { 0 };
    s.good_mask = st.good_mask;
    s.num_eliminated = st.num_eliminated;
    for p in 0..st.num_players as usize {
        let mut h = 0u64;
        for j in 0..st.hand_count[p] as usize {
            h |= 1u64 << st.hand[p][j];
        }
        s.hand[p] = h;
        s.status_p[p] = st.pstatus[p];
        if st.pstatus[p] == PLAYER_STATUS_IN {
            s.in_mask |= 1u32 << p;
        } else if st.pstatus[p] == PLAYER_STATUS_OUT {
            s.out_mask |= 1u32 << p;
        }
    }
    for i in 0..st.num_eliminated as usize { s.elim_order[i] = st.elim[i] as i8; }
    for i in 0..st.num_battles as usize {
        s.atk[i] = st.atk[i];
        s.table_vmask |= masks.value[id_value(st.atk[i] as u32) as usize];
        if st.def[i] != 255 {
            s.def[i] = st.def[i];
            s.covered_mask |= 1u64 << i;
            s.table_vmask |= masks.value[id_value(st.def[i] as u32) as usize];
        }
    }
    s.deck_n = st.deck_count as i16;
    for i in 0..st.deck_count as usize { s.deck[i] = st.deck[i]; }
    s
}

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
            rep_sum = fnv1a_u32(rep_sum, s.in_mask);
            rep_sum = fnv1a_u32(rep_sum, s.out_mask);
            rep_sum = fnv1a_u32(rep_sum, s.num_eliminated as u32);
            rep_sum = fnv1a(rep_sum, &elim_bytes(&s.elim_order));
            rep_sum = fnv1a_u32(rep_sum, s.discard_pile_length as u32);
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
