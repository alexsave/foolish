// roster_kernel.ts - the C Roster (c/src/roster.h) as the parity tests drive it.
//
// A PRIVATE bots.wasm instance, so these calls share no kernel slot with any
// other test in the same process, over the test-only wasm_roster_* exports
// (c/wasm/wasm_bots_api.c). Shared by e2e/roster_c_parity.test.ts and the Swift
// half of e2e/packed_roster_wire.test.ts, so both hold the same C writer to the
// same bytes. Knows no roster byte layout: the SPEC below is the exports' own
// test input shape, and everything C writes comes back as opaque bytes.

import { loadWasmGz } from '../../sdk/ts/wasm/wasm_asset.ts';

// ROSTER_* (c/src/roster.h).
export const ROSTER_BYTES = 1227;
export const ROSTER_E_ID = -4;
export const ROSTER_E_FULL = -9;
export const ROSTER_E_PADDING = -13;
export const ROSTER_STATUS_NAMES = ['waiting', 'playing', 'game_over'];

interface RosterExports {
    memory: WebAssembly.Memory;
    wasm_io_ptr(): number;
    wasm_io_cap(): number;
    wasm_roster_encode(inLen: number): number;
    wasm_roster_decode(len: number): number;
    wasm_roster_trailer_write(rosterLen: number, gidLen: number, status: number, goodMask: number): number;
    wasm_roster_trailer_read(len: number): number;
}

let cached: RosterExports | null = null;
function kernel(): RosterExports {
    if (!cached) {
        const inst = new WebAssembly.Instance(new WebAssembly.Module(loadWasmGz('bots') as BufferSource), {});
        cached = inst.exports as unknown as RosterExports;
    }
    return cached;
}

const enc = new TextEncoder();
const io = () => new Uint8Array(kernel().memory.buffer, kernel().wasm_io_ptr(), kernel().wasm_io_cap());
const put = (b: Uint8Array) => { io().set(b, 0); };
const get = (n: number) => io().slice(0, n);

export interface RosterSeatSpec { id: string; name: string; brain: string }
export interface RosterTable {
    gid: string; title: string; status: number; goodMask: number; seats: RosterSeatSpec[];
}

// The exports' SPEC: u8 n, u8 title_len, title, n x { u16 id, u16 name, u8 brain }.
function spec(title: string, seats: RosterSeatSpec[]): Uint8Array {
    const out: number[] = [seats.length];
    const t = enc.encode(title);
    out.push(t.length, ...t);
    for (const s of seats) {
        const id = enc.encode(s.id), name = enc.encode(s.name), brain = enc.encode(s.brain);
        out.push(id.length & 0xff, id.length >> 8, ...id);
        out.push(name.length & 0xff, name.length >> 8, ...name);
        out.push(brain.length, ...brain);
    }
    return Uint8Array.from(out);
}

/** roster_set_title + roster_seat_add per seat, then roster_encode: the durable bytes, or the refusal. */
export function cRosterEncode(title: string, seats: RosterSeatSpec[]): Uint8Array | number {
    const s = spec(title, seats);
    put(s);
    const n = kernel().wasm_roster_encode(s.length);
    return n < 0 ? n : get(n);
}

/** roster_decode then roster_encode: the same bytes when decode is lossless, or the refusal. */
export function cRosterDecodeReencode(durable: Uint8Array): Uint8Array | number {
    put(durable);
    const n = kernel().wasm_roster_decode(durable.length);
    return n < 0 ? n : get(n);
}

/** roster_trailer_write for a table: the envelope trailer bytes, or the refusal. */
export function cRosterTrailer(t: RosterTable): Uint8Array | number {
    const durable = cRosterEncode(t.title, t.seats);
    if (typeof durable === 'number') return durable;
    const gid = enc.encode(t.gid);
    const buf = new Uint8Array(durable.length + gid.length);
    buf.set(durable, 0);
    buf.set(gid, durable.length);
    put(buf);
    const n = kernel().wasm_roster_trailer_write(durable.length, gid.length, t.status, t.goodMask);
    return n < 0 ? n : get(n);
}

/** roster_trailer_read: what C read out of a trailer, or the refusal. */
export function cRosterTrailerRead(trailer: Uint8Array):
    { status: number; aiMask: number; consumed: number; gid: string; durable: Uint8Array } | number {
    put(trailer);
    const n = kernel().wasm_roster_trailer_read(trailer.length);
    if (n < 0) return n;
    const b = get(n);
    const dv = new DataView(b.buffer);
    const gidLen = b[7];
    return {
        status: b[0], aiMask: dv.getUint32(1, true), consumed: dv.getUint16(5, true),
        gid: new TextDecoder().decode(b.subarray(8, 8 + gidLen)),
        durable: b.slice(8 + gidLen),
    };
}

/** The roster island the retired TS encoder (sdk/ts/wire/roster.ts) took. */
export interface PackedRoster {
    id: string;
    name: string;
    status: string;
    players: { player_id: string; name: string; is_ai: boolean }[];
    good_players: string[];
    good_timestamp: number | null;
}

/**
 * The PackedRoster the TS encoder needs to write the SAME trailer C writes:
 * is_ai from the brain, good ids in seat order from the mask, no timestamp.
 */
export function tsRosterFor(t: RosterTable): PackedRoster {
    return {
        id: t.gid, name: t.title, status: ROSTER_STATUS_NAMES[t.status],
        players: t.seats.map(s => ({ player_id: s.id, name: s.name, is_ai: s.brain !== '' })),
        good_players: t.seats.filter((_, i) => (t.goodMask >> i) & 1).map(s => s.id),
        good_timestamp: null,
    };
}
