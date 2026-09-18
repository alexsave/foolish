// The wool worker: weaves the page background off the page's thread and hands
// back the encoded image (src/components/WoolBackground.tsx). On the page's
// thread the weave, and the textures painted beside it (the wood, the title's
// fern), queued behind one another; here the weave runs alongside them.

import { paintWool, WOOL_MIME, WOOL_QUALITY, type WoolWeave } from './woolPixels';

export type WoolReply = { ok: true; blob: Blob } | { ok: false; error: string };

interface WorkerScope {
    postMessage(m: WoolReply): void;
    onmessage: ((e: MessageEvent<WoolWeave>) => void) | null;
}

export async function weave(job: WoolWeave): Promise<WoolReply> {
    try {
        const canvas = new OffscreenCanvas(job.width, job.height);
        const ctx = canvas.getContext('2d');
        if (!ctx) return { ok: false, error: 'no 2d context on the worker canvas' };
        const image = ctx.createImageData(job.width, job.height);
        await paintWool(image.data, job);
        ctx.putImageData(image, 0, 0);
        const blob = await canvas.convertToBlob({ type: WOOL_MIME, quality: WOOL_QUALITY });
        return { ok: true, blob };
    } catch (e) {
        return { ok: false, error: e instanceof Error ? e.message : String(e) };
    }
}

const scope = self as unknown as WorkerScope;
scope.onmessage = (e) => {
    void weave(e.data).then((reply) => scope.postMessage(reply));
};
