import { useEffect, useRef, useState } from 'react';

interface WoodTextureProps {
  width?: number;
  height?: number;
  onTextureReady?: (dataUrl: string) => void;
}

// Global cache for single wood texture
let woodTextureDataUrl: string | null = null;
let woodTexturePromise: Promise<string> | null = null;

const WoodTexture: React.FC<WoodTextureProps> = ({ 
  width = 1920, 
  height = 1080,
  onTextureReady 
}) => {
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const [textureReady, setTextureReady] = useState<boolean>(false);

  const generateWoodTexture = (): Promise<string> => {
    // TEMPORARY DISABLE: Skip complex texture generation to prevent Safari iOS crashes
    // TODO: Remove this early return when ready to re-enable wood texture

    // Return cached texture if available
    if (woodTextureDataUrl) {
      return Promise.resolve(woodTextureDataUrl);
    }

    // Return existing promise if generation is in progress
    if (woodTexturePromise) {
      return woodTexturePromise;
    }

    // Create new promise for texture generation
    woodTexturePromise = new Promise((resolve) => {
      const canvas = document.createElement('canvas');
      canvas.width = width;
      canvas.height = height;
      const ctx = canvas.getContext('2d');
      
      if (!ctx) {
        resolve('');
        return;
      }

      // Base wood color
      ctx.fillStyle = '#8B4513';
      ctx.fillRect(0, 0, width, height);

      // Dwitter wood algorithm: const D = (T) => {for(I=1080;I--;)for(var k=13,b=T/k;k--;x.fillStyle=R(b*120,b*b*14,9,.1),b>0&&x.fillRect(T*200%1920,I,40,1))b=C(I/1e3+b*C(b*b/2)+4)*b-2.8};for(i=0;i<576;i++)D(i/60)
      const C = Math.cos;
      const R = (r: number, g: number, b: number, a: number) => `rgba(${Math.floor(Math.abs(r))},${Math.floor(Math.abs(g))},${Math.floor(Math.abs(b))},${a})`;
      
      // The D function - exact dwitter implementation
      const D = (T: number) => {
        for (let I = 1080; I >= 0; I--) {
          for (let k = 24, b = T / k; k >= 0; k--) {
            b = C(I / 1000 + b * C(b * b / 2) + 4) * b - 2.8;
            
            if (b > 0) {
              const red = b * 120;
              const green = b * b * 14;
              const blue = 9;
              
              ctx.fillStyle = R(red, green, blue, 0.1);
              
              // Exact dwitter positioning
              const xPos = (T * 200) % 1920;
              
              ctx.fillRect(xPos, I, 40, 1);
            }
          }
        }
      };
      
      // Run the D function with increasing time values
      for (let i = 0; i < 576; i++) {
        D(i / 60);
      }

      // Convert to data URL and cache
      const dataUrl = canvas.toDataURL('image/png', 1.0);
      woodTextureDataUrl = dataUrl;
      woodTexturePromise = null; // Clear promise
      resolve(dataUrl);
    });

    return woodTexturePromise;
  };

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

// Hook to get wood texture data URL
export const useWoodTexture = () => {
  const [textureUrl, setTextureUrl] = useState<string | null>(woodTextureDataUrl);

  useEffect(() => {
    if (!woodTextureDataUrl) {
      // Generate texture if not cached
      const generator = new (WoodTexture as any)({ width: 1920, height: 1080 });
      generator.generateWoodTexture?.().then(setTextureUrl);
    }
  }, []);

  return textureUrl;
};

// Function to get wood texture style with random positioning for variety
export const getWoodTextureStyle = (randomSeed?: number, willRotate: boolean = false): React.CSSProperties => {
  if (!woodTextureDataUrl) {
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
  const seed = randomSeed || Math.random();
  const xOffset = Math.floor(seed * 1920);
  const yOffset = Math.floor((seed * 1000) % 1080);

  // Scale up background size for rotated elements to avoid gaps
  const scaleFactor = willRotate ? 1.5 : 1; // 150% size for rotated elements
  const scaledWidth = Math.floor(1920 * scaleFactor);
  const scaledHeight = Math.floor(1080 * scaleFactor);
  
  // Adjust positioning for scaled background
  const adjustedXOffset = Math.floor(xOffset * scaleFactor);
  const adjustedYOffset = Math.floor(yOffset * scaleFactor);

  return {
    backgroundImage: `url(${woodTextureDataUrl})`,
    backgroundSize: `${scaledWidth}px ${scaledHeight}px`,
    backgroundPosition: `${adjustedXOffset}px ${adjustedYOffset}px`,
    backgroundRepeat: 'repeat'
  };
};

// Initialize wood texture on module load using the exact dwitter algorithm
const initializeWoodTexture = () => {
  // TEMPORARY DISABLE: Skip initialization to prevent Safari iOS crashes  
  // TODO: Remove this early return when ready to re-enable wood texture
  if (typeof window !== 'undefined' && !woodTextureDataUrl) {
    const canvas = document.createElement('canvas');
    canvas.width = 1920;
    canvas.height = 1080;
    const ctx = canvas.getContext('2d');
    
    if (ctx) {
      // Base wood color
      ctx.fillStyle = '#8B4513';
      ctx.fillRect(0, 0, 1920, 1080);

      const C = Math.cos;
      const R = (r: number, g: number, b: number, a: number) => `rgba(${Math.floor(Math.abs(r))},${Math.floor(Math.abs(g))},${Math.floor(Math.abs(b))},${a})`;
      
      const D = (T: number) => {
        for (let I = 1080; I >= 0; I--) {
          for (let k = 24, b = T / k; k >= 0; k--) {
            b = C(I / 1000 + b * C(b * b / 2) + 4) * b - 2.8;
            
            if (b > 0) {
              const red = b * 120;
              const green = b * b * 14;
              const blue = 9;
              
              ctx.fillStyle = R(red, green, blue, 0.1);
              
              const xPos = (T * 200) % 1920;
              
              ctx.fillRect(xPos, I, 40, 1);
            }
          }
        }
      };
      
      for (let i = 0; i < 576; i++) {
        D(i / 60);
      }
      
      woodTextureDataUrl = canvas.toDataURL('image/png', 1.0);
    }
  }
};

// Initialize on module load
initializeWoodTexture();

export default WoodTexture; 