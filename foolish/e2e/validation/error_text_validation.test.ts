/* =============================================================================
 * Every refusal code the kernel can return has its sentence, and only in C
 * =============================================================================
 * c/errtext/error_text.h holds what each refusal code says, one { code, text }
 * row per code, and tools/datagen writes the tables out as
 * sdk/ts/gen/errtext/*.ts. A row is written against the constant, so a renamed
 * or removed code is a compile error in that header. What C cannot say is the
 * other direction: that a code ADDED to game.h got a row. It has no way to
 * enumerate its own #defines, and the cost of that is on record -
 * ENGINE_REJECT_PASS_DISABLED was returned by the kernel for a release while the
 * hand-typed TypeScript table it should have been in stopped at 21.
 *
 * So the families are held together here, from the generated side of both:
 * structgen writes every constant of each family (sdk/ts/gen/
 * kernel_consts.bots.ts, by --const prefix - nobody lists them), datagen writes
 * the rows, and each family's codes must be exactly its table's.
 *
 * And the tables stay out of what ships: c/errtext is in no source list, and
 * the module every visitor downloads is searched for the sentences.
 *
 * Pure test - no Postgres, no network, no compiler.
 * ========================================================================== */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { gunzipSync } from 'node:zlib';
import { fileURLToPath } from 'node:url';

import * as K from '../../sdk/ts/gen/kernel_consts.bots.ts';
import { MoveRejectText } from '../../sdk/ts/gen/errtext/move_reject.ts';
import { MsgErrorText } from '../../sdk/ts/gen/errtext/msg.ts';
import { ReplayErrorText } from '../../sdk/ts/gen/errtext/replay.ts';
import { ReplayExtrasErrorText } from '../../sdk/ts/gen/errtext/replay_extras.ts';
import { rejectMessage } from '../../src/wasm/rejectMessages.ts';

const ROOT = fileURLToPath(new URL('../../', import.meta.url));

type Row = { readonly code: number; readonly text: string };

/** The generated constants whose names match, as [name, value]. */
const family = (name: RegExp): [string, number][] =>
    Object.entries(K as Record<string, unknown>)
        .filter((e): e is [string, number] => name.test(e[0]) && typeof e[1] === 'number');

/**
 * One family against its table: every code but the family's "no error" has
 * exactly one row, under the number the kernel returns it as (`sign` is -1 for
 * the families that return a code negated), and the table holds nothing else
 * but `extra`.
 */
function holds(what: string, codes: [string, number][], rows: readonly Row[], sign: 1 | -1, extra: number[] = []): void {
    assert.ok(codes.length >= 7, `${what}: the family was read out of the generated constants (${codes.length} found)`);
    const returned = codes.filter(([, v]) => v !== 0).map(([n, v]) => [n, sign * v] as const);
    const missing = returned.filter(([, v]) => !rows.some((r) => r.code === v)).map(([n]) => n);
    assert.deepEqual(missing, [], `${what}: these codes have no row in c/errtext/error_text.h, so a host would print a bare number for them`);
    const known = new Set([...returned.map(([, v]) => v), ...extra]);
    assert.deepEqual(rows.filter((r) => !known.has(r.code)).map((r) => r.code), [], `${what}: rows for codes that are not in the family`);
    assert.equal(new Set(rows.map((r) => r.code)).size, rows.length, `${what}: a code has two rows`);
    assert.deepEqual(rows.filter((r) => !r.text.trim()).map((r) => r.code), [], `${what}: a row with nothing to say`);
}

test('every code the action path can refuse a move with has a row', () => {
    holds('MOVE_REJECT_TEXT', family(/^ENGINE_REJECT_/), MoveRejectText, 1,
        // The three answers that are not a rule: the edge's stale round, and a
        // wire that is not a move as the client slot and the rules bridge say it.
        [K.TABLE_REJECT_STALE_ROUND, K.CLIENT_E_MOVE, -1]);
    for (const code of [K.TABLE_REJECT_STALE_ROUND, K.CLIENT_E_MOVE, -1]) {
        assert.ok(MoveRejectText.some((r) => r.code === code), `MOVE_REJECT_TEXT has no row for ${code}`);
    }
    // A rule's text starts with the name of its own constant.
    for (const [name, value] of family(/^ENGINE_REJECT_/)) {
        if (value === 0) continue;
        const text = MoveRejectText.find((r) => r.code === value)!.text;
        assert.ok(text.startsWith(`${name.slice('ENGINE_REJECT_'.length)}: `), `${name} reads "${text}"`);
        assert.equal(K.ENGINE_REJECT_NAMES[value], name.slice('ENGINE_REJECT_'.length));
    }
});

test('every FMSG, replay and extras refusal has a row, under the number it is returned as', () => {
    // A name with nothing after the E-word: REPLAY_EXTRAS_* also starts REPLAY_E.
    holds('MSG_ERROR_TEXT', family(/^MSG_E[A-Z]+$/), MsgErrorText, 1);
    holds('REPLAY_ERROR_TEXT', family(/^REPLAY_E[A-Z]+$/), ReplayErrorText, -1);
    holds('REPLAY_EXTRAS_ERROR_TEXT', family(/^REPLAY_EXTRAS_E[A-Z]+$/), ReplayExtrasErrorText, -1);
});

test('rejectMessage is that table, and a code no build knows still says its number', () => {
    assert.equal(rejectMessage(K.ENGINE_REJECT_NOT_IN_HAND), 'NOT_IN_HAND: card not in hand');
    // The code the hand-typed table never had.
    assert.equal(rejectMessage(K.ENGINE_REJECT_PASS_DISABLED), 'PASS_DISABLED: this table plays without passing');
    assert.equal(rejectMessage(K.TABLE_REJECT_STALE_ROUND), 'STALE_ROUND: a round closed before this move landed');
    assert.equal(rejectMessage(K.CLIENT_E_MOVE), 'MALFORMED: unreadable action wire');
    assert.equal(rejectMessage(-1), 'MALFORMED: unreadable action wire');
    assert.equal(rejectMessage(9999), 'move rejected (code 9999)');
});

test('c/errtext is linked by nothing, and its sentences are not in the shipped module', () => {
    const mk = readFileSync(`${ROOT}c/Makefile`, 'utf8');
    assert.ok(!/\berrtext\b/.test(mk),
        'c/Makefile names errtext. The tables are read by tools/datagen and linked by nothing: a kernel that '
        + 'returns a number has no use for the sentence, and every visitor downloads web.wasm.gz.');
    const wasm = gunzipSync(readFileSync(`${ROOT}sdk/ts/wasm/web.wasm.gz`));
    const all = [...MoveRejectText, ...MsgErrorText, ...ReplayErrorText, ...ReplayExtrasErrorText];
    assert.ok(all.length > 50, `the tables were read (${all.length} rows)`);
    const found = all.filter((r) => wasm.includes(Buffer.from(r.text, 'utf8'))).map((r) => r.text);
    assert.deepEqual(found, [], 'these sentences are inside sdk/ts/wasm/web.wasm.gz');
});
