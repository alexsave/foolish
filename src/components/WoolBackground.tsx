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

    const generateWoolTexture = (ctx: CanvasRenderingContext2D) => {
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
        
        setIsGenerating(false);
        
        // Defer logging to not block rendering - run async after generation
        setTimeout(async () => {
            try {
                const { errorLogger } = await import('../utils/errorLogger');
                errorLogger.logCanvasOperation('Wool Background Generation Start', {
                    canvasWidth: width,
                    canvasHeight: height,
                    estimatedMemoryMB: ((width * height * 4) / (1024 * 1024)).toFixed(2),
                    maxIterations: Math.floor((width * height / (1920 * 1080)) * 2000000),
                });
                errorLogger.logCanvasOperation('Wool Background Generation Complete', {
                    generationTimeMs: generationTime.toFixed(2),
                    estimatedFinalMemoryMB: ((width * height * 4) / (1024 * 1024)).toFixed(2),
                    note: 'Optimized with ImageData - direct pixel manipulation',
                });
            } catch (e) {
                console.error('Logging error:', e);
            }
        }, 0);
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