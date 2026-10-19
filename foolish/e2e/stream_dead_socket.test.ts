/* =============================================================================
 * The client's "this socket is dead" frame closes the socket and reopens it
 * =============================================================================
 * The kernel's netclient hands out ONE frame of length 0 (nc_stream_next) when a
 * PINGREQ went ten seconds with nothing heard: the half-open socket the OS still
 * calls open. backend.ts Stream.send must close that socket at once and open a
 * new one after the delay the client names (nc_stream_closed), and must not send
 * the empty frame. Run against a fake client and a fake WebSocket: the kernel's
 * side of it is held by server/test/unit_netclient.c.
 * ========================================================================== */

/* MUTATION CHECKS (backend.ts Stream.send, each made, seen red, restored):
 *   the len-0 branch without ws.close()            "the dead socket is closed"
 *   the len-0 branch without this.lost(ws)         "a new socket after the delay"
 *   the len-0 branch removed (empty frame sent)    "the dead socket is closed"
 *   lost() without its `this.socket !== ws` guard  "a late onclose ... reopens nothing"
 */

import { test, mock } from 'node:test';
import assert from 'node:assert/strict';

process.env.NEXT_PUBLIC_BACKEND_URL = 'http://h.test';

class FakeWS {
    static OPEN = 1;
    static all: FakeWS[] = [];
    readyState = 1;
    binaryType = '';
    sent: Uint8Array[] = [];
    closed = 0;
    onopen: (() => void) | null = null;
    onclose: (() => void) | null = null;
    onmessage: ((m: unknown) => void) | null = null;
    constructor(readonly url: URL, readonly protocol: string) { FakeWS.all.push(this); }
    send(b: Uint8Array) { this.sent.push(b); }
    close() { this.closed++; this.readyState = 3; }
}
(globalThis as any).WebSocket = FakeWS;

// The client: a queue of frames for the socket, a retry delay it names on a close.
const frames: Uint8Array[] = [];
const calls: string[] = [];
const fakeClient = {
    session: () => ({ signedIn: true, userId: 'u1', username: 'Me' }),
    load: () => {}, reset: () => {}, sessionBlob: () => null,
    nextRequest: () => null, nextDone: () => null, dueMs: () => -1,
    subscribe: (what: number, gid: string) => { calls.push(`sub ${what} ${gid}`); },
    unsubscribe: () => {},
    streamPath: () => '/v1/stream',
    streamNext: () => frames.shift() ?? null,
    streamOpened: () => { calls.push('opened'); },
    streamClosed: () => { calls.push('closed'); return 5; },
    streamIn: () => {},
};
mock.module('../sdk/ts/net/net_client.ts', { namedExports: { netClient: () => fakeClient, NetConst: new Proxy({}, { get: () => 1 }) } });
mock.module('../sdk/ts/wasm/bots.ts', { namedExports: { ensureBotsAsync: async () => {} } });

const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));

test('a len-0 frame closes the socket, sends nothing, and a new socket opens after the client\'s delay', async () => {
    const { stream } = await import('../src/common/backend.ts');
    const release = stream.game('g1');
    assert.equal(FakeWS.all.length, 1, 'holding a game opens the socket');
    const first = FakeWS.all[0];
    first.onopen!();
    // A live frame is sent as it is.
    frames.push(new Uint8Array([1, 2, 3]));
    stream.send();
    assert.deepEqual(first.sent.map((b) => [...b]), [[1, 2, 3]]);
    // The dead-socket frame: closed, not sent, and the frames behind it wait for the next socket.
    frames.push(new Uint8Array(0), new Uint8Array([9]));
    stream.send();
    assert.equal(first.closed, 1, 'the dead socket is closed');
    assert.equal(first.sent.length, 1, 'the empty frame is not sent');
    assert.equal(frames.length, 1, 'the frame behind it is left for the new socket');
    assert.ok(calls.includes('closed'), 'the client was told it closed (it names the delay)');
    assert.equal(FakeWS.all.length, 1, 'not reopened before the delay');
    await sleep(40);
    assert.equal(FakeWS.all.length, 2, 'a new socket after the delay');
    // The old socket's own close event is ignored: no third socket.
    first.onclose!();
    await sleep(40);
    assert.equal(FakeWS.all.length, 2, 'a late onclose of the dead socket reopens nothing');
    release();
    stream.stop();
});
