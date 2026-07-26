// Rust port of c/src/msg_wire.c's structural layer: msg_decode + msg_encode
// over the same envelope corpus (valid + corrupted) as the C harness. The
// borrowed zero-copy `actions` pointer becomes a (offset, len) into the input
// slice — the same zero-copy design, but the lifetime is compiler-checked.
use rustpoc::*;

const MSG_MAGIC: u8 = 0xF7;
const MSG_FORMAT_V6: u8 = 2;
const MSG_PHASE_WAITING: u8 = 0;
const MSG_PHASE_ACCEPT: u8 = 1;
const MSG_PHASE_FINISHED: u8 = 3;
const MSG_FLAG_FAIR_DEAL: u8 = 0x01;
const MSG_FLAG_GZIP: u8 = 0x02;
const MSG_MAX_NAME: usize = 64;
const MSG_MAX_JOINS: usize = 8;
const MAX_PLAYERS: u8 = 8;
const MSG_SEED_LEN: usize = 32;
const MSG_PARENT_LEN: usize = 8;
const MSG_HEADER_LEN: usize = 59;
const MSG_MAX_ACTIONS: i32 = 1024;
const MSG_MAX_ACTION_BYTES: i32 = 4096;

const MSG_EOK: i32 = 0;
const MSG_ESHORT: i32 = -1;
const MSG_EMAGIC: i32 = -2;
const MSG_EFORMAT: i32 = -3;
const MSG_EFLAGS: i32 = -4;
const MSG_EPHASE: i32 = -5;
const MSG_EPLAYERS: i32 = -6;
const MSG_EVARIANT: i32 = -7;
const MSG_ESEAT: i32 = -8;
const MSG_ENAME: i32 = -9;
const MSG_ESEED: i32 = -10;
const MSG_EACTION: i32 = -11;
const MSG_ETURN: i32 = -12;
const MSG_ECAP: i32 = -14;
const MSG_EJOINS: i32 = -17;

#[derive(Clone, Copy)]
struct MsgJoin {
    seat: u8,
    name_len: u8,
    name: [u8; MSG_MAX_NAME],
}

struct MsgEnvelope {
    format: u8,
    flags: u8,
    phase: u8,
    game_id: u64,
    turn: u16,
    last_actor_seat: u8,
    n_players: u8,
    variant: u8,
    round: u8,
    parent8: [u8; MSG_PARENT_LEN],
    seed: [u8; MSG_SEED_LEN],
    n_joins: i32,
    joins: [MsgJoin; MSG_MAX_JOINS],
    n_actions: i32,
    actions_len: i32,
    actions_off: usize, // borrowed range into the decode input
}

impl MsgEnvelope {
    fn zeroed() -> Self {
        MsgEnvelope {
            format: 0, flags: 0, phase: 0, game_id: 0, turn: 0, last_actor_seat: 0,
            n_players: 0, variant: 0, round: 0, parent8: [0; MSG_PARENT_LEN],
            seed: [0; MSG_SEED_LEN], n_joins: 0,
            joins: [MsgJoin { seat: 0, name_len: 0, name: [0; MSG_MAX_NAME] }; MSG_MAX_JOINS],
            n_actions: 0, actions_len: 0, actions_off: 0,
        }
    }
}

fn seed_is_zero(seed: &[u8; MSG_SEED_LEN]) -> bool {
    seed.iter().fold(0u8, |a, &b| a | b) == 0
}

fn name_is_clean(name: &[u8]) -> bool {
    name.iter().all(|&c| c >= 0x20 && c != 0x7f)
}

fn validate_fields(e: &MsgEnvelope) -> i32 {
    if e.format != MSG_FORMAT_V6 { return MSG_EFORMAT; }
    if e.flags & !(MSG_FLAG_FAIR_DEAL | MSG_FLAG_GZIP) != 0 { return MSG_EFLAGS; }
    if e.flags & MSG_FLAG_FAIR_DEAL != 0 { return MSG_EFLAGS; }
    if e.flags & MSG_FLAG_GZIP != 0 { return MSG_EFLAGS; }

    if e.phase > MSG_PHASE_FINISHED { return MSG_EPHASE; }
    if e.phase == MSG_PHASE_ACCEPT { return MSG_EPHASE; }

    if e.n_players < 2 || e.n_players > MAX_PLAYERS { return MSG_EPLAYERS; }
    if e.variant != 0 { return MSG_EVARIANT; }
    if e.last_actor_seat >= e.n_players { return MSG_ESEAT; }

    if seed_is_zero(&e.seed) { return MSG_ESEED; }

    if e.n_joins < 1 || e.n_joins > e.n_players as i32 { return MSG_EJOINS; }
    let mut seen = 0u32;
    for i in 0..e.n_joins as usize {
        let j = &e.joins[i];
        if j.seat >= e.n_players { return MSG_ESEAT; }
        if seen & (1u32 << j.seat) != 0 { return MSG_ESEAT; }
        seen |= 1u32 << j.seat;
        if j.name_len as usize > MSG_MAX_NAME { return MSG_ENAME; }
        if !name_is_clean(&j.name[..j.name_len as usize]) { return MSG_ENAME; }
    }

    if e.phase == MSG_PHASE_WAITING && (e.n_actions != 0 || e.round != 0) { return MSG_EPHASE; }

    if e.n_actions < 0 || e.n_actions > MSG_MAX_ACTIONS { return MSG_EACTION; }
    if e.actions_len < 0 || e.actions_len > MSG_MAX_ACTION_BYTES { return MSG_EACTION; }
    if e.turn != e.n_actions as u16 { return MSG_ETURN; }
    MSG_EOK
}

fn msg_decode(input: &[u8], out: &mut MsgEnvelope) -> i32 {
    let in_len = input.len();
    if in_len < MSG_HEADER_LEN { return MSG_ESHORT; }
    if input[0] != MSG_MAGIC { return MSG_EMAGIC; }
    if input[1] != MSG_FORMAT_V6 { return MSG_EFORMAT; }

    *out = MsgEnvelope::zeroed();
    out.format = input[1];
    out.flags = input[2];
    out.phase = input[3];
    out.game_id = u64::from_le_bytes(input[4..12].try_into().unwrap());
    out.turn = u16::from_le_bytes(input[12..14].try_into().unwrap());
    out.last_actor_seat = input[14];
    out.n_players = input[15];
    out.variant = input[16];
    out.round = input[17];
    out.parent8.copy_from_slice(&input[18..26]);
    out.seed.copy_from_slice(&input[26..58]);

    let n_joins = input[58] as i32;
    if n_joins < 1 || n_joins > MSG_MAX_JOINS as i32 { return MSG_EJOINS; }
    out.n_joins = n_joins;

    let mut off = MSG_HEADER_LEN;
    for i in 0..n_joins as usize {
        if off + 2 > in_len { return MSG_ESHORT; }
        let seat = input[off];
        let nlen = input[off + 1] as usize;
        off += 2;
        if nlen > MSG_MAX_NAME { return MSG_ENAME; }
        if off + nlen > in_len { return MSG_ESHORT; }
        out.joins[i].seat = seat;
        out.joins[i].name_len = nlen as u8;
        out.joins[i].name[..nlen].copy_from_slice(&input[off..off + nlen]);
        off += nlen;
    }

    if off + 2 > in_len { return MSG_ESHORT; }
    let n_actions = u16::from_le_bytes(input[off..off + 2].try_into().unwrap()) as i32;
    off += 2;
    if n_actions > MSG_MAX_ACTIONS { return MSG_EACTION; }

    let actions_len = in_len as i32 - off as i32;
    if actions_len < 0 { return MSG_ESHORT; }
    if actions_len > MSG_MAX_ACTION_BYTES { return MSG_EACTION; }

    if input[15] < 2 || input[15] > MAX_PLAYERS { return MSG_EPLAYERS; }

    out.n_actions = n_actions;
    out.actions_len = actions_len;
    out.actions_off = off;

    validate_fields(out)
}

fn msg_encode(e: &MsgEnvelope, actions: &[u8], out: &mut [u8]) -> i32 {
    let rc = validate_fields(e);
    if rc != MSG_EOK { return rc; }

    let mut need = MSG_HEADER_LEN;
    for i in 0..e.n_joins as usize { need += 2 + e.joins[i].name_len as usize; }
    need += 2 + e.actions_len as usize;
    if need > out.len() { return MSG_ECAP; }

    out[0] = MSG_MAGIC;
    out[1] = e.format;
    out[2] = e.flags;
    out[3] = e.phase;
    out[4..12].copy_from_slice(&e.game_id.to_le_bytes());
    out[12..14].copy_from_slice(&e.turn.to_le_bytes());
    out[14] = e.last_actor_seat;
    out[15] = e.n_players;
    out[16] = e.variant;
    out[17] = e.round;
    out[18..26].copy_from_slice(&e.parent8);
    out[26..58].copy_from_slice(&e.seed);
    out[58] = e.n_joins as u8;

    let mut off = MSG_HEADER_LEN;
    for i in 0..e.n_joins as usize {
        out[off] = e.joins[i].seat;
        out[off + 1] = e.joins[i].name_len;
        off += 2;
        let nlen = e.joins[i].name_len as usize;
        out[off..off + nlen].copy_from_slice(&e.joins[i].name[..nlen]);
        off += nlen;
    }
    out[off..off + 2].copy_from_slice(&(e.n_actions as u16).to_le_bytes());
    off += 2;
    let alen = e.actions_len as usize;
    out[off..off + alen].copy_from_slice(&actions[..alen]);
    off += alen;
    off as i32
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let path = args.get(1).map(|s| s.as_str()).unwrap_or("envelopes.bin");
    let reps: usize = args.get(2).and_then(|s| s.parse().ok()).unwrap_or(300);

    let buf = std::fs::read(path).unwrap();
    let magic = u32::from_le_bytes(buf[0..4].try_into().unwrap());
    let count = u32::from_le_bytes(buf[4..8].try_into().unwrap()) as usize;
    assert_eq!(magic, 0x564E4546, "bad envelope file");
    let mut envs: Vec<(usize, usize)> = Vec::with_capacity(count); // (offset, len)
    let mut p = 8usize;
    let mut total_bytes = 0usize;
    for _ in 0..count {
        let len = u16::from_le_bytes(buf[p..p + 2].try_into().unwrap()) as usize;
        p += 2;
        envs.push((p, len));
        p += len;
        total_bytes += len;
    }

    let mut e = MsgEnvelope::zeroed();
    let mut out = [0u8; 8192];
    let mut sum = FNV_INIT;
    let mut best = f64::INFINITY;
    let mut t_total = 0.0;
    for _ in 0..reps {
        let mut rep_sum = FNV_INIT;
        let t0 = std::time::Instant::now();
        for &(off, len) in &envs {
            let env_bytes = &buf[off..off + len];
            let rc = msg_decode(env_bytes, &mut e);
            rep_sum = fnv1a_u32(rep_sum, rc as u32);
            if rc == MSG_EOK {
                let actions = &env_bytes[e.actions_off..e.actions_off + e.actions_len as usize];
                let elen = msg_encode(&e, actions, &mut out);
                rep_sum = fnv1a_u32(rep_sum, elen as u32);
                if elen > 0 {
                    rep_sum = fnv1a(rep_sum, &out[..elen as usize]);
                }
            }
        }
        let dt = t0.elapsed().as_secs_f64();
        t_total += dt;
        if dt < best { best = dt; }
        sum = rep_sum;
    }

    println!("bench=wire impl=rust envelopes={count} reps={reps} corpus_bytes={total_bytes} checksum={sum:016x}");
    println!("bench=wire impl=rust best_ms={:.3} mean_ms={:.3} ns_per_env={:.1} mb_per_s={:.1} peak_rss_kb={} sizeof_MsgEnvelope={}",
             best * 1e3, t_total / reps as f64 * 1e3,
             best * 1e9 / count as f64, total_bytes as f64 / best / 1e6,
             peak_rss_kb(), std::mem::size_of::<MsgEnvelope>());
}
