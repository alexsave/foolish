// Use the actual Khokhloma pattern PNG
const FRACTAL_PATTERN_PNG = "/khokhloma-pattern.png";

// Main export - optimized version using pre-generated PNG
export const generateCardBackPattern = async (width: number, height: number): Promise<string> => {
    return new Promise((resolve, reject) => {
        const canvas = document.createElement('canvas');
        const ctx = canvas.getContext('2d')!;
        
        // Set canvas size with device pixel ratio for crisp rendering
        const dpr = window.devicePixelRatio || 1;
        canvas.width = width * dpr;
        canvas.height = height * dpr;
        canvas.style.width = width + 'px';
        canvas.style.height = height + 'px';
        ctx.scale(dpr, dpr);
        
        // Enable high-quality image smoothing for better downscaling
        ctx.imageSmoothingEnabled = true;
        ctx.imageSmoothingQuality = 'high';
        
        // Black background
        ctx.fillStyle = '#000000';
        ctx.fillRect(0, 0, width, height);
        
        // Load the pre-generated fractal pattern
        const img = new Image();
        img.onload = () => {
            // Stretch the pattern to fill the entire card
            ctx.drawImage(img, 0, 0, width, height);
            
            resolve(canvas.toDataURL());
        };
        img.onerror = () => {
            console.error('Failed to load fractal pattern, falling back to generated pattern');
            // Fallback to generated pattern if PNG fails to load
            resolve(generateCardBackPatternFallback(width, height));
        };
        img.src = FRACTAL_PATTERN_PNG;
    });
};

// Optimized version using pre-generated PNG (legacy name)
export const generateCardBackPatternOptimized = generateCardBackPattern;

// Even more optimized version if you have the ImageData directly
export const generateCardBackPatternFromImageData = (width: number, height: number, sourceImageData: ImageData): string => {
    const canvas = document.createElement('canvas');
    const ctx = canvas.getContext('2d')!;
    
    // Set canvas size with device pixel ratio
    const dpr = window.devicePixelRatio || 1;
    canvas.width = width * dpr;
    canvas.height = height * dpr;
    canvas.style.width = width + 'px';
    canvas.style.height = height + 'px';
    ctx.scale(dpr, dpr);
    
    // Enable high-quality image smoothing for better downscaling
    ctx.imageSmoothingEnabled = true;
    ctx.imageSmoothingQuality = 'high';
    
    // Black background
    ctx.fillStyle = '#000000';
    ctx.fillRect(0, 0, width, height);
    
    // Create temporary canvas for the source pattern
    const tempCanvas = document.createElement('canvas');
    const tempCtx = tempCanvas.getContext('2d')!;
    tempCanvas.width = sourceImageData.width;
    tempCanvas.height = sourceImageData.height;
    tempCtx.putImageData(sourceImageData, 0, 0);
    
    // Scale and draw the pattern
    const scale = Math.min(width / sourceImageData.width, height / sourceImageData.height);
    const scaledWidth = sourceImageData.width * scale;
    const scaledHeight = sourceImageData.height * scale;
    const x = (width - scaledWidth) / 2;
    const y = (height - scaledHeight) / 2;
    
    ctx.drawImage(tempCanvas, x, y, scaledWidth, scaledHeight);
    
    return canvas.toDataURL();
};

// Fallback fractal generation (renamed from original)
export const generateCardBackPatternFallback = (width: number, height: number): string => {
    // Create SVG-based Khokhloma pattern with proper scaling
    const scale = Math.min(width, height) / 60; // Adjusted scale for fractal leaf
    
    // IFS functions for maple leaf (translated from Python)
    const applyTransform = (x: number, y: number, transformIndex: number): [number, number] => {
        switch(transformIndex) {
            case 0: return [0.8 * x + 0.1, 0.8 * y + 0.04];
            case 1: return [0.5 * x + 0.25, 0.5 * y + 0.4];
            case 2: return [0.4 * x - 0.3 * y + 0.25, 0.3 * x + 0.4 * y + 0.1];
            case 3: return [0.4 * x + 0.3 * y + 0.35, -0.3 * x + 0.4 * y + 0.4];
            default: return [x, y];
        }
    };
    
    // Generate fractal maple leaf points
    const generateMaplePoints = (iterations: number): [number, number][] => {
        let points: [number, number][] = [[0.5, 0.0]];
        
        for (let i = 0; i < iterations; i++) {
            const newPoints: [number, number][] = [];
            for (const point of points) {
                for (let j = 0; j < 4; j++) {
                    newPoints.push(applyTransform(point[0], point[1], j));
                }
            }
            points = newPoints;
        }
        return points;
    };
    
    // Generate points for the fractal maple leaf
    const leafPoints = generateMaplePoints(7); // 7 iterations for good detail
    
    // Convert points to SVG path coordinates
    const svgPoints = leafPoints.map(([x, y]) => {
        const svgX = (x - 0.5) * 40 * scale; // Center and scale
        const svgY = (0.5 - y) * 40 * scale; // Flip Y and scale
        return `${svgX},${svgY}`;
    });
    
    const svgPattern = `
        <svg width="${width}" height="${height}" xmlns="http://www.w3.org/2000/svg">
            <defs>
                <!-- Fractal maple leaf based on IFS -->
                <g id="fractal-maple-leaf">
                    <!-- Create multiple small circles at fractal points -->
                    ${leafPoints.map(([x, y]) => {
                        const svgX = (x - 0.5) * 40 * scale;
                        const svgY = (0.5 - y) * 40 * scale;
                        return `<circle cx="${svgX}" cy="${svgY}" r="${0.8 * scale}" fill="#DC143C" opacity="0.8"/>`;
                    }).join('')}
                    
                    <!-- Add connecting lines for leaf structure -->
                    <path d="M ${svgPoints.join(' L ')}" 
                          fill="none" 
                          stroke="#8B0000" 
                          stroke-width="${0.3 * scale}" 
                          opacity="0.6"/>
                    
                    <!-- Main leaf veins -->
                    <line x1="0" y1="0" x2="0" y2="${-15 * scale}" stroke="#8B0000" stroke-width="${0.8 * scale}"/>
                    <line x1="0" y1="${-8 * scale}" x2="${-8 * scale}" y2="${-12 * scale}" stroke="#8B0000" stroke-width="${0.5 * scale}"/>
                    <line x1="0" y1="${-8 * scale}" x2="${8 * scale}" y2="${-12 * scale}" stroke="#8B0000" stroke-width="${0.5 * scale}"/>
                    
                    <!-- Stem -->
                    <rect x="-${0.8 * scale}" y="0" width="${1.6 * scale}" height="${8 * scale}" fill="#8B0000"/>
                    
                    <!-- Enhanced fractal detail overlay -->
                    <g transform="scale(0.7)">
                        ${leafPoints.slice(0, Math.floor(leafPoints.length / 4)).map(([x, y]) => {
                            const svgX = (x - 0.5) * 40 * scale;
                            const svgY = (0.5 - y) * 40 * scale;
                            return `<circle cx="${svgX}" cy="${svgY}" r="${0.5 * scale}" fill="#DC143C" opacity="0.9"/>`;
                        }).join('')}
                    </g>
                </g>
            </defs>
            
            <!-- Black background -->
            <rect width="100%" height="100%" fill="#000000"/>
            
            <!-- Single central fractal maple leaf -->
            <g transform="translate(${width/2}, ${height/2})">
                <use href="#fractal-maple-leaf"/>
            </g>
        </svg>
    `;
    
    // Convert SVG to data URL
    const svgBlob = new Blob([svgPattern], { type: 'image/svg+xml' });
    return URL.createObjectURL(svgBlob);
};

export const SUIT_MAP: Record<number, string> = {
    // emojis
    0: '♠️',
    1: '♥️',
    2: '♣️',
    3: '♦️',
}

export const VALUE_MAP: Record<number, string> = {
    1: '2',
    2: '3',
    3: '4',
    4: '5',
    5: '6',
    6: '7',
    7: '8',
    8: '9',
    9: '10',
    10: 'J',
    11: 'Q',
    12: 'K',
    13: 'A',
}

// Ok let's actually look at the game state to see if we are defending and modify options