import { useEffect, useState } from 'react';
import { getCachedTexture, setCachedTexture } from '../utils/textureCache';
import { paintWool, WOOL_MIME, WOOL_QUALITY } from '../utils/woolPixels';
import type { WoolReply } from '../utils/woolWorker';

// Global cache for wool texture to prevent regeneration - using blob URL to reduce JS heap memory
let woolTextureBlobUrl: string | null = null;
let woolTexturePromise: Promise<string> | null = null;

// Generate random offsets once per page load
let globalRandomOffsetX: number | null = null;
let globalRandomOffsetY: number | null = null;

const getRandomOffsets = () => {
  if (globalRandomOffsetX === null || globalRandomOffsetY === null) {
    globalRandomOffsetX = Math.random() * 1000 - 500;
    globalRandomOffsetY = Math.random() * 1000 - 500;
  }
  return { offsetX: globalRandomOffsetX, offsetY: globalRandomOffsetY };
};

// A worker that has not answered by then is taken for dead (a phone can kill a
// worker for memory without an error event), and the page weaves the texture
// itself rather than leave the background flat for good.
const WORKER_DEADLINE_MS = 20000;

// The weave on the wool worker, off the page's thread. Null when this browser has
// no worker canvas, or the worker fails: the caller weaves on the page instead.
function weaveOnWorker(width: number, height: number): Promise<string | null> {
  if (typeof Worker === 'undefined' || typeof OffscreenCanvas === 'undefined') return Promise.resolve(null);
  return new Promise((resolve) => {
    let worker: Worker;
    try {
      worker = new Worker(new URL('../utils/woolWorker.ts', import.meta.url), { type: 'module' });
    } catch {
      resolve(null);
      return;
    }
    const done = (url: string | null) => {
      clearTimeout(deadline);
      worker.terminate();
      resolve(url);
    };
    const deadline = setTimeout(() => done(null), WORKER_DEADLINE_MS);
    worker.onmessage = (e: MessageEvent<WoolReply>) => {
      if (e.data.ok && e.data.blob.size > 0) {
        done(URL.createObjectURL(e.data.blob));
      } else {
        console.error('Wool worker failed:', e.data.ok ? 'an empty image' : e.data.error);
        done(null);
      }
    };
    worker.onerror = () => done(null);
    worker.onmessageerror = () => done(null);
    worker.postMessage({ width, height, ...getRandomOffsets() });
  });
}

// The weave on the page's thread, letting a frame through between fibre batches.
async function weaveOnPage(width: number, height: number): Promise<string> {
  const canvas = document.createElement('canvas');
  canvas.width = width;
  canvas.height = height;
  const ctx = canvas.getContext('2d');
  if (!ctx) {
    return '';
  }
  const imageData = ctx.createImageData(width, height);
  await paintWool(imageData.data, { width, height, ...getRandomOffsets() }, () => new Promise<void>((resolve) => {
    if ('requestIdleCallback' in window) {
      requestIdleCallback(() => resolve(), { timeout: 50 });
    } else {
      setTimeout(resolve, 0);
    }
  }));
  ctx.putImageData(imageData, 0, 0);

  // A null blob (Safari canvas memory limit) must reject so callers can retry
  // smaller, not cache ''.
  return new Promise((resolve, reject) => {
    canvas.toBlob((blob) => {
      if (blob) {
        resolve(URL.createObjectURL(blob));
      } else {
        reject(new Error('canvas.toBlob returned null (canvas memory limit?)'));
      }
    }, WOOL_MIME, WOOL_QUALITY);
  });
}

// The page background, woven once per session and kept in IndexedDB after that.
async function generateWoolTexture(): Promise<string> {
  // Return cached texture if available
  if (woolTextureBlobUrl) {
    console.log('Using cached wool texture');
    return Promise.resolve(woolTextureBlobUrl);
  }

  // Return existing promise if generation is already in progress
  if (woolTexturePromise) {
    console.log('Wool texture generation already in progress, reusing promise');
    return woolTexturePromise;
  }

  // Create and cache the promise to prevent duplicate generations
  woolTexturePromise = (async () => {
    try {
      // Check IndexedDB cache first for persistent storage
      const cachedFromDB = await getCachedTexture('wool').catch(() => null);
      if (cachedFromDB) {
        woolTextureBlobUrl = cachedFromDB;
        return cachedFromDB;
      }

      const startTime = performance.now();

      // The texture must stay 4K: the weave's apparent scale is tied to the
      // pixel grid, so a 1080p render shows up 2x zoomed-in on screen. The
      // weave peaks around ~35 MB at 4K, which a phone holds; only if even that
      // fails do we accept a zoomed 1080p texture over a flat background.
      let blobUrl = await weaveOnWorker(3840, 2160);
      if (!blobUrl) {
        // Yield control first to let React render.
        await new Promise(resolve => setTimeout(resolve, 0));
        try {
          blobUrl = await weaveOnPage(3840, 2160);
          if (!blobUrl) throw new Error('empty blob at 4K');
        } catch (err) {
          console.error('4K wool generation failed, retrying at 1080p:', err);
          blobUrl = await weaveOnPage(1920, 1080);
        }
      }
      if (!blobUrl) {
        throw new Error('wool texture generation produced no blob');
      }
      woolTextureBlobUrl = blobUrl;

      // Cache to IndexedDB for persistence across sessions
      setCachedTexture('wool', blobUrl).catch(() => {
      });

      const endTime = performance.now();
      const generationTime = endTime - startTime;
      console.log(`Wool texture generated in ${generationTime.toFixed(2)}ms`);

      return blobUrl;
    } finally {
      // Success cached woolTextureBlobUrl above; on failure this lets the
      // next caller retry instead of reusing a dead rejected promise forever.
      woolTexturePromise = null;
    }
  })();

  return woolTexturePromise;
}

// Hook to get wool texture blob URL with lazy loading
export const useWoolTexture = () => {
  const [textureUrl, setTextureUrl] = useState<string | null>(woolTextureBlobUrl);

  useEffect(() => {
    if (woolTextureBlobUrl) {
      // If already cached, set it immediately
      setTextureUrl(woolTextureBlobUrl);
      return;
    }
    let cancelled = false;
    let retryTimer: ReturnType<typeof setTimeout> | null = null;
    const attempt = (retriesLeft: number) => {
      generateWoolTexture()
        .then((url) => {
          if (cancelled) return;
          if (url) {
            setTextureUrl(url);
          } else if (retriesLeft > 0) {
            retryTimer = setTimeout(() => attempt(retriesLeft - 1), 1500);
          }
        })
        .catch((err) => {
          console.error('Wool texture failed, retrying:', err);
          if (!cancelled && retriesLeft > 0) {
            retryTimer = setTimeout(() => attempt(retriesLeft - 1), 1500);
          }
        });
    };
    attempt(2);
    return () => {
      cancelled = true;
      if (retryTimer) clearTimeout(retryTimer);
    };
  }, []);

  return textureUrl;
};



export default generateWoolTexture;
