import { useEffect, useRef, useState, useMemo } from 'react';

interface WoodTextureProps {
  width?: number;
  height?: number;
  onTextureReady?: (dataUrl: string) => void;
}

// Global cache for single wood texture - using blob URL to reduce JS heap memory
let woodTextureBlobUrl: string | null = null;
let woodTexturePromise: Promise<string> | null = null;

// Generate wood pixel data in JavaScript (like fernFractal generates points/colors)
async function generateWoodPixelData(width: number, height: number): Promise<{
  positions: Float32Array;
  colors: Float32Array;
  pointCount: number;
}> {
  const C = Math.cos;
  const BASE_R = 70 / 255;
  const BASE_G = 14 / 255;
  const BASE_B = 9 / 255;
  
  // Storage for all pixel colors
  const pixelColors = new Float32Array(width * height * 3);
  
  // Pre-fill with base color
  for (let i = 0; i < width * height; i++) {
    pixelColors[i * 3] = BASE_R;
    pixelColors[i * 3 + 1] = BASE_G;
    pixelColors[i * 3 + 2] = BASE_B;
  }
  
  const I_factors = new Float32Array(height);
  for (let I = 0; I < height; I++) {
    I_factors[I] = I * 0.001;
  }
  
  const D = (T: number) => {
    const xPosFloat = (T * 200) % width;
    const xPos = xPosFloat | 0;
    const RECT_WIDTH = 40;
    const xEnd = Math.min(xPos + RECT_WIDTH, width);
    const xCenter = xPos + RECT_WIDTH / 2;
    
    for (let I = height - 1; I >= 0; I--) {
      const I_factor = I_factors[I];
      
      let b = T / 24;
      for (let k = 24; k >= 0; k--) {
        const b_sq_half = (b * b) * 0.5;
        b = C(I_factor + C(b_sq_half) * b + 4) * b - 2.8;
        
        if (b > 0) {
          const red = (b * 120) / 255;
          const green = (b * b * 14) / 255;
          const blue = 9 / 255;
          
          const ALPHA = 0.1;
          
          for (let x = xPos; x < xEnd; x++) {
            const idx = (I * width + x) * 3;
            const distFromCenter = Math.abs(x - xCenter) / (RECT_WIDTH / 2);
            const edgeSoftness = Math.max(0.3, 1 - distFromCenter * 0.5);
            const effectiveAlpha = ALPHA * edgeSoftness;
            const invEffectiveAlpha = 1 - effectiveAlpha;
            
            pixelColors[idx] = red * effectiveAlpha + pixelColors[idx] * invEffectiveAlpha;
            pixelColors[idx + 1] = green * effectiveAlpha + pixelColors[idx + 1] * invEffectiveAlpha;
            pixelColors[idx + 2] = blue * effectiveAlpha + pixelColors[idx + 2] * invEffectiveAlpha;
          }
        }
      }
    }
  };
  
  for (let i = 0; i < 576; i++) {
    D(i / 60);
    if (i > 0 && i % 200 === 0) {
      await new Promise(resolve => setTimeout(resolve, 0));
    }
  }
  
  // Convert to positions and colors arrays for WebGL
  const totalPixels = width * height;
  const positions = new Float32Array(totalPixels * 2);
  const colors = new Float32Array(totalPixels * 3);
  
  let pointIndex = 0;
  for (let y = 0; y < height; y++) {
    for (let x = 0; x < width; x++) {
      const idx = (y * width + x) * 3;
      
      positions[pointIndex * 2] = x + 0.5;
      positions[pointIndex * 2 + 1] = y + 0.5;
      
      colors[pointIndex * 3] = pixelColors[idx];
      colors[pointIndex * 3 + 1] = pixelColors[idx + 1];
      colors[pointIndex * 3 + 2] = pixelColors[idx + 2];
      
      pointIndex++;
    }
  }
  
  return {
    positions,
    colors,
    pointCount: totalPixels
  };
}

// GPU-accelerated wood texture rendering using WebGL (CPU generates, GPU draws)
const generateWoodTextureAsync = async (width: number = 1920, height: number = 1080): Promise<string> => {
  const canvas = document.createElement('canvas');
  canvas.width = width;
  canvas.height = height;
  const gl = canvas.getContext('webgl', { preserveDrawingBuffer: true });
  
  if (!gl) {
    console.error('WebGL not available, falling back to CPU-only');
    return generateWoodTextureFallback(width, height);
  }

  try {
    // Step 1: Generate pixel data using CPU (like fernFractal generates points)
    const pixelData = await generateWoodPixelData(width, height);
    
    // Step 2: Use WebGL to draw the pixels efficiently
    const vertexShaderSource = `
      attribute vec2 position;
      attribute vec3 color;
      varying vec3 vColor;
      uniform vec2 resolution;
      
      void main() {
        vec2 normalized = position / resolution;
        vec2 clipSpace = normalized * 2.0 - 1.0;
        clipSpace.y = -clipSpace.y;
        gl_Position = vec4(clipSpace, 0.0, 1.0);
        gl_PointSize = 1.0;
        vColor = color;
      }
    `;

    const fragmentShaderSource = `
      precision mediump float;
      varying vec3 vColor;
      
      void main() {
        gl_FragColor = vec4(vColor, 1.0);
      }
    `;

    // Compile shaders
    const vertexShader = gl.createShader(gl.VERTEX_SHADER)!;
    gl.shaderSource(vertexShader, vertexShaderSource);
    gl.compileShader(vertexShader);

    const fragmentShader = gl.createShader(gl.FRAGMENT_SHADER)!;
    gl.shaderSource(fragmentShader, fragmentShaderSource);
    gl.compileShader(fragmentShader);

    const program = gl.createProgram()!;
    gl.attachShader(program, vertexShader);
    gl.attachShader(program, fragmentShader);
    gl.linkProgram(program);
    gl.useProgram(program);

    // Upload position data (one point per pixel)
    const positionBuffer = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, positionBuffer);
    gl.bufferData(gl.ARRAY_BUFFER, pixelData.positions, gl.STATIC_DRAW);

    const positionLocation = gl.getAttribLocation(program, 'position');
    gl.enableVertexAttribArray(positionLocation);
    gl.vertexAttribPointer(positionLocation, 2, gl.FLOAT, false, 0, 0);

    // Upload color data
    const colorBuffer = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, colorBuffer);
    gl.bufferData(gl.ARRAY_BUFFER, pixelData.colors, gl.STATIC_DRAW);

    const colorLocation = gl.getAttribLocation(program, 'color');
    gl.enableVertexAttribArray(colorLocation);
    gl.vertexAttribPointer(colorLocation, 3, gl.FLOAT, false, 0, 0);

    // Set uniforms
    const resolutionLocation = gl.getUniformLocation(program, 'resolution');
    gl.uniform2f(resolutionLocation, width, height);

    // Clear and draw all pixels
    gl.clearColor(70 / 255, 14 / 255, 9 / 255, 1);
    gl.clear(gl.COLOR_BUFFER_BIT);
    gl.drawArrays(gl.POINTS, 0, pixelData.pointCount);

    gl.flush();
    await new Promise(resolve => setTimeout(resolve, 10));

    // Convert to Blob URL
    return new Promise((resolve) => {
      canvas.toBlob((blob) => {
        if (blob) {
          const blobUrl = URL.createObjectURL(blob);
          resolve(blobUrl);
        } else {
          resolve('');
        }
      }, 'image/png', 1.0);
    });
  } catch (error) {
    console.error('WebGL wood texture generation failed, using fallback:', error);
    return generateWoodTextureFallback(width, height);
  }
};

// CPU implementation (original code)
const generateWoodTextureFallback = async (width: number = 1920, height: number = 1080): Promise<string> => {
  const canvas = document.createElement('canvas');
  canvas.width = width;
  canvas.height = height;
  const ctx = canvas.getContext('2d');
  
  if (!ctx) {
    return '';
  }

  const imageData = ctx.createImageData(width, height);
  const data = new Uint8ClampedArray(imageData.data.buffer);
  
  const C = Math.cos;
  const BASE_R = 70;
  const BASE_G = 14;
  const BASE_B = 9;
  const ALPHA = 0.1;
  
  for (let i = 0; i < data.length; i += 4) {
    data[i] = BASE_R;
    data[i + 1] = BASE_G;
    data[i + 2] = BASE_B;
    data[i + 3] = 255;
  }
  
  const I_factors = new Float32Array(height);
  for (let I = 0; I < height; I++) {
    I_factors[I] = I * 0.001;
  }
  
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
          const red = (b * 120) | 0;
          const green = (b * b * 14) | 0;
          const blue = 9;
          
          for (let x = xPos; x < xEnd; x++) {
            const idx = (rowOffset + x) << 2;
            const distFromCenter = Math.abs(x - xCenter) / (RECT_WIDTH / 2);
            const edgeSoftness = Math.max(0.3, 1 - distFromCenter * 0.5);
            const effectiveAlpha = ALPHA * edgeSoftness;
            const invEffectiveAlpha = 1 - effectiveAlpha;
            
            data[idx] = red * effectiveAlpha + data[idx] * invEffectiveAlpha;
            data[idx + 1] = green * effectiveAlpha + data[idx + 1] * invEffectiveAlpha;
            data[idx + 2] = blue * effectiveAlpha + data[idx + 2] * invEffectiveAlpha;
          }
        }
      }
    }
  };
  
  for (let i = 0; i < 576; i++) {
    D(i / 60);
    if (i > 0 && i % 200 === 0) {
      await new Promise(resolve => setTimeout(resolve, 0));
    }
  }
  
  ctx.putImageData(imageData, 0, 0);
  
  // Convert to Blob URL instead of data URL
  return new Promise((resolve) => {
    canvas.toBlob((blob) => {
      if (blob) {
        const blobUrl = URL.createObjectURL(blob);
        resolve(blobUrl);
      } else {
        resolve('');
      }
    }, 'image/png', 1.0);
  });
};

// Async generator function similar to fern fractal with promise caching
export async function generateWoodTexture(): Promise<string> {
  // Return cached texture if available
  if (woodTextureBlobUrl) {
    console.log('Using cached wood texture');
    return Promise.resolve(woodTextureBlobUrl);
  }

  // Return existing promise if generation is already in progress
  if (woodTexturePromise) {
    console.log('Wood texture generation already in progress, reusing promise');
    return woodTexturePromise;
  }

  // Create and cache the promise to prevent duplicate generations
  woodTexturePromise = (async () => {
    // Yield control immediately to let React render first
    await new Promise(resolve => setTimeout(resolve, 0));
    
    // Use WebGL with CPU-generated data for best performance
    const blobUrl = await generateWoodTextureAsync(1920, 1080);
    woodTextureBlobUrl = blobUrl;
    
    console.log('Wood texture generated (CPU generate + WebGL render, blob URL for lower memory)');
    
    // Clear the promise so future calls can detect the cache is ready
    woodTexturePromise = null;
    
    return blobUrl;
  })();

  return woodTexturePromise;
}

const WoodTexture: React.FC<WoodTextureProps> = ({ 
  width = 1920, 
  height = 1080,
  onTextureReady 
}) => {
  const canvasRef = useRef<HTMLCanvasElement>(null);

  useEffect(() => {
    generateWoodTexture().then((dataUrl) => {
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

// Hook to get wood texture blob URL with lazy loading (for advanced use cases)
export const useWoodTexture = () => {
  const [textureUrl, setTextureUrl] = useState<string | null>(woodTextureBlobUrl);

  useEffect(() => {
    if (woodTextureBlobUrl) {
      // If already cached, set it immediately
      setTextureUrl(woodTextureBlobUrl);
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

  // Memoize calculations to prevent randomSeed from changing on every render
  const styleParams = useMemo(() => {
    const randomSeed = seed !== undefined ? seed : Math.random();
    const xOffset = Math.floor(randomSeed * 1920);
    const yOffset = Math.floor((randomSeed * 1000) % 1080);
    const scaleFactor = willRotate ? 1.5 : 1;
    const scaledWidth = Math.floor(1920 * scaleFactor);
    const scaledHeight = Math.floor(1080 * scaleFactor);
    const adjustedXOffset = Math.floor(xOffset * scaleFactor);
    const adjustedYOffset = Math.floor(yOffset * scaleFactor);
    
    return { scaledWidth, scaledHeight, adjustedXOffset, adjustedYOffset };
  }, [seed, willRotate]);

  // Memoize the style object - SAME structure always, just texture changes
  const style = useMemo(() => {
    // Always return the same structure - texture just "pops in" when loaded
    return {
      backgroundColor: '#8B4513',
      backgroundImage: textureUrl 
        ? `url(${textureUrl})`
        : `repeating-linear-gradient(90deg, transparent, transparent 1px, rgba(0,0,0,0.1) 1px, rgba(0,0,0,0.1) 2px)`,
      backgroundSize: `${styleParams.scaledWidth}px ${styleParams.scaledHeight}px`,
      backgroundPosition: `${styleParams.adjustedXOffset}px ${styleParams.adjustedYOffset}px`,
      backgroundRepeat: 'repeat'
    };
  }, [textureUrl, styleParams]);
  
  return style;
};

// Legacy function for non-React contexts (kept for backward compatibility)
export const getWoodTextureStyle = (randomSeed?: number, willRotate: boolean = false): React.CSSProperties => {
  // Trigger lazy loading if not already generated - the async chunking will handle yielding
  if (!woodTextureBlobUrl && !woodTexturePromise && typeof window !== 'undefined') {
    generateWoodTexture();
  }

  if (!woodTextureBlobUrl) {
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
    backgroundImage: `url(${woodTextureBlobUrl})`,
    backgroundSize: `${scaledWidth}px ${scaledHeight}px`,
    backgroundPosition: `${adjustedXOffset}px ${adjustedYOffset}px`,
    backgroundRepeat: 'repeat'
  };
};

export default WoodTexture; 