// cast.ts - a realtime payload as the suites take it apart.
//
// A broadcast's payload is { b } and nothing else: the kernel's cast (c/src/
// evwire.h - the committed version, then the as3 push), base64. The product
// reads it whole (client_table.ts readCast). A suite that wants the parts - the
// version a message names, the bare push to walk byte by byte - gets them here:
// the version through the kernel, the push by the header length the kernel's
// own constant states.
import assert from 'node:assert/strict';
import { base64ToBytes } from '../../sdk/ts/wire/bytes.ts';
import { clientTable } from '../../sdk/ts/table/client_table.ts';
import * as V from '../../sdk/ts/gen/view_layout.bots.ts';

export interface CastParts {
    /** The whole payload: what client_cast_open reads. */
    cast: Uint8Array;
    /** The as3 push behind the header: what readPush({ as3: true }) reads. */
    push: Uint8Array;
    /** The committed version the header names. */
    version: number;
}

/** The parts of a broadcast payload, which must be exactly { b }. */
export function castParts(payload: unknown, what = 'payload'): CastParts {
    assert.deepEqual(Object.keys(payload as object), ['b'], `${what}: the payload is exactly { b } - the cast and nothing beside it`);
    const b = (payload as { b: unknown }).b;
    assert.equal(typeof b, 'string', `${what}: b is base64 text`);
    const cast = base64ToBytes(b as string);
    const version = clientTable().castVersion(cast);
    assert.notEqual(version, null, `${what}: the bytes open with a cast header`);
    return { cast, push: cast.subarray(V.EVW_CAST_HEADER_BYTES), version: version as number };
}
