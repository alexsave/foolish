// The column transports of table_io.ts: the base64 a blob parameter rides in
// and the hex text a SELECT hands back.
//
// Node has Uint8Array.prototype.toBase64, so every other suite runs the native
// encoder; the edge runtime has none (V8 11.6 locally) and runs the table loop.
// So the loop is held here to Node's own base64 at every length that exercises
// a padding case and at blob sizes the server sends, and the reader to both
// forms of hex text a column can come back as.

import './harness.ts';
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { base64, base64Table, columnHexToBytes } from '../server/impls/supabase/functions/_shared/adapter/table_io.ts';
import { suiteRng } from './helpers/rng.ts';

const rng = suiteRng('table_io_transport');
const blob = (n: number) => Uint8Array.from({ length: n }, () => rng.int(256));

test('base64Table is standard padded base64 at every length 0..300 and at a full commit\'s blob sizes', () => {
    for (const n of [...Array.from({ length: 301 }, (_, i) => i), 1227, 1500, 8192]) {
        const b = blob(n);
        assert.equal(base64Table(b), Buffer.from(b).toString('base64'), `length ${n}`);
    }
    const every = Uint8Array.from({ length: 256 }, (_, i) => i);
    assert.equal(base64Table(every), Buffer.from(every).toString('base64'), 'every byte value');
    assert.equal(base64(every), Buffer.from(every).toString('base64'), 'the exported encoder agrees');
});

test('columnHexToBytes reads a BYTEA column\'s \\x-hex and a TEXT column\'s bare hex, and refuses what is not whole hex', () => {
    const b = blob(64);
    const hex = Buffer.from(b).toString('hex');
    assert.deepEqual(columnHexToBytes(`\\x${hex}`), b);
    assert.deepEqual(columnHexToBytes(hex), b);
    assert.deepEqual(columnHexToBytes(hex.toUpperCase()), b);
    assert.deepEqual(columnHexToBytes('\\x'), new Uint8Array(0), 'an empty BYTEA');
    assert.throws(() => columnHexToBytes(`\\x${hex}0`), /odd number of digits/);
    assert.throws(() => columnHexToBytes('\\x0g'), /non-hex character/);
});
