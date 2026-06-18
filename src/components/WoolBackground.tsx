import { useEffect, useState } from 'react';
import { getCachedTexture, setCachedTexture } from '../utils/textureCache';
import { PixelData, pixelsToPointArrays, generateTextureViaWebGL } from '../utils/webglTexture';

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

// Generate wool pixel data in JavaScript (like fernFractal generates points/colors)
async function generateWoolPixelData(width: number, height: number): Promise<PixelData> {
  const { offsetX, offsetY } = getRandomOffsets();
  
  const C = Math.cos;
  const S = Math.sin;
  const T = Math.tan;

  // We'll generate positions and colors for each pixel that differs from base
  // Start with base brown color filled
  const BASE_R = 113 / 255;
  const BASE_G = 65 / 255;
  const BASE_B = 27 / 255;
  
  // Temporary storage for all pixels
  const pixelColors = new Float32Array(width * height * 3);
  
  // Pre-fill with base color
  for (let i = 0; i < width * height; i++) {
    pixelColors[i * 3] = BASE_R;
    pixelColors[i * 3 + 1] = BASE_G;
    pixelColors[i * 3 + 2] = BASE_B;
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

  const writePixel = (x: number, y: number, red: number, green: number, blue: number, size: number = 2) => {
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
          const idx = (py * width + px) * 3;
          
          const distX = Math.abs(dx - fx);
          const distY = Math.abs(dy - fy);
          const edgeSoftness = Math.max(0.1, 1 - (distX + distY) / 4);
          const alpha = edgeSoftness * 0.99;
          const invAlpha = 1 - alpha;
          
          pixelColors[idx] = pixelColors[idx] * invAlpha + red * alpha;
          pixelColors[idx + 1] = pixelColors[idx + 1] * invAlpha + green * alpha;
          pixelColors[idx + 2] = pixelColors[idx + 2] * invAlpha + blue * alpha;
        }
      }
    }
  };

  for (let i = 0; i < maxIterations; i++) {
    if (i > 0 && i % CHUNK_SIZE === 0) {
      await new Promise(resolve => setTimeout(resolve, 0));
    }
    
    if (i === switchPoint) {
      h = 0;
      r = 0;
    }

    if (h) {
      // Horizontal wool fiber phase
      if (i % width === 0) {
        u = zValue() * 500 + 100;
        r += 5;
      }
      
      const phase = S(i / u);
      const dx = S(i - 1) + phase * 6;
      
      const zVal = zValue();
      const red = ((T((Math.floor((r + offsetX) / 80 + zVal / 4)) ^ (Math.floor((i % height + offsetY) / 80)))) > 0.3) ? 100 : 0;
      
      const colorR = ((209 + 46 * phase + red) / 255);
      const colorG = ((208 + 45 * phase - red) / 255);
      const colorB = ((183 + 53 * phase - red / 2) / 255);
      
      const x = r + dx;
      const y = i % height;
      
      if (x >= 0 && x < width && y >= 0 && y < height) {
        writePixel(x, y, colorR, colorG, colorB, 2);
      }
    } else {
      // Vertical wool fiber phase
      if (i % height === 0) {
        u = zValue() * 500 + 100;
        r += 3;
      }
      
      const phase = S(i / u);
      const zVal = zValue();
      const red = ((T((Math.floor((r + offsetY) / 80 + zVal / 4)) ^ (Math.floor((i % width + offsetX) / 80)))) > 0.3) ? 100 : 0;
      const dx = 4 * S(i - 1) + S(i / u) * 4;
      
      const colorR = ((189 + 46 * phase + red) / 255);
      const colorG = ((188 + 45 * phase - red) / 255);
      const colorB = ((163 + 53 * phase - red / 2) / 255);
      
      const x = i % width;
      const y = r + dx;
      const pixelWidth = 1.4 * (phase + 1.7);
      
      if (x >= 0 && x < width && y >= 0 && y < height) {
        writePixel(x, y, colorR, colorG, colorB, pixelWidth);
      }
    }
  }

  return pixelsToPointArrays(pixelColors, width, height);
}

// GPU-accelerated wool texture rendering using WebGL (CPU generates, GPU draws)
const generateWoolTextureAsync = (width: number = 3840, height: number = 2160): Promise<string> =>
  generateTextureViaWebGL(
    width,
    height,
    'wool',
    [0, 0, 0, 1],
    generateWoolPixelData,
    generateWoolTextureFallback
  );

// Async generator function with chunked processing to yield control
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

      // Yield control immediately to let React render first
      await new Promise(resolve => setTimeout(resolve, 0));

      // The texture must stay 4K — the weave's apparent scale is tied to the
      // pixel grid, so a 1080p render shows up 2x zoomed-in on screen. What
      // kills iOS Safari is not the resolution but the WebGL point-cloud
      // path's ~270 MB of Float32Arrays; the CPU path peaks around ~35 MB at
      // 4K. So: desktop renders 4K via WebGL (fast), constrained devices
      // render 4K via CPU (chunked, idle-yielding), and only if even that
      // fails do we accept a zoomed 1080p texture over a flat background.
      const constrained =
        typeof navigator !== 'undefined' &&
        (/iP(hone|od|ad)/.test(navigator.userAgent) ||
          Math.min(window.screen.width, window.screen.height) < 500);
      let blobUrl: string;
      try {
        blobUrl = constrained
          ? await generateWoolTextureFallback(3840, 2160)
          : await generateWoolTextureAsync(3840, 2160);
        if (!blobUrl) throw new Error('empty blob at 4K');
      } catch (err) {
        console.error('4K wool generation failed, retrying at 1080p:', err);
        blobUrl = await generateWoolTextureFallback(1920, 1080);
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
      console.log(`Wool texture generated in ${generationTime.toFixed(2)}ms (CPU generate + WebGL render, blob URL)`);

      return blobUrl;
    } finally {
      // Success cached woolTextureBlobUrl above; on failure this lets the
      // next caller retry instead of reusing a dead rejected promise forever.
      woolTexturePromise = null;
    }
  })();

  return woolTexturePromise;
}

// CPU implementation (original code)
async function generateWoolTextureFallback(width: number, height: number): Promise<string> {
  const canvas = document.createElement('canvas');
  canvas.width = width;
  canvas.height = height;
  const ctx = canvas.getContext('2d');
  
  if (!ctx) {
    return '';
  }

  const C = Math.cos;
  const S = Math.sin;
  const T = Math.tan;

  const imageData = ctx.createImageData(width, height);
  const data = new Uint8ClampedArray(imageData.data.buffer);
  
  // Pre-fill with brown base color
  const BASE_R = 113;
  const BASE_G = 65;
  const BASE_B = 27;
  for (let i = 0; i < data.length; i += 4) {
    data[i] = BASE_R;
    data[i + 1] = BASE_G;
    data[i + 2] = BASE_B;
    data[i + 3] = 255;
  }
  
  const { offsetX, offsetY } = getRandomOffsets();

  let r = 0;
  const zValue = () => {
    const cr = C(r) * 1000;
    return cr - Math.floor(cr);
  };
  let h = 1;
  let u = 0;

  const maxIterations = Math.floor((width * height / (1920 * 1080)) * 2000000);
  const switchPoint = Math.floor(maxIterations * 0.4);
  
  // Yield every 10k iterations to stay more responsive
  const CHUNK_SIZE = 5000000;

  const writePixel = (x: number, y: number, r: number, g: number, b: number, size: number = 2) => {
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
          
          data[idx] = data[idx] * invAlpha + r * alpha;
          data[idx + 1] = data[idx + 1] * invAlpha + g * alpha;
          data[idx + 2] = data[idx + 2] * invAlpha + b * alpha;
          data[idx + 3] = 255;
        }
      }
    }
  };

  for (let i = 0; i < maxIterations; i++) {
    // Yield control every CHUNK_SIZE iterations using requestIdleCallback for better performance
    if (i > 0 && i % CHUNK_SIZE === 0) {
      await new Promise(resolve => {
        if ('requestIdleCallback' in window) {
          requestIdleCallback(() => resolve(undefined), { timeout: 50 });
        } else {
          setTimeout(resolve, 0);
        }
      });
    }
    
    if (i === switchPoint) {
      h = 0;
      r = 0;
    }

    if (h) {
      // Horizontal wool fiber phase
      if (i % width === 0) {
        u = zValue() * 500 + 100;
        r += 5;
      }
      
      const phase = S(i / u);
      const dx = S(i - 1) + phase * 6;
      
      const zVal = zValue();
      const red = ((T((Math.floor((r + offsetX) / 80 + zVal / 4)) ^ (Math.floor((i % height + offsetY) / 80)))) > 0.3) ? 100 : 0;
      
      const colorR = (209 + 46 * phase + red) | 0;
      const colorG = (208 + 45 * phase - red) | 0;
      const colorB = (183 + 53 * phase - red / 2) | 0;
      
      const x = r + dx;
      const y = i % height;
      
      if (x >= 0 && x < width && y >= 0 && y < height) {
        writePixel(x, y, colorR, colorG, colorB, 2);
      }
    } else {
      // Vertical wool fiber phase
      if (i % height === 0) {
        u = zValue() * 500 + 100;
        r += 3;
      }
      
      const phase = S(i / u);
      const zVal = zValue();
      const red = ((T((Math.floor((r + offsetY) / 80 + zVal / 4)) ^ (Math.floor((i % width + offsetX) / 80)))) > 0.3) ? 100 : 0;
      const dx = 4 * S(i - 1) + S(i / u) * 4;
      
      const colorR = (189 + 46 * phase + red) | 0;
      const colorG = (188 + 45 * phase - red) | 0;
      const colorB = (163 + 53 * phase - red / 2) | 0;
      
      const x = i % width;
      const y = r + dx;
      const pixelWidth = 1.4 * (phase + 1.7);
      
      if (x >= 0 && x < width && y >= 0 && y < height) {
        writePixel(x, y, colorR, colorG, colorB, pixelWidth);
      }
    }
  }
  
  ctx.putImageData(imageData, 0, 0);

  // Convert to Blob URL instead of data URL. A null blob (Safari canvas
  // memory limit) must reject so callers can retry smaller, not cache ''.
  return new Promise((resolve, reject) => {
    canvas.toBlob((blob) => {
      if (blob) {
        const blobUrl = URL.createObjectURL(blob);
        resolve(blobUrl);
      } else {
        reject(new Error('canvas.toBlob returned null (canvas memory limit?)'));
      }
    }, 'image/png', 1.0);
  });
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
