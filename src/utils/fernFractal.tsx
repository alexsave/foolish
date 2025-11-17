import React, { createContext, useContext, useEffect, useState, useCallback, useRef } from 'react';

interface FernParameters {
    render: {
        iterations: number;
        scaleY: number;
        translateX: number;
        translateY: number;
        dotSize: number;
    };
    colors: {
        primary: string;
        secondary: string;
        tertiary: string;
    };
    probabilities: {
        f1_stem: number;
        f2_main: number;
        f3_left: number;
        f4_right: number;
        circle1: number;
        circle2: number;
    };
    transforms: {
        f1: { a: number; b: number; c: number; d: number; e: number; f: number };
        f2: { a: number; b: number; c: number; d: number; e: number; f: number };
        f3: { a: number; b: number; c: number; d: number; e: number; f: number };
        f4: { a: number; b: number; c: number; d: number; e: number; f: number };
    };
    circles: {
        circle1: { cx: number; cy: number; r: number; color: string };
        circle2: { cx: number; cy: number; r: number; color: string };
    };
}

const DEFAULT_FERN_PARAMS: FernParameters = {
    render: {
        iterations: 1000000,
        scaleY: 6,
        translateX: 0,
        translateY: 0,
        dotSize: 1
    },
    colors: {
        primary: "#ffd700",
        secondary: "#ff0000",
        tertiary: "#bd7800"
    },
    probabilities: {
        f1_stem: 0.01,
        f2_main: 0.8,
        f3_left: 0.08,
        f4_right: 0.08,
        circle1: 0.02,
        circle2: 0.01
    },
    transforms: {
        f1: { a: 0.02317, b: -0.0013, c: 0, d: 0.21422, e: 0, f: 0 },
        f2: { a: 0.789, b: 0.1533, c: -0.1877, d: 0.8734, e: 0.0617, f: 2 },
        f3: { a: -0.4556, b: -0.2832, c: -0.3847, d: 0.3305, e: 0, f: 1 },
        f4: { a: 0.3, b: 0.2, c: -0.2, d: 0.2, e: 0, f: 0 }
    },
    circles: {
        circle1: { cx: 2.803, cy: 0.5296, r: 0.4817, color: "#ff0000" },
        circle2: { cx: -4.5784, cy: 1.4463, r: 0.2894, color: "#bd7800" }
    }
};

function normalizeProbabilities(probs: FernParameters['probabilities']): number[] {
    const values = [probs.f1_stem, probs.f2_main, probs.f3_left, probs.f4_right, probs.circle1, probs.circle2];
    const sum = values.reduce((a, b) => a + b, 0);
    const normalized = values.map(v => v / sum);

    // Create cumulative distribution
    const cumulative = [];
    let acc = 0;
    for (const p of normalized) {
        acc += p;
        cumulative.push(acc);
    }
    return cumulative;
}

function pickTransformIndex(cumulative: number[]): number {
    const r = Math.random();
    for (let i = 0; i < cumulative.length; i++) {
        if (r < cumulative[i]) return i;
    }
    return cumulative.length - 1;
}

function applyAffineTransform(
    x: number,
    y: number,
    transform: { a: number; b: number; c: number; d: number; e: number; f: number }
): [number, number] {
    return [
        transform.a * x + transform.b * y + transform.e,
        transform.c * x + transform.d * y + transform.f
    ];
}

function randomPointInCircle(cx: number, cy: number, r: number): [number, number] {
    const t = Math.random();
    const angle = Math.random() * Math.PI * 2;
    const radius = Math.sqrt(t) * r;
    return [cx + radius * Math.cos(angle), cy + radius * Math.sin(angle)];
}

function executeTransformStep(
    index: number,
    x: number,
    y: number,
    params: FernParameters
): [number, number] {
    const { transforms, circles } = params;

    switch (index) {
        case 0: return applyAffineTransform(x, y, transforms.f1);
        case 1: return applyAffineTransform(x, y, transforms.f2);
        case 2: return applyAffineTransform(x, y, transforms.f3);
        case 3: return applyAffineTransform(x, y, transforms.f4);
        case 4: return randomPointInCircle(circles.circle1.cx, circles.circle1.cy, circles.circle1.r);
        case 5: return randomPointInCircle(circles.circle2.cx, circles.circle2.cy, circles.circle2.r);
        default: return [x, y];
    }
}

// TEMPORARY DISABLE FLAG - Set to false to re-enable complex fern fractal
const FERN_TEXTURE_DISABLED = false;

// Global cache for the fractal pattern
let cachedFernPattern: string | null = null;
let fernPatternPromise: Promise<string> | null = null;

// Create card-specific parameters for small sizes
function createCardParams(canvasWidth: number, canvasHeight: number): FernParameters {
    // Scale down for small card sizes
    const scale = Math.min(canvasWidth, canvasHeight) / 70; // Base scale on 70px reference

    return {
        ...DEFAULT_FERN_PARAMS,
        render: {
            ...DEFAULT_FERN_PARAMS.render,
            iterations: 5000000, // Fewer iterations for small cards
            scaleY: 14 * scale, // Zoom in 1.5x from previous scale
            translateX: 0,
            //translateY: 8 * scale, // Adjust to center better
        }
    };
}

export async function generateFernPattern(
    //canvasWidth: number,
    //canvasHeight: number,
    params?: FernParameters
): Promise<string> {
    
    // TEMPORARY DISABLE: Skip complex fractal generation to prevent Safari iOS crashes
    if (FERN_TEXTURE_DISABLED) {
        console.log('Fern pattern disabled - using simple green background');
        // Create a simple green canvas instead of complex fractal
        const canvas = document.createElement('canvas');
        canvas.width = 200 * 5;
        canvas.height = 280 * 5;
        const ctx = canvas.getContext('2d');
        if (ctx) {
            // Simple green gradient background
            const gradient = ctx.createLinearGradient(0, 0, 0, canvas.height);
            gradient.addColorStop(0, '#2d5016');
            gradient.addColorStop(1, '#1a3009');
            ctx.fillStyle = gradient;
            ctx.fillRect(0, 0, canvas.width, canvas.height);
        }
        return Promise.resolve(canvas.toDataURL('image/png'));
    }
    
    
    // Return cached pattern if available  
    if (cachedFernPattern) {
        console.log('Using cached fern pattern');
        return Promise.resolve(cachedFernPattern);
    }
    
    // Return existing promise if generation is already in progress
    if (fernPatternPromise) {
        console.log('Fern pattern generation already in progress, reusing promise');
        return fernPatternPromise;
    }
    
    console.log('Generating new fern pattern...');
    
    // Log the start of fern pattern generation
    
    // Create and cache the promise to prevent duplicate generations
    fernPatternPromise = (async () => {
        const startTime = performance.now();
        
        // Yield control immediately to let React render first
        await new Promise(resolve => setTimeout(resolve, 0));
        
        const canvas = document.createElement('canvas');
        const canvasWidth = 200 * 5;
        const canvasHeight = 280 * 5
        canvas.width = canvasWidth;
        canvas.height = canvasHeight;
        const ctx = canvas.getContext('2d');

        if (!ctx) {
            return '';
        }

        // Use ImageData for direct pixel manipulation (much faster than fillRect)
        const imageData = ctx.createImageData(canvasWidth, canvasHeight);
        const data = new Uint8ClampedArray(imageData.data.buffer);

        // Use provided params or auto-scale for card size
        const finalParams = params || createCardParams(canvasWidth, canvasHeight);
        const { render, colors, probabilities } = finalParams;
        const cumulative = normalizeProbabilities(probabilities);

        // Parse hex colors to RGB once (avoid repeated parsing)
        const parseHex = (hex: string) => {
            const r = parseInt(hex.slice(1, 3), 16);
            const g = parseInt(hex.slice(3, 5), 16);
            const b = parseInt(hex.slice(5, 7), 16);
            return [r, g, b];
        };
        const [primaryR, primaryG, primaryB] = parseHex(colors.primary);
        const [secondaryR, secondaryG, secondaryB] = parseHex(colors.secondary);
        const [tertiaryR, tertiaryG, tertiaryB] = parseHex(colors.tertiary);

        // Pre-calculate rotation constants (used millions of times)
        const rotationRadians = -67 * Math.PI / 180;
        const cosRot = Math.cos(rotationRadians);
        const sinRot = Math.sin(rotationRadians);
        
        // Pre-calculate canvas center
        const centerX = canvasWidth / 2;
        const centerY = canvasHeight / 2;

        let x = 0, y = 0;
        let propagateColor = 0; // 0=primary, 1=secondary (circle1), 2=tertiary (circle2)

        const pointSize = Math.max(1, Math.floor(render.dotSize));
        
        // Helper to draw pixel directly to ImageData
        const drawPixel = (px: number, py: number, r: number, g: number, b: number) => {
            const pxi = px | 0;
            const pyi = py | 0;
            for (let dy = 0; dy < pointSize; dy++) {
                for (let dx = 0; dx < pointSize; dx++) {
                    const x = pxi + dx;
                    const y = pyi + dy;
                    if (x >= 0 && x < canvasWidth && y >= 0 && y < canvasHeight) {
                        const idx = (y * canvasWidth + x) << 2;
                        data[idx] = r;
                        data[idx + 1] = g;
                        data[idx + 2] = b;
                        data[idx + 3] = 255;
                    }
                }
            }
        };

        // Skip first 20 iterations to let the system settle
        for (let i = 0; i < 20; i++) {
            const idx = pickTransformIndex(cumulative);
            [x, y] = executeTransformStep(idx, x, y, finalParams);
        }

        // Main iteration loop with yielding
        for (let i = 0; i < render.iterations; i++) {
            // Yield every 10000 iterations to let React render
            if (i > 0 && i % 10000 === 0) {
                await new Promise(resolve => setTimeout(resolve, 0));
            }
            
            const idx = pickTransformIndex(cumulative);
            [x, y] = executeTransformStep(idx, x, y, finalParams);

            // Handle circle splash points (f5/f6)
            if (idx === 4) {
                // Circle 1 splash - draw directly and set color propagation
                let [dx, dy] = randomPointInCircle(finalParams.circles.circle1.cx, finalParams.circles.circle1.cy, finalParams.circles.circle1.r);

                // Random 180-degree rotation around origin
                if (Math.random() < 0.5) {
                    dx = -dx;
                    dy = -dy;
                }

                // Apply rotation (pre-calculated)
                const rotatedDx = dx * cosRot - dy * sinRot;
                const rotatedDy = dx * sinRot + dy * cosRot;

                const px = centerX + rotatedDx * render.scaleY + render.translateX;
                const py = centerY - (rotatedDy * render.scaleY - render.translateY);

                drawPixel(px, py, secondaryR, secondaryG, secondaryB);

                propagateColor = 1;
                continue;
            }

            if (idx === 5) {
                // Circle 2 splash - draw directly and set color propagation  
                let [dx, dy] = randomPointInCircle(finalParams.circles.circle2.cx, finalParams.circles.circle2.cy, finalParams.circles.circle2.r);

                // Random 180-degree rotation around origin
                if (Math.random() < 0.5) {
                    dx = -dx;
                    dy = -dy;
                }

                // Apply rotation (pre-calculated)
                const rotatedDx = dx * cosRot - dy * sinRot;
                const rotatedDy = dx * sinRot + dy * cosRot;

                const px = centerX + rotatedDx * render.scaleY + render.translateX;
                const py = centerY - (rotatedDy * render.scaleY - render.translateY);

                drawPixel(px, py, tertiaryR, tertiaryG, tertiaryB);

                propagateColor = 2;
                continue;
            }

            // Reset color propagation on f1 (stem)
            if (idx === 0) {
                propagateColor = 0;
            }

            // Draw regular fractal point with current color
            let drawX = x, drawY = y;

            // Random 180-degree rotation around origin for visual variety
            if (Math.random() < 0.5) {
                drawX = -x;
                drawY = -y;
            }

            // Apply rotation (pre-calculated)
            const rotatedX = drawX * cosRot - drawY * sinRot;
            const rotatedY = drawX * sinRot + drawY * cosRot;

            const px = centerX + rotatedX * render.scaleY + render.translateX;
            const py = centerY - (rotatedY * render.scaleY - render.translateY);

            // Use propagated color
            if (propagateColor === 2) {
                drawPixel(px, py, tertiaryR, tertiaryG, tertiaryB);
            } else if (propagateColor === 1) {
                drawPixel(px, py, secondaryR, secondaryG, secondaryB);
            } else {
                drawPixel(px, py, primaryR, primaryG, primaryB);
            }
        }

        // Write ImageData to canvas in one operation
        ctx.putImageData(imageData, 0, 0);

        // Convert canvas to data URL and cache it
        const dataUrl = canvas.toDataURL('image/png');
        cachedFernPattern = dataUrl;
        const endTime = performance.now();
        const generationTime = endTime - startTime;
        
        console.log('Fern pattern generated and cached in', generationTime.toFixed(2), 'ms');
        
        // Clear the promise so future calls can detect the cache is ready
        fernPatternPromise = null;
        
        return dataUrl;
    })();
    
    return fernPatternPromise;
}

// React Context for sharing the fractal pattern
interface FernFractalContextType {
  fernPattern: string;
  isLoading: boolean;
  triggerGeneration?: () => void;
}

const FernFractalContext = createContext<FernFractalContextType | undefined>(undefined);

export const FernFractalProvider: React.FC<{ children: React.ReactNode }> = ({ children }) => {
  const [fernPattern, setFernPattern] = useState<string>(cachedFernPattern || '');
  const [isLoading, setIsLoading] = useState<boolean>(!cachedFernPattern);
  const generationRequestedRef = useRef<boolean>(false);

  // Function to trigger generation (called by useFernFractal when component needs it)
  const triggerGeneration = useCallback(() => {
    if (cachedFernPattern) {
      // Already have cached pattern
      setFernPattern(cachedFernPattern);
      setIsLoading(false);
      return;
    }

    if (generationRequestedRef.current) {
      // Already requested
      return;
    }

    generationRequestedRef.current = true;
    console.log('FernFractalProvider: Generating fractal pattern on demand...');
    
    generateFernPattern()
      .then((dataUrl: string) => {
        console.log('FernFractalProvider: Pattern generated and ready');
        setFernPattern(dataUrl);
        setIsLoading(false);
      })
      .catch((error: any) => {
        console.error('FernFractalProvider: Error generating pattern:', error);
        setIsLoading(false);
      });
  }, []);

  return (
    <FernFractalContext.Provider value={{ fernPattern, isLoading, triggerGeneration }}>
      {children}
    </FernFractalContext.Provider>
  );
};

export const useFernFractal = (): FernFractalContextType => {
  const context = useContext(FernFractalContext);
  if (context === undefined) {
    throw new Error('useFernFractal must be used within a FernFractalProvider');
  }
  
  // Trigger generation on first use
  useEffect(() => {
    if (context && context.triggerGeneration) {
      context.triggerGeneration();
    }
  }, [context]);
  
  return context;
};