// Rust port of c/src/legal.c's calculate_legal_moves, benchmarked over the
// same dumped states as the C harness. Two variants:
//   faithful  — line-for-line port, same bool tables, safe indexing throughout
//   idiomatic — same emitted move sequence, but the `used`/`seen`/table-value
//               bool arrays become u64/u16 bitmasks (no per-leaf memsets)
// Both must reproduce the C checksum exactly.
use rustpoc::*;

const MAX_MOVE_CARDS: usize = 8; // native build parameter (c/legal.h)
const MAX_LEGAL_MOVES: usize = 4096;
const MAX_HAND_SIZE: usize = 64;
const MAX_BATTLES_N: usize = 64; // dump allows up to 64; native Game caps at 32
const ACE_VALUE: u8 = 13;

const MOVE_ATTACK: u8 = 0;
const MOVE_COVER: u8 = 1;
const MOVE_PASS: u8 = 2;
const MOVE_PICKUP: u8 = 3;
const MOVE_GOOD: u8 = 4;

// One byte, like the C bitfield Card: value in the high bits, suit in the low.
#[derive(Clone, Copy, PartialEq, Eq, Default)]
struct Card(u8);
impl Card {
    #[inline] fn new(suit: u8, value: u8) -> Card { Card((value << 2) | suit) }
    #[inline] fn from_id(id: u8) -> Card { Card::new(id / 13, id % 13 + 1) }
    #[inline] fn suit(self) -> u8 { self.0 & 3 }
    #[inline] fn value(self) -> u8 { self.0 >> 2 }
    #[inline] fn id(self) -> u32 { (self.suit() as u32) * 13 + (self.value() as u32) - 1 }
}

#[derive(Clone, Copy)]
struct Battle {
    attack: Card,
    defense: u8, // wire byte: 255 = uncovered, else card id
}
impl Battle {
    #[inline] fn covered(&self) -> bool { self.defense != 255 }
    #[inline] fn defense_card(&self) -> Card { Card::from_id(self.defense) }
}

struct PlayerV {
    status: u8,
    hand_count: usize,
    hand: [Card; MAX_HAND_SIZE],
}

struct GameV {
    status: u8,
    num_players: usize,
    power_suit: u8,
    first_attacker: usize,
    defender: usize,
    num_battles: usize,
    good_mask: u32,
    battles: [Battle; MAX_BATTLES_N],
    players: [PlayerV; 8],
}

const PLAYER_STATUS_IN: u8 = 2;
const PLAYER_STATUS_OUT: u8 = 3;
const GAME_STATUS_PLAYING: u8 = 1;

#[derive(Clone, Copy)]
struct LegalMove {
    typ: u8,
    n_cards: u8,
    cards: [Card; MAX_MOVE_CARDS],
    attack_cards: [Card; MAX_MOVE_CARDS],
}

struct LegalMoves {
    n: usize,
    moves: Box<[LegalMove; MAX_LEGAL_MOVES]>,
}

impl LegalMoves {
    fn new() -> Self {
        LegalMoves {
            n: 0,
            moves: vec![LegalMove { typ: 0, n_cards: 0, cards: [Card(0); 8], attack_cards: [Card(0); 8] };
                        MAX_LEGAL_MOVES]
                .into_boxed_slice()
                .try_into()
                .map_err(|_| ())
                .unwrap(),
        }
    }
    // push_move: append or None past the cap; slot NOT zeroed (mirrors legal.c).
    #[inline]
    fn push(&mut self) -> Option<&mut LegalMove> {
        if self.n >= MAX_LEGAL_MOVES { return None; }
        let m = &mut self.moves[self.n];
        self.n += 1;
        m.n_cards = 0;
        Some(m)
    }
}

#[inline]
fn can_cover(attack: Card, defense: Card, power_suit: u8) -> bool {
    if defense.suit() != attack.suit() {
        return defense.suit() == power_suit && attack.suit() != power_suit;
    }
    defense.value() > attack.value()
}

fn get_next_player_index(g: &GameV, current: usize) -> usize {
    let n = g.num_players;
    let in_count = (0..n).filter(|&i| g.players[i].status == PLAYER_STATUS_IN).count();
    if in_count <= 1 { return current; }
    let mut next = (current + 1) % n;
    while g.players[next].status == PLAYER_STATUS_OUT { next = (next + 1) % n; }
    next
}

// ---------- faithful port ------------------------------------------------

fn emit_attack(out: &mut LegalMoves, combo: &[Card], k: usize, defender_cards: usize, uncovered: usize) {
    if defender_cards < uncovered + k { return; }
    let Some(m) = out.push() else { return };
    m.typ = MOVE_ATTACK;
    m.n_cards = k as u8;
    m.cards[..k].copy_from_slice(&combo[..k]);
}

fn combinations_attack(arr: &[Card], n: usize, start: usize, k: usize, buf: &mut [Card; MAX_MOVE_CARDS],
                       depth: usize, out: &mut LegalMoves, defender_cards: usize, uncovered: usize) {
    if out.n >= MAX_LEGAL_MOVES { return; }
    if depth == k {
        emit_attack(out, buf, k, defender_cards, uncovered);
        return;
    }
    let mut i = start;
    while i + (k - depth) <= n {
        buf[depth] = arr[i];
        combinations_attack(arr, n, i + 1, k, buf, depth + 1, out, defender_cards, uncovered);
        i += 1;
    }
}

fn emit_pass(out: &mut LegalMoves, combo: &[Card], k: usize, next_player_cards: usize, n_battles: usize) {
    if next_player_cards < k + n_battles { return; }
    let Some(m) = out.push() else { return };
    m.typ = MOVE_PASS;
    m.n_cards = k as u8;
    m.cards[..k].copy_from_slice(&combo[..k]);
}

fn combinations_pass(arr: &[Card], n: usize, start: usize, k: usize, buf: &mut [Card; MAX_MOVE_CARDS],
                     depth: usize, out: &mut LegalMoves, next_player_cards: usize, n_battles: usize) {
    if out.n >= MAX_LEGAL_MOVES { return; }
    if depth == k {
        emit_pass(out, buf, k, next_player_cards, n_battles);
        return;
    }
    let mut i = start;
    while i + (k - depth) <= n {
        buf[depth] = arr[i];
        combinations_pass(arr, n, i + 1, k, buf, depth + 1, out, next_player_cards, n_battles);
        i += 1;
    }
}

fn calc_first_attack_moves(g: &GameV, p: &PlayerV, out: &mut LegalMoves) {
    let defender_cards = g.players[g.defender].hand_count;
    let mut seen = [false; 16];
    let mut buf = [Card(0); MAX_MOVE_CARDS];
    for i in 0..p.hand_count {
        let v = p.hand[i].value();
        if v < 1 || v > ACE_VALUE { continue; }
        if seen[v as usize] { continue; }
        seen[v as usize] = true;
        let mut group = [Card(0); MAX_MOVE_CARDS];
        let mut gn = 0usize;
        for j in 0..p.hand_count {
            if gn >= MAX_MOVE_CARDS { break; }
            if p.hand[j].value() == v { group[gn] = p.hand[j]; gn += 1; }
        }
        let k_max = gn.min(defender_cards);
        for k in 1..=k_max {
            combinations_attack(&group, gn, 0, k, &mut buf, 0, out, defender_cards, 0);
        }
    }
}

fn calc_regular_attack_moves(g: &GameV, p: &PlayerV, out: &mut LegalMoves) {
    let mut table_values = [false; 16];
    for i in 0..g.num_battles {
        let av = g.battles[i].attack.value();
        if av >= 1 && av <= ACE_VALUE { table_values[av as usize] = true; }
        if g.battles[i].covered() {
            let dv = g.battles[i].defense_card().value();
            if dv >= 1 && dv <= ACE_VALUE { table_values[dv as usize] = true; }
        }
    }
    let mut valid = [Card(0); MAX_HAND_SIZE];
    let mut vn = 0usize;
    for i in 0..p.hand_count {
        let v = p.hand[i].value();
        if v >= 1 && v <= ACE_VALUE && table_values[v as usize] { valid[vn] = p.hand[i]; vn += 1; }
    }
    if vn == 0 { return; }
    let defender_cards = g.players[g.defender].hand_count;
    let uncovered = (0..g.num_battles).filter(|&i| !g.battles[i].covered()).count();
    let mut buf = [Card(0); MAX_MOVE_CARDS];
    let mut k_max = vn.min(MAX_MOVE_CARDS);
    let cap = defender_cards as isize - uncovered as isize;
    if cap < k_max as isize { k_max = cap.max(0) as usize; }
    for k in 1..=k_max {
        combinations_attack(&valid, vn, 0, k, &mut buf, 0, out, defender_cards, uncovered);
    }
}

struct CoverOption {
    covers_n: usize,
    covers: [Card; MAX_HAND_SIZE],
    cover_hand_idx: [i8; MAX_HAND_SIZE],
    attack_card: Card,
}

fn emit_cover_combo(out: &mut LegalMoves, opts: &[&CoverOption], chosen_idx: &mut [usize; MAX_BATTLES_N],
                    depth: usize, used: &mut [bool; MAX_HAND_SIZE]) {
    if out.n >= MAX_LEGAL_MOVES { return; }
    let n_opts = opts.len();
    if depth == n_opts {
        let Some(m) = out.push() else { return };
        m.typ = MOVE_COVER;
        m.n_cards = n_opts as u8;
        for i in 0..n_opts {
            m.cards[i] = opts[i].covers[chosen_idx[i]];
            m.attack_cards[i] = opts[i].attack_card;
        }
        return;
    }
    for i in 0..opts[depth].covers_n {
        let hi = opts[depth].cover_hand_idx[i] as usize;
        if used[hi] { continue; }
        used[hi] = true;
        chosen_idx[depth] = i;
        emit_cover_combo(out, opts, chosen_idx, depth + 1, used);
        used[hi] = false;
    }
}

fn choose_attack_subset(n_uncovered: usize, all_opts: &[CoverOption], start: usize, k_left: usize,
                        picked: &mut [usize; MAX_BATTLES_N], picked_n: usize, out: &mut LegalMoves) {
    if k_left == 0 {
        // Stack array of refs, like C's `const CoverOption *opts[MAX_BATTLES]`.
        let mut opts: [&CoverOption; MAX_BATTLES_N] = [&all_opts[0]; MAX_BATTLES_N];
        for i in 0..picked_n { opts[i] = &all_opts[picked[i]]; }
        for i in 0..picked_n { if opts[i].covers_n == 0 { return; } }
        let mut chosen = [0usize; MAX_BATTLES_N];
        let mut used = [false; MAX_HAND_SIZE];
        emit_cover_combo(out, &opts[..picked_n], &mut chosen, 0, &mut used);
        return;
    }
    let mut i = start;
    while i + k_left <= n_uncovered {
        picked[picked_n] = i;
        choose_attack_subset(n_uncovered, all_opts, i + 1, k_left - 1, picked, picked_n + 1, out);
        i += 1;
    }
}

fn calc_cover_moves(g: &GameV, defender: &PlayerV, out: &mut LegalMoves,
                    opts: &mut Vec<CoverOption>) {
    let mut uncovered_battles = [0usize; MAX_BATTLES_N];
    let mut n_uncovered = 0usize;
    for i in 0..g.num_battles {
        if !g.battles[i].covered() { uncovered_battles[n_uncovered] = i; n_uncovered += 1; }
    }
    if n_uncovered == 0 { return; }

    opts.clear();
    for i in 0..n_uncovered {
        let b = &g.battles[uncovered_battles[i]];
        let mut o = CoverOption {
            covers_n: 0,
            covers: [Card(0); MAX_HAND_SIZE],
            cover_hand_idx: [0; MAX_HAND_SIZE],
            attack_card: b.attack,
        };
        for j in 0..defender.hand_count {
            if can_cover(b.attack, defender.hand[j], g.power_suit) {
                o.cover_hand_idx[o.covers_n] = j as i8;
                o.covers[o.covers_n] = defender.hand[j];
                o.covers_n += 1;
            }
        }
        opts.push(o);
    }

    let mut picked = [0usize; MAX_BATTLES_N];
    let k_max = n_uncovered.min(MAX_MOVE_CARDS);
    for k in 1..=k_max {
        choose_attack_subset(n_uncovered, opts, 0, k, &mut picked, 0, out);
    }
}

fn calc_pass_moves(g: &GameV, defender: &PlayerV, out: &mut LegalMoves) {
    let mut any_covered = false;
    for i in 0..g.num_battles { if g.battles[i].covered() { any_covered = true; } }
    if any_covered { return; }
    if g.num_battles == 0 { return; }

    let v0 = g.battles[0].attack.value();
    for i in 1..g.num_battles { if g.battles[i].attack.value() != v0 { return; } }

    let mut matching = [Card(0); MAX_HAND_SIZE];
    let mut mn = 0usize;
    for i in 0..defender.hand_count {
        if defender.hand[i].value() == v0 { matching[mn] = defender.hand[i]; mn += 1; }
    }
    if mn == 0 { return; }

    let next = get_next_player_index(g, g.defender);
    let next_cards = g.players[next].hand_count;

    let mut buf = [Card(0); MAX_MOVE_CARDS];
    let mut k_max = mn.min(MAX_MOVE_CARDS);
    let cap = next_cards as isize - g.num_battles as isize;
    if cap < k_max as isize { k_max = cap.max(0) as usize; }
    for k in 1..=k_max {
        combinations_pass(&matching, mn, 0, k, &mut buf, 0, out, next_cards, g.num_battles);
    }
}

fn calculate_legal_moves(g: &GameV, bot_idx: usize, out: &mut LegalMoves,
                         scratch: &mut Vec<CoverOption>) {
    out.n = 0;
    if g.status != GAME_STATUS_PLAYING { return; }
    let p = &g.players[bot_idx];
    if p.status != PLAYER_STATUS_IN { return; }
    let is_def = bot_idx == g.defender;
    let is_first_attacker = bot_idx == g.first_attacker;
    let first_attack = g.num_battles == 0;
    let mut all_covered = g.num_battles > 0;
    for i in 0..g.num_battles { if !g.battles[i].covered() { all_covered = false; } }

    if first_attack && is_first_attacker {
        calc_first_attack_moves(g, p, out);
    } else if is_def && g.num_battles > 0 {
        calc_cover_moves(g, p, out, scratch);
        if !all_covered {
            if let Some(m) = out.push() { m.typ = MOVE_PICKUP; }
        }
        calc_pass_moves(g, p, out);
    } else if !is_def && g.num_battles > 0 {
        let said_good = g.good_mask & (1u32 << bot_idx) != 0;
        if !said_good {
            calc_regular_attack_moves(g, p, out);
            if let Some(m) = out.push() { m.typ = MOVE_GOOD; }
        }
    }
}

// ---------- idiomatic variant: bitmask `used`/`seen`/table-values --------
// Emits the exact same move sequence: the bitmasks replace bool tables 1:1.

fn emit_cover_combo_bm(out: &mut LegalMoves, opts: &[&CoverOption], chosen_idx: &mut [usize; MAX_BATTLES_N],
                       depth: usize, used: u64) {
    if out.n >= MAX_LEGAL_MOVES { return; }
    let n_opts = opts.len();
    if depth == n_opts {
        let Some(m) = out.push() else { return };
        m.typ = MOVE_COVER;
        m.n_cards = n_opts as u8;
        for i in 0..n_opts {
            m.cards[i] = opts[i].covers[chosen_idx[i]];
            m.attack_cards[i] = opts[i].attack_card;
        }
        return;
    }
    for i in 0..opts[depth].covers_n {
        let hi = opts[depth].cover_hand_idx[i] as u32;
        if used & (1u64 << hi) != 0 { continue; }
        chosen_idx[depth] = i;
        emit_cover_combo_bm(out, opts, chosen_idx, depth + 1, used | (1u64 << hi));
    }
}

fn choose_attack_subset_bm(n_uncovered: usize, all_opts: &[CoverOption], start: usize, k_left: usize,
                           picked: &mut [usize; MAX_BATTLES_N], picked_n: usize, out: &mut LegalMoves) {
    if k_left == 0 {
        let mut opts: [&CoverOption; MAX_BATTLES_N] = [&all_opts[0]; MAX_BATTLES_N];
        for i in 0..picked_n {
            if all_opts[picked[i]].covers_n == 0 { return; }
            opts[i] = &all_opts[picked[i]];
        }
        let mut chosen = [0usize; MAX_BATTLES_N];
        emit_cover_combo_bm(out, &opts[..picked_n], &mut chosen, 0, 0);
        return;
    }
    let mut i = start;
    while i + k_left <= n_uncovered {
        picked[picked_n] = i;
        choose_attack_subset_bm(n_uncovered, all_opts, i + 1, k_left - 1, picked, picked_n + 1, out);
        i += 1;
    }
}

fn calc_cover_moves_bm(g: &GameV, defender: &PlayerV, out: &mut LegalMoves,
                       opts: &mut Vec<CoverOption>) {
    let mut uncovered_battles = [0usize; MAX_BATTLES_N];
    let mut n_uncovered = 0usize;
    for i in 0..g.num_battles {
        if !g.battles[i].covered() { uncovered_battles[n_uncovered] = i; n_uncovered += 1; }
    }
    if n_uncovered == 0 { return; }
    opts.clear();
    for i in 0..n_uncovered {
        let b = &g.battles[uncovered_battles[i]];
        let mut o = CoverOption {
            covers_n: 0,
            covers: [Card(0); MAX_HAND_SIZE],
            cover_hand_idx: [0; MAX_HAND_SIZE],
            attack_card: b.attack,
        };
        for j in 0..defender.hand_count {
            if can_cover(b.attack, defender.hand[j], g.power_suit) {
                o.cover_hand_idx[o.covers_n] = j as i8;
                o.covers[o.covers_n] = defender.hand[j];
                o.covers_n += 1;
            }
        }
        opts.push(o);
    }
    let mut picked = [0usize; MAX_BATTLES_N];
    let k_max = n_uncovered.min(MAX_MOVE_CARDS);
    for k in 1..=k_max {
        choose_attack_subset_bm(n_uncovered, opts, 0, k, &mut picked, 0, out);
    }
}

fn calc_first_attack_moves_bm(g: &GameV, p: &PlayerV, out: &mut LegalMoves) {
    let defender_cards = g.players[g.defender].hand_count;
    let mut seen: u16 = 0;
    let mut buf = [Card(0); MAX_MOVE_CARDS];
    for i in 0..p.hand_count {
        let v = p.hand[i].value();
        if v < 1 || v > ACE_VALUE { continue; }
        if seen & (1u16 << v) != 0 { continue; }
        seen |= 1u16 << v;
        let mut group = [Card(0); MAX_MOVE_CARDS];
        let mut gn = 0usize;
        for j in 0..p.hand_count {
            if gn >= MAX_MOVE_CARDS { break; }
            if p.hand[j].value() == v { group[gn] = p.hand[j]; gn += 1; }
        }
        let k_max = gn.min(defender_cards);
        for k in 1..=k_max {
            combinations_attack(&group, gn, 0, k, &mut buf, 0, out, defender_cards, 0);
        }
    }
}

fn calc_regular_attack_moves_bm(g: &GameV, p: &PlayerV, out: &mut LegalMoves) {
    let mut table_values: u16 = 0;
    for i in 0..g.num_battles {
        let av = g.battles[i].attack.value();
        if av >= 1 && av <= ACE_VALUE { table_values |= 1u16 << av; }
        if g.battles[i].covered() {
            let dv = g.battles[i].defense_card().value();
            if dv >= 1 && dv <= ACE_VALUE { table_values |= 1u16 << dv; }
        }
    }
    let mut valid = [Card(0); MAX_HAND_SIZE];
    let mut vn = 0usize;
    for i in 0..p.hand_count {
        let v = p.hand[i].value();
        if v >= 1 && v <= ACE_VALUE && table_values & (1u16 << v) != 0 { valid[vn] = p.hand[i]; vn += 1; }
    }
    if vn == 0 { return; }
    let defender_cards = g.players[g.defender].hand_count;
    let uncovered = (0..g.num_battles).filter(|&i| !g.battles[i].covered()).count();
    let mut buf = [Card(0); MAX_MOVE_CARDS];
    let mut k_max = vn.min(MAX_MOVE_CARDS);
    let cap = defender_cards as isize - uncovered as isize;
    if cap < k_max as isize { k_max = cap.max(0) as usize; }
    for k in 1..=k_max {
        combinations_attack(&valid, vn, 0, k, &mut buf, 0, out, defender_cards, uncovered);
    }
}

fn calculate_legal_moves_bm(g: &GameV, bot_idx: usize, out: &mut LegalMoves,
                            scratch: &mut Vec<CoverOption>) {
    out.n = 0;
    if g.status != GAME_STATUS_PLAYING { return; }
    let p = &g.players[bot_idx];
    if p.status != PLAYER_STATUS_IN { return; }
    let is_def = bot_idx == g.defender;
    let is_first_attacker = bot_idx == g.first_attacker;
    let first_attack = g.num_battles == 0;
    let mut all_covered = g.num_battles > 0;
    for i in 0..g.num_battles { if !g.battles[i].covered() { all_covered = false; } }

    if first_attack && is_first_attacker {
        calc_first_attack_moves_bm(g, p, out);
    } else if is_def && g.num_battles > 0 {
        calc_cover_moves_bm(g, p, out, scratch);
        if !all_covered {
            if let Some(m) = out.push() { m.typ = MOVE_PICKUP; }
        }
        calc_pass_moves(g, p, out);
    } else if !is_def && g.num_battles > 0 {
        let said_good = g.good_mask & (1u32 << bot_idx) != 0;
        if !said_good {
            calc_regular_attack_moves_bm(g, p, out);
            if let Some(m) = out.push() { m.typ = MOVE_GOOD; }
        }
    }
}

// ---------- harness ------------------------------------------------------

fn state_to_game(s: &PocState) -> GameV {
    let mut g = GameV {
        status: s.status,
        num_players: s.num_players as usize,
        power_suit: s.power_suit,
        first_attacker: s.first_attacker as usize,
        defender: s.defender as usize,
        num_battles: s.num_battles as usize,
        good_mask: s.good_mask,
        battles: [Battle { attack: Card(0), defense: 255 }; MAX_BATTLES_N],
        players: std::array::from_fn(|_| PlayerV { status: 0, hand_count: 0, hand: [Card(0); MAX_HAND_SIZE] }),
    };
    for i in 0..g.num_battles {
        g.battles[i].attack = Card::from_id(s.atk[i]);
        g.battles[i].defense = s.def[i];
    }
    for p in 0..g.num_players {
        g.players[p].status = s.pstatus[p];
        g.players[p].hand_count = s.hand_count[p] as usize;
        for j in 0..g.players[p].hand_count {
            g.players[p].hand[j] = Card::from_id(s.hand[p][j]);
        }
    }
    g
}

fn run(games: &[GameV], actors: &[usize], reps: usize, label: &str,
       f: fn(&GameV, usize, &mut LegalMoves, &mut Vec<CoverOption>)) {
    let mut out = LegalMoves::new();
    let mut scratch: Vec<CoverOption> = Vec::with_capacity(MAX_BATTLES_N);
    let mut sum = FNV_INIT;
    let mut total_moves = 0u64;
    let mut best = f64::INFINITY;
    let mut t_total = 0.0;
    for _ in 0..reps {
        let mut rep_sum = FNV_INIT;
        let mut rep_moves = 0u64;
        let t0 = std::time::Instant::now();
        for (g, &actor) in games.iter().zip(actors) {
            f(g, actor, &mut out, &mut scratch);
            rep_moves += out.n as u64;
            rep_sum = fnv1a_u32(rep_sum, out.n as u32);
            for m in &out.moves[..out.n] {
                rep_sum = fnv1a_u32(rep_sum, ((m.typ as u32) << 8) | m.n_cards as u32);
                for c in 0..m.n_cards as usize {
                    rep_sum = fnv1a_u32(rep_sum, m.cards[c].id());
                    if m.typ == MOVE_COVER {
                        rep_sum = fnv1a_u32(rep_sum, m.attack_cards[c].id());
                    }
                }
            }
        }
        let dt = t0.elapsed().as_secs_f64();
        t_total += dt;
        if dt < best { best = dt; }
        sum = rep_sum;
        total_moves = rep_moves;
    }
    let n = games.len();
    println!("bench=legal impl=rust-{label} states={n} reps={reps} moves_per_pass={total_moves} checksum={sum:016x}");
    println!("bench=legal impl=rust-{label} best_ms={:.3} mean_ms={:.3} ns_per_call={:.1} ns_per_move={:.2} peak_rss_kb={} sizeof_LegalMoves={}",
             best * 1e3, t_total / reps as f64 * 1e3,
             best * 1e9 / n as f64, best * 1e9 / total_moves as f64,
             peak_rss_kb(), std::mem::size_of::<LegalMove>() * MAX_LEGAL_MOVES + 8);
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let path = args.get(1).map(|s| s.as_str()).unwrap_or("states.bin");
    let reps: usize = args.get(2).and_then(|s| s.parse().ok()).unwrap_or(40);
    let states = load_states(path);
    let games: Vec<GameV> = states.iter().map(state_to_game).collect();
    let actors: Vec<usize> = states.iter().map(|s| s.actor as usize).collect();
    run(&games, &actors, reps, "faithful", calculate_legal_moves);
    run(&games, &actors, reps, "idiomatic", calculate_legal_moves_bm);
}
