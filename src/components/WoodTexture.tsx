import { useEffect, useRef, useState } from 'react';

interface WoodTextureProps {
  width?: number;
  height?: number;
  onTextureReady?: (dataUrl: string) => void;
}

// Global cache for single wood texture
let woodTextureDataUrl: string | null = null;
let woodTexturePromise: Promise<string> | null = null;

// Shared function to generate wood texture with optimized algorithm
const generateWoodTextureSync = (width: number = 1920, height: number = 1080): string => {
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
  const BASE_R = 70; // #8B
  const BASE_G = 14;  // #45
  const BASE_B = 9;  // #13
  const ALPHA = 0.1;
  const INV_ALPHA = 0.9;
  
  // Pre-fill entire buffer with base wood color
  for (let i = 0; i < data.length; i += 4) {
    data[i] = BASE_R;
    data[i + 1] = BASE_G;
    data[i + 2] = BASE_B;
    data[i + 3] = 255;
  }
  
  // Pre-calculate I_factor values for all rows (eliminates 15.5M divisions)
  const I_factors = new Float32Array(height);
  for (let I = 0; I < height; I++) {
    I_factors[I] = I * 0.001;
  }
  
  // The D function - highly optimized with direct pixel manipulation
  const D = (T: number) => {
    const xPos = ((T * 200) % width) | 0;
    const xEnd = Math.min(xPos + 30, width);
    
    for (let I = height - 1; I >= 0; I--) {
      const I_factor = I_factors[I];
      const rowOffset = I * width;
      
      let b = T / 24;
      for (let k = 24; k >= 0; k--) {
        const b_sq_half = (b * b) * 0.5;
        b = C(I_factor+C(b_sq_half)*b + 4) * b - 2.8;
        
        if (b > 0) {
          // Calculate colors - exactly as in original
          const red = (b * 120) | 0;
          const green = (b * b * 14) | 0;
          const blue = 9;
          
          // Write to all 40 pixels in this horizontal stripe with proper alpha compositing
          for (let x = xPos; x < xEnd; x++) {
            const idx = (rowOffset + x) << 2;
            // Proper alpha compositing: result = src * alpha + dest * (1 - alpha)
            data[idx] = red * ALPHA + data[idx] * INV_ALPHA;
            data[idx + 1] = green * ALPHA + data[idx + 1] * INV_ALPHA;
            data[idx + 2] = blue * ALPHA + data[idx + 2] * INV_ALPHA;
            // Alpha channel already set to 255
          }
        }
      }
    }
  };
  
  // Run the D function with increasing time values
  for (let i = 0; i < 576; i++) {
    D(i / 60);
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
    // Import errorLogger dynamically to avoid circular imports
    const { errorLogger } = await import('../utils/errorLogger');
    
    // Log the start of wood texture generation
    errorLogger.logCanvasOperation('Wood Texture Generation Start', {
      canvasWidth: 1920,
      canvasHeight: 1080,
      estimatedMemoryMB: ((1920 * 1080 * 4) / (1024 * 1024)).toFixed(2),
    });

    const startTime = performance.now();
    const dataUrl = generateWoodTextureSync(1920, 1080);
    woodTextureDataUrl = dataUrl;
    
    const endTime = performance.now();
    const generationTime = endTime - startTime;
    
    console.log('Wood texture generated and cached');
    
    // Log completion
    errorLogger.logCanvasOperation('Wood Texture Generation Complete', {
      generationTimeMs: generationTime.toFixed(2),
      dataUrlSizeKB: (dataUrl.length / 1024).toFixed(2),
    });
    
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
    if (!woodTextureDataUrl && !woodTexturePromise) {
      // Defer generation until browser is idle (after FCP) for better perceived performance
      const scheduleGeneration = () => {
        if ('requestIdleCallback' in window) {
          // Use requestIdleCallback for best performance - runs when browser is idle
          requestIdleCallback(() => {
            generateWoodTexture().then(setTextureUrl);
          }, { timeout: 1000 }); // Fallback timeout after 1s
        } else {
          // Fallback for browsers without requestIdleCallback
          setTimeout(() => {
            generateWoodTexture().then(setTextureUrl);
          }, 100); // Small delay to ensure FCP happens first
        }
      };
      
      scheduleGeneration();
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
  // Trigger lazy loading after browser is idle for better FCP
  if (!woodTextureDataUrl && !woodTexturePromise && typeof window !== 'undefined') {
    if ('requestIdleCallback' in window) {
      requestIdleCallback(() => {
        if (!woodTextureDataUrl && !woodTexturePromise) {
          generateWoodTexture();
        }
      }, { timeout: 1000 });
    } else {
      setTimeout(() => {
        if (!woodTextureDataUrl && !woodTexturePromise) {
          generateWoodTexture();
        }
      }, 100);
    }
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