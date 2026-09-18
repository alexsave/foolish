// Candidate accessor SHAPES the generator could emit instead of what it emits
// today, hand-written at the offsets sdk/ts/gen/game_layout.bots.ts reports and
// exported from their own module exactly like generated code, so bench.ts
// compares like for like (cross-module calls, same argument shapes).
import { Game_deck_count_at, Game_good_players_mask_at, Player_hand_at } from '../../../sdk/ts/gen/game_layout.bots.ts';
export interface MemX { u8: Uint8Array; i8: Int8Array; i16: Int16Array; u16: Uint16Array; i32: Int32Array; u32: Uint32Array; dv: DataView }
export const memXOf = (b: ArrayBuffer): MemX => ({
    u8: new Uint8Array(b), i8: new Int8Array(b), i16: new Int16Array(b), u16: new Uint16Array(b),
    i32: new Int32Array(b), u32: new Uint32Array(b), dv: new DataView(b),
});
const DECK_COUNT = Game_deck_count_at(0), GOOD_MASK = Game_good_players_mask_at(0), HAND = Player_hand_at(0, 0);
// 2- and 4-byte scalars through aligned typed-array views (valid when p is a
// real, aligned pointer to the record and the field is at an aligned offset).
export const ta_get_deck_count = (m: MemX, p: number) => m.i16[(p + DECK_COUNT) >> 1];
export const ta_set_deck_count = (m: MemX, p: number, v: number) => { m.i16[(p + DECK_COUNT) >> 1] = v; };
export const ta_get_good_mask = (m: MemX, p: number) => m.u32[(p + GOOD_MASK) >> 2];
export const ta_set_good_mask = (m: MemX, p: number, v: number) => { m.u32[(p + GOOD_MASK) >> 2] = v; };
// A fixed array of 1-byte records copied in one call.
export const hand_set_bytes = (m: MemX, p: number, bytes: Uint8Array, n: number) => { m.u8.set(bytes.subarray(0, n), p + HAND); };
export const hand_get_bytes = (m: MemX, p: number, n: number) => m.u8.subarray(p + HAND, p + HAND + n);
