/* =============================================================================
 * The fern on the card back: one point generator, two renderers, the same fern
 * =============================================================================
 * src/utils/fernFractal.tsx draws the fern with WebGL, and on a 2D canvas when
 * there is no WebGL. Each renderer used to carry its own copy of the iterated
 * function system that computes the points (and each copy wrote the
 * rotate-scale-store step three times). There is one generator now and each
 * renderer is a sink for its points.
 *
 * The points come off Math.random, so the fern is a function of the ORDER the
 * generator draws in: one draw more, fewer, or moved, and every later point
 * is a different point. These cases run the shipped title fern (12M points on
 * 1400x1960) off a seeded stream through each renderer and hold what reaches
 * the sink - the two vertex buffers handed to WebGL, the pixels put on the 2D
 * canvas - to digests recorded from the code before the two copies became
 * one, along with the number of draws taken.
 *
 * Only the canvas is stubbed: jsdom has none.
 * ========================================================================== */

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { JSDOM } from 'jsdom';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; console.error = () => {}; }

const dom = new JSDOM('<!DOCTYPE html><html><body></body></html>', { url: 'http://localhost/', pretendToBeVisual: true });
const g = globalThis as any;
g.window = dom.window;
g.document = dom.window.document;

const WIDTH = 1400, HEIGHT = 1960, POINTS = 12000000;

const digest = (a: Float32Array | Uint8ClampedArray) =>
    createHash('sha256').update(new Uint8Array(a.buffer, a.byteOffset, a.byteLength)).digest('hex');

/** What one render handed its sink. */
interface Sink { vertexBuffers: Float32Array[]; uniforms: number[][]; drawn: number[]; pixels: Uint8ClampedArray | null; draws: number }

/** Renders the title fern off the stream `seed` starts, with or without WebGL. */
async function titleFern(seed: number, webgl: boolean, copy: string): Promise<Sink> {
    const sink: Sink = { vertexBuffers: [], uniforms: [], drawn: [], pixels: null, draws: 0 };
    const gl = {
        VERTEX_SHADER: 1, FRAGMENT_SHADER: 2, ARRAY_BUFFER: 3, STATIC_DRAW: 4, FLOAT: 5, POINTS: 6, COLOR_BUFFER_BIT: 7,
        createShader: () => ({}), shaderSource() {}, compileShader() {}, createProgram: () => ({}), attachShader() {},
        linkProgram() {}, useProgram() {}, createBuffer: () => ({}), bindBuffer() {},
        bufferData: (_target: number, data: Float32Array) => { sink.vertexBuffers.push(data); },
        getAttribLocation: () => 0, enableVertexAttribArray() {}, vertexAttribPointer() {}, getUniformLocation: () => ({}),
        uniform2f: (_at: unknown, a: number, b: number) => { sink.uniforms.push([a, b]); },
        uniform1f: (_at: unknown, a: number) => { sink.uniforms.push([a]); },
        clearColor() {}, clear() {}, flush() {},
        drawArrays: (_mode: number, first: number, count: number) => { sink.drawn.push(first, count); },
    };
    const proto = dom.window.HTMLCanvasElement.prototype as any;
    const realGetContext = proto.getContext, realToBlob = proto.toBlob, realRandom = Math.random;
    proto.getContext = function getContext(kind: string) {
        if (kind === 'webgl') return webgl ? gl : null;
        return {
            createImageData: (w: number, h: number) => ({ data: new Uint8ClampedArray(w * h * 4) }),
            putImageData: (image: { data: Uint8ClampedArray }) => { sink.pixels = image.data; },
        };
    };
    proto.toBlob = function toBlob(cb: (b: Blob | null) => void) { cb(new Blob([new Uint8Array(4)], { type: 'image/png' })); };
    let s = seed >>> 0;
    Math.random = () => { sink.draws++; s = (s * 1664525 + 1013904223) >>> 0; return s / 4294967296; };
    try {
        // Each case loads its own copy of the module: the title fern is made once per page load.
        const { generateTitleFern } = await import(`../src/utils/fernFractal.tsx?case=${copy}`);
        assert.match(await generateTitleFern(), /^blob:/, 'the fern is an image');
    } finally {
        Math.random = realRandom;
        proto.getContext = realGetContext;
        proto.toBlob = realToBlob;
    }
    return sink;
}

test('WebGL is handed the same positions and colours, off the same number of draws', async () => {
    const sink = await titleFern(1, true, 'webgl');
    assert.equal(sink.draws, 25440838, 'draws from the random stream');
    assert.deepEqual(sink.uniforms, [[WIDTH, HEIGHT], [2]], 'resolution, then a 2 px point');
    assert.deepEqual(sink.drawn, [0, POINTS], 'every iteration is one point');
    assert.equal(sink.pixels, null, 'nothing is painted on a 2D canvas');
    const [positions, colors] = sink.vertexBuffers;
    assert.equal(sink.vertexBuffers.length, 2, 'two vertex buffers');
    assert.equal(positions.length, POINTS * 2);
    assert.equal(colors.length, POINTS * 3);
    assert.equal(digest(positions), '12c6268a4eccfa0b9634b43dd0e10f6232dda4cad5eabe162ef1c1cf726b2eb6', 'the positions, point for point');
    assert.equal(digest(colors), '1b450a28dc48e2961c7cb97cf028840f5eb0327664b95bd4d10267f100f9407c', 'the colours, point for point');
});

test('without WebGL the 2D canvas is painted the same pixels', async () => {
    const sink = await titleFern(1, false, '2d');
    // The points are computed before WebGL is asked for, so this path computes them twice.
    assert.equal(sink.draws, 50884422, 'draws from the random stream, over both passes');
    assert.equal(sink.vertexBuffers.length, 0, 'nothing reaches WebGL');
    assert.ok(sink.pixels, 'the canvas was painted');
    assert.equal(sink.pixels.length, WIDTH * HEIGHT * 4);
    assert.equal(digest(sink.pixels), '9c51cfc05121234948dfa83cf87b3214182c840c79583bacb02eda44cf8c07b6', 'the image, pixel for pixel');
});
