import { useEffect, useState, useMemo } from 'react';
import { getCachedTexture, setCachedTexture } from '../utils/textureCache';

// Global cache for concrete texture - using blob URL
let concreteTextureBlobUrl: string | null = null;
let concreteTexturePromise: Promise<string> | null = null;

// Generate concrete texture with specks of lighter and darker gray
async function generateConcreteTexture(width: number = 512, height: number = 512): Promise<string> {
  // Return cached texture if available
  if (concreteTextureBlobUrl) {
    return Promise.resolve(concreteTextureBlobUrl);
  }

  // Return existing promise if generation is already in progress
  if (concreteTexturePromise) {
    return concreteTexturePromise;
  }

  concreteTexturePromise = (async () => {
    // Check IndexedDB cache first
    const cachedFromDB = await getCachedTexture('concrete');
    if (cachedFromDB) {
      concreteTextureBlobUrl = cachedFromDB;
      concreteTexturePromise = null;
      return cachedFromDB;
    }

    await new Promise(resolve => setTimeout(resolve, 0));

    const canvas = document.createElement('canvas');
    canvas.width = width;
    canvas.height = height;
    const ctx = canvas.getContext('2d');

    if (!ctx) {
      concreteTexturePromise = null;
      return '';
    }

    // Base concrete gray color
    const BASE_GRAY = 74; // #4A4A4A
    
    const imageData = ctx.createImageData(width, height);
    const data = imageData.data;

    // Use a seeded random for consistency
    let seed = 12345;
    const seededRandom = () => {
      seed = (seed * 1103515245 + 12345) & 0x7fffffff;
      return seed / 0x7fffffff;
    };

    // Fill with base color and add specks
    for (let i = 0; i < data.length; i += 4) {
      const x = (i / 4) % width;
      const y = Math.floor((i / 4) / width);
      
      // Start with base gray
      let gray = BASE_GRAY;
      
      // Add subtle noise to every pixel
      gray += (seededRandom() - 0.5) * 10;
      
      // Occasional lighter specks (small aggregates)
      if (seededRandom() < 0.02) {
        gray += 20 + seededRandom() * 30;
      }
      
      // Occasional darker specks (small aggregates)
      if (seededRandom() < 0.02) {
        gray -= 15 + seededRandom() * 20;
      }
      
      // Very occasional larger lighter patches
      if (seededRandom() < 0.003) {
        gray += 40 + seededRandom() * 20;
      }
      
      // Clamp values
      gray = Math.max(30, Math.min(120, gray));
      
      data[i] = gray;
      data[i + 1] = gray;
      data[i + 2] = gray;
      data[i + 3] = 255;
    }

    // Add some horizontal streaks for a brushed/poured effect
    for (let streak = 0; streak < height / 20; streak++) {
      const y = Math.floor(seededRandom() * height);
      const intensity = (seededRandom() - 0.5) * 15;
      const length = 50 + seededRandom() * 200;
      const startX = seededRandom() * width;
      
      for (let x = startX; x < Math.min(startX + length, width); x++) {
        const idx = (y * width + Math.floor(x)) * 4;
        const fade = 1 - Math.abs(x - startX - length / 2) / (length / 2);
        data[idx] = Math.max(30, Math.min(120, data[idx] + intensity * fade));
        data[idx + 1] = data[idx];
        data[idx + 2] = data[idx];
      }
    }

    ctx.putImageData(imageData, 0, 0);

    // Convert to blob URL
    const blobUrl = await new Promise<string>((resolve) => {
      canvas.toBlob((blob) => {
        if (blob) {
          resolve(URL.createObjectURL(blob));
        } else {
          resolve('');
        }
      }, 'image/png', 1.0);
    });

    concreteTextureBlobUrl = blobUrl;

    // Cache to IndexedDB
    setCachedTexture('concrete', blobUrl).catch(() => {});

    concreteTexturePromise = null;
    return blobUrl;
  })();

  return concreteTexturePromise;
}

// Hook to get concrete texture URL
export const useConcreteTexture = () => {
  const [textureUrl, setTextureUrl] = useState<string | null>(concreteTextureBlobUrl);

  useEffect(() => {
    if (concreteTextureBlobUrl) {
      setTextureUrl(concreteTextureBlobUrl);
    } else if (concreteTexturePromise) {
      concreteTexturePromise.then(setTextureUrl);
    } else {
      generateConcreteTexture().then(setTextureUrl);
    }
  }, []);

  return textureUrl;
};

// Hook that returns concrete texture style with seeded positioning
export const useConcreteStyle = (seed?: number, willRotate: boolean = false): React.CSSProperties => {
  const textureUrl = useConcreteTexture();

  const styleParams = useMemo(() => {
    const randomSeed = seed !== undefined ? seed : Math.random();
    const xOffset = Math.floor(randomSeed * 512);
    const yOffset = Math.floor((randomSeed * 1000) % 512);
    const scaleFactor = willRotate ? 1.5 : 1;
    const scaledWidth = Math.floor(512 * scaleFactor);
    const scaledHeight = Math.floor(512 * scaleFactor);
    const adjustedXOffset = Math.floor(xOffset * scaleFactor);
    const adjustedYOffset = Math.floor(yOffset * scaleFactor);

    return { scaledWidth, scaledHeight, adjustedXOffset, adjustedYOffset };
  }, [seed, willRotate]);

  const style = useMemo(() => {
    return {
      backgroundColor: '#4A4A4A',
      backgroundImage: textureUrl
        ? `url(${textureUrl})`
        : `repeating-linear-gradient(0deg, transparent, transparent 2px, rgba(0,0,0,0.05) 2px, rgba(0,0,0,0.05) 4px)`,
      backgroundSize: `${styleParams.scaledWidth}px ${styleParams.scaledHeight}px`,
      backgroundPosition: `${styleParams.adjustedXOffset}px ${styleParams.adjustedYOffset}px`,
      backgroundRepeat: 'repeat'
    };
  }, [textureUrl, styleParams]);

  return style;
};

// Legacy function for non-React contexts (used in map callbacks)
export const getConcreteTextureStyle = (randomSeed?: number, willRotate: boolean = false): React.CSSProperties => {
  // Trigger lazy loading if not already generated
  if (!concreteTextureBlobUrl && !concreteTexturePromise && typeof window !== 'undefined') {
    generateConcreteTexture();
  }

  if (!concreteTextureBlobUrl) {
    return {
      backgroundColor: '#4A4A4A',
      backgroundImage: `repeating-linear-gradient(
        0deg,
        transparent,
        transparent 2px,
        rgba(0,0,0,0.05) 2px,
        rgba(0,0,0,0.05) 4px
      )`
    };
  }

  const seed = randomSeed !== undefined ? randomSeed : Math.random();
  const xOffset = Math.floor(seed * 512);
  const yOffset = Math.floor((seed * 1000) % 512);

  const scaleFactor = willRotate ? 1.5 : 1;
  const scaledWidth = Math.floor(512 * scaleFactor);
  const scaledHeight = Math.floor(512 * scaleFactor);
  const adjustedXOffset = Math.floor(xOffset * scaleFactor);
  const adjustedYOffset = Math.floor(yOffset * scaleFactor);

  return {
    backgroundColor: '#4A4A4A',
    backgroundImage: `url(${concreteTextureBlobUrl})`,
    backgroundSize: `${scaledWidth}px ${scaledHeight}px`,
    backgroundPosition: `${adjustedXOffset}px ${adjustedYOffset}px`,
    backgroundRepeat: 'repeat'
  };
};

export { generateConcreteTexture };
