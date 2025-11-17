import { useEffect, useState } from 'react';

// Global cache for wool texture to prevent regeneration
let woolTextureDataUrl: string | null = null;
let woolTexturePromise: Promise<string> | null = null;

// Generate random offsets once per page load
let globalRandomOffsetX: number | null = null;
let globalRandomOffsetY: number | null = null;

const getRandomOffsets = () => {
  if (globalRandomOffsetX === null || globalRandomOffsetY === null) {
    globalRandomOffsetX = Math.random() * 1000 - 500; // Random offset between -500 and 500
    globalRandomOffsetY = Math.random() * 1000 - 500; // Random offset between -500 and 500
    console.log('Generated new random wool pattern offsets:', { x: globalRandomOffsetX, y: globalRandomOffsetY });
  }
  return { offsetX: globalRandomOffsetX, offsetY: globalRandomOffsetY };
};

// Shared function to generate wool texture
const generateWoolTextureSync = (width: number = 3840, height: number = 2160): string => {
  const canvas = document.createElement('canvas');
  canvas.width = width;
  canvas.height = height;
  const ctx = canvas.getContext('2d');
  
  if (!ctx) {
    return '';
  }

  // Dwitter shortcuts translated to JavaScript
  const C = Math.cos;
  const S = Math.sin;
  const T = Math.tan;

        // Optimized wool texture using ImageData for direct pixel manipulation
        const imageData = ctx.createImageData(width, height);
        const data = new Uint8ClampedArray(imageData.data.buffer);
        
        // Pre-fill with brown base color (#71411b = rgb(113, 65, 27))
        const BASE_R = 113;
        const BASE_G = 65;
        const BASE_B = 27;
        for (let i = 0; i < data.length; i += 4) {
            data[i] = BASE_R;
            data[i + 1] = BASE_G;
            data[i + 2] = BASE_B;
            data[i + 3] = 255;
        }
        
        // Get random offsets for this pattern
        const { offsetX, offsetY } = getRandomOffsets();

        let r = 0;
        // Pre-calculate z function - eliminate repeated floor operations
        const zValue = () => {
            const cr = C(r) * 1000;
            return cr - Math.floor(cr);
        };
        let h = 1;
        let u = 0;

        // Scale up the iteration for larger canvas
        const maxIterations = Math.floor((width * height / (1920 * 1080)) * 2000000);
        const switchPoint = Math.floor(maxIterations * 0.4);

        // Helper to write pixel with soft edges (anti-aliasing) like canvas fillRect
        const writePixel = (x: number, y: number, r: number, g: number, b: number, size: number = 2) => {
            const xi = Math.floor(x);
            const yi = Math.floor(y);
            const sizeInt = Math.ceil(size);
            
            // Calculate fractional parts for anti-aliasing
            const fx = x - xi;
            const fy = y - yi;
            
            for (let dy = 0; dy < sizeInt; dy++) {
                for (let dx = 0; dx < sizeInt; dx++) {
                    const px = xi + dx;
                    const py = yi + dy;
                    if (px >= 0 && px < width && py >= 0 && py < height) {
                        const idx = (py * width + px) << 2;
                        
                        // Calculate alpha for anti-aliasing at edges (mimics subpixel rendering)
                        // Center pixels get full opacity, edge pixels get partial for soft look
                        const distX = Math.abs(dx - fx);
                        const distY = Math.abs(dy - fy);
                        const edgeSoftness = Math.max(0.1, 1 - (distX + distY) / 4);
                        const alpha = edgeSoftness * 0.99; // High opacity for brightness, soft edges
                        const invAlpha = 1 - alpha;
                        
                        // Alpha blend with existing pixel for soft overlapping effect
                        data[idx] = data[idx] * invAlpha + r * alpha;
                        data[idx + 1] = data[idx + 1] * invAlpha + g * alpha;
                        data[idx + 2] = data[idx + 2] * invAlpha + b * alpha;
                        data[idx + 3] = 255;
                    }
                }
            }
        };

        for (let i = 0; i < maxIterations; i++) {
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
                
                // Simplified red calculation - pre-calculate z and offsets
                const zVal = zValue();
                const red = ((T((Math.floor((r + offsetX) / 80 + zVal / 4)) ^ (Math.floor((i % height + offsetY) / 80)))) > 0.3) ? 100 : 0;
                
                // Calculate colors directly
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
        
        // Single canvas operation - write all pixels at once
        ctx.putImageData(imageData, 0, 0);
  
  // Convert to data URL
  return canvas.toDataURL('image/png', 1.0);
};

// Async generator function with chunked processing to yield control
export async function generateWoolTexture(): Promise<string> {
  // Return cached texture if available
  if (woolTextureDataUrl) {
    console.log('Using cached wool texture');
    return Promise.resolve(woolTextureDataUrl);
  }

  // Return existing promise if generation is already in progress
  if (woolTexturePromise) {
    console.log('Wool texture generation already in progress, reusing promise');
    return woolTexturePromise;
  }

  console.log('Generating new wool texture (async with yielding)...');
  
  // Create and cache the promise to prevent duplicate generations
  woolTexturePromise = (async () => {
    const startTime = performance.now();
    
    // Yield control immediately to let React render first
    await new Promise(resolve => setTimeout(resolve, 0));
    
    const dataUrl = await generateWoolTextureAsync(3840, 2160);
    woolTextureDataUrl = dataUrl;
    
    const endTime = performance.now();
    const generationTime = endTime - startTime;
    
    console.log('Wool texture generated and cached in', generationTime.toFixed(2), 'ms');
    
    // Clear the promise so future calls can detect the cache is ready
    woolTexturePromise = null;
    
    return dataUrl;
  })();

  return woolTexturePromise;
}

// Async version that yields control periodically
async function generateWoolTextureAsync(width: number, height: number): Promise<string> {
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
  
  // Yield every 50k iterations to let React render
  const CHUNK_SIZE = 50000;

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
    // Yield control every CHUNK_SIZE iterations
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
  return canvas.toDataURL('image/png', 1.0);
}

// Hook to get wool texture with lazy loading
export const useWoolTexture = () => {
  const [textureUrl, setTextureUrl] = useState<string | null>(woolTextureDataUrl);

  useEffect(() => {
    if (woolTextureDataUrl) {
      // If already cached, set it immediately
      setTextureUrl(woolTextureDataUrl);
    } else if (woolTexturePromise) {
      // If generation is in progress, wait for it
      woolTexturePromise.then(setTextureUrl);
    } else {
      // Start generation - the async chunking will handle yielding
      generateWoolTexture().then(setTextureUrl);
    }
  }, []);

  return textureUrl;
};


// Legacy function for non-React contexts (kept for backward compatibility)
export const getWoolTextureStyle = (): React.CSSProperties => {
  // Trigger lazy loading if not already generated
  if (!woolTextureDataUrl && !woolTexturePromise && typeof window !== 'undefined') {
    generateWoolTexture();
  }

  if (!woolTextureDataUrl) {
    return {
      backgroundColor: '#cac5af',
    };
  }

  return {
    backgroundImage: `url(${woolTextureDataUrl})`,
    backgroundSize: 'cover',
    backgroundPosition: 'center',
    backgroundRepeat: 'no-repeat'
  };
};

export default generateWoolTexture;
