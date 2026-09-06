pub mod sim;

// Shared harness for the Rust-side POC benchmarks: portable state loading,
// FNV-1a checksumming (must match ../bench_common.h exactly), the engine LCG,
// and peak-RSS reporting.

pub const FNV_INIT: u64 = 1469598103934665603;

#[inline]
pub fn fnv1a(mut h: u64, data: &[u8]) -> u64 {
    for &b in data {
        h ^= b as u64;
        h = h.wrapping_mul(1099511628211);
    }
    h
}

#[inline]
pub fn fnv1a_u32(h: u64, v: u32) -> u64 {
    fnv1a(h, &v.to_le_bytes())
}

// Mirrors game.c's LCG: g_seed = g_seed * 1664525 + 1013904223.
pub struct Lcg(pub u32);

impl Lcg {
    pub fn set_seed(&mut self, s: u32) {
        self.0 = if s == 0 { 1 } else { s };
    }
    #[inline]
    pub fn next_u32(&mut self) -> u32 {
        self.0 = self.0.wrapping_mul(1664525).wrapping_add(1013904223);
        self.0
    }
    #[inline]
    pub fn random(&mut self) -> f64 {
        self.next_u32() as f64 / 4294967296.0
    }
}

pub struct PocState {
    pub num_players: u8,
    pub power_suit: u8,
    pub defender: u8,
    pub first_attacker: u8,
    pub status: u8,
    pub num_battles: u8,
    pub actor: u8,
    pub has_flipped: u8,
    pub flipped_id: u8,
    pub num_eliminated: u8,
    pub good_mask: u32,
    pub discard_len: u16,
    pub deck_count: u16,
    pub elim: [u8; 8],
    pub deck: [u8; 64],
    pub atk: [u8; 64],
    pub def: [u8; 64], // 255 = uncovered
    pub pstatus: [u8; 8],
    pub hand_count: [u8; 8],
    pub hand: [[u8; 64]; 8],
}

pub fn load_states(path: &str) -> Vec<PocState> {
    let buf = std::fs::read(path).unwrap_or_else(|e| panic!("{path}: {e}"));
    let mut p = 0usize;
    let rd32 = |b: &[u8], p: usize| u32::from_le_bytes(b[p..p + 4].try_into().unwrap());
    let magic = rd32(&buf, 0);
    let version = rd32(&buf, 4);
    let count = rd32(&buf, 8);
    assert_eq!(magic, 0x434F5046, "bad states file");
    assert_eq!(version, 1);
    p += 12;
    let mut out = Vec::with_capacity(count as usize);
    for _ in 0..count {
        let mut s = PocState {
            num_players: buf[p],
            power_suit: buf[p + 1],
            defender: buf[p + 2],
            first_attacker: buf[p + 3],
            status: buf[p + 4],
            num_battles: buf[p + 5],
            actor: buf[p + 6],
            has_flipped: buf[p + 7],
            flipped_id: buf[p + 8],
            num_eliminated: buf[p + 9],
            good_mask: rd32(&buf, p + 10),
            discard_len: u16::from_le_bytes(buf[p + 14..p + 16].try_into().unwrap()),
            deck_count: u16::from_le_bytes(buf[p + 16..p + 18].try_into().unwrap()),
            elim: [0; 8],
            deck: [0; 64],
            atk: [0; 64],
            def: [0; 64],
            pstatus: [0; 8],
            hand_count: [0; 8],
            hand: [[0; 64]; 8],
        };
        p += 18;
        for j in 0..s.num_eliminated as usize { s.elim[j] = buf[p]; p += 1; }
        for j in 0..s.deck_count as usize { s.deck[j] = buf[p]; p += 1; }
        for j in 0..s.num_battles as usize {
            s.atk[j] = buf[p];
            s.def[j] = buf[p + 1];
            p += 2;
        }
        for pl in 0..s.num_players as usize {
            s.pstatus[pl] = buf[p];
            s.hand_count[pl] = buf[p + 1];
            p += 2;
            for j in 0..s.hand_count[pl] as usize { s.hand[pl][j] = buf[p]; p += 1; }
        }
        out.push(s);
    }
    assert_eq!(p, buf.len(), "trailing bytes");
    out
}

pub fn peak_rss_kb() -> i64 {
    let Ok(s) = std::fs::read_to_string("/proc/self/status") else { return -1 };
    for line in s.lines() {
        if let Some(rest) = line.strip_prefix("VmHWM:") {
            return rest.trim().trim_end_matches(" kB").trim().parse().unwrap_or(-1);
        }
    }
    -1
}
