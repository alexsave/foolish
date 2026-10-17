// cast.ts - a pushed cast as the suites take it apart.
//
// A cast is the kernel's bytes (c/src/evwire.h - the committed version, then
// the as3 push), carried whole by the stream's NW_WS_CAST. The product reads it
// whole (client_table.ts readCast). A suite that wants the parts - the version a
// cast names, the bare push to walk byte by byte - gets them here: the version
// through the kernel, the push by the header length the kernel's own constant
// states.
import assert from 'node:assert/strict';
import { clientTable } from '../../sdk/ts/table/client_table.ts';
import * as V from '../../sdk/ts/gen/view_layout.bots.ts';

export interface CastParts {
    /** The whole cast: what client_cast_open reads. */
    cast: Uint8Array;
    /** The as3 push behind the header: what readPush({ as3: true }) reads. */
    push: Uint8Array;
    /** The committed version the header names. */
    version: number;
}

/** The parts of a cast. */
export function castParts(cast: Uint8Array, what = 'cast'): CastParts {
    const version = clientTable().castVersion(cast);
    assert.notEqual(version, null, `${what}: the bytes open with a cast header`);
    return { cast, push: cast.subarray(V.EVW_CAST_HEADER_BYTES), version: version as number };
}
