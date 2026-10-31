// The promo codes (c/promo/promo_codes.h, read through datagen into
// sdk/ts/gen/promo_codes.ts) keep the rules their header states, and the route
// finds each one in any case.
//
// A code is a path segment the site also answers for game ids and replay codes
// (src/app/[game_id]/GameRoute.tsx), so the first rule is the one that keeps the
// three apart: a game id is exactly six of "0123456789abcdef"
// (server/src/store/game_id.h), and a code has a letter outside that alphabet.
//
// Pure test - no Postgres, no network, no compiler, no wasm.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { PromoCodes } from '../../sdk/ts/gen/promo_codes.ts';
import { FoolishLanguages } from '../../sdk/ts/gen/i18n/languages.ts';
import { promoOf } from '../../src/common/promo.ts';
import { classifyPathSegment } from '../../server/api/common/replay_codec.ts';

test('every promo code keeps the header\'s rules', () => {
    assert.ok(PromoCodes.length >= 2, `read only ${PromoCodes.length} codes - the generated table drifted?`);
    const languages = new Set(FoolishLanguages.map((l) => l.code));
    const codes = new Set<string>(), srcs = new Set<string>();
    for (const p of PromoCodes) {
        assert.match(p.code, /^[a-z]+$/, `${p.code}: lowercase ASCII letters`);
        assert.match(p.code, /[g-z]/, `${p.code}: a letter no game id has`);
        assert.equal(classifyPathSegment(p.code), 'shortcode', `${p.code}: never read as a replay code`);
        assert.ok(languages.has(p.lang), `${p.code}: ${p.lang} is a language c/i18n declares`);
        assert.match(p.src, /^[a-z][a-z-]*$/, `${p.code}: src names a place`);
        assert.ok(!codes.has(p.code) && !srcs.has(p.src), `${p.code}: one row per code and per place`);
        codes.add(p.code);
        srcs.add(p.src);
    }
});

test('the cards Alex asked for: /br is Brighton in Russian, /ws Washington Square in English', () => {
    assert.deepEqual({ ...promoOf('br') }, { code: 'br', src: 'brighton', lang: 'ru' });
    assert.deepEqual({ ...promoOf('ws') }, { code: 'ws', src: 'washington-square', lang: 'en' });
});

test('a code is found in any case, and nothing else is a code', () => {
    for (const s of ['br', 'BR', 'Br', 'bR']) assert.equal(promoOf(s)?.code, 'br', s);
    for (const s of ['', 'b', 'brr', 'abc123', 'tk', 'ho', 'AJG7NYNQG4RFXXGVPGD7QR2ETSOYTWMC']) assert.equal(promoOf(s), null, s);
});
