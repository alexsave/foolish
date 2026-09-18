// roster_kernel.ts - the C Roster (c/src/roster.h) as tests drive it directly.
//
// A PRIVATE instance of the test build (e2e/helpers/bots_test_wasm.ts), so these
// calls share no kernel slot with any other test in the same process, over the
// test-only wasm_roster_* exports (c/wasm/wasm_bots_api.c). Used by the Swift half
// of e2e/packed_roster_wire.test.ts, the hidden-information scan and the expand
// migration test.
//
// Knows no byte layout at all. The table a test states crosses as the kernel's
// own RosterSpec and what reading a trailer said as its RosterTrailerRead, both
// through the generated writer and reader (sdk/ts/gen/game_layout.bots.ts); the
// durable roster and the trailer are opaque bytes this file only forwards.

import * as L from '../../sdk/ts/gen/game_layout.bots.ts';
import { botsTestWasm } from './bots_test_wasm.ts';

interface RosterExports {
    memory: WebAssembly.Memory;
    wasm_io_ptr(): number;
    wasm_io_cap(): number;
    wasm_roster_spec_ptr(): number;
    wasm_roster_trailer_ptr(): number;
    wasm_roster_encode(): number;
    wasm_roster_decode(len: number): number;
    wasm_roster_trailer_write(rosterLen: number, gidLen: number, status: number, goodMask: number): number;
    wasm_roster_trailer_read(len: number): number;
}

let cached: RosterExports | null = null;
function kernel(): RosterExports {
    if (!cached) {
        const inst = new WebAssembly.Instance(new WebAssembly.Module(botsTestWasm() as BufferSource), {});
        cached = inst.exports as unknown as RosterExports;
    }
    return cached;
}

const enc = new TextEncoder();
const mem = () => L.memOf(kernel().memory.buffer);
const io = () => new Uint8Array(kernel().memory.buffer, kernel().wasm_io_ptr(), kernel().wasm_io_cap());
const put = (b: Uint8Array) => { io().set(b, 0); };
const get = (n: number) => io().slice(0, n);

export interface RosterSeatSpec { id: string; name: string; brain: string }
export interface RosterTable {
    gid: string; title: string; status: number; goodMask: number; seats: RosterSeatSpec[];
}

/** The table into the kernel's RosterSpec, through the generated writer. */
function writeSpec(title: string, seats: RosterSeatSpec[]): void {
    L.writeRosterSpec(mem(), kernel().wasm_roster_spec_ptr(), {
        title,
        seats: seats.map((s) => ({ id: s.id, name: s.name, brain: s.brain })),
    });
}

/** roster_set_title + roster_seat_add per seat, then roster_encode: the durable bytes, or the refusal. */
export function cRosterEncode(title: string, seats: RosterSeatSpec[]): Uint8Array | number {
    writeSpec(title, seats);
    const n = kernel().wasm_roster_encode();
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
    const r = L.readRosterTrailerRead(mem(), kernel().wasm_roster_trailer_ptr());
    return { status: r.status, aiMask: r.aiMask, consumed: r.consumed, gid: r.gid, durable: get(n) };
}
