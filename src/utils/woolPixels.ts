// The wool weave, painted into an RGBA buffer. One function for both places the
// texture is made: the wool worker (src/utils/woolWorker.ts), which keeps the
// work off the page's thread, and the page itself when a browser has no worker
// canvas (WoolBackground.tsx). Horizontal fibres first, then vertical ones, over
// a brown base; the offsets move the red plaid so each page load weaves its own.

export interface WoolWeave {
    width: number;
    height: number;
    offsetX: number;
    offsetY: number;
}

/**
 * Paints the weave into `data` (width * height * 4 bytes). `pause` is awaited
 * every few million fibre steps, so a caller on the page's thread can let a
 * frame through; the worker passes none.
 */
export async function paintWool(data: Uint8ClampedArray, weave: WoolWeave, pause?: () => Promise<void>): Promise<void> {
    const { width, height, offsetX, offsetY } = weave;
    const C = Math.cos;
    const S = Math.sin;
    const T = Math.tan;

    const BASE_R = 113;
    const BASE_G = 65;
    const BASE_B = 27;
    for (let i = 0; i < data.length; i += 4) {
        data[i] = BASE_R;
        data[i + 1] = BASE_G;
        data[i + 2] = BASE_B;
        data[i + 3] = 255;
    }

    let r = 0;
    const zValue = () => {
        const cr = C(r) * 1000;
        return cr - Math.floor(cr);
    };
    let h = 1;
    let u = 0;

    const maxIterations = Math.floor((width * height / (1920 * 1080)) * 2000000);
    const switchPoint = Math.floor(maxIterations * 0.4);
    const CHUNK_SIZE = 5000000;

    const writePixel = (x: number, y: number, red: number, green: number, blue: number, size: number) => {
        const xi = Math.floor(x);
        const yi = Math.floor(y);
        const sizeInt = Math.ceil(size);
        const fx = x - xi;
        const fy = y - yi;
        for (let dy = 0; dy < sizeInt; dy++) {
            for (let dx = 0; dx < sizeInt; dx++) {
                const px = xi + dx;
                const py = yi + dy;
                if (px >= 0 && px < width && py >= 0 && py < height) {
                    const idx = (py * width + px) << 2;
                    const distX = Math.abs(dx - fx);
                    const distY = Math.abs(dy - fy);
                    const edgeSoftness = Math.max(0.1, 1 - (distX + distY) / 4);
                    const alpha = edgeSoftness * 0.99;
                    const invAlpha = 1 - alpha;
                    data[idx] = data[idx] * invAlpha + red * alpha;
                    data[idx + 1] = data[idx + 1] * invAlpha + green * alpha;
                    data[idx + 2] = data[idx + 2] * invAlpha + blue * alpha;
                    data[idx + 3] = 255;
                }
            }
        }
    };

    for (let i = 0; i < maxIterations; i++) {
        if (pause && i > 0 && i % CHUNK_SIZE === 0) await pause();

        if (i === switchPoint) {
            h = 0;
            r = 0;
        }

        if (h) {
            // Horizontal fibres
            if (i % width === 0) {
                u = zValue() * 500 + 100;
                r += 5;
            }
            const phase = S(i / u);
            const dx = S(i - 1) + phase * 6;
            const zVal = zValue();
            const red = ((T((Math.floor((r + offsetX) / 80 + zVal / 4)) ^ (Math.floor((i % height + offsetY) / 80)))) > 0.3) ? 100 : 0;
            const x = r + dx;
            const y = i % height;
            if (x >= 0 && x < width && y >= 0 && y < height) {
                writePixel(x, y, (209 + 46 * phase + red) | 0, (208 + 45 * phase - red) | 0, (183 + 53 * phase - red / 2) | 0, 2);
            }
        } else {
            // Vertical fibres
            if (i % height === 0) {
                u = zValue() * 500 + 100;
                r += 3;
            }
            const phase = S(i / u);
            const zVal = zValue();
            const red = ((T((Math.floor((r + offsetY) / 80 + zVal / 4)) ^ (Math.floor((i % width + offsetX) / 80)))) > 0.3) ? 100 : 0;
            const dx = 4 * S(i - 1) + S(i / u) * 4;
            const x = i % width;
            const y = r + dx;
            if (x >= 0 && x < width && y >= 0 && y < height) {
                writePixel(x, y, (189 + 46 * phase + red) | 0, (188 + 45 * phase - red) | 0, (163 + 53 * phase - red / 2) | 0, 1.4 * (phase + 1.7));
            }
        }
    }
}

/**
 * How the weave is kept: a JPEG. The 4K weave is noise to a lossless encoder -
 * as a PNG it is about 24 MB and took about 2 s to encode before the page could
 * show it, a flat brown screen all that time on a first visit. As a JPEG it is
 * about a fifth of that, encodes in a fraction of the time, and decodes faster.
 */
export const WOOL_MIME = 'image/jpeg';
export const WOOL_QUALITY = 0.92;
