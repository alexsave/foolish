// The app shell (out/_shell.html) is what the edge serves for every address
// that is not a file and not a known page (foolish/cbase.conf's CBASE_SHELL),
// so the app decides what such an address is: shellSegment says which ones are
// a game path (one segment) and which get the app's own not-found view.
//
// Pure test - no Postgres, no network, no compiler, no wasm.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { shellSegment } from '../../src/utils/gameShell.ts';

test('one segment is the game path, trailing slashes ignored, decoded', () => {
    assert.equal(shellSegment('/ab12cd'), 'ab12cd');
    assert.equal(shellSegment('/ab12cd/'), 'ab12cd');
    assert.equal(shellSegment('/ab12cd//'), 'ab12cd');
    assert.equal(shellSegment('/Replay%20Code'), 'Replay Code');
});

test('a deeper path, the root and a bad escape are no game path', () => {
    assert.equal(shellSegment('/ab12cd/extra'), null);
    assert.equal(shellSegment('/a/b/c'), null);
    assert.equal(shellSegment('/'), null);
    assert.equal(shellSegment(''), null);
    assert.equal(shellSegment('//ab12cd'), null);
    assert.equal(shellSegment('/%E0%A4%A'), null);
});
