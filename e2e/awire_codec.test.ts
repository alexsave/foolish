// Action-wire ("awire" v1) codec fuzz — the TS mirror of cnitro/src/awire.h.
// encodeAction/decodeAction are the client's move->bytes builder and the
// server's legacy-fallback decoder; the decoder must mirror awire_decode's
// strictness exactly: null (never a throw, never a partial parse) on any
// malformed buffer. Pure TS — needs no Postgres and no wasm.

import { test } from 'node:test';
import assert from 'node:assert/strict';

import { Card } from '../supabase/functions/_shared/types.ts';
import {
    AWIRE_KIND, AWIRE_MAX_CARDS, AwireKindName, AwireMove,
    decodeAction, encodeAction,
} from '../supabase/functions/_shared/wire/awire.ts';

// Deterministic RNG so a failure reproduces from the printed seed.
let seed = Number(process.env.FUZZ_SEED || 0xa11ce) >>> 0;
const rnd = () => { seed = (seed * 1664525 + 1013904223) >>> 0; return seed / 0x100000000; };
const ri = (n: number) => Math.floor(rnd() * n);
const pick = <T>(a: T[]): T => a[ri(a.length)];

const KINDS = Object.keys(AWIRE_KIND) as AwireKindName[];
const randCard = (): Card => ({ suit: ri(4), value: 1 + ri(13) });

const randValidMove = (): AwireMove => {
    const kind = pick(KINDS);
    if (kind === 'pickup' || kind === 'good') return { kind, cards: [] };
    const n = 1 + ri(AWIRE_MAX_CARDS);
    const move: AwireMove = { kind, cards: Array.from({ length: n }, randCard) };
    if (kind === 'cover') move.attack_cards = Array.from({ length: n }, randCard);
    return move;
};

test('awire: encode/decode round-trips every valid move exactly', () => {
    for (let i = 0; i < 3000; i++) {
        const move = randValidMove();
        const wire = encodeAction(move);
        const decoded = decodeAction(wire);
        assert.ok(decoded, `valid move decodes (iter ${i}, kind ${move.kind})`);
        assert.equal(decoded!.kind, move.kind, 'kind survives');
        assert.deepEqual(decoded!.cards, move.cards, 'cards survive in order');
        if (move.kind === 'cover') {
            assert.deepEqual(decoded!.attack_cards, move.attack_cards, 'positional attack pairs survive');
        } else {
            assert.equal(decoded!.attack_cards, undefined, 'no attack pairs outside cover');
        }
        // Re-encoding the decode is byte-identical — the codec is a bijection
        // on the valid-move space.
        assert.deepEqual(encodeAction(decoded!), wire, 'encode(decode(bytes)) is byte-identical');
    }
});

test('awire: encodeAction refuses unencodable moves (client bugs, not race conditions)', () => {
    assert.throws(() => encodeAction({ kind: 'bogus' as AwireKindName, cards: [] }), /unknown kind/);
    assert.throws(() => encodeAction({ kind: 'attack', cards: Array.from({ length: AWIRE_MAX_CARDS + 1 }, randCard) }), /exceeds/);
    assert.throws(() => encodeAction({ kind: 'pickup', cards: [randCard()] }), /carries no cards/);
    assert.throws(() => encodeAction({ kind: 'good', cards: [randCard()] }), /carries no cards/);
    assert.throws(() => encodeAction({ kind: 'cover', cards: [randCard(), randCard()], attack_cards: [randCard()] }), /mismatched/);
});

test('awire: decodeAction returns null on every malformed shape', () => {
    const cases: { label: string; wire: Uint8Array }[] = [
        { label: 'empty', wire: new Uint8Array([]) },
        { label: 'single byte', wire: new Uint8Array([0]) },
        { label: 'unknown kind 5', wire: new Uint8Array([5, 0]) },
        { label: 'unknown kind 255', wire: new Uint8Array([255, 1, 3]) },
        { label: 'n > max (attack)', wire: new Uint8Array([0, AWIRE_MAX_CARDS + 1, ...Array(AWIRE_MAX_CARDS + 1).fill(1)]) },
        { label: 'n > max (cover)', wire: new Uint8Array([1, AWIRE_MAX_CARDS + 1, ...Array(2 * (AWIRE_MAX_CARDS + 1)).fill(1)]) },
        { label: 'pickup with n=1', wire: new Uint8Array([3, 1, 7]) },
        { label: 'pickup with n=1, no card bytes', wire: new Uint8Array([3, 1]) },
        { label: 'good with n=2', wire: new Uint8Array([4, 2, 7, 8]) },
        { label: 'attack truncated', wire: new Uint8Array([0, 2, 7]) },
        { label: 'attack with trailing garbage', wire: new Uint8Array([0, 1, 7, 9]) },
        { label: 'pass truncated', wire: new Uint8Array([2, 3, 7, 8]) },
        { label: 'cover missing the attack half', wire: new Uint8Array([1, 2, 7, 8]) },
        { label: 'cover half-truncated pairs', wire: new Uint8Array([1, 2, 7, 8, 9]) },
        { label: 'cover with trailing garbage', wire: new Uint8Array([1, 1, 7, 8, 9]) },
        { label: 'good with trailing garbage', wire: new Uint8Array([4, 0, 1]) },
    ];
    for (const { label, wire } of cases) {
        assert.equal(decodeAction(wire), null, `${label} decodes to null`);
    }
    // Well-formed zero-card attack/cover/pass are STRUCTURALLY valid (2-byte
    // buffers) — the rules layer rejects them, not the codec; and n=0
    // pickup/good are the real no-card moves.
    for (const kind of [0, 1, 2, 3, 4]) {
        const d = decodeAction(new Uint8Array([kind, 0]));
        assert.ok(d, `[${kind},0] is structurally valid`);
        assert.equal(d!.cards!.length, 0);
    }
});

test('awire: decodeAction never throws on random byte strings, and any accept is well-formed', () => {
    let accepted = 0;
    for (let i = 0; i < 4000; i++) {
        // Three of four buffers are pure noise (a structurally-consistent
        // accident is ~1 in 13k, so these exercise the reject path); every
        // fourth is near-valid — random kind byte 0..5 and count byte 0..30
        // with a body sized to the count — so the accept path and the
        // just-off-by-one rejects (kind 5, n 29/30, nonzero pickup/good n)
        // both actually run.
        let wire: Uint8Array;
        if (i % 4 === 0) {
            const kind = ri(6);
            const n = ri(31);
            const body = new Uint8Array(kind === 1 ? 2 * n : n);
            for (let j = 0; j < body.length; j++) body[j] = ri(256);
            wire = new Uint8Array([kind, n, ...body]);
        } else {
            wire = new Uint8Array(ri(80));
            for (let j = 0; j < wire.length; j++) wire[j] = ri(256);
        }
        let decoded: AwireMove | null = null;
        // Must never throw — a hostile POST body cannot crash the decoder.
        assert.doesNotThrow(() => { decoded = decodeAction(wire); }, `random bytes never throw (iter ${i}, seed ${process.env.FUZZ_SEED || '0xa11ce'})`);
        if (decoded === null) continue;
        accepted++;
        const d = decoded as AwireMove;
        // Anything accepted must be a well-formed move honoring the wire's
        // own invariants.
        assert.ok(KINDS.includes(d.kind), 'accepted kind is known');
        assert.equal(d.cards!.length, wire[1], 'card count matches the n byte');
        assert.ok(d.cards!.length <= AWIRE_MAX_CARDS, 'n bounded');
        if (d.kind === 'pickup' || d.kind === 'good') assert.equal(d.cards!.length, 0, 'no-card kinds carry no cards');
        if (d.kind === 'cover') assert.equal(d.attack_cards!.length, d.cards!.length, 'cover pairs positional');
        for (const c of [...d.cards!, ...(d.attack_cards ?? [])]) {
            const hidden = c.suit === -1 && c.value === -1; // 0xFE wire byte
            assert.ok(hidden || (c.suit >= 0 && c.suit <= 3 && c.value >= 1 && c.value <= 13),
                `decoded card in range: ${JSON.stringify(c)}`);
        }
    }
    // The near-valid quarter guarantees the accept path actually ran.
    assert.ok(accepted > 100, `enough random buffers decoded to exercise the accept path (${accepted})`);
});
