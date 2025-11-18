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

// Global cache for the fractal pattern - using blob URL to reduce JS heap memory
let cachedFernPatternBlobUrl: string | null = null;
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

// OpenGL-based fern pattern generation (like C++ example: CPU computes IFS, GL renders)
export async function generateFernPattern(
    params?: FernParameters
): Promise<string> {
    
    // Return cached pattern if available  
    if (cachedFernPatternBlobUrl) {
        return Promise.resolve(cachedFernPatternBlobUrl);
    }
    
    // Return existing promise if generation is already in progress
    if (fernPatternPromise) {
        return fernPatternPromise;
    }
    
    // Create and cache the promise to prevent duplicate generations
    fernPatternPromise = (async () => {
        // Yield control immediately to let React render first
        await new Promise(resolve => setTimeout(resolve, 0));
        
        const canvasWidth = 200 * 5;
        const canvasHeight = 280 * 5;
        const finalParams = params || createCardParams(canvasWidth, canvasHeight);

        try {
            const { render, colors, probabilities } = finalParams;
            const cumulative = normalizeProbabilities(probabilities);

            const parseHex = (hex: string) => {
                const r = parseInt(hex.slice(1, 3), 16);
                const g = parseInt(hex.slice(3, 5), 16);
                const b = parseInt(hex.slice(5, 7), 16);
                return [r, g, b];
            };
            const [primaryR, primaryG, primaryB] = parseHex(colors.primary);
            const [secondaryR, secondaryG, secondaryB] = parseHex(colors.secondary);
            const [tertiaryR, tertiaryG, tertiaryB] = parseHex(colors.tertiary);

            const rotationRadians = -67 * Math.PI / 180;
            const cosRot = Math.cos(rotationRadians);
            const sinRot = Math.sin(rotationRadians);
            const centerX = canvasWidth / 2;
            const centerY = canvasHeight / 2;

            // Like C++: compute IFS sequence iteratively (each point depends on previous)
            const points = new Float32Array(render.iterations * 2);
            const colorData = new Uint8Array(render.iterations * 3);
            
            let x = 0, y = 0;
            let propagateColor = 0;
            let pointCount = 0;

            // Warmup iterations (like C++ code)
            for (let i = 0; i < 20; i++) {
                const idx = pickTransformIndex(cumulative);
                [x, y] = executeTransformStep(idx, x, y, finalParams);
            }

            // Main loop: compute and store each point (like C++ but batch render)
            for (let i = 0; i < render.iterations; i++) {
                // Yield occasionally to keep UI responsive
                if (i > 0 && i % 100000 === 0) {
                    await new Promise(resolve => setTimeout(resolve, 0));
                }

                const idx = pickTransformIndex(cumulative);
                [x, y] = executeTransformStep(idx, x, y, finalParams);

                if (idx === 4) {
                    let [dx, dy] = randomPointInCircle(finalParams.circles.circle1.cx, finalParams.circles.circle1.cy, finalParams.circles.circle1.r);
                    if (Math.random() < 0.5) { dx = -dx; dy = -dy; }
                    const rotatedDx = dx * cosRot - dy * sinRot;
                    const rotatedDy = dx * sinRot + dy * cosRot;
                    const px = centerX + rotatedDx * render.scaleY + render.translateX;
                    const py = centerY - (rotatedDy * render.scaleY - render.translateY);
                    
                    points[pointCount * 2] = px;
                    points[pointCount * 2 + 1] = py;
                    colorData[pointCount * 3] = secondaryR;
                    colorData[pointCount * 3 + 1] = secondaryG;
                    colorData[pointCount * 3 + 2] = secondaryB;
                    pointCount++;
                    propagateColor = 1;
                    continue;
                }

                if (idx === 5) {
                    let [dx, dy] = randomPointInCircle(finalParams.circles.circle2.cx, finalParams.circles.circle2.cy, finalParams.circles.circle2.r);
                    if (Math.random() < 0.5) { dx = -dx; dy = -dy; }
                    const rotatedDx = dx * cosRot - dy * sinRot;
                    const rotatedDy = dx * sinRot + dy * cosRot;
                    const px = centerX + rotatedDx * render.scaleY + render.translateX;
                    const py = centerY - (rotatedDy * render.scaleY - render.translateY);
                    
                    points[pointCount * 2] = px;
                    points[pointCount * 2 + 1] = py;
                    colorData[pointCount * 3] = tertiaryR;
                    colorData[pointCount * 3 + 1] = tertiaryG;
                    colorData[pointCount * 3 + 2] = tertiaryB;
                    pointCount++;
                    propagateColor = 2;
                    continue;
                }

                if (idx === 0) { propagateColor = 0; }

                let drawX = x, drawY = y;
                if (Math.random() < 0.5) { drawX = -x; drawY = -y; }

                const rotatedX = drawX * cosRot - drawY * sinRot;
                const rotatedY = drawX * sinRot + drawY * cosRot;
                const px = centerX + rotatedX * render.scaleY + render.translateX;
                const py = centerY - (rotatedY * render.scaleY - render.translateY);

                points[pointCount * 2] = px;
                points[pointCount * 2 + 1] = py;
                
                if (propagateColor === 2) {
                    colorData[pointCount * 3] = tertiaryR;
                    colorData[pointCount * 3 + 1] = tertiaryG;
                    colorData[pointCount * 3 + 2] = tertiaryB;
                } else if (propagateColor === 1) {
                    colorData[pointCount * 3] = secondaryR;
                    colorData[pointCount * 3 + 1] = secondaryG;
                    colorData[pointCount * 3 + 2] = secondaryB;
                } else {
                    colorData[pointCount * 3] = primaryR;
                    colorData[pointCount * 3 + 1] = primaryG;
                    colorData[pointCount * 3 + 2] = primaryB;
                }
                pointCount++;
            }

            // Now use OpenGL/WebGL to render all points efficiently
            const canvas = document.createElement('canvas');
            canvas.width = canvasWidth;
            canvas.height = canvasHeight;
            const gl = canvas.getContext('webgl', { preserveDrawingBuffer: true });

            if (!gl) {
                throw new Error('WebGL not available');
            }

            // Simple vertex shader - just positions points
            const vertexShaderSource = `
                attribute vec2 position;
                attribute vec3 color;
                varying vec3 vColor;
                uniform vec2 resolution;
                uniform float pointSize;
                
                void main() {
                    vec2 normalized = position / resolution * 2.0 - 1.0;
                    normalized.y = -normalized.y;
                    gl_Position = vec4(normalized, 0.0, 1.0);
                    gl_PointSize = pointSize;
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

            // Upload position data
            const trimmedPoints = points.slice(0, pointCount * 2);
            const positionBuffer = gl.createBuffer();
            gl.bindBuffer(gl.ARRAY_BUFFER, positionBuffer);
            gl.bufferData(gl.ARRAY_BUFFER, trimmedPoints, gl.STATIC_DRAW);

            const positionLocation = gl.getAttribLocation(program, 'position');
            gl.enableVertexAttribArray(positionLocation);
            gl.vertexAttribPointer(positionLocation, 2, gl.FLOAT, false, 0, 0);

            // Upload color data (normalize to 0-1)
            const trimmedColors = colorData.slice(0, pointCount * 3);
            const normalizedColors = new Float32Array(trimmedColors.length);
            for (let i = 0; i < trimmedColors.length; i++) {
                normalizedColors[i] = trimmedColors[i] / 255;
            }

            const colorBuffer = gl.createBuffer();
            gl.bindBuffer(gl.ARRAY_BUFFER, colorBuffer);
            gl.bufferData(gl.ARRAY_BUFFER, normalizedColors, gl.STATIC_DRAW);

            const colorLocation = gl.getAttribLocation(program, 'color');
            gl.enableVertexAttribArray(colorLocation);
            gl.vertexAttribPointer(colorLocation, 3, gl.FLOAT, false, 0, 0);

            // Set uniforms
            const resolutionLocation = gl.getUniformLocation(program, 'resolution');
            gl.uniform2f(resolutionLocation, canvasWidth, canvasHeight);
            
            const pointSizeLocation = gl.getUniformLocation(program, 'pointSize');
            const glPointSize = Math.max(1.0, Math.floor(render.dotSize));
            gl.uniform1f(pointSizeLocation, glPointSize);

            // Clear and draw all points (like glVertex2fv in C++ but batched)
            gl.clearColor(0, 0, 0, 0);
            gl.clear(gl.COLOR_BUFFER_BIT);
            gl.drawArrays(gl.POINTS, 0, pointCount);

            gl.flush();
            await new Promise(resolve => setTimeout(resolve, 10));

            // Convert to Blob URL
            const blobUrl = await new Promise<string>((resolve) => {
                canvas.toBlob((blob) => {
                    if (blob) {
                        resolve(URL.createObjectURL(blob));
                    } else {
                        resolve('');
                    }
                }, 'image/png', 1.0);
            });
            
            cachedFernPatternBlobUrl = blobUrl;
            fernPatternPromise = null;
            
            return blobUrl;
        } catch (error) {
            console.error('WebGL fern generation failed, using fallback:', error);
            fernPatternPromise = null;
            return generateFernPatternFallback(canvasWidth, canvasHeight, finalParams);
        }
    })();
    
    return fernPatternPromise;
}

// Fallback CPU implementation
async function generateFernPatternFallback(
    canvasWidth: number,
    canvasHeight: number,
    finalParams: FernParameters
): Promise<string> {
    const canvas = document.createElement('canvas');
        canvas.width = canvasWidth;
        canvas.height = canvasHeight;
        const ctx = canvas.getContext('2d');

    if (!ctx) return '';

        const imageData = ctx.createImageData(canvasWidth, canvasHeight);
        const data = new Uint8ClampedArray(imageData.data.buffer);

        const { render, colors, probabilities } = finalParams;
        const cumulative = normalizeProbabilities(probabilities);

        const parseHex = (hex: string) => {
            const r = parseInt(hex.slice(1, 3), 16);
            const g = parseInt(hex.slice(3, 5), 16);
            const b = parseInt(hex.slice(5, 7), 16);
            return [r, g, b];
        };
        const [primaryR, primaryG, primaryB] = parseHex(colors.primary);
        const [secondaryR, secondaryG, secondaryB] = parseHex(colors.secondary);
        const [tertiaryR, tertiaryG, tertiaryB] = parseHex(colors.tertiary);

        const rotationRadians = -67 * Math.PI / 180;
        const cosRot = Math.cos(rotationRadians);
        const sinRot = Math.sin(rotationRadians);
        const centerX = canvasWidth / 2;
        const centerY = canvasHeight / 2;

        let x = 0, y = 0;
    let propagateColor = 0;
        const pointSize = Math.max(1, Math.floor(render.dotSize));
        
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

        for (let i = 0; i < 20; i++) {
            const idx = pickTransformIndex(cumulative);
            [x, y] = executeTransformStep(idx, x, y, finalParams);
        }

        for (let i = 0; i < render.iterations; i++) {
            if (i > 0 && i % 1000000 === 0) {
                await new Promise(resolve => setTimeout(resolve, 0));
            }
            
            const idx = pickTransformIndex(cumulative);
            [x, y] = executeTransformStep(idx, x, y, finalParams);

            if (idx === 4) {
                let [dx, dy] = randomPointInCircle(finalParams.circles.circle1.cx, finalParams.circles.circle1.cy, finalParams.circles.circle1.r);
            if (Math.random() < 0.5) { dx = -dx; dy = -dy; }
                const rotatedDx = dx * cosRot - dy * sinRot;
                const rotatedDy = dx * sinRot + dy * cosRot;
                const px = centerX + rotatedDx * render.scaleY + render.translateX;
                const py = centerY - (rotatedDy * render.scaleY - render.translateY);
                drawPixel(px, py, secondaryR, secondaryG, secondaryB);
                propagateColor = 1;
                continue;
            }

            if (idx === 5) {
                let [dx, dy] = randomPointInCircle(finalParams.circles.circle2.cx, finalParams.circles.circle2.cy, finalParams.circles.circle2.r);
            if (Math.random() < 0.5) { dx = -dx; dy = -dy; }
                const rotatedDx = dx * cosRot - dy * sinRot;
                const rotatedDy = dx * sinRot + dy * cosRot;
                const px = centerX + rotatedDx * render.scaleY + render.translateX;
                const py = centerY - (rotatedDy * render.scaleY - render.translateY);
                drawPixel(px, py, tertiaryR, tertiaryG, tertiaryB);
                propagateColor = 2;
                continue;
            }

        if (idx === 0) { propagateColor = 0; }

            let drawX = x, drawY = y;
        if (Math.random() < 0.5) { drawX = -x; drawY = -y; }

            const rotatedX = drawX * cosRot - drawY * sinRot;
            const rotatedY = drawX * sinRot + drawY * cosRot;
            const px = centerX + rotatedX * render.scaleY + render.translateX;
            const py = centerY - (rotatedY * render.scaleY - render.translateY);

            if (propagateColor === 2) {
                drawPixel(px, py, tertiaryR, tertiaryG, tertiaryB);
            } else if (propagateColor === 1) {
                drawPixel(px, py, secondaryR, secondaryG, secondaryB);
            } else {
                drawPixel(px, py, primaryR, primaryG, primaryB);
            }
        }

        ctx.putImageData(imageData, 0, 0);
    
    // Convert to Blob URL instead of data URL
    return new Promise((resolve) => {
        canvas.toBlob((blob) => {
            if (blob) {
                resolve(URL.createObjectURL(blob));
            } else {
                resolve('');
            }
        }, 'image/png', 1.0);
    });
}

// React Context for sharing the fractal pattern
interface FernFractalContextType {
  fernPattern: string;
  isLoading: boolean;
  triggerGeneration?: () => void;
}

const FernFractalContext = createContext<FernFractalContextType | undefined>(undefined);

export const FernFractalProvider: React.FC<{ children: React.ReactNode }> = ({ children }) => {
  const [fernPattern, setFernPattern] = useState<string>(cachedFernPatternBlobUrl || '');
  const [isLoading, setIsLoading] = useState<boolean>(!cachedFernPatternBlobUrl);
  const generationRequestedRef = useRef<boolean>(false);

  // Function to trigger generation (called by useFernFractal when component needs it)
  const triggerGeneration = useCallback(() => {
    if (cachedFernPatternBlobUrl) {
      // Already have cached pattern
      setFernPattern(cachedFernPatternBlobUrl);
      setIsLoading(false);
      return;
    }

    if (generationRequestedRef.current) {
      // Already requested
      return;
    }

    generationRequestedRef.current = true;
    
    generateFernPattern()
      .then((dataUrl: string) => {
        setFernPattern(dataUrl);
        setIsLoading(false);
      })
      .catch((error: any) => {
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