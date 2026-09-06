// roster.ts - the roster, packed, as the TypeScript twin of
// sdk/swift/RosterWire.swift.
//
// The client-server envelope (encodeGameResponse, this directory's view.ts)
// carried a JSON island inside an otherwise packed payload: the seats, their
// names, their ids. It was the last JSON on any path that matters, and the
// owner's standing position is that there should be none. The FMSG roster went
// packed first (RosterWire.swift, the kernel's own `n_joins(1)` then
// `n_joins x { seat(1) name_len(1) name[] }`), so the shape was already
// decided; this file writes it from the server.
//
// THE NAMES BLOCK IS THE KERNEL'S, BYTE FOR BYTE. `writeNames`/`readNames`
// below emit and read exactly what `RosterWire.encode`/`RosterWire.decode`
// emit and read - same u8 count, same seat byte, same u8 length prefix, same
// 64-byte budget trimmed by whole Unicode scalars. e2e/packed_roster_wire.
// test.ts compiles the REAL Swift decoder against this encoder's bytes so the
// two cannot drift on the part with any judgement in it (the trim rule).
//
// The identity fields the envelope needs and the kernel's roster does not -
// player_id, is_ai, the game's id/name/status, the good-players order and its
// timestamp - ride ALONGSIDE the names block rather than inside it, so the
// kernel's shape stays the kernel's shape.

// MSG_MAX_NAME (c/src/msg_wire.h) - a name is <=64 UTF-8 BYTES, not characters.
export const ROSTER_MAX_NAME_BYTES = 64;
// MSG_MAX_JOINS == MAX_PLAYERS.
export const ROSTER_MAX_JOINS = 8;

// The trailer's own version byte. Bumping it is how a future field lands
// without a second negotiation: a reader that does not know the value stops
// and falls back, rather than reading a field that moved.
export const ROSTER_WIRE_FORMAT = 1;

// game.status, packed. The strings are the server's (GAME_STATUS); the ints are
// this wire's, and they match view.c's G_STATUS ordering so a reader that
// already has that table needs no second one.
export const ROSTER_STATUS: readonly string[] = ['waiting', 'playing', 'game_over'];

/** One seat, as the envelope carries it. */
export interface PackedRosterPlayer {
    player_id: string;
    name: string;
    is_ai: boolean;
}

/** Everything the envelope's roster island carried, losing nothing. */
export interface PackedRoster {
    id: string;
    name: string;
    status: string;
    players: PackedRosterPlayer[];
    good_players: string[];
    good_timestamp: number | null;
}

// ---------------------------------------------------------------------------
// Byte primitives - the TS mirror of sdk/swift/PackedBytes.swift. Little-endian
// scalars, length-prefixed blobs, and a reader whose only two outcomes are "the
// whole record" and "nothing".
// ---------------------------------------------------------------------------

class Writer {
    readonly bytes: number[] = [];
    u8(v: number) { this.bytes.push(v & 0xff); }
    u16(v: number) { this.bytes.push(v & 0xff, (v >> 8) & 0xff); }
    f64(v: number) {
        const dv = new DataView(new ArrayBuffer(8));
        dv.setFloat64(0, v, /*littleEndian*/ true);
        for (let i = 0; i < 8; i++) this.bytes.push(dv.getUint8(i));
    }
    /** A byte string with a u8 length - the width the kernel's own records use. */
    blob8(v: Uint8Array) {
        if (v.length > 0xff) throw new Error(`roster: ${v.length}B does not fit a u8 length prefix`);
        this.u8(v.length);
        for (const b of v) this.bytes.push(b);
    }
    /** A byte string with a u16 length - the width this side invents. */
    blob(v: Uint8Array) {
        if (v.length > 0xffff) throw new Error(`roster: ${v.length}B does not fit a u16 length prefix`);
        this.u16(v.length);
        for (const b of v) this.bytes.push(b);
    }
    text(s: string) { this.blob(new TextEncoder().encode(s)); }
}

class Reader {
    constructor(private readonly b: Uint8Array, public at: number) {}
    u8(): number | null {
        if (this.at >= this.b.length) return null;
        return this.b[this.at++];
    }
    u16(): number | null {
        if (this.at + 2 > this.b.length) return null;
        const v = this.b[this.at] | (this.b[this.at + 1] << 8);
        this.at += 2;
        return v;
    }
    f64(): number | null {
        if (this.at + 8 > this.b.length) return null;
        const dv = new DataView(this.b.buffer, this.b.byteOffset + this.at, 8);
        this.at += 8;
        return dv.getFloat64(0, /*littleEndian*/ true);
    }
    bytes(n: number): Uint8Array | null {
        if (n < 0 || this.at + n > this.b.length) return null;
        const out = this.b.subarray(this.at, this.at + n);
        this.at += n;
        return out;
    }
    blob8(): Uint8Array | null {
        const n = this.u8();
        return n === null ? null : this.bytes(n);
    }
    blob(): Uint8Array | null {
        const n = this.u16();
        return n === null ? null : this.bytes(n);
    }
    text8(): string | null {
        const v = this.blob8();
        return v === null ? null : new TextDecoder().decode(v);
    }
    text(): string | null {
        const v = this.blob();
        return v === null ? null : new TextDecoder().decode(v);
    }
}

/**
 * One name's UTF-8 bytes, trimmed to the budget on a SCALAR boundary - byte for
 * byte with RosterWire.nameBytes. Scalars and not grapheme clusters: `Array.
 * from` splits on code points and so does Swift's `unicodeScalars`, and both
 * sides trimming the same way matters more than either trimming prettily.
 */
export function rosterNameBytes(name: string): Uint8Array {
    const points = Array.from(name);
    let bytes = new TextEncoder().encode(points.join(''));
    while (bytes.length > ROSTER_MAX_NAME_BYTES && points.length > 0) {
        points.pop();
        bytes = new TextEncoder().encode(points.join(''));
    }
    return bytes;
}

/**
 * The kernel's names block: n(1), then n x { seat(1), name_len(1), name[] }.
 * Exported on its own because it is the half with a Swift twin -
 * `RosterWire.encode` writes exactly these bytes, and
 * e2e/packed_roster_wire.test.ts asserts the two are equal rather than merely
 * mutually readable. Seats are the array index: this wire's roster is
 * seat-indexed, as the envelope's always was.
 */
export function encodeRosterNames(names: string[]): Uint8Array {
    const w = new Writer();
    writeNames(w, names);
    return Uint8Array.from(w.bytes);
}

function writeNames(w: Writer, names: string[]) {
    const n = Math.min(names.length, ROSTER_MAX_JOINS);
    w.u8(n);
    for (let seat = 0; seat < n; seat++) {
        w.u8(seat);
        w.blob8(rosterNameBytes(names[seat]));
    }
}

/** The packed roster - everything the JSON island carried, in bytes. */
export function encodePackedRoster(roster: PackedRoster): Uint8Array {
    const status = ROSTER_STATUS.indexOf(roster.status);
    if (status < 0) throw new Error(`roster: unknown status ${roster.status}`);
    const w = new Writer();
    w.u8(ROSTER_WIRE_FORMAT);
    w.text(roster.id);
    w.text(roster.name);
    w.u8(status);
    // The kernel's block first, then the identity fields for the SAME seats in
    // the SAME order - a reader that only wants names stops after the block.
    const players = roster.players.slice(0, ROSTER_MAX_JOINS);
    writeNames(w, players.map(p => p.name));
    for (const p of players) {
        w.blob8(new TextEncoder().encode(p.player_id));
        w.u8(p.is_ai ? 1 : 0);
    }
    // good_players is an ORDER, not a set, and it may name a player who has
    // since left the table - so it rides as ids, not as a seat mask.
    const good = roster.good_players ?? [];
    if (good.length > 0xff) throw new Error(`roster: ${good.length} good players exceeds the u8 count`);
    w.u8(good.length);
    for (const id of good) w.blob8(new TextEncoder().encode(id));
    if (roster.good_timestamp === null || roster.good_timestamp === undefined) {
        w.u8(0);
    } else {
        w.u8(1);
        w.f64(roster.good_timestamp);
    }
    return Uint8Array.from(w.bytes);
}

/**
 * The packed roster back out of a buffer, starting at `at`. Returns the roster
 * and the offset just past it, or null if any field runs off the end - the same
 * all-or-nothing RosterWire keeps, since a roster read short is a different
 * table.
 */
export function decodePackedRoster(
    buf: Uint8Array, at: number,
): { roster: PackedRoster; next: number } | null {
    const r = new Reader(buf, at);
    if (r.u8() !== ROSTER_WIRE_FORMAT) return null;
    const id = r.text();
    const name = r.text();
    const statusInt = r.u8();
    if (id === null || name === null || statusInt === null) return null;
    if (statusInt >= ROSTER_STATUS.length) return null;
    const n = r.u8();
    if (n === null) return null;
    const names: string[] = [];
    for (let i = 0; i < n; i++) {
        const seat = r.u8();
        const nm = r.text8();
        if (seat === null || nm === null || seat !== i) return null;
        names.push(nm);
    }
    const players: PackedRosterPlayer[] = [];
    for (let i = 0; i < n; i++) {
        const pid = r.text8();
        const isAi = r.u8();
        if (pid === null || isAi === null) return null;
        players.push({ player_id: pid, name: names[i], is_ai: isAi !== 0 });
    }
    const nGood = r.u8();
    if (nGood === null) return null;
    const good: string[] = [];
    for (let i = 0; i < nGood; i++) {
        const id2 = r.text8();
        if (id2 === null) return null;
        good.push(id2);
    }
    const hasTs = r.u8();
    if (hasTs === null) return null;
    let ts: number | null = null;
    if (hasTs !== 0) {
        ts = r.f64();
        if (ts === null) return null;
    }
    return {
        roster: {
            id, name, status: ROSTER_STATUS[statusInt], players,
            good_players: good, good_timestamp: ts,
        },
        next: r.at,
    };
}
