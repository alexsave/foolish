// The shell's routing (lib/shell.ts). The edge serves out/_shell.html, with a
// 200, for every path that is not a file or a known page, and has no
// redirects, so the app says what an address is: a moved address, a code, or
// no game.
//
//   npm test
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { loadTs } from '../test-support/ts_load.mjs';

const { shellRoute } = await loadTs('lib/shell.ts');

test('/234 goes to /243, its search kept, trailing slash or not', () => {
    assert.deepEqual(shellRoute('/234', ''), { redirect: '/243' });
    assert.deepEqual(shellRoute('/234/', '?seed=00ff'), { redirect: '/243?seed=00ff' });
});

test('one segment is a code, decoded, trailing slashes ignored', () => {
    assert.deepEqual(shellRoute('/9ABCDEF', ''), { code: '9ABCDEF' });
    assert.deepEqual(shellRoute('/9ABCDEF/', '?x=1'), { code: '9ABCDEF' });
    assert.deepEqual(shellRoute('/a%20b', ''), { code: 'a b' });
    assert.deepEqual(shellRoute('/2345', ''), { code: '2345' });
});

test('a deeper path, the root and a bad escape hold no game', () => {
    assert.deepEqual(shellRoute('/9ABCDEF/more', ''), { notFound: true });
    assert.deepEqual(shellRoute('/234/x', ''), { notFound: true });
    assert.deepEqual(shellRoute('/', ''), { notFound: true });
    assert.deepEqual(shellRoute('//9ABCDEF', ''), { notFound: true });
    assert.deepEqual(shellRoute('/%E0%A4%A', ''), { notFound: true });
});
