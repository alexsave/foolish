// roster_kernel.ts - the C Roster (c/src/roster.h) as tests drive it directly.
//
// A PRIVATE table host (e2e/helpers/table_host.ts), so these calls share no
// kernel slot with any other test in the same process, over the wasm_roster_*
// entries no shipped module exports (c/wasm/wasm_bots_api.c). Used by the Swift half
// of e2e/packed_roster_wire.test.ts, the hidden-information scan and the expand
// migration test.
//
// Knows no byte layout at all. The table a test states crosses as the kernel's
// own RosterSpec and what reading a trailer said as its RosterTrailerRead, both
// through the generated writer and reader (sdk/ts/gen/game_layout.bots.ts); the
// durable roster and the trailer are opaque bytes this file only forwards.

import * as L from '../../sdk/ts/gen/game_layout.bots.ts';
import { startTableHost, type TableHost } from './table_host.ts';

let cached: TableHost | null = null;
function host(): TableHost { return (cached ??= startTableHost()); }
const kernel = () => host().ex;

const enc = new TextEncoder();
const put = (b: Uint8Array) => { host().write(kernel().wasm_io_ptr(), b); };
const get = (n: number) => host().read(kernel().wasm_io_ptr(), n);

export interface RosterSeatSpec { id: string; name: string; brain: string }
export interface RosterTable {
    gid: string; title: string; status: number; goodMask: number; seats: RosterSeatSpec[];
    /** The table's rules: 1 (the default) the classic passing game, 0 podkidnoy. */
    passing?: number;
}

/** The table into the kernel's RosterSpec, through the generated writer. */
function writeSpec(title: string, seats: RosterSeatSpec[]): void {
    const m = L.memOf(new ArrayBuffer(L.RosterSpec_SIZE));
    L.writeRosterSpec(m, 0, {
        title,
        seats: seats.map((s) => ({ id: s.id, name: s.name, brain: s.brain })),
    });
    host().write(kernel().wasm_roster_spec_ptr(), m.u8);
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
    const n = kernel().wasm_roster_trailer_write(durable.length, gid.length, t.status, t.goodMask, t.passing ?? 1);
    return n < 0 ? n : get(n);
}

/** roster_trailer_read: what C read out of a trailer, or the refusal. */
export function cRosterTrailerRead(trailer: Uint8Array):
    { status: number; aiMask: number; consumed: number; passing: number; gid: string; durable: Uint8Array } | number {
    put(trailer);
    const n = kernel().wasm_roster_trailer_read(trailer.length);
    if (n < 0) return n;
    const r = L.readRosterTrailerRead(L.memOf(host().read(kernel().wasm_roster_trailer_ptr(), L.RosterTrailerRead_SIZE).buffer), 0);
    return { status: r.status, aiMask: r.aiMask, consumed: r.consumed, passing: r.passing, gid: r.gid, durable: get(n) };
}
