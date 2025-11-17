import { useEffect, useRef, useState } from 'react';

interface WoodTextureProps {
  width?: number;
  height?: number;
  onTextureReady?: (dataUrl: string) => void;
}

// Global cache for single wood texture
let woodTextureDataUrl: string | null = null;
let woodTexturePromise: Promise<string> | null = null;

// Async version that yields control to prevent blocking
const generateWoodTextureAsync = async (width: number = 1920, height: number = 1080): Promise<string> => {
  const canvas = document.createElement('canvas');
  canvas.width = width;
  canvas.height = height;
  const ctx = canvas.getContext('2d');
  
  if (!ctx) {
    return '';
  }

  // Create ImageData for direct pixel manipulation
  const imageData = ctx.createImageData(width, height);
  const data = new Uint8ClampedArray(imageData.data.buffer);
  
  const C = Math.cos;
  
  // Base wood color RGB values
  const BASE_R = 70;
  const BASE_G = 14;
  const BASE_B = 9;
  const ALPHA = 0.1;
  
  // Pre-fill entire buffer with base wood color
  for (let i = 0; i < data.length; i += 4) {
    data[i] = BASE_R;
    data[i + 1] = BASE_G;
    data[i + 2] = BASE_B;
    data[i + 3] = 255;
  }
  
  // Pre-calculate I_factor values for all rows
  const I_factors = new Float32Array(height);
  for (let I = 0; I < height; I++) {
    I_factors[I] = I * 0.001;
  }
  
  // The D function - highly optimized with direct pixel manipulation and soft edges
  const D = (T: number) => {
    const xPosFloat = (T * 200) % width;
    const xPos = xPosFloat | 0;
    const RECT_WIDTH = 40;
    const xEnd = Math.min(xPos + RECT_WIDTH, width);
    const xCenter = xPos + RECT_WIDTH / 2;
    
    for (let I = height - 1; I >= 0; I--) {
      const I_factor = I_factors[I];
      const rowOffset = I * width;
      
      let b = T / 24;
      for (let k = 24; k >= 0; k--) {
        const b_sq_half = (b * b) * 0.5;
        b = C(I_factor+C(b_sq_half)*b + 4) * b - 2.8;
        
        if (b > 0) {
          // Calculate colors
          const red = (b * 120) | 0;
          const green = (b * b * 14) | 0;
          const blue = 9;
          
          // Write to all 40 pixels in horizontal stripe with soft edges
          for (let x = xPos; x < xEnd; x++) {
            const idx = (rowOffset + x) << 2;
            
            // Calculate edge softness based on distance from center
            const distFromCenter = Math.abs(x - xCenter) / (RECT_WIDTH / 2);
            const edgeSoftness = Math.max(0.3, 1 - distFromCenter * 0.5);
            const effectiveAlpha = ALPHA * edgeSoftness;
            const invEffectiveAlpha = 1 - effectiveAlpha;
            
            // Alpha compositing with soft edges
            data[idx] = red * effectiveAlpha + data[idx] * invEffectiveAlpha;
            data[idx + 1] = green * effectiveAlpha + data[idx + 1] * invEffectiveAlpha;
            data[idx + 2] = blue * effectiveAlpha + data[idx + 2] * invEffectiveAlpha;
          }
        }
      }
    }
  };
  
  // Run the D function with increasing time values, yielding periodically
  for (let i = 0; i < 576; i++) {
    D(i / 60);
    // Yield every 5 iterations to let React render (very frequent to stay responsive)
    if (i > 0 && i % 5 === 0) {
      await new Promise(resolve => setTimeout(resolve, 0));
    }
  }
  
  // Single canvas operation - write all pixels at once
  ctx.putImageData(imageData, 0, 0);

  // Convert to data URL
  return canvas.toDataURL('image/png', 1.0);
};

// Async generator function similar to fern fractal with promise caching
export async function generateWoodTexture(): Promise<string> {
  // Return cached texture if available
  if (woodTextureDataUrl) {
    console.log('Using cached wood texture');
    return Promise.resolve(woodTextureDataUrl);
  }

  // Return existing promise if generation is already in progress
  if (woodTexturePromise) {
    console.log('Wood texture generation already in progress, reusing promise');
    return woodTexturePromise;
  }

  console.log('Generating new wood texture...');
  
  // Create and cache the promise to prevent duplicate generations
  woodTexturePromise = (async () => {
    const startTime = performance.now();
    
    // Yield control immediately to let React render first
    await new Promise(resolve => setTimeout(resolve, 0));
    
    const dataUrl = await generateWoodTextureAsync(1920, 1080);
    woodTextureDataUrl = dataUrl;
    
    const endTime = performance.now();
    const generationTime = endTime - startTime;
    
    console.log('Wood texture generated and cached in', generationTime.toFixed(2), 'ms');
    
    // Log completion
    
    // Clear the promise so future calls can detect the cache is ready
    woodTexturePromise = null;
    
    return dataUrl;
  })();

  return woodTexturePromise;
}

const WoodTexture: React.FC<WoodTextureProps> = ({ 
  width = 1920, 
  height = 1080,
  onTextureReady 
}) => {
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const [textureReady, setTextureReady] = useState<boolean>(false);

  useEffect(() => {
    generateWoodTexture().then((dataUrl) => {
      setTextureReady(true);
      if (onTextureReady) {
        onTextureReady(dataUrl);
      }
    });
  }, [width, height, onTextureReady]);

  return (
    <canvas
      ref={canvasRef}
      width={width}
      height={height}
      style={{ display: 'none' }} // Hidden since we only need the data URL
    />
  );
};

// Hook to get wood texture data URL with lazy loading (for advanced use cases)
export const useWoodTexture = () => {
  const [textureUrl, setTextureUrl] = useState<string | null>(woodTextureDataUrl);

  useEffect(() => {
    if (woodTextureDataUrl) {
      // If already cached, set it immediately
      setTextureUrl(woodTextureDataUrl);
    } else if (woodTexturePromise) {
      // If generation is in progress, wait for it
      woodTexturePromise.then(setTextureUrl);
    } else {
      // Start generation - the async chunking will handle yielding
      generateWoodTexture().then(setTextureUrl);
    }
  }, []);

  return textureUrl;
};

// Main hook: Returns wood texture style, triggers loading, and handles fallback
export const useWoodStyle = (seed?: number, willRotate: boolean = false): React.CSSProperties => {
  const textureUrl = useWoodTexture();

  if (!textureUrl) {
    // Fallback to solid wood color if texture not ready
    return {
      backgroundColor: '#8B4513',
      backgroundImage: `repeating-linear-gradient(
        90deg,
        transparent,
        transparent 1px,
        rgba(0,0,0,0.1) 1px,
        rgba(0,0,0,0.1) 2px
      )`
    };
  }

  // Generate random position based on seed or truly random
  const randomSeed = seed !== undefined ? seed : Math.random();
  const xOffset = Math.floor(randomSeed * 1920);
  const yOffset = Math.floor((randomSeed * 1000) % 1080);

  // Scale up background size for rotated elements to avoid gaps
  const scaleFactor = willRotate ? 1.5 : 1; // 150% size for rotated elements
  const scaledWidth = Math.floor(1920 * scaleFactor);
  const scaledHeight = Math.floor(1080 * scaleFactor);
  
  // Adjust positioning for scaled background
  const adjustedXOffset = Math.floor(xOffset * scaleFactor);
  const adjustedYOffset = Math.floor(yOffset * scaleFactor);

  return {
    backgroundImage: `url(${textureUrl})`,
    backgroundSize: `${scaledWidth}px ${scaledHeight}px`,
    backgroundPosition: `${adjustedXOffset}px ${adjustedYOffset}px`,
    backgroundRepeat: 'repeat'
  };
};

// Legacy function for non-React contexts (kept for backward compatibility)
export const getWoodTextureStyle = (randomSeed?: number, willRotate: boolean = false): React.CSSProperties => {
  // Trigger lazy loading if not already generated - the async chunking will handle yielding
  if (!woodTextureDataUrl && !woodTexturePromise && typeof window !== 'undefined') {
    generateWoodTexture();
  }

  if (!woodTextureDataUrl) {
    return {
      backgroundColor: '#8B4513',
      backgroundImage: `repeating-linear-gradient(
        90deg,
        transparent,
        transparent 1px,
        rgba(0,0,0,0.1) 1px,
        rgba(0,0,0,0.1) 2px
      )`
    };
  }

  const seed = randomSeed || Math.random();
  const xOffset = Math.floor(seed * 1920);
  const yOffset = Math.floor((seed * 1000) % 1080);

  const scaleFactor = willRotate ? 1.5 : 1;
  const scaledWidth = Math.floor(1920 * scaleFactor);
  const scaledHeight = Math.floor(1080 * scaleFactor);
  
  const adjustedXOffset = Math.floor(xOffset * scaleFactor);
  const adjustedYOffset = Math.floor(yOffset * scaleFactor);

  return {
    backgroundImage: `url(${woodTextureDataUrl})`,
    backgroundSize: `${scaledWidth}px ${scaledHeight}px`,
    backgroundPosition: `${adjustedXOffset}px ${adjustedYOffset}px`,
    backgroundRepeat: 'repeat'
  };
};

export default WoodTexture; 