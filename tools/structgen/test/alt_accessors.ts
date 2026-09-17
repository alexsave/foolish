// Candidate accessor SHAPES the generator could emit instead of what it emits
// today, hand-written at the same offsets as build/harness/game_layout.ts and
// exported from their own module exactly like generated code, so bench.ts
// compares like for like (cross-module calls, same argument shapes).
export interface MemX { u8: Uint8Array; i8: Int8Array; i16: Int16Array; u16: Uint16Array; i32: Int32Array; u32: Uint32Array; dv: DataView }
export const memXOf = (b: ArrayBuffer): MemX => ({
    u8: new Uint8Array(b), i8: new Int8Array(b), i16: new Int16Array(b), u16: new Uint16Array(b),
    i32: new Int32Array(b), u32: new Uint32Array(b), dv: new DataView(b),
});
// 2- and 4-byte scalars through aligned typed-array views (valid when p is a
// real, aligned pointer to the record and the field is at an aligned offset).
export const ta_get_deck_count = (m: MemX, p: number) => m.i16[(p + 6) >> 1];
export const ta_set_deck_count = (m: MemX, p: number, v: number) => { m.i16[(p + 6) >> 1] = v; };
export const ta_get_good_mask = (m: MemX, p: number) => m.u32[(p + 1144) >> 2];
export const ta_set_good_mask = (m: MemX, p: number, v: number) => { m.u32[(p + 1144) >> 2] = v; };
// A fixed array of 1-byte records copied in one call.
export const hand_set_bytes = (m: MemX, p: number, bytes: Uint8Array, n: number) => { m.u8.set(bytes.subarray(0, n), p + 4); };
export const hand_get_bytes = (m: MemX, p: number, n: number) => m.u8.subarray(p + 4, p + 4 + n);
