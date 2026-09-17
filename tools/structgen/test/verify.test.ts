// Generator genericity check: accessors generated from kinds.h (every field kind)
// and from the real anim_plan.h / legal.h, checked against a wasm32 module whose
// C code reads, writes and offsetof()s the same structs (test/verify.c).
// Build first: bash tools/structgen/gen.sh (writes gen/kinds.ts, gen/verify.wasm and sdk/ts/gen/*.ts).
// Run: node --import tsx --test tools/structgen/test/verify.test.ts
import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import * as K from '../gen/kinds.ts';
import * as A from '../../../sdk/ts/gen/anim.bots.ts';

const wasm = readFileSync(new URL('../gen/verify.wasm', import.meta.url));
const ex = new WebAssembly.Instance(new WebAssembly.Module(wasm), {}).exports as Record<string, any>;
const m = K.memOf(ex.memory.buffer);

test('kinds: generated getters read what C initialized', () => {
    const p = ex.k_ptr();
    assert.equal(K.Kinds_get_tag(m, p), -5);
    assert.equal(K.Kinds_get_big(m, p), -1234567890123n);
    assert.equal(K.Kinds_get_ubig(m, p), 0xF123456789ABCDEFn);
    assert.equal(K.Kinds_get_f(m, p), 1.5);
    assert.equal(K.Kinds_get_d(m, p), -2.25);
    assert.equal(K.Kinds_get_e(m, p), -1);
    assert.equal(K.Kinds_get_p(m, p), 1);
    assert.equal(K.Kinds_get_name(m, p), 0x1234);
    const card = (i: number, j: number) => { const c = K.Kinds_cards_at(p, i, j); return [K.KCard_get_s(m, c), K.KCard_get_v(m, c)]; };
    assert.deepEqual([card(0, 0), card(0, 1), card(1, 0), card(1, 1), card(2, 0), card(2, 1)],
        [[-1, -1], [3, 13], [0, 1], [2, 7], [-4, 15], [1, -16]]);
    assert.equal(K.KNamed_get_a(m, K.Kinds_n_at(p)), -300);
    assert.equal(K.KNamed_get_d(m, K.Kinds_narr_at(p, 1)), 9.75);
    assert.equal(K.Kinds_u_get_i(m, K.Kinds_u_at(p)), -42);
    assert.equal(K.Kinds_get_w(m, p), 0x7abc);
    assert.equal(K.Kinds_pt_get_x(m, K.Kinds_pt_at(p, 1)), 250);
    assert.equal(K.Kinds_get_flag(m, p), true);
    assert.equal(K.Kinds_get_bits(m, p), 5);
    assert.equal(K.Kinds_get_wide(m, p), 0xABCDE);
    assert.equal(K.Kinds_get_bflag(m, p), true);
    assert.equal(K.Kinds_get_text(m, p, 3), 'd'.charCodeAt(0));
    assert.equal(K.Kinds_get_u16s(m, p, 1), 65535);
    assert.equal(K.Kinds_get_i32(m, p), -99);
});

test('kinds: generated setters write what C reads back', () => {
    const p = ex.k_ptr();
    K.Kinds_set_tag(m, p, 17); K.Kinds_set_big(m, p, 77n); K.Kinds_set_ubig(m, p, 0x8000000000000001n);
    K.Kinds_set_f(m, p, -0.125); K.Kinds_set_d(m, p, 1e300); K.Kinds_set_e(m, p, 7);
    const c21 = K.Kinds_cards_at(p, 2, 1);
    K.KCard_set_s(m, c21, -3); K.KCard_set_v(m, c21, 9);       // must not disturb cards[2][0]
    const n1 = K.Kinds_narr_at(p, 1);
    K.KNamed_set_a(m, n1, 12345); K.KNamed_set_d(m, n1, -0.5);
    K.Kinds_u_set_b(m, K.Kinds_u_at(p), 3, 0x80);
    K.Kinds_pt_set_y(m, K.Kinds_pt_at(p, 1), 7);
    K.Kinds_set_flag(m, p, false);
    K.Kinds_set_bits(m, p, 2); K.Kinds_set_wide(m, p, 0xFFFFF); K.Kinds_set_bflag(m, p, false);
    K.Kinds_set_text(m, p, 4, 'z'.charCodeAt(0));
    K.Kinds_set_u16s(m, p, 2, 40000); K.Kinds_set_i32(m, p, -2147483648);
    const kp = K.Kinds_packed_at(p);   // all-bitfield record: read by field, by raw + unpack, set by raw + pack
    assert.deepEqual([K.KPacked_get_lo(m, kp), K.KPacked_get_mid(m, kp), K.KPacked_get_on(m, kp), K.KPacked_get_top(m, kp)], [4000, -1000, true, 200]);
    const r = K.KPacked_raw_get(m, kp);
    assert.deepEqual([K.KPacked_unpack_lo(r), K.KPacked_unpack_mid(r), K.KPacked_unpack_on(r), K.KPacked_unpack_top(r)], [4000, -1000, true, 200]);
    K.KPacked_raw_set(m, kp, K.KPacked_pack(1, -1024, false, 255));
    assert.equal(ex.k_check(), 0, `C saw wrong fields, bitmask ${ex.k_check().toString(2)}`);
});

test('anim_plan.h / legal.h: generated addresses equal compiler offsetof', () => {
    const got = [
        A.AnimPlan_SIZE, A.AnimPlanStep_hand_at(A.AnimPlan_steps_at(0, 3), 2), A.AnimCounts_flipped_at(A.AnimPlan_pre_at(0)),
        A.AnimPlan_veil_ids_at(0, 7), A.AnimPlan_total_ms_at(0),
        A.AnimBeats_SIZE, A.AnimBeat_placed_ids_at(A.AnimBeats_beats_at(0, 5)), A.AnimBeats_first_good_mask_at(0),
        A.AnimEvent_cards_at(0), A.AnimEvent_mask_cards_at(0),
        A.LegalMoves_SIZE, A.LegalMove_attack_cards_at(A.LegalMoves_moves_at(0, 4095), 27), A.LegalMove_cards_at(A.LegalMoves_moves_at(0, 1), 3),
    ];
    assert.deepEqual(got, got.map((_, i) => ex.off_at(i)));
});

test('anim_plan.h: values C wrote read back through nested generated accessors', () => {
    const b = ex.beats_fill();
    assert.equal(A.AnimBeat_get_placed_ids(m, A.AnimBeats_beats_at(b, 5)), 0x0123456789ABCDEFn);
    assert.equal(A.AnimBeat_get_outs_mask(m, A.AnimBeats_beats_at(b, 127)), 0x80000001);
    assert.equal(A.AnimBeats_get_first_good_mask(m, b), -7);
    const pl = ex.plan_fill();
    const f = A.AnimCounts_flipped_at(A.AnimPlan_pre_at(pl));
    assert.deepEqual([A.Card_get_suit(m, f), A.Card_get_value(m, f)], [-1, 12]);
    assert.equal(A.AnimPlanStep_get_hand(m, A.AnimPlan_steps_at(pl, 3), 2), -123456);
});

test('--const: enum constants and integer #defines carry the values C compiled', () => {
    assert.deepEqual([K.K_NEG, K.K_POS, K.KFLAG_LOW, K.KFLAG_HIGH, K.KFLAG_NEG],
        [0, 1, 2, 3, 4].map(i => Number(ex.const_at(i))));
});

test('char[N]: UTF-8, NUL-terminated, at most N-1 bytes; the setter throws rather than truncate', () => {
    const p = ex.k_ptr();
    const bytes = () => [0, 1, 2, 3, 4].map(i => ex.k_text_byte(i));
    const eacute = String.fromCharCode(0xe9), nul = String.fromCharCode(0);
    ex.k_set_text_utf8();                                 // C wrote e-acute, 't', NUL, junk
    assert.equal(K.Kinds_get_text_str(m, p), `${eacute}t`);
    K.Kinds_set_text_str(m, p, 'ab');
    assert.deepEqual(bytes(), [97, 98, 0, 0, 0]);
    K.Kinds_set_text_str(m, p, eacute + eacute);          // 4 UTF-8 bytes: exactly fits
    assert.deepEqual(bytes(), [0xc3, 0xa9, 0xc3, 0xa9, 0]);
    assert.equal(K.Kinds_get_text_str(m, p), eacute + eacute);
    K.Kinds_set_text_str(m, p, 'abcd');
    assert.equal(K.Kinds_get_text_str(m, p), 'abcd');
    assert.throws(() => K.Kinds_set_text_str(m, p, 'abcde'), /5 UTF-8 bytes do not fit in 4/);
    assert.throws(() => K.Kinds_set_text_str(m, p, `ab${eacute}e`), /5 UTF-8 bytes do not fit in 4/);
    assert.throws(() => K.Kinds_set_text_str(m, p, `a${nul}b`), /NUL/);
    assert.equal(K.Kinds_get_text_str(m, p), 'abcd', 'a rejected write leaves the field untouched');
});
