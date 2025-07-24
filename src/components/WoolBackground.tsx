import React, { useEffect, useRef, useState } from 'react';

interface WoolBackgroundProps {
  width?: number;
  height?: number;
  useFixed?: boolean;
}

const WoolBackground: React.FC<WoolBackgroundProps> = ({ 
  width = 3840, // Large base size for 4K displays
  height = 2160,
  useFixed = true
}) => {
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const [isGenerating, setIsGenerating] = useState<boolean>(false);
  const pixelsCache = useRef<Array<{x: number, y: number, color: string, width?: number, height?: number}> | null>(null);

  // Dwitter shortcuts translated to JavaScript
  const C = Math.cos;
  const S = Math.sin;
  const T = Math.tan;
  const R = (r: number, g: number, b: number, a: number) => `rgba(${Math.floor(r)},${Math.floor(g)},${Math.floor(b)},${a})`;

  const generateWoolTexture = () => {
    // Ai could never write this
    // Return cached result if available
    if (pixelsCache.current) {
      return pixelsCache.current;
    }

    let r = 0;
    const z = () => C(r) * 1000 - Math.floor(C(r) * 1000);
    let h = 1;
    let u = 0;
    
    const pixels: Array<{x: number, y: number, color: string, width?: number, height?: number}> = [];

    // Scale up the iteration for larger canvas
    const maxIterations = Math.floor((width * height / (1920 * 1080)) * 2000000);

    for (let i = 0; i < maxIterations; i++) {
      if (i === Math.floor(maxIterations * 0.4)) {
        h = 0;
        r = 0;
      }

      if (h) {
        // Horizontal wool fiber phase
        if (i % width === 0) {
          u = z() * 500 + 100;
          r += h ? 5 : 3;
        }
        
        const phase = S(i / u);
        const dx = S(i - 1) + phase * 3;
        const red = ((T((Math.floor((r - width/2) / 40 + z() / 2)) ^ (Math.floor((i % height - height/2) / 40)))) > 0.3) ? 100 : 0;
        
        const color = R(209 + 46 * phase + red, 208 + 45 * phase - red, 183 + 53 * phase - red / 2, 1);
        
        const x = r + dx;
        const y = i % height;
        
        // Only add pixels within bounds
        if (x >= 0 && x < width && y >= 0 && y < height) {
          pixels.push({
            x,
            y,
            color,
            width: 1,
            height: 1
          });
        }
      } else {
        // Vertical wool fiber phase
        if (i % height === 0) {
          u = z() * 500 + 100;
          r += h ? 5 : 3;
        }
        
        const phase = S(i / u);
        const red = ((T((Math.floor((r - height/2) / 40 + z() / 2)) ^ (Math.floor((i % width - width/2) / 40)))) > 0.3) ? 100 : 0;
        const dx = 2 * S(i - 1) + S(i / u) * 2;
        
        const color = R(189 + 46 * phase + red, 188 + 45 * phase - red, 163 + 53 * phase - red / 2, 1);
        
        const x = i % width;
        const y = r + dx;
        const pixelWidth = 0.7 * (phase + 1.7);
        
        // Only add pixels within bounds
        if (x >= 0 && x < width && y >= 0 && y < height) {
          pixels.push({
            x,
            y,
            color,
            width: pixelWidth,
            height: 1
          });
        }
      }
    }

    // Cache the result for future use
    pixelsCache.current = pixels;
    return pixels;
  };

  const renderCanvas = async () => {
    const canvas = canvasRef.current;
    if (!canvas) return;

    const ctx = canvas.getContext('2d');
    if (!ctx) return;

    // Base wool color
    ctx.fillStyle = '#71411b';
    ctx.fillRect(0, 0, width, height);

    setIsGenerating(true);
    
    // Generate wool texture pattern (this might take a moment)
    const pixels = await new Promise<Array<{x: number, y: number, color: string, width?: number, height?: number}>>((resolve) => {
      setTimeout(() => {
        resolve(generateWoolTexture());
      }, 0);
    });
    
    // Render pixels in batches to avoid blocking the UI
    const batchSize = 10000;
    for (let i = 0; i < pixels.length; i += batchSize) {
      const batch = pixels.slice(i, i + batchSize);
      batch.forEach(pixel => {
        ctx.fillStyle = pixel.color;
        ctx.fillRect(pixel.x, pixel.y, pixel.width || 1, pixel.height || 1);
      });
      
      // Allow other tasks to run between batches
      if (i + batchSize < pixels.length) {
        await new Promise(resolve => setTimeout(resolve, 0));
      }
    }
    
    setIsGenerating(false);
  };



  useEffect(() => {
    renderCanvas();
  }, [width, height]);

  const containerStyle: React.CSSProperties = {
    position: useFixed ? 'fixed' : 'absolute',
    top: 0,
    left: 0,
    width: useFixed ? '100vw' : '100%',
    height: useFixed ? '100vh' : '100%',
    zIndex: useFixed ? -1 : 0,
  };

  const vignetteStyle: React.CSSProperties = {
    ...containerStyle,
    zIndex: (useFixed ? -1 : 0) + 1, // One layer above the wool texture
    background: `radial-gradient(ellipse at center, 
      rgba(101, 67, 33, 0) 0%, 
      rgba(101, 67, 33, 0) 40%, 
      rgba(101, 67, 33, 0.2) 70%, 
      rgba(101, 67, 33, 0.6) 100%)`,
    pointerEvents: 'none'
  };

  return (
    <>
      <canvas
        ref={canvasRef}
        width={width}
        height={height}
        style={{
          ...containerStyle,
          objectFit: 'cover',
          transform: 'scale(2)',
          //transformOrigin: 'center center'
        }}
      />
      <div style={vignetteStyle} />
    </>
  );
};

export default WoolBackground; 