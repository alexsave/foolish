/* =============================================================================
 * The wool background on a first visit: woven off the page's thread, kept as a JPEG
 * =============================================================================
 * On a first visit (nothing in IndexedDB yet) the welcome page stood flat brown
 * for about 3 s before the wool appeared. Measured in Chrome: the weave itself
 * took about 0.8 s, and encoding the 3840x2160 result as a lossless PNG (24 MB,
 * the weave is noise to a PNG encoder) took about 2 s more, on a page thread the
 * wood and title textures were also painting on. Now the weave runs on a worker
 * (src/utils/woolWorker.ts) and is kept as a JPEG (about 5 MB), and the wool is
 * on screen after about 1.6 s; a later visit decodes the smaller image too.
 *
 * These hold the parts a browser run does not pin down by itself: the page
 * hands the weave to a worker when it can, weaves on its own thread only when
 * it cannot, and neither path encodes a PNG.
 * ========================================================================== */

import { test, after } from 'node:test';
import assert from 'node:assert/strict';
import { JSDOM } from 'jsdom';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; console.error = () => {}; }

const dom = new JSDOM('<!DOCTYPE html><html><body></body></html>', { url: 'http://localhost/', pretendToBeVisual: true });
const g = globalThis as any;
g.window = dom.window;
g.document = dom.window.document;
after(() => { delete g.Worker; delete g.OffscreenCanvas; });

// Each case loads its own copy of the module: the texture is cached per page load.
let copy = 0;
const freshWool = async () => (await import(`../src/components/WoolBackground.tsx?case=${++copy}`)).default as () => Promise<string>;

interface Weave { width: number; height: number; offsetX: number; offsetY: number }

test('the page hands the weave to a worker and paints nothing itself', async () => {
    const posted: Weave[] = [];
    let canvases = 0;
    const realCreate = dom.window.document.createElement.bind(dom.window.document);
    dom.window.document.createElement = ((tag: string) => {
        if (tag.toLowerCase() === 'canvas') canvases++;
        return realCreate(tag);
    }) as typeof document.createElement;
    g.OffscreenCanvas = class {};
    g.Worker = class {
        onmessage: ((e: { data: unknown }) => void) | null = null;
        onerror: (() => void) | null = null;
        onmessageerror: (() => void) | null = null;
        terminated = false;
        postMessage(job: Weave) {
            posted.push(job);
            setTimeout(() => this.onmessage?.({ data: { ok: true, blob: new Blob([new Uint8Array([0xff, 0xd8, 0xff])], { type: 'image/jpeg' }) } }), 5);
        }
        terminate() { this.terminated = true; }
    };
    try {
        const generate = await freshWool();
        const url = await generate().catch((e: Error) => `refused: ${e.message}`);
        assert.equal(posted.length, 1, 'one weave is posted to a worker');
        assert.deepEqual([posted[0].width, posted[0].height], [3840, 2160], 'at 4K: the weave\'s scale is tied to its pixel grid');
        assert.ok(Number.isFinite(posted[0].offsetX) && Number.isFinite(posted[0].offsetY), 'with this page load\'s plaid offsets');
        assert.equal(canvases, 0, 'the page\'s thread makes no canvas of its own');
        assert.match(url, /^blob:/, `the background is the worker's image (${url})`);
    } finally {
        dom.window.document.createElement = realCreate as typeof document.createElement;
        delete g.Worker;
        delete g.OffscreenCanvas;
    }
});

test('without a worker canvas the page weaves it, and keeps a JPEG, not a PNG', async () => {
    const encoded: string[] = [];
    const proto = dom.window.HTMLCanvasElement.prototype as any;
    const realGetContext = proto.getContext, realToBlob = proto.toBlob;
    proto.getContext = function getContext() {
        return {
            createImageData: (w: number, h: number) => ({ data: new Uint8ClampedArray(w * h * 4) }),
            putImageData: () => {},
        };
    };
    proto.toBlob = function toBlob(cb: (b: Blob | null) => void, type?: string) {
        encoded.push(type ?? 'image/png');
        cb(new Blob([new Uint8Array(4)], { type: type ?? 'image/png' }));
    };
    try {
        const generate = await freshWool();
        const url = await generate();
        assert.match(url, /^blob:/, 'the page wove a background');
        assert.deepEqual(encoded, ['image/jpeg'], 'one encode, as a JPEG');
    } finally {
        proto.getContext = realGetContext;
        proto.toBlob = realToBlob;
    }
});

test('the worker weaves every pixel opaque and encodes a JPEG', async () => {
    const encodes: { type?: string; quality?: number }[] = [];
    let painted: Uint8ClampedArray | null = null;
    g.self ??= globalThis;
    g.OffscreenCanvas = class {
        constructor(readonly width: number, readonly height: number) {}
        getContext() {
            return {
                createImageData: (w: number, h: number) => ({ data: new Uint8ClampedArray(w * h * 4) }),
                putImageData: (image: { data: Uint8ClampedArray }) => { painted = image.data; },
            };
        }
        convertToBlob(opts: { type?: string; quality?: number }) {
            encodes.push(opts);
            return Promise.resolve(new Blob([new Uint8Array(4)], { type: opts.type }));
        }
    };
    try {
        const { weave } = await import('../src/utils/woolWorker.ts');
        const reply = await weave({ width: 192, height: 108, offsetX: 12.5, offsetY: -40 });
        assert.equal(reply.ok, true, `the worker answers with an image: ${JSON.stringify(reply)}`);
        assert.deepEqual(encodes.map((e) => e.type), ['image/jpeg'], 'encoded once, as a JPEG');
        assert.ok(encodes[0].quality! >= 0.9, `at a quality the weave keeps its fibres (${encodes[0].quality})`);
        assert.ok(painted, 'the weave was put on the canvas');
        const data = painted as Uint8ClampedArray;
        let transparent = 0, base = 0;
        for (let i = 0; i < data.length; i += 4) {
            if (data[i + 3] !== 255) transparent++;
            if (data[i] === 113 && data[i + 1] === 65 && data[i + 2] === 27) base++;
        }
        assert.equal(transparent, 0, 'no pixel is left transparent');
        assert.ok(base < data.length / 4 / 2, `the fibres cover most of the brown base (${base} bare pixels)`);
    } finally {
        delete g.OffscreenCanvas;
    }
});
