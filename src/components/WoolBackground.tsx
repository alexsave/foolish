import React, { useEffect, useRef, useState } from 'react';

interface WoolBackgroundProps {
  width?: number;
  height?: number;
  useFixed?: boolean;
}

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

const WoolBackground: React.FC<WoolBackgroundProps> = ({ 
  width = 3840, // Large base size for 4K displays
  height = 2160,
  useFixed = true
}) => {
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const [isGenerating, setIsGenerating] = useState<boolean>(false);
  const hasRendered = useRef<boolean>(false);

  // Dwitter shortcuts translated to JavaScript
  const C = Math.cos;
  const S = Math.sin;
  const T = Math.tan;
  const R = (r: number, g: number, b: number, a: number) => `rgba(${Math.floor(r)},${Math.floor(g)},${Math.floor(b)},${a})`;

  const generateWoolTexture = (ctx: CanvasRenderingContext2D) => {
    // Ai could never write this
    // Render DIRECTLY to canvas instead of storing 8 million objects in memory!
    
    // Get random offsets for this pattern
    const { offsetX, offsetY } = getRandomOffsets();

    let r = 0;
    const z = () => C(r) * 1000 - Math.floor(C(r) * 1000);
    let h = 1;
    let u = 0;

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
        const dx = S(i - 1) + phase * 6; // Doubled from 3 to 6 for 2x scale
        const red = ((T((Math.floor((r + offsetX) / 80 + z() / 4)) ^ (Math.floor((i % height + offsetY) / 80)))) > 0.3) ? 100 : 0; // Using random offsets instead of centering
        
        const color = R(209 + 46 * phase + red, 208 + 45 * phase - red, 183 + 53 * phase - red / 2, 1);
        
        const x = r + dx;
        const y = i % height;
        
        // Only draw pixels within bounds - DIRECTLY to canvas
        if (x >= 0 && x < width && y >= 0 && y < height) {
          ctx.fillStyle = color;
          ctx.fillRect(x, y, 2, 2); // Doubled thread thickness
        }
      } else {
        // Vertical wool fiber phase
        if (i % height === 0) {
          u = z() * 500 + 100;
          r += h ? 5 : 3;
        }
        
        const phase = S(i / u);
        const red = ((T((Math.floor((r + offsetY) / 80 + z() / 4)) ^ (Math.floor((i % width + offsetX) / 80)))) > 0.3) ? 100 : 0; // Using random offsets instead of centering
        const dx = 4 * S(i - 1) + S(i / u) * 4; // Doubled from 2 to 4 for 2x scale
        
        const color = R(189 + 46 * phase + red, 188 + 45 * phase - red, 163 + 53 * phase - red / 2, 1);
        
        const x = i % width;
        const y = r + dx;
        const pixelWidth = 1.4 * (phase + 1.7); // Doubled from 0.7 to 1.4
        
        // Only draw pixels within bounds - DIRECTLY to canvas
        if (x >= 0 && x < width && y >= 0 && y < height) {
          ctx.fillStyle = color;
          ctx.fillRect(x, y, pixelWidth, 2); // Doubled thread thickness
        }
      }
    }
  };

  const drawFractalBranch = (ctx: CanvasRenderingContext2D, xPos: number, yPos: number, size: number, rotationFactor: number, pointIndex: number) => {
    // Base case: If the branch size is small enough, draw a dot and stop
    if (size <= 1) {
      // Scale and center the final coordinates on the screen - adjusted for native 2x scale
      const screenX = 4 * xPos * (width / 640) + width / 2; // Doubled from 2 to 4
      const screenY = 4 * yPos * (height / 1080) + height / 2; // Doubled from 2 to 4
      
      // Draw a bigger point for 2x scale
      ctx.fillRect(screenX, screenY, 0.6, 2); // Doubled from 0.3, 1 to 0.6, 2
    } else {
      // Recursive step: calculate the next segment
      drawFractalBranch(
        ctx,
        // New X position calculation
        yPos + size * Math.tan(size / 300 + rotationFactor * pointIndex) * Math.sin(rotationFactor * pointIndex),
        // New Y position is the old X position
        xPos,
        // The size is halved for the next segment
        size / 2,
        // The rotation factor is scaled up
        rotationFactor / 0.4,
        pointIndex
      );
    }
  };

  const drawWearPattern = (ctx: CanvasRenderingContext2D) => {
    ctx.fillStyle = 'rgba(255, 255, 255, 0.12)'; // Toned down wear marks
    let animationTime = 0.1;

    // Add wear pattern layers - reduced for subtlety
    for (let t = 0; t < 800; t++) { // Much fewer iterations
      animationTime += 0.01;

      // Draw fractal structures for wear
      for (let pointIndex = 0; pointIndex < 1000; pointIndex++) { // Fewer points for subtler pattern
        drawFractalBranch(ctx, 0, 0, 240, animationTime, pointIndex);
      }
    }
  };

  const applySpeckleNoise = (ctx: CanvasRenderingContext2D) => {
    const imgData = ctx.getImageData(0, 0, width, height);
    const data = imgData.data;
    const totalPixels = width * height;
    const speckleCount = totalPixels * 0.15; // 15% of pixels get speckles (much more visible)

    for (let i = 0; i < speckleCount; i++) {
      const p = (Math.random() * totalPixels) | 0; // Random pixel index
      const idx = p * 4; // RGBA start index
      const delta = Math.random() < 0.5 ? -40 : 40; // Much stronger brightness change

      data[idx] = Math.max(0, Math.min(255, data[idx] + delta));     // R
      data[idx + 1] = Math.max(0, Math.min(255, data[idx + 1] + delta)); // G
      data[idx + 2] = Math.max(0, Math.min(255, data[idx + 2] + delta)); // B
    }
    ctx.putImageData(imgData, 0, 0);
  };

  const applyVignette = (ctx: CanvasRenderingContext2D) => {
    // Create radial gradient for vignette - back to normal since no CSS scaling
    const centerX = width / 2;
    const centerY = height / 2;
    const radius = Math.max(width, height) * 0.7; // Normal radius for natural vignette
    
    const gradient = ctx.createRadialGradient(centerX, centerY, 0, centerX, centerY, radius);
    gradient.addColorStop(0, 'rgba(0, 0, 0, 0)');
    gradient.addColorStop(0.4, 'rgba(0, 0, 0, 0.05)'); // Natural progression
    gradient.addColorStop(0.7, 'rgba(0, 0, 0, 0.3)'); 
    gradient.addColorStop(1, 'rgba(0, 0, 0, 0.8)'); // Strong but natural edge darkening
    
    ctx.fillStyle = gradient;
    ctx.fillRect(0, 0, width, height);
  };

  const renderCanvas = async () => {
    console.log('renderCanvas for wool background');
    const canvas = canvasRef.current;
    if (!canvas) return;

    // TEMPORARY DISABLE: Skip complex texture generation to prevent Safari iOS crashes
    // TODO: Remove this early return when ready to re-enable wool texture

    // Prevent double generation
    if (hasRendered.current || isGenerating) {
      console.log('Skipping render - already generated or generating');
      return;
    }

    const ctx = canvas.getContext('2d');
    if (!ctx) return;

    console.log('Starting wool texture generation with random pattern...');
    
    // Import errorLogger and log the massive operation
    const { errorLogger } = await import('../utils/errorLogger');
    errorLogger.logCanvasOperation('Wool Background Generation Start', {
      canvasWidth: width,
      canvasHeight: height,
      estimatedMemoryMB: ((width * height * 4) / (1024 * 1024)).toFixed(2),
      maxIterations: Math.floor((width * height / (1920 * 1080)) * 2000000),
    });
    
    const startTime = performance.now();
    hasRendered.current = true;

    // Base wool color
    ctx.fillStyle = '#71411b';
    ctx.fillRect(0, 0, width, height);

    setIsGenerating(true);
    
    // Generate wool texture pattern directly to canvas (no memory-hungry array!)
    generateWoolTexture(ctx);

    // Add enhancement layers after main texture with visible delays
    console.log('Adding fractal wear pattern...');
    //await new Promise(resolve => setTimeout(resolve, 500)); // 500ms delay
    //drawWearPattern(ctx);
    
    console.log('Adding speckle noise...');
    //await new Promise(resolve => setTimeout(resolve, 500)); // 500ms delay
    //applySpeckleNoise(ctx);
    
    console.log('Adding vignette...');
    //await new Promise(resolve => setTimeout(resolve, 500)); // 500ms delay
    //applyVignette(ctx);
    
    console.log('Wool texture complete!');
    
    const endTime = performance.now();
    const generationTime = endTime - startTime;
    
    // Log completion with memory analysis (reuse errorLogger variable)
    errorLogger.logCanvasOperation('Wool Background Generation Complete', {
      generationTimeMs: generationTime.toFixed(2),
      estimatedFinalMemoryMB: ((width * height * 4) / (1024 * 1024)).toFixed(2),
      note: 'Another 40 TB to memory. Now rendering directly to canvas - no pixel array overhead!',
    });
    
    setIsGenerating(false);
  };



  useEffect(() => {
    console.log('useEffect for wool background');
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